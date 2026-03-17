# Phase 1: Infrastructure & Target Setup

### 1.1 Define `hybris_generic` build targets
- [x] **Action:** Create `device/board/oniro/hybris_generic`, `device/soc/oniro/hybris_generic`, and `vendor/oniro/hybris_generic`.
- **Deliverable:** The build system recognizes `--product-name hybris_generic`.
- **Status & Notes:** Completed. Directories initialized and basic configuration files are in place.

### 1.2 Implement Container Mode Detection
- [x] **Action:** Enhance `InContainerMode()` in `base/startup/init/services/utils/init_utils.c` to detect `container=lxc`.
- **Deliverable:** `init` can branch logic based on the environment.
- **Status & Notes:** Completed. Detection works via environment variable check.

### 1.3 Patch `init` for Restricted Ops (Container Mode)
- [x] **Action:** Disable `MountBasicFs`, `CreateDeviceNode`, skip `chmod/chown`, and bypass first-stage init (`SystemPrepare`).
- **Deliverable:** `init` proceeds without failing on read-only or restricted filesystem operations.
- **Status & Notes:** Completed.
    *   **Boot Flow:** `main.c` patched to skip `SystemPrepare` in container mode.
    *   **Resources:** Disabled redundant operations including basic FS mounting, kernel module loading (`insmod`), and system time adjustment (`settimeofday`).
    *   **Stability:** Fixed a typo in `init_common_service.c` and improved `LogInit` for existing `/dev/kmsg`.

### 1.4 Disable SELinux and AccessControl
- [x] **Action:** Skip policy loading and `setexeccon` in `selinux_adp.c` and `SetAccessToken` in `init_service.c`.
- **Deliverable:** Services start without SELinux context errors.
- **Status & Notes:** Completed. SELinux is managed by the host (Ubuntu Touch).
