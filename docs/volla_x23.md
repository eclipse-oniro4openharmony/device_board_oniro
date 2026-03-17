# Volla X23

This documentation provides instructions for building and deploying OpenHarmony on the Volla X23 device using the `hybris_generic` target. OpenHarmony runs as an LXC container on top of Ubuntu Touch (Halium 12), using libhybris to bridge Android HALs for graphics, input, and other hardware access.

<img src="./images/screenshot-launcher.jpg" alt="launcher on volla x23" width="300"/>
<img src="./images/screenshot-settings.jpg" alt="settings on volla x23" width="300"/>

## Prerequisites

- Volla X23 (MT6789 / Helio G99, aarch64)
- Device flashed with Ubuntu Touch via the [UBports Installer](https://devices.ubuntu-touch.io/device/vidofnir/)
- ADB connection to the device (`adb devices`)
- OpenHarmony source tree with the `hybris_generic` product target set up (see [Phase 1](hybris_generic/phase1_infrastructure_target_setup.md))

## Building the RootFS

Apply the system patches, then build the `hybris_generic` product:

```bash
chmod +x device/board/oniro/system_patch/system_patch.sh
./device/board/oniro/system_patch/system_patch.sh

./build.sh --product-name hybris_generic --ccache
```

The built rootfs will be at `out/hybris_generic/packages/phone/`.

## Deploying the LXC Container

The deploy script packages the rootfs, transfers it to the device, configures the LXC container, and sets up a systemd service for automatic startup:

```bash
chmod +x device/board/oniro/hybris_generic/utils/deploy-lxc-container.sh
./device/board/oniro/hybris_generic/utils/deploy-lxc-container.sh
```

Options:
- `-p <tarball>` — use a prebuilt rootfs tarball instead of creating one from the build output
- `-d` — disable the OHOS systemd service and reboot (useful for debugging)

The LXC container configuration is at `device/board/oniro/hybris_generic/utils/lxc/config` and gets deployed to `/var/lib/lxc/openharmony/config` on the device.

## Building and Deploying the Kernel

The kernel build and deployment are fully automated via scripts in `device/board/oniro/hybris_generic/kernel/x23/`.

### Build

The build script clones the Volla X23 kernel tree, applies all OpenHarmony patches (HDF, binder, staging drivers, config fragments), and builds using the Halium build system:

```bash
chmod +x device/board/oniro/hybris_generic/kernel/x23/build_kernel.sh
./device/board/oniro/hybris_generic/kernel/x23/build_kernel.sh
```

Build artifacts (`boot.img`, `vendor_boot.img`, `dtbo.img`, `modules.tar.gz`) are placed in `kernel/linux/volla-vidofnir/out/`.

### Deploy

The deploy script pushes the kernel images and modules to the device, flashes the correct boot slot, and reboots:

```bash
chmod +x device/board/oniro/hybris_generic/kernel/x23/deploy-kernel.sh
./device/board/oniro/hybris_generic/kernel/x23/deploy-kernel.sh
```

For details on the kernel adaptation, see [Phase 2 — Kernel Adaptation](hybris_generic/phase2_kernel_adaptation.md).

## Architecture Overview

The system runs as two LXC containers on the Ubuntu Touch host:

| Container | Purpose | Rootfs |
|-----------|---------|--------|
| `android` | Android HAL services (hwservicemanager, allocator, etc.) | `/var/lib/lxc/android/` |
| `openharmony` | OpenHarmony userspace | `/home/phablet/openharmony/rootfs/` |

Both containers share the host IPC namespace so that OHOS can reach Android HAL services via `/dev/hwbinder`. The `libhybris` library translates calls from the OHOS graphics stack (display composer/buffer VDIs) to the Android HWC2 and gralloc HALs.

### Key components

- **Display:** OHOS RenderService uses custom VDI libraries (`libdisplay_composer_vdi_impl.z.so`, `libdisplay_buffer_vdi_impl.z.so`) that wrap Android HWC2/gralloc via libhybris
- **Input:** `/dev/input` is bind-mounted into the container; `multimodalinput` uses libinput with `CAP_DAC_OVERRIDE`
- **Binder:** Separate binderfs devices (`ohos-binder`) for OHOS to avoid collision with the Android binder context

## Detailed Development Roadmap

For the full development history, per-phase implementation details, bug fixes, and current status, see the [hybris_generic roadmap](hybris_generic/README.md).

| Phase | Title | Status |
|-------|-------|--------|
| 1 | [Infrastructure & Target Setup](hybris_generic/phase1_infrastructure_target_setup.md) | Complete |
| 2 | [Kernel Adaptation](hybris_generic/phase2_kernel_adaptation.md) | Complete |
| 3 | [Core Service Stability](hybris_generic/phase3_core_service_stability.md) | Complete |
| 4 | [Deployment & Automation](hybris_generic/phase4_deployment_automation.md) | Complete |
| 5 | [Libhybris Integration & HAL Bridge](hybris_generic/phase5_libhybris_integration.md) | Complete |
| 6 | [Graphics Stack & RenderService](hybris_generic/phase6_graphics_stack.md) | In Progress |
| 7 | [Input System Integration](hybris_generic/phase7_input_system.md) | Complete |
| 8 | [System Stability](hybris_generic/phase8_system_stability.md) | In Progress |
| 9 | [Volla Tablet (mimir) Bring-Up](hybris_generic/phase9_volla_tablet_mimir.md) | Complete |
