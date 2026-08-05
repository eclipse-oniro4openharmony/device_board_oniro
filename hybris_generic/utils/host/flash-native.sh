#!/bin/bash
#
# Copyright (C) 2026 Oniro / Hybris Generic.
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Flash a native-boot OHOS build to a hybris_generic device sitting in LK
# fastboot.  One pass, no fastbootd switch, slot _a.
#
# Devices (-d/--device, or $HYBRIS_DEVICE):
#
#   x23   Volla Phone X23 (vidofnir) — MT6789, Halium 12, boot header v2
#     super          <- out/hybris_generic/super.img               (kernel/x23/build_super_img.sh)
#     boot_a         <- out/hybris_generic/boot-chainload.img      (kernel/x23/build_boot_img_chainload.sh)
#     vendor_boot_a  <- kernel/linux/volla-vidofnir/out/vendor_boot.img
#                       OPTIONAL — only exists once the OHOS-patched kernel
#                       is in the chainload; it replaces vendor_boot's kernel
#                       modules so vermagic matches the running kernel.
#                       Without matching modules /dev/access_token_id and
#                       many other drivers fail to load.
#
#   ansuz Volla Phone Plinius (ansuz) — MT6878, Halium 14, boot header v4
#     super          <- out/hybris_generic/super.img               (kernel/ansuz/build_super_img.sh)
#     boot_a         <- kernel/linux/volla-ansuz/out/boot.img      (kernel/ansuz/build_kernel.sh)
#     init_boot_a    <- out/hybris_generic/init_boot-chainload.img (kernel/ansuz/build_init_boot_chainload.sh)
#     vendor_boot_a  <- out/hybris_generic/vendor_boot-ohos.img    (same script; carries
#                       "ohos.boot.hardware=ansuz lsm=selinux" — NEVER flash this under UT)
#
# `super` carries the OHOS partitions *and* the Halium ones, and its LP
# geometry is per-device: flash only a super.img produced by the matching
# kernel/<device>/build_super_img.sh.
#
# No fastbootd switch.  `super` is an ordinary *physical* partition in the
# GPT and build_super_img.sh produces a complete lpmake image (LP metadata
# + every sub-partition baked in), so LK fastboot can write it raw.
# fastbootd is only needed to flash an *individual logical* partition
# (`fastboot flash system_a …`), which this script never does.  Skipping
# fastbootd also avoids the Halium-boot splash hang that can leave the
# device unable to reach userspace fastboot.
#
# Getting the device into LK fastboot:
#   from OHOS: hdc shell "param set ohos.startup.powerctrl reboot,bootloader"
#   from UT:   adb shell "echo <pw> | sudo -S systemctl reboot --reboot-argument=bootloader"
#   by hand:   power off, hold Vol-Down + Power, select `fastboot`
#
# Restoring the stock/UT install:
#   x23:   out/hybris_generic/backups/boot_a.bak
#   ansuz: halium-blobs/ansuz/backups/{boot,init_boot,vendor_boot}_a.img
#          (super's UT rootfs lives in system_a, which we overwrite — a full
#           UT restore additionally needs the channel rootfs push, plan P9)
#
# Host prerequisites: `fastboot` (Android platform-tools) on whichever host
# the phone is plugged into — this one, or the `--remote` ssh host.
#
# Usage:  ./flash-native.sh -d <device> [options]        (--help for the rest)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OHOS_ROOT="$(cd "$HERE/../../../../../.." && pwd)"
OUT="${OUT:-$OHOS_ROOT/out/hybris_generic}"

DEVICE="${HYBRIS_DEVICE:-}"
REMOTE="${FLASH_REMOTE:-}"                       # empty = phone on this host
REMOTE_DIR="${FLASH_REMOTE_DIR:-/tmp/flash-native}"
SERIAL=""
ONLY=""
REBOOT=1
DRY_RUN=0
declare -a IMAGE_OVERRIDES=()

