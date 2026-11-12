#!/usr/bin/env bash
# ---------------------------------------------------------------
#  Oniro / OpenHarmony QEMU Emulator Launcher
#
#  The image directory holds either an x86_64 guest (bzImage) or an arm64
#  guest (Image); the launcher picks QEMU and the machine to match.
#
#  Supported host platforms:
#    x86_64 guest:
#    - Linux  (KVM)
#    - macOS  x86_64 (HVF) / Apple Silicon (TCG, slow)
#    - Windows via Git Bash, MSYS2, or Cygwin (WHPX)
#    arm64 guest:
#    - macOS  Apple Silicon (HVF)
#    - Linux  arm64 (KVM)
#    - any other host, or one without KVM/HVF (TCG, slow)
# ---------------------------------------------------------------
set -euo pipefail

# ---- Defaults --------------------------------------------------
DEFAULT_SMP=6
DEFAULT_MEM="4096M"
DEFAULT_RES="360x720"
DEFAULT_CONNECT_KEY="127.0.0.1:55555"
DEFAULT_VNC_DISPLAY=0           # VNC :0 -> TCP 5900
DEFAULT_SERIAL_PORT=4444        # telnet serial console

# ---- Usage -----------------------------------------------------
usage() {
  cat <<EOF
Usage: $(basename "$0") [OPTIONS] [IMAGE_DIR]

Launch the Oniro/OpenHarmony emulator in QEMU.

Arguments:
  IMAGE_DIR              Directory containing emulator images.
                         Defaults to the script's own directory.

Options:
  -c, --connect KEY      HDC connect key in host:port form
                         (default: $DEFAULT_CONNECT_KEY)
  -s, --smp N            Number of virtual CPUs (default: $DEFAULT_SMP)
  -m, --memory SIZE      RAM size (default: $DEFAULT_MEM)
  -r, --resolution WxH   Display resolution (default: $DEFAULT_RES)
      --headless         Headless mode – VNC display + telnet serial
                         (no local window required)
      --vnc-display N    VNC display number (default: $DEFAULT_VNC_DISPLAY,
                         i.e. TCP port $((5900 + DEFAULT_VNC_DISPLAY)))
      --serial-port PORT Telnet serial console port (default: $DEFAULT_SERIAL_PORT)
      --display UI       Window backend: gtk, cocoa or sdl (default: cocoa on
                         macOS, else gtk if QEMU has it, else sdl). The gtk and
                         cocoa windows have Machine > Power Down, which is the
                         phone's power button.
      --audio DRIVER     Host audio backend for the guest's sound card, as a
                         QEMU -audiodev driver with optional suboptions, e.g.
                         pa, pipewire, alsa, coreaudio, dsound, sdl,
                         wav,path=out.wav or none (default: auto, i.e.
                         coreaudio on macOS, dsound on Windows, and on Linux
                         the PulseAudio/PipeWire server or ALSA if found)
      --wifi             Give the guest WiFi instead of ethernet: a simulated
                         access point "VirtWifi" (virt_wifi) carries the
                         network once you join it in Settings > WLAN, and the
                         ethernet link only carries hdc.
      --shared-dir DIR   Share a host directory with the guest. It appears
                         at /mnt/host (hdc shell; not visible to apps).
                         Needs a QEMU with 9p support (Linux, macOS).
      --userdata-size SIZE
                         Grow the /data image to SIZE (e.g. 8G, 16384M)
                         before starting. The guest resizes the file system
                         at boot. Images never shrink.
      --instance N       Run instance N (1-99) alongside others. It keeps
                         its own /data in IMAGE_DIR/instances/N (a copy of
                         userdata.img made on first use; delete the
                         directory to reset it) and, unless given, uses HDC
                         port 55555+N, VNC display N and serial port 4444+N.
  -a, --accel MODE       Accelerator: auto, kvm, hvf, whpx or tcg (default:
                         auto, i.e. the host's hypervisor; an arm64 guest
                         falls back to TCG where there is none)
  -q, --qemu PATH        Path to the QEMU binary (default: qemu-system-x86_64
                         for an x86_64 guest, qemu-system-aarch64 for arm64)
  -h, --help             Show this help message
EOF
  exit 0
}

