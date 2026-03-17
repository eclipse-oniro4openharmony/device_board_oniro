# Phase 4: Deployment & Automation

### 4.1 Deployment Scripts
- [x] **Action:** Develop utility scripts for rapid deployment of rootfs to the device.
- **Deliverable:** Scripts in `device/board/oniro/hybris_generic/utils`.
- **Status & Notes:** Completed.
    *   `deploy-lxc-container.sh`: Packages and deploys the rootfs from `out/hybris_generic/packages/phone/system` to the device.
    *   Automates merging of `root`, `system`, and `vendor` directories.
    *   Handles LXC configuration (bind mounts for binder/ashmem, `autodev = 1`, etc.).

### 4.2 LXC Configuration
- [x] **Action:** Standardize LXC container configuration for OpenHarmony.
- **Deliverable:** `lxc.conf` with necessary bind mounts and environment variables.
- **Status & Notes:** Completed.
    *   Bind mounts: `/dev/binder`, `/dev/ashmem`, `/dev/kmsg`, `/dev/pmsg0`.
    *   Env: `container=lxc`, `OHOS_RUNTIME_CONFIG=1`.
