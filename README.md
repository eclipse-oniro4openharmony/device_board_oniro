# Oniro Board Support Packages

This repository contains Board Support Packages (BSPs) for devices supported by
the Oniro Project:

| Target | Device | Notes |
|--------|--------|-------|
| `x86_general` | QEMU virtual device | The **Oniro Emulator** — an `x86_64` build run under QEMU/KVM. Best starting point for app and platform development. |
| `hybris_generic` | Volla X23 phone | Oniro booting **natively** on MediaTek hardware via libhybris. See [Volla X23](#oniro-on-the-volla-x23-hybris_generic-target). |

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

The `x86_general` build requires patches to several subsystems (build,
powermgr, selinux_adapter, mindspore, storage_service, bms, …). Apply them
before building:

```bash
bash vendor/oniro/x86_general/hook/do_patch.sh
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
bash vendor/oniro/x86_general/hook/undo_patch.sh
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

## Oniro on the Volla X23 (hybris_generic target)

Oniro runs **natively** on the Volla X23 (MediaTek MT6789, aarch64): the device
boots straight into Oniro — there is **no Ubuntu Touch host and no LXC
container**. A Halium boot image chain-loads directly into OHOS `init`, and a
companion `androidd` process runs the device's Android (Halium) HAL services in
a child namespace so the OHOS graphics/HAL stack can reach the hardware through
**libhybris**.

Full build, flash, and architecture details are in the
[Volla X23 documentation](./docs/volla_x23.md).

---

## Contributing

Contributions to improve the board support packages are welcome. Please submit
a pull request with your proposed changes.

## License

This repository is distributed under the Apache 2.0 License.