# ---- Helpers ---------------------------------------------------
info()  { echo "[INFO]  $*"; }
warn()  { echo "[WARN]  $*" >&2; }
error() { echo "[ERROR] $*" >&2; exit 1; }

# ---- Parse arguments -------------------------------------------
image_dir=""
connect_key="$DEFAULT_CONNECT_KEY"
smp="$DEFAULT_SMP"
mem="$DEFAULT_MEM"
resolution="$DEFAULT_RES"
headless=false
vnc_display="$DEFAULT_VNC_DISPLAY"
serial_port="$DEFAULT_SERIAL_PORT"
ui=""
audio="auto"
qemu_bin="${QEMU_BIN:-}"
accel="${QEMU_ACCEL:-auto}"
wifi=false
shared_dir=""
userdata_size=""
instance=""
connect_set=false
vnc_set=false
serial_set=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    -c|--connect)     connect_key="${2:?missing connect key}"; connect_set=true; shift 2 ;;
    -s|--smp)         smp="${2:?missing smp value}"; shift 2 ;;
    -m|--memory)      mem="${2:?missing memory value}"; shift 2 ;;
    -r|--resolution)  resolution="${2:?missing resolution}"; shift 2 ;;
    --headless)       headless=true; shift ;;
    --vnc-display)    vnc_display="${2:?missing VNC display number}"; vnc_set=true; shift 2 ;;
    --serial-port)    serial_port="${2:?missing serial port}"; serial_set=true; shift 2 ;;
    --display)        ui="${2:?missing display backend}"; shift 2 ;;
    --audio)          audio="${2:?missing audio driver}"; shift 2 ;;
    --wifi)           wifi=true; shift ;;
    --shared-dir)     shared_dir="${2:?missing directory}"; shift 2 ;;
    --userdata-size)  userdata_size="${2:?missing size}"; shift 2 ;;
    --instance)       instance="${2:?missing instance number}"; shift 2 ;;
    -a|--accel)       accel="${2:?missing accelerator}"; shift 2 ;;
    -q|--qemu)        qemu_bin="${2:?missing qemu path}"; shift 2 ;;
    -h|--help)        usage ;;
    -*)               error "Unknown option: $1" ;;
    *)                image_dir="$1"; shift ;;
  esac
done

# ---- Instance defaults -----------------------------------------
if [[ -n "$instance" ]]; then
  if ! [[ "$instance" =~ ^[0-9]+$ ]] || (( instance < 1 || instance > 99 )); then
    error "Instance must be a number from 1 to 99, got '$instance'."
  fi
  [[ "$connect_set" == true ]] || connect_key="127.0.0.1:$((55555 + instance))"
  [[ "$vnc_set" == true ]] || vnc_display="$instance"
  [[ "$serial_set" == true ]] || serial_port="$((DEFAULT_SERIAL_PORT + instance))"
fi

# ---- Resolve image directory -----------------------------------
if [[ -z "$image_dir" ]]; then
  image_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi

if [[ ! -d "$image_dir" ]]; then
  error "Image directory does not exist: $image_dir"
fi

# ---- Guest architecture ----------------------------------------
# Taken from the kernel the images come with.
if [[ -f "$image_dir/Image" ]]; then
  guest_arch=arm64
  kernel_file=Image
  default_qemu=qemu-system-aarch64
  qemu_pkg_deb=qemu-system-arm
  qemu_pkg_rpm=qemu-system-aarch64-core
  console=ttyAMA0
