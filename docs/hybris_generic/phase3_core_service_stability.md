# Phase 3: Core Service Stability (Debugging)

### 3.1 Refine `hilogd` for Containerization
- [x] **Action:** Patch `hilogd` to skip cgroup operations and adjust configuration.
- **Deliverable:** `hilogd` successfully starts and listens for logs.
- **Status & Notes:** Completed.
    *   Modified `hilogd.cfg` to run as root and disabled sandboxing.
    *   Updated `init.cfg` to prevent remounting the root filesystem as read-only.
    *   `hilogd` is now functional and accessible via `hilog`.

### 3.2 System-wide `mksandbox` Bypass
- [x] **Action:** Globally disable sandboxing for critical services in container mode.
- **Deliverable:** Services start without failing due to namespace/mount restrictions.
- **Status & Notes:** Completed via patches to `init` and service-specific `.cfg` files.

### 3.3 Audit Socket Creation
- [x] **Action:** Verify `CreateSocketForService` in `init_service_socket.c` for path availability in `/dev/unix/socket/`.
- **Deliverable:** Ensure sockets are correctly created and accessible to services.
- **Status & Notes:** Verified. Sockets are correctly owned by root as configured.