usage() {
    cat <<'EOF'
Usage: flash-native.sh -d <device> [options]

Flash a native-boot OHOS build to a hybris_generic device in LK fastboot.

Devices:
  x23   | vidofnir   Volla Phone X23      super, boot_a, [vendor_boot_a]
  ansuz | plinius    Volla Phone Plinius  super, boot_a, init_boot_a, vendor_boot_a

Options:
  -d, --device NAME        device to flash                    (env HYBRIS_DEVICE)
  -r, --remote [USER@]HOST run fastboot on HOST over ssh instead of on this
                           machine; the images are staged there with scp
                           first.  Use this when the phone is plugged into a
                           relay box (e.g. a Raspberry Pi) rather than into
                           the build host.                    (env FLASH_REMOTE)
      --remote-dir DIR     staging directory on HOST      (env FLASH_REMOTE_DIR)
                           (default: /tmp/flash-native; cleaned up afterwards)
  -s, --serial SERIAL      pass `-s SERIAL` to fastboot (several devices attached)
      --only LIST          comma-separated partitions to flash, e.g.
                           --only boot_a,init_boot_a  (skips the 3 GB super)
      --image PART=PATH    override the image flashed to PART (repeatable)
      --no-reboot          leave the device in fastboot when done
  -n, --dry-run            print the commands instead of running them
  -h, --help               this text

Examples:
  # phone plugged into this host — the normal case
  ./flash-native.sh -d ansuz

  # phone plugged into an ssh-reachable relay box
  ./flash-native.sh -d ansuz --remote frankpi

  # kernel-only rebuild: skip super, keep the device in fastboot
  ./flash-native.sh -d x23 --only boot_a --no-reboot
EOF
}

die() { echo "Error: $*" >&2; exit 1; }

need_val() { [[ $# -ge 2 ]] || die "$1 requires a value"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--device)     need_val "$@"; DEVICE="$2"; shift 2 ;;
        --device=*)      DEVICE="${1#*=}"; shift ;;
        -r|--remote)     need_val "$@"; REMOTE="$2"; shift 2 ;;
        --remote=*)      REMOTE="${1#*=}"; shift ;;
        --remote-dir)    need_val "$@"; REMOTE_DIR="$2"; shift 2 ;;
        --remote-dir=*)  REMOTE_DIR="${1#*=}"; shift ;;
        -s|--serial)     need_val "$@"; SERIAL="$2"; shift 2 ;;
        --serial=*)      SERIAL="${1#*=}"; shift ;;
        --only)          need_val "$@"; ONLY="$2"; shift 2 ;;
        --only=*)        ONLY="${1#*=}"; shift ;;
        --image)         need_val "$@"; IMAGE_OVERRIDES+=("$2"); shift 2 ;;
        --image=*)       IMAGE_OVERRIDES+=("${1#*=}"); shift ;;
        --no-reboot)     REBOOT=0; shift ;;
        -n|--dry-run)    DRY_RUN=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               echo "Unknown argument: $1" >&2; echo >&2; usage >&2; exit 2 ;;
    esac
done

# ---------------------------------------------------------------------------
# Device table — partition name, image, and whether it must exist.
# ---------------------------------------------------------------------------
declare -a PART=() IMG=() REQ=()
add_artifact() { PART+=("$1"); IMG+=("$2"); REQ+=("$3"); }