elif [[ -f "$image_dir/bzImage" ]]; then
  guest_arch=x86_64
  kernel_file=bzImage
  default_qemu=qemu-system-x86_64
  qemu_pkg_deb=qemu-system-x86
  qemu_pkg_rpm=qemu-system-x86-core
  console=ttyS0
else
  error "No kernel (bzImage or Image) in '$image_dir'."
fi

case "$accel" in
  auto|kvm|hvf|whpx|tcg) ;;
  *) error "Unknown accelerator: $accel (use auto, kvm, hvf, whpx or tcg)" ;;
esac

# ---- Validate connect key --------------------------------------
if [[ "$connect_key" != *:* ]]; then
  error "Connect key must be in host:port form, got '$connect_key'."
fi
forward_host="${connect_key%:*}"
forward_port="${connect_key##*:}"
if [[ -z "$forward_host" || -z "$forward_port" ]]; then
  error "Connect key must be in host:port form, got '$connect_key'."
fi
if ! [[ "$forward_port" =~ ^[0-9]+$ ]]; then
  error "Port must be numeric, got '$forward_port'."
fi

# ---- Parse resolution ------------------------------------------
if ! [[ "$resolution" =~ ^[0-9]+x[0-9]+$ ]]; then
  error "Resolution must be in WxH form (e.g. 360x720), got '$resolution'."
fi
res_w="${resolution%x*}"
res_h="${resolution#*x}"

# ---- Locate QEMU binary ---------------------------------------
if [[ -z "$qemu_bin" ]]; then
  qemu_bin="$default_qemu"
fi

if ! command -v "$qemu_bin" >/dev/null 2>&1; then
  cat >&2 <<EOF
[ERROR] '$qemu_bin' was not found on PATH.

Install QEMU for your platform:
  Linux (Debian/Ubuntu) :  sudo apt install $qemu_pkg_deb
  Linux (Fedora/RHEL)   :  sudo dnf install $qemu_pkg_rpm
  macOS (Homebrew)       :  brew install qemu
  Windows (Chocolatey)   :  choco install qemu
  Windows (manual)       :  https://www.qemu.org/download/#windows

After installation, ensure '$default_qemu' is on your PATH and retry.
EOF
  exit 1
fi

qemu_version=$("$qemu_bin" --version | head -1)
info "Using $qemu_version"
qemu_major=$(sed -n 's/^QEMU emulator version \([0-9][0-9]*\)\..*/\1/p' <<<"$qemu_version")
qemu_minor=$(sed -n 's/^QEMU emulator version [0-9][0-9]*\.\([0-9][0-9]*\).*/\1/p' <<<"$qemu_version")

# Detect the correct ES1370 audio device name (case varies by QEMU version).
# A QEMU built without the card gives an emulator without sound.
qemu_devices=$("$qemu_bin" -device help 2>&1 || true)
if grep -q '"es1370"' <<<"$qemu_devices"; then
  audio_device="es1370"
elif grep -q '"ES1370"' <<<"$qemu_devices"; then
  audio_device="ES1370"
else
  audio_device=""
fi

# ---- Check required image files --------------------------------
required_files=(
  "$kernel_file"
  ramdisk.img
  updater.img
  system.img
  vendor.img
  userdata.img
)

missing=()
for f in "${required_files[@]}"; do
  if [[ ! -f "$image_dir/$f" ]]; then
    missing+=("$f")
  fi
done

