#!/usr/bin/env bash
# ---------------------------------------------------------------
#  Oniro / OpenHarmony x86_general QEMU Emulator Launcher
#
#  Supported host platforms:
#    - Linux  (KVM)
#    - macOS  x86_64 (HVF) / Apple Silicon (TCG)
#    - Windows via Git Bash, MSYS2, or Cygwin (WHPX)
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

Launch the Oniro/OpenHarmony x86_general emulator in QEMU.

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
  -q, --qemu PATH        Path to qemu-system-x86_64 binary
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
qemu_bin="${QEMU_BIN:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -c|--connect)     connect_key="${2:?missing connect key}"; shift 2 ;;
    -s|--smp)         smp="${2:?missing smp value}"; shift 2 ;;
    -m|--memory)      mem="${2:?missing memory value}"; shift 2 ;;
    -r|--resolution)  resolution="${2:?missing resolution}"; shift 2 ;;
    --headless)       headless=true; shift ;;
    --vnc-display)    vnc_display="${2:?missing VNC display number}"; shift 2 ;;
    --serial-port)    serial_port="${2:?missing serial port}"; shift 2 ;;
    -q|--qemu)        qemu_bin="${2:?missing qemu path}"; shift 2 ;;
    -h|--help)        usage ;;
    -*)               error "Unknown option: $1" ;;
    *)                image_dir="$1"; shift ;;
  esac
done

# ---- Resolve image directory -----------------------------------
if [[ -z "$image_dir" ]]; then
  image_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fi

if [[ ! -d "$image_dir" ]]; then
  error "Image directory does not exist: $image_dir"
fi

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
  qemu_bin="qemu-system-x86_64"
fi

if ! command -v "$qemu_bin" >/dev/null 2>&1; then
  cat >&2 <<EOF
[ERROR] '$qemu_bin' was not found on PATH.

Install QEMU for your platform:
  Linux (Debian/Ubuntu) :  sudo apt install qemu-system-x86
  Linux (Fedora/RHEL)   :  sudo dnf install qemu-system-x86-core
  macOS (Homebrew)       :  brew install qemu
  Windows (Chocolatey)   :  choco install qemu
  Windows (manual)       :  https://www.qemu.org/download/#windows

After installation, ensure 'qemu-system-x86_64' is on your PATH and retry.
EOF
  exit 1
fi

qemu_version=$("$qemu_bin" --version | head -1)
info "Using $qemu_version"

# Detect the correct ES1370 audio device name (case varies by QEMU version)
if "$qemu_bin" -device help 2>&1 | grep -q '"es1370"'; then
  audio_device="es1370"
else
  audio_device="ES1370"
fi

# ---- Check required image files --------------------------------
required_files=(
  bzImage
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

# ---- Detect host OS and configure acceleration -----------------
host_os="$(uname -s)"
host_arch="$(uname -m)"

accel_args=()
cpu_args=()

case "$host_os" in
  Linux)
    info "Host: Linux ($host_arch)"
    if [[ ! -e /dev/kvm ]]; then
      cat >&2 <<EOF
[ERROR] /dev/kvm is not available.

To enable KVM:
  1. Ensure your CPU supports hardware virtualization (Intel VT-x / AMD-V).
  2. Enable virtualization in your BIOS/UEFI settings.
  3. Load the KVM module:  sudo modprobe kvm-intel  (or kvm-amd)
  4. Verify:  ls -l /dev/kvm
EOF
      exit 1
    fi
    if [[ ! -r /dev/kvm || ! -w /dev/kvm ]]; then
      cat >&2 <<EOF
[ERROR] Current user cannot access /dev/kvm.

Fix with one of:
  sudo usermod -aG kvm \$USER   (then log out and back in)
  sudo chmod 666 /dev/kvm       (temporary)
EOF
      exit 1
    fi
    accel_args=(-enable-kvm)
    cpu_args=(-cpu host)
    ;;

  Darwin)
    info "Host: macOS ($host_arch)"
    if [[ "$host_arch" == "x86_64" ]]; then
      # Native x86_64 Mac – use Hypervisor.framework
      if ! sysctl -n kern.hv_support 2>/dev/null | grep -q 1; then
        warn "Hypervisor.framework may not be available. Trying HVF anyway..."
      fi
      accel_args=(-accel hvf)
      cpu_args=(-cpu host)
    else
      # Apple Silicon – x86_64 emulation via TCG
      info "Apple Silicon detected; using TCG emulation (slower than native)."
      accel_args=(-accel tcg,thread=multi)
      cpu_args=(-cpu max)
    fi
    ;;

  MINGW*|MSYS*|CYGWIN*)
    info "Host: Windows ($host_os / $host_arch)"
    # Check for Windows Hypervisor Platform (WHPX)
    # We can't easily test WHPX from bash, so trust QEMU to report errors.
    info "Using WHPX acceleration. If QEMU fails, enable Windows Hypervisor Platform:"
    info "  Settings > Apps > Optional Features > More Windows Features > Windows Hypervisor Platform"
    accel_args=(-accel whpx)
    cpu_args=()
    ;;

  *)
    error "Unsupported host OS: $host_os. Supported: Linux, macOS, Windows (Git Bash/MSYS2/Cygwin)."
    ;;
esac

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
  display_args=(
    -nographic
    -vga none
    -device "virtio-gpu-pci,xres=${res_w},yres=${res_h},max_outputs=1,addr=08.0"
    -display sdl,gl=off
  )
fi

# ---- Summary ---------------------------------------------------
info "Image directory : $image_dir"
info "Connect key     : $connect_key"
info "CPUs: $smp | RAM: $mem | Resolution: ${res_w}x${res_h}"
info "Acceleration    : ${accel_args[*]:-none} ${cpu_args[*]:-}"
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
  info "Mode            : graphical (SDL)"
fi
info "Starting QEMU..."

# ---- Launch QEMU -----------------------------------------------
cd "$image_dir"

exec "$qemu_bin" \
  -machine q35 \
  -smp "$smp" \
  -m "$mem" \
  -boot c \
  "${display_args[@]}" \
  -rtc base=utc,clock=host \
  -device "$audio_device" \
  -initrd ramdisk.img \
  -kernel bzImage \
  -drive if=none,file=updater.img,format=raw,id=updater,index=0 \
  -device virtio-blk-pci,drive=updater \
  -drive if=none,file=system.img,format=raw,id=system,index=1 \
  -device virtio-blk-pci,drive=system \
  -drive if=none,file=vendor.img,format=raw,id=vendor,index=2 \
  -device virtio-blk-pci,drive=vendor \
  -drive if=none,file=userdata.img,format=raw,id=userdata,index=3 \
  -device virtio-blk-pci,drive=userdata \
  "${serial_args[@]}" \
  -append "ip=dhcp loglevel=4 console=ttyS0,115200 init=init root=/dev/ram0 rw ohos.boot.hardware=x86_general ohos.required_mount.system=/dev/block/vdb@/usr@ext4@ro,barrier=1@wait,required ohos.required_mount.vendor=/dev/block/vdc@/vendor@ext4@ro,barrier=1@wait,required ohos.required_mount.misc=/dev/block/vda@/misc@none@none=@wait,required" \
  "${accel_args[@]}" \
  "${cpu_args[@]}" \
  -netdev "user,id=net0,hostfwd=tcp:${forward_host}:${forward_port}-:55555" \
  -device virtio-net-pci,netdev=net0
