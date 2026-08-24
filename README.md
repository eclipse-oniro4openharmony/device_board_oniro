# Oniro Board Support Packages

This repository contains Board Support Packages (BSPs) for devices supported by
the Oniro Project:

| Target | Device | Notes |
|--------|--------|-------|
| `x86_general` | QEMU virtual device | The **Oniro Emulator** — an `x86_64` build run under QEMU/KVM. Best starting point for app and platform development. |
| `hybris_generic` | Volla Phone X23, Volla Phone Plinius | Oniro booting **natively** on MediaTek hardware via libhybris. See [Volla phones](#oniro-on-volla-phones-hybris_generic-target). |

These BSPs enable developers to build and deploy Oniro on supported hardware.

---

## Oniro Emulator (x86_general target)

Step-by-step instructions to **build and run the Oniro Emulator** from the
`OpenHarmony-6.1-Release` source.

### 📦 Prerequisites

- A Linux host with [Docker](https://docs.docker.com/engine/install/).
- For hardware acceleration, KVM (`/dev/kvm` present and accessible). Without
  it the emulator still runs under TCG, just slower.
- Enough free disk for the source tree, toolchains, and build output (~90 GB).
- Have followed the [Quick Build Setup](https://docs.oniroproject.org/device-development/building-oniro/)
  guide to prepare your environment.

### ⬇️ Download the source

```bash
repo init -u https://github.com/eclipse-oniro4openharmony/manifest.git \
     -b OpenHarmony-6.1-Release -m oniro.xml --no-repo-verify
repo sync -c
repo forall -c 'git lfs pull'
```

### 🐳 Set up the build container

The build runs inside the OpenHarmony build container. The upstream
`docker_oh_standard:3.2` image is missing a few host tools that a *cold* build
needs — autotools (for `third_party/libnl`) and `cmake` (for
`third_party/libtiff`) — so this repo ships a Dockerfile that adds them on top
of the upstream image:

```bash
# Build the image once (see device/board/oniro/docker/Dockerfile for the
# exact package list).
sudo docker build -t oniro-oh-standard:3.2 device/board/oniro/docker

# Start a long-lived container with the source tree mounted at the workdir.
# Mounting a persistent ccache dir makes rebuilds dramatically faster.
sudo docker run -d -it --name oniro-build \
     -w /home/openharmony \
     -v "$PWD":/home/openharmony/workdir \
     -v "$HOME/.ccache":/root/.ccache \
     oniro-oh-standard:3.2 /bin/bash
```

> The commands below are shown as `docker exec` into that container. You can
> equally `docker exec -it oniro-build bash` and run them interactively from
> `/home/openharmony/workdir`.

### 🩹 Apply source patches

The build requires the shared Oniro patch series applied to several
subsystems (build, selinux_adapter, mindspore, storage_service, bms, …).
The same series also carries the `hybris_generic` adaptations, so one
patched tree builds every Oniro product. Apply it once per fresh tree:

```bash
bash device/board/oniro/system_patch/do_patch.sh
```

### 🛠️ Build the images

```bash
sudo docker exec -u root -w /home/openharmony/workdir oniro-build \
     ./build.sh --product-name x86_general --ccache
```

On success the image set is written to:

```
out/x86_general/packages/phone/images/
```
(`system.img`, `vendor.img`, `userdata.img`, `updater.img`, the `bzImage`
kernel, `ramdisk.img`, and the `run.sh` / `run.bat` launchers.)

### 🔄 (Optional) Revert patches

```bash
bash device/board/oniro/system_patch/undo_patch.sh
```

### ▶️ Run the emulator

From the images directory:

```bash
cd out/x86_general/packages/phone/images

./run.sh              # Linux/macOS — graphical (SDL) window, KVM on Linux
./run.sh --headless   # no window: VNC on :0 (TCP 5900) + telnet serial (4444)
.\run.bat             # Windows
```

`run.sh` auto-selects acceleration (KVM on Linux, HVF/TCG on macOS, WHPX on
Windows) and switches to headless automatically when no display is available
(e.g. over SSH). Useful flags: `-s N` (vCPUs), `-m SIZE` (RAM), `-r WxH`
(resolution). Run `./run.sh --help` for the full list.

> **Note:** the build writes the images as `root` when the container runs as
> root. If `run.sh` fails with `Could not reopen file: Permission denied`,
> take ownership of the images first: `sudo chown "$USER" *.img bzImage`.

### 🔌 Connect and verify

QEMU forwards the guest hdc port to the host on `127.0.0.1:55555`:

```bash
hdc tconn 127.0.0.1:55555
hdc shell "uname -a"
hdc shell "ps -A | wc -l"        # system processes up
```

Give it a minute to boot; the OHOS lockscreen then renders (SDL window, or over
VNC in headless mode).

---

## Oniro on Volla phones (hybris_generic target)

Oniro runs **natively** on two MediaTek Volla phones — the device boots straight
into Oniro. A Halium boot image chain-loads directly into OHOS `init`, and a companion `androidd` process
runs the device's Android (Halium) HAL services in a child mount/PID namespace so
the OHOS graphics/HAL stack can reach the hardware through **libhybris**.

| Device | Codename | SoC | Halium / kernel | Chainload lives in |
|---|---|---|---|---|
| Volla Phone X23 | `vidofnir` | MT6789 (Helio G99) | Halium 12, 5.10 vendor kernel | `boot_a` (header v2) |
| Volla Phone Plinius | `ansuz` | MT6878 (Dimensity 7300) | Halium 14, android14-6.1 GKI | `init_boot_a` (header v4) |

**One image set serves both devices.** They are told apart at *runtime* by
`ohos.boot.hardware` (set from the per-device `vendor_boot` cmdline), which selects
`init.<device>.cfg` / `fstab.<device>` — there is no build-time device switch.

### 📋 Prerequisites

- The phone with an **unlocked bootloader**, and Ubuntu Touch (Halium) installed at
  least once: the Android `system`/`vendor` blobs and the donor boot images come
  from that install.
- **`fastboot`** (Android platform-tools) on whichever host the phone is plugged into.
- An OHOS source tree and build container — see
  [Set up the build container](#-set-up-the-build-container) above.
- **Halium blobs** for the device. They provide the Android HAL runtime; an
  OHOS-only image builds and boots without them, but has no graphics.
  - X23: fetched host-side, SHA256-pinned, by
    `hybris_generic/utils/host/pull-halium-blobs.sh` (once per tree).
  - Plinius: dumped from the live UT device into
    `hybris_generic/halium-blobs/ansuz/` as
    `halium_{system,vendor,vendor_dlkm,system_dlkm}_a.img` — no public download.

### 🛠️ Build

Shared steps first (`do_patch.sh` runs host-side — `git am` needs your git identity;
without it the build stops with an unknown-product error, since the series is what
registers `hybris_generic`):

```bash
# once per fresh checkout
bash device/board/oniro/system_patch/do_patch.sh

# once — Oniro distribution HAPs (app store, keyboard); needs network + OHOS SDK
bash vendor/oniro/oniro-haps/build-oniro-haps.sh

# OHOS rootfs: system / vendor / sys_prod / chip_prod
sudo docker exec -u root -w /home/openharmony/workdir oniro-build \
     ./build.sh --product-name hybris_generic --ccache

# the container builds as root — take back the dirs the host-side steps write to
sudo chown "$USER" out out/hybris_generic
```

Then the device-specific images (host-side; `build_kernel.sh` also clones the port
repo and downloads the Halium tools — `lpmake`, `mkbootimg` — the later steps need):

```bash
D=device/board/oniro/hybris_generic

# --- Volla X23 ---
bash $D/kernel/x23/build_kernel.sh
bash $D/utils/host/pull-halium-blobs.sh          # once
bash $D/kernel/x23/build_super_img.sh
bash $D/kernel/x23/build_boot_img_chainload.sh

# --- Volla Plinius ---
bash $D/kernel/ansuz/build_kernel.sh
bash $D/kernel/ansuz/build_super_img.sh
bash $D/kernel/ansuz/build_init_boot_chainload.sh
```

Artifacts:

| Device | Images |
|---|---|
| X23 | `out/hybris_generic/{super.img, boot-chainload.img}`, `kernel/linux/volla-vidofnir/out/vendor_boot.img` |
| Plinius | `out/hybris_generic/{super.img, init_boot-chainload.img, vendor_boot-ohos.img}`, `kernel/linux/volla-ansuz/out/boot.img` |

**Kernel notes.** The chainload runs the OHOS-patched kernel (staging drivers:
`access_tokenid`, `hilog`, `hievent`, binder token-id; the Plinius adds hmdfs,
sharefs, epfs and the DFX set). Its matching `vendor_boot` must always be flashed
with it, or the vendor modules fail to load with a vermagic mismatch. On the
Plinius, `vendor_boot-ohos.img` carries `ohos.boot.hardware=ansuz lsm=selinux` —
never flash it under Ubuntu Touch.

### ⚡ Flash

Put the device into LK fastboot — from OHOS
`hdc shell "param set ohos.startup.powerctrl reboot,bootloader"`, or by hand: power
off, hold **Vol-Down + Power**, pick `fastboot`. Then flash every partition in one
pass (slot `_a`):

```bash
bash device/board/oniro/hybris_generic/utils/host/flash-native.sh -d x23     # or: -d ansuz
```

### 🔌 Verify

~60–70 s after reboot the phone enumerates over USB and answers hdc; the Oniro
lockscreen renders on the panel.

```bash
hdc list targets
hdc shell "uname -a"
```

### 🧩 Architecture

```
  MTK LK bootloader
        │  (loads the chainload image: boot_a on the X23, init_boot_a on the Plinius)
        ▼
  Linux kernel  +  Halium ramdisk   (ramdisk /init = init-chainload.sh)
        │  • modprobe vendor modules  • parse-android-dynparts → /dev/mapper/*
        │  • mount OHOS system_a at /root, Halium system at /root/android
        ▼
  exec env OHOS_NATIVE_BOOT=1 chroot /root /system/bin/init --second-stage
        │  (kernel keeps PID 1 across exec — OHOS init becomes PID 1)
        ▼
  OHOS userspace: samgr, hdf, render_service, launcher …
        └── androidd → clone() child NS → Halium /system/bin/init
                        → hwservicemanager, composer@2.x, gralloc@4.0 (Android HALs)
```

A single custom `super` partition (LP-formatted) carries both worlds:
`system_a`, `vendor_a`, `sys_prod_a`, `chip_prod_a` (OHOS) plus `halium_system_a`
and `halium_vendor_a` (Android HAL runtime; the Plinius adds
`halium_vendor_dlkm_a` and `halium_system_dlkm_a`). Android's `hwbinder`/`vndbinder`
are shared between the OHOS root namespace and `androidd`'s Halium namespace — that
shared binder is the bridge over which libhybris-based OHOS services
(`composer_host`, `allocator_host`) call into the Halium HAL services. libhybris
loads the Android EGL/HWC2/gralloc `.so`s with an embedded bionic linker and remaps
`/system`, `/vendor` to `/android/…`.

### 📈 Status

Native boot, USB hdc, display, touch, WiFi and audio work on both phones. The
Plinius is the actively developed device and additionally has hardware keys,
sensors, vibrator, camera and cellular (mobile data, SMS, voice) up; Bluetooth,
NFC and fingerprint are not enabled yet on either.

---

## Contributing

Contributions to improve the board support packages are welcome. Please submit
a pull request with your proposed changes.

## License

This repository is distributed under the Apache 2.0 License.