if [[ ${#missing[@]} -gt 0 ]]; then
  error "Missing image files in '$image_dir': ${missing[*]}"
fi

# ---- Writable images: per instance -----------------------------
# userdata.img (/data) and updater.img (the misc partition) are written by
# the guest; system.img and vendor.img are mounted read-only. An instance
# gets its own copies of the first two and opens the others read-only and
# without image locking, so any number of instances share them.
file_size() { stat -c %s "$1" 2>/dev/null || stat -f %z "$1"; }
copy_image() {
  # Sparse copy on GNU cp, a clone on APFS, a plain copy elsewhere.
  cp --sparse=always "$1" "$2" 2>/dev/null && return
  rm -f "$2"
  cp -c "$1" "$2" 2>/dev/null && return
  rm -f "$2"
  cp "$1" "$2"
}

disk_dir="."
shared_ro=""
if [[ -n "$instance" ]]; then
  disk_dir="instances/$instance"
  mkdir -p "$image_dir/$disk_dir"
  for f in userdata.img updater.img; do
    if [[ ! -f "$image_dir/$disk_dir/$f" ]]; then
      info "Instance $instance: creating $disk_dir/$f"
      copy_image "$image_dir/$f" "$image_dir/$disk_dir/$f" ||
        error "Cannot copy $f for instance $instance."
    fi
  done
  shared_ro=",readonly=on,file.locking=off"
fi

# ---- /data size ------------------------------------------------
# The guest runs resize2fs on /data at every boot, so growing the image
# file is all it takes. dd with count=0 extends it sparsely on any host.
if [[ -n "$userdata_size" ]]; then
  if [[ "$userdata_size" =~ ^([0-9]+)([MmGg]?)$ ]]; then
    size_mib="${BASH_REMATCH[1]}"
    [[ "${BASH_REMATCH[2]}" =~ [Gg] ]] && size_mib=$((size_mib * 1024))
  else
    error "Userdata size must be a number of MiB or end in M or G (e.g. 8G), got '$userdata_size'."
  fi
  userdata_img="$image_dir/$disk_dir/userdata.img"
  want_bytes=$((size_mib * 1024 * 1024))
  have_bytes=$(file_size "$userdata_img")
  if (( want_bytes > have_bytes )); then
    info "Growing /data to ${size_mib} MiB"
    dd if=/dev/null of="$userdata_img" bs=1 count=0 seek="$want_bytes" 2>/dev/null ||
      error "Cannot grow $userdata_img."
  elif (( want_bytes < have_bytes )); then
    warn "/data is already $((have_bytes / 1024 / 1024)) MiB; it is not shrunk."
  fi
fi

# ---- Shared directory (virtio-9p) ------------------------------
share_args=()
share_cmdline=""
if [[ -n "$shared_dir" ]]; then
  [[ -d "$shared_dir" ]] || error "Shared directory does not exist: $shared_dir"
  if ! grep -q '"virtio-9p-pci"' <<<"$qemu_devices"; then
    error "This QEMU has no 9p file sharing (virtio-9p-pci), so --shared-dir is not available."
  fi
  shared_dir="$(cd "$shared_dir" && pwd)"
  # QEMU option values escape a comma by doubling it.
  share_args=(
    -fsdev "local,id=hostshare,path=${shared_dir//,/,,},security_model=mapped-xattr"
    -device "virtio-9p-pci,fsdev=hostshare,mount_tag=hostshare"
  )
  share_cmdline=" ohos.boot.share=hostshare"
fi

# ---- Detect host OS and configure acceleration -----------------
host_os="$(uname -s)"
host_arch="$(uname -m)"

accel_args=()
cpu_args=()
machine_args=()
# Multiplier for the guest's watchdogs and start-up timeouts (ability
# framework, hicollie thread watchdogs, app freeze detection). Pure TCG
# emulation is several times slower than a hypervisor, and at 1x the watchdogs
# kill render_service, SceneBoard and apps that are merely slow.
timeout_ratio=1

case "$host_os" in
  Linux)                 info "Host: Linux ($host_arch)" ;;
  Darwin)                info "Host: macOS ($host_arch)" ;;
  MINGW*|MSYS*|CYGWIN*)  info "Host: Windows ($host_os / $host_arch)" ;;
  *) error "Unsupported host OS: $host_os. Supported: Linux, macOS, Windows (Git Bash/MSYS2/Cygwin)." ;;
esac

kvm_usable() { [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]]; }