case "$DEVICE" in
    x23|vidofnir)
        DEVICE="x23"
        DEVICE_LABEL="Volla Phone X23 (vidofnir)"
        add_artifact super         "$OUT/super.img"          required
        add_artifact boot_a        "$OUT/boot-chainload.img" required
        add_artifact vendor_boot_a \
            "${OHOS_VENDOR_BOOT:-$OHOS_ROOT/kernel/linux/volla-vidofnir/out/vendor_boot.img}" optional
        ;;
    ansuz|plinius)
        DEVICE="ansuz"
        DEVICE_LABEL="Volla Phone Plinius (ansuz)"
        add_artifact super         "$OUT/super.img"                          required
        add_artifact boot_a        "$OHOS_ROOT/kernel/linux/volla-ansuz/out/boot.img" required
        add_artifact init_boot_a   "$OUT/init_boot-chainload.img"            required
        add_artifact vendor_boot_a "$OUT/vendor_boot-ohos.img"               required
        ;;
    "")
        die "no device selected — pass -d x23 or -d ansuz (or set HYBRIS_DEVICE)"
        ;;
    *)
        die "unknown device '$DEVICE' — known: x23 (vidofnir), ansuz (plinius)"
        ;;
esac

# --image PART=PATH overrides
for ov in ${IMAGE_OVERRIDES[@]+"${IMAGE_OVERRIDES[@]}"}; do
    [[ "$ov" == *=* ]] || die "--image expects PART=PATH, got '$ov'"
    ov_part="${ov%%=*}"; ov_path="${ov#*=}"
    found=0
    for i in "${!PART[@]}"; do
        if [[ "${PART[$i]}" == "$ov_part" ]]; then
            IMG[$i]="$ov_path"; REQ[$i]="required"; found=1
        fi
    done
    (( found )) || die "--image: '$ov_part' is not a partition of $DEVICE (${PART[*]})"
done

# --only LIST filter
if [[ -n "$ONLY" ]]; then
    IFS=',' read -r -a want <<< "$ONLY"
    for w in "${want[@]}"; do
        [[ " ${PART[*]} " == *" $w "* ]] \
            || die "--only: '$w' is not a partition of $DEVICE (${PART[*]})"
    done
    declare -a sel_part=() sel_img=() sel_req=()
    for i in "${!PART[@]}"; do
        if [[ " ${want[*]} " == *" ${PART[$i]} "* ]]; then
            sel_part+=("${PART[$i]}"); sel_img+=("${IMG[$i]}"); sel_req+=(required)
        fi
    done
    PART=("${sel_part[@]}"); IMG=("${sel_img[@]}"); REQ=("${sel_req[@]}")
fi

# Drop missing optional images; a missing required one is fatal.
declare -a keep_part=() keep_img=()
for i in "${!PART[@]}"; do
    if [[ -f "${IMG[$i]}" ]]; then
        keep_part+=("${PART[$i]}"); keep_img+=("${IMG[$i]}")
    elif [[ "${REQ[$i]}" == "required" ]]; then
        die "${IMG[$i]} missing (needed for ${PART[$i]}) — build it first"
    else
        echo "Note: ${IMG[$i]} not present — skipping ${PART[$i]}"
    fi