kvm_help() {
  if [[ ! -e /dev/kvm ]]; then
    cat >&2 <<EOF
[ERROR] /dev/kvm is not available.

To enable KVM:
  1. Ensure your CPU supports hardware virtualization (Intel VT-x / AMD-V).
  2. Enable virtualization in your BIOS/UEFI settings.
  3. Load the KVM module:  sudo modprobe kvm-intel  (or kvm-amd)
  4. Verify:  ls -l /dev/kvm
EOF
  else
    cat >&2 <<EOF
[ERROR] Current user cannot access /dev/kvm.

Fix with one of:
  sudo usermod -aG kvm \$USER   (then log out and back in)
  sudo chmod 666 /dev/kvm       (temporary)
EOF
  fi
  exit 1
}

# Does the host run the guest's instruction set?
native_host=false
case "$guest_arch:$host_arch" in
  x86_64:x86_64|x86_64:amd64|x86_64:AMD64) native_host=true ;;
  arm64:arm64|arm64:aarch64|arm64:ARM64)   native_host=true ;;
esac

if [[ "$accel" == auto ]]; then
  if [[ "$native_host" == true ]]; then
    case "$host_os" in
      Linux)
        if kvm_usable; then
          accel=kvm
        elif [[ "$guest_arch" == x86_64 ]]; then
          kvm_help
        else
          warn "KVM is not usable here; using TCG emulation (slow)."
          accel=tcg
        fi
        ;;
      Darwin)
        if sysctl -n kern.hv_support 2>/dev/null | grep -q 1; then
          accel=hvf
        elif [[ "$guest_arch" == arm64 ]]; then
          # No Hypervisor.framework, e.g. inside a macOS VM (GitHub's arm64
          # macOS runners have no nested virtualization).
          warn "Hypervisor.framework is not available; using TCG emulation (slow)."
          accel=tcg
        else
          warn "Hypervisor.framework may not be available. Trying HVF anyway..."
          accel=hvf
        fi
        ;;
      *)
        if [[ "$guest_arch" == x86_64 ]]; then
          accel=whpx
        else
          accel=tcg
        fi
        ;;
    esac
  else
    info "The $guest_arch guest is emulated on this $host_arch host (TCG, slower than native)."
    accel=tcg
  fi
fi

case "$accel" in
  kvm)
    [[ "$host_os" == Linux ]] || error "KVM is only available on Linux."
    kvm_usable || kvm_help
    accel_args=(-enable-kvm)
    cpu_args=(-cpu host)
    ;;
  hvf)
    accel_args=(-accel hvf)
    cpu_args=(-cpu host)
    ;;
  whpx)
    # We can't easily test WHPX from bash, so trust QEMU to report errors.
    info "Using WHPX acceleration. If QEMU fails, enable Windows Hypervisor Platform:"
    info "  Settings > Apps > Optional Features > More Windows Features > Windows Hypervisor Platform"
    accel_args=(-accel whpx)
    if [[ "$guest_arch" == x86_64 ]]; then
      # Without -cpu QEMU exposes the baseline qemu64 model, which lacks AVX2 and
      # BMI. The guest's openHiTLS crypto assembly uses them with no fallback:
      # foundation dies with SIGILL and init reboots the guest. WHPX rejects
      # -cpu host, so ask for the x86-64-v3 feature set instead.
      cpu_args=(-cpu qemu64,+ssse3,+sse4.1,+sse4.2,+popcnt,+cx16,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm)
    fi
    ;;
  tcg)
    accel_args=(-accel tcg,thread=multi)
    if [[ "$guest_arch" == arm64 ]]; then
      # The architected pointer-authentication algorithm is slow to emulate;
      # the implementation-defined one is much cheaper.
      cpu_args=(-cpu max,pauth-impdef=on)
    else
      cpu_args=(-cpu max)
    fi
    # 20: a UI test (aa test) keeps its test ability's main thread busy while
    # the uitest daemon starts, and at 10 the ability manager's foreground
    # timeout (5 s x ratio) killed it on a 4-vCPU TCG host.
    timeout_ratio=20
    ;;
esac

# App freeze detection takes its multiplier in per mille, at most 4 digits.
appfreeze_ratio=$(( timeout_ratio * 1000 ))
(( appfreeze_ratio > 9999 )) && appfreeze_ratio=9999

if [[ "$guest_arch" == arm64 ]]; then
  # KVM uses the host's GIC model; TCG and HVF use QEMU's own GICv3.
  if [[ "$accel" == kvm ]]; then
    machine_args=(-machine virt,gic-version=host)
  else
    machine_args=(-machine virt,gic-version=3)
  fi
else
  machine_args=(-machine q35)
fi

# ---- Auto-detect headless if no display is available -----------
if [[ "$headless" == false ]]; then
  case "$host_os" in
    Linux)
      if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
        info "No display server detected (DISPLAY/WAYLAND_DISPLAY unset). Switching to headless mode."
        headless=true
      fi
      ;;
    Darwin)
      # macOS always has a display server (WindowServer), even over SSH
      # with screen sharing. No auto-detection needed.
      ;;
    MINGW*|MSYS*|CYGWIN*)
      # Windows always has a display. No auto-detection needed.
      ;;
  esac
fi

# ---- Host audio ------------------------------------------------
# The guest plays to and records from the ES1370 card, and -audiodev connects
# it to the host. QEMU 8 and later exit when a requested backend cannot start,
# so "auto" only picks a sound server that is actually there. With "none" the
# card still works but its output is dropped.
if [[ "$audio" == auto ]]; then
  # QEMU 7 and later list their drivers with "-audiodev help"; older ones fail.
  audio_drivers=$("$qemu_bin" -audiodev help 2>/dev/null | sed -n '2,$p' || true)
  has_audio_driver() { grep -qx "$1" <<<"$audio_drivers"; }
  runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  case "$host_os" in
    Darwin)                audio=coreaudio ;;
    MINGW*|MSYS*|CYGWIN*)  audio=dsound ;;
    *)
      # PipeWire desktops run pipewire-pulse too, so pa covers both servers.
      if [[ -n "${PULSE_SERVER:-}" || -S "$runtime_dir/pulse/native" ]] &&
         { [[ -z "$audio_drivers" ]] || has_audio_driver pa; }; then
        audio=pa
      elif [[ -S "$runtime_dir/pipewire-0" ]] && has_audio_driver pipewire; then
        audio=pipewire
      elif [[ -e /dev/snd/controlC0 ]] &&
           { [[ -z "$audio_drivers" ]] || has_audio_driver alsa; }; then
        audio=alsa
      else
        audio=none
      fi
      ;;
  esac
fi

if [[ -z "$audio_device" ]]; then
  warn "This QEMU has no ES1370 sound card: the emulator has no sound."
  audio=none
  audio_args=()
elif [[ "${qemu_major:-0}" -gt 4 ]] ||
   [[ "${qemu_major:-0}" -eq 4 && "${qemu_minor:-0}" -ge 2 ]]; then
  audio_args=(-audiodev "${audio},id=snd0" -device "$audio_device,audiodev=snd0")
else
  # Sound cards take an audiodev property since QEMU 4.2; before that the
  # backend comes from the environment.
  export QEMU_AUDIO_DRV="${audio%%,*}"
  audio_args=(-device "$audio_device")
fi

# ---- Build display and serial arguments -------------------------
display_args=()
serial_args=()
if [[ "$headless" == true ]]; then
  vnc_tcp_port=$((5900 + vnc_display))
  display_args=(
    -nographic
    -vga none
    -device "virtio-gpu-pci,xres=${res_w},yres=${res_h},max_outputs=1,addr=08.0"
    -display "vnc=0.0.0.0:${vnc_display}"
  )
  serial_args=(-serial "telnet:0.0.0.0:${serial_port},server,nowait")