done
(( ${#keep_part[@]} )) || die "nothing to flash"
PART=("${keep_part[@]}"); IMG=("${keep_img[@]}")

# ---------------------------------------------------------------------------
# Command plumbing — everything below runs either locally or over ssh.
# ---------------------------------------------------------------------------
# Join args into one shell-safe command string (for `ssh host "<cmd>"` and
# for --dry-run output).  Only the args that need it get quoted, so the
# printed commands stay copy-pasteable.
quote() {
    local a
    local -a out=()
    for a in "$@"; do
        if [[ "$a" =~ ^[A-Za-z0-9_@%+=:,./-]+$ ]]; then
            out+=("$a")
        else
            out+=("'${a//\'/\'\\\'\'}'")
        fi
    done
    echo "${out[*]}"
}

run() {                     # honour --dry-run
    if (( DRY_RUN )); then
        printf '+ %s\n' "$(quote "$@")"
    else
        "$@"
    fi
}

sh_remote() { run ssh "$REMOTE" "$(quote "$@")"; }

fb() {                      # one fastboot invocation
    local -a cmd=(fastboot)
    if [[ -n "$SERIAL" ]]; then cmd+=(-s "$SERIAL"); fi
    cmd+=("$@")
    if [[ -n "$REMOTE" ]]; then
        sh_remote "${cmd[@]}"
    else
        run "${cmd[@]}"
    fi
}

fb_query() {                # read-only fastboot, always executed
    if [[ -n "$REMOTE" ]]; then
        ssh "$REMOTE" "$(quote fastboot "$@")" 2>&1
    else
        fastboot "$@" 2>&1
    fi
}

human() { numfmt --to=iec --suffix=B "$(stat -c %s "$1")" 2>/dev/null || echo "?"; }

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------
if [[ -n "$REMOTE" ]]; then
    rc=0; ssh "$REMOTE" 'command -v fastboot >/dev/null' || rc=$?
    if (( rc == 255 )); then
        die "cannot ssh to '$REMOTE' — check the host name / your ~/.ssh/config"
    elif (( rc != 0 )); then
        die "no fastboot on $REMOTE (install android-tools-fastboot there)"
    fi
else
    command -v fastboot >/dev/null \
        || die "fastboot not found — install Android platform-tools"
fi

if (( ! DRY_RUN )); then
    devs="$(fb_query devices || true)"
    devs="$(grep -c 'fastboot' <<< "$devs" || true)"
    (( devs > 0 )) || die "no device in fastboot${REMOTE:+ behind $REMOTE} — reboot the phone into LK fastboot first:
  from OHOS: hdc shell \"param set ohos.startup.powerctrl reboot,bootloader\"
  by hand:   power off, hold Vol-Down + Power, select \`fastboot\`"
    if [[ -n "$SERIAL" ]]; then
        fb_query devices | grep -q "^$SERIAL[[:space:]]" \
            || die "serial '$SERIAL' is not in fastboot${REMOTE:+ behind $REMOTE}"
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo "Device:   $DEVICE — $DEVICE_LABEL"
if [[ -n "$REMOTE" ]]; then
    echo "fastboot: on $REMOTE over ssh${SERIAL:+ [-s $SERIAL]}"
else
    echo "fastboot: local — phone plugged into this host${SERIAL:+ [-s $SERIAL]}"
fi
echo "Flashing:"
for i in "${!PART[@]}"; do
    printf '  %-14s <- %s (%s)\n' "${PART[$i]}" "${IMG[$i]}" "$(human "${IMG[$i]}")"
done
echo

# ---------------------------------------------------------------------------
# Remote mode: stage the images on the relay host first.  fastboot can only
# flash from a local file, so each image is scp'd to $REMOTE_DIR/<part>.img.
# ---------------------------------------------------------------------------
declare -a SRC=("${IMG[@]}")
if [[ -n "$REMOTE" ]]; then
    declare -a stale=()
    for i in "${!PART[@]}"; do
        SRC[$i]="$REMOTE_DIR/${PART[$i]}.img"
        stale+=("${SRC[$i]}")
    done
    echo "==> Staging on $REMOTE:$REMOTE_DIR"
    sh_remote mkdir -p "$REMOTE_DIR"
    sh_remote rm -f "${stale[@]}"
    for i in "${!PART[@]}"; do
        run scp "${IMG[$i]}" "$REMOTE:${SRC[$i]}"
    done
fi

# ---------------------------------------------------------------------------
# Flash
# ---------------------------------------------------------------------------
n=${#PART[@]}
for i in "${!PART[@]}"; do
    echo "==> [$((i + 1))/$n] Flashing ${PART[$i]}"
    fb flash "${PART[$i]}" "${SRC[$i]}"
done

if [[ -n "$REMOTE" ]]; then
    sh_remote rm -f "${SRC[@]}"          # the relay's /tmp is usually tmpfs
fi

if (( REBOOT )); then
    echo "==> Rebooting into native OHOS"
    fb reboot
else
    echo "==> Done — device left in fastboot (--no-reboot)"
fi