else
  # Prefer a window with a menu: Machine > Power Down sends an ACPI power button
  # press (x86_64) or presses the virt machine's GPIO power key (arm64), which
  # the guest takes as the phone's power key. QEMU 6 and later list
  # their backends with "-display help"; older ones only in -help.
  if [[ -z "$ui" ]]; then
    backends=$("$qemu_bin" -display help 2>/dev/null | sed -n '2,$p' || true)
    if [[ -z "$backends" ]]; then
      backends=$("$qemu_bin" -help 2>/dev/null | sed -n 's/^-display \([a-z-]*\).*/\1/p' || true)
    fi
    if [[ "$host_os" == Darwin ]]; then
      ui=cocoa
    elif grep -qx gtk <<<"$backends" || ! grep -qx sdl <<<"$backends"; then
      ui=gtk
    else
      ui=sdl
    fi
  fi
  # GTK zooms to fit by default with virtio-gpu and then sends the window size
  # to the guest as its display mode, replacing xres/yres: the window opens at
  # 640x480 and the guest never gets a picture on screen. Size the window to
  # the guest instead.
  case "$ui" in
    gtk)       ui_opts="$ui,gl=off,zoom-to-fit=off" ;;
    sdl)       ui_opts="$ui,gl=off" ;;
    cocoa)     ui_opts="$ui" ;;
    *)         error "Unknown display backend: $ui (use gtk, cocoa or sdl)" ;;
  esac
  # With an absolute pointer (virtio-tablet) the window hides the host pointer
  # and expects the guest to draw one, but the guest turns clicks into touches
  # and draws none: keep showing it. The display suboption exists since QEMU
  # 5.0; older versions take -show-cursor.
  if [[ "${qemu_major:-0}" -ge 5 ]]; then
    ui_args=(-display "$ui_opts,show-cursor=on")
  else
    ui_args=(-display "$ui_opts" -show-cursor)
  fi
  display_args=(
    -nographic
    -vga none
    -device "virtio-gpu-pci,xres=${res_w},yres=${res_h},max_outputs=1,addr=08.0"
    "${ui_args[@]}"
  )
fi

# The window UIs report their initial 640x480 placeholder size to virtio-gpu
# before the guest starts, so its preferred mode becomes 640x480 and xres/yres
# are ignored. A video= mode on the kernel command line takes precedence.
video_mode="video=Virtual-1:${res_w}x${res_h}"

# The camera is the kernel's vivid test driver; keep it to one capture node
# with one webcam input (the default also adds output, VBI, radio and SDR).
camera_cmdline="vivid.node_types=0x1 vivid.num_inputs=1"

# ---- Network ---------------------------------------------------
# One user-mode network carries hdc (port forward to the guest's 55555). With
# --wifi a second one, on a NIC the guest turns into virt_wifi's wlan0, carries
# everything else; the first is then restricted to the host and kept on its own
# subnet, and the guest gives eth0 a fixed LAN address (emulator_wifi).
# romfile= : no PXE option ROM. The guest never boots from the network, and
# without it QEMU needs no efi-virtio.rom (ipxe-qemu is only a recommended
# package of qemu-system-arm, so minimal installs lack it).
mac_suffix=$(printf %02x "${instance:-0}")
if [[ "$wifi" == true ]]; then
  net_args=(
    -netdev "user,id=net0,net=10.0.3.0/24,restrict=on,hostfwd=tcp:${forward_host}:${forward_port}-:55555"
    -device "virtio-net-pci,netdev=net0,mac=52:54:00:12:35:${mac_suffix},addr=0a.0,romfile="
    -netdev "user,id=net1"
    -device "virtio-net-pci,netdev=net1,mac=52:54:00:12:36:${mac_suffix},addr=0b.0,romfile="
  )
  ip_cmdline="ip=10.0.3.15::0.0.0.0:255.255.255.0::eth0:off"
else
  net_args=(
    -netdev "user,id=net0,hostfwd=tcp:${forward_host}:${forward_port}-:55555"
    -device "virtio-net-pci,netdev=net0${instance:+,mac=52:54:00:12:35:${mac_suffix}},romfile="
  )
  ip_cmdline="ip=dhcp"
fi

# ---- Summary ---------------------------------------------------
info "Image directory : $image_dir"
info "Connect key     : $connect_key"
info "Guest           : $guest_arch"
info "CPUs: $smp | RAM: $mem | Resolution: ${res_w}x${res_h}"
info "Acceleration    : ${accel_args[*]:-none} ${cpu_args[*]:-}"
info "Audio           : ${audio}"
[[ -n "$instance" ]] && info "Instance        : $instance ($image_dir/$disk_dir)"
[[ "$wifi" == true ]] && info "Network         : WiFi (VirtWifi); ethernet for hdc only"
[[ -n "$shared_dir" ]] && info "Shared dir      : $shared_dir -> /mnt/host"
if [[ "$headless" == true ]]; then
  info "Mode            : headless"
  info "  VNC display   : localhost:$vnc_tcp_port (VNC :$vnc_display)"
  info "  Serial console: localhost:$serial_port (telnet, one client at a time)"
  echo ""
  info "To connect:"
  info "  VNC viewer -> localhost:$vnc_display"
  info "  Serial     -> nc localhost $serial_port"
  info "  HDC        -> hdc tconn $connect_key"
else
  info "Mode            : graphical ($ui)"
  if [[ "$ui" == sdl ]]; then
    info "Power button    : type Ctrl-A c here, then system_powerdown"
  else
    info "Power button    : Machine > Power Down in the window menu"
  fi
fi
info "Starting QEMU..."

# ---- Launch QEMU -----------------------------------------------
# virtio-tablet-pci is an absolute pointer: QEMU does not grab the host mouse,
# and the guest's emulator_touch daemon turns clicks and drags in the window
# into touches (left button = finger, right button = Back).
cd "$image_dir"

exec "$qemu_bin" \
  -name "Oniro Emulator${instance:+ $instance}" \
  "${machine_args[@]}" \
  -smp "$smp" \
  -m "$mem" \
  -boot c \
  "${display_args[@]}" \
  -rtc base=utc,clock=host \
  ${audio_args[@]+"${audio_args[@]}"} \
  -initrd ramdisk.img \
  -kernel "$kernel_file" \
  -drive "if=none,file=${disk_dir}/updater.img,format=raw,id=updater,index=0" \
  -device virtio-blk-pci,drive=updater \
  -drive "if=none,file=system.img,format=raw,id=system,index=1${shared_ro}" \
  -device virtio-blk-pci,drive=system \
  -drive "if=none,file=vendor.img,format=raw,id=vendor,index=2${shared_ro}" \
  -device virtio-blk-pci,drive=vendor \
  -drive "if=none,file=${disk_dir}/userdata.img,format=raw,id=userdata,index=3" \
  -device virtio-blk-pci,drive=userdata \
  ${share_args[@]+"${share_args[@]}"} \
  "${serial_args[@]}" \
  -append "${ip_cmdline} loglevel=4 ${video_mode} console=${console},115200 panic=5 init=init root=/dev/ram0 rw ohos.boot.hardware=emulator ohos.boot.timeout_ratio=${timeout_ratio} ohos.boot.appfreeze_ratio=${appfreeze_ratio}${share_cmdline} ${camera_cmdline} ohos.required_mount.system=/dev/block/vdb@/usr@ext4@ro,barrier=1@wait,required ohos.required_mount.vendor=/dev/block/vdc@/vendor@ext4@ro,barrier=1@wait,required ohos.required_mount.misc=/dev/block/vda@/misc@none@none=@wait,required" \
  "${accel_args[@]}" \
  ${cpu_args[@]+"${cpu_args[@]}"} \
  "${net_args[@]}" \
  -device virtio-tablet-pci
