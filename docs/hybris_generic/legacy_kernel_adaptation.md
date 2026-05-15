# Phase 2: Kernel Adaptation (Volla X23)

> **Legacy (LXC-era) document.** Describes the original OHOS-as-LXC-container
> path, which is **no longer maintained** — the project now boots OHOS
> natively (no Ubuntu Touch host, no LXC). Kept as a reference for the HAL /
> driver bring-up detail (libhybris, graphics, audio, WiFi, …) that still
> applies under native boot. For current status start at [README.md](README.md).

### 2.1 Initialize Kernel Workspace
- [x] **Action:** Clone the Volla X23 kernel repository into the kernel tree and prepare the build environment.
- **Commands:**
  ```bash
  git clone https://gitlab.com/ubports/porting/reference-device-ports/halium12/volla-x23/volla-vidofnir.git kernel/linux/volla-vidofnir
  cd kernel/linux/volla-vidofnir
  ./build.sh -b build-dir -c
  ```
- **Deliverable:** Kernel source tree ready in `kernel/linux/volla-vidofnir/build-dir/downloads/kernel-volla-mt6789`.
- **Status & Notes:** Completed.

### 2.2 Apply OpenHarmony Config Fragments
- [x] **Action:** Create `arch/arm64/configs/openharmony.config` with mandatory OHOS options.
- **Deliverable:** Kernel `.config` adapted with mandatory OHOS options (Binder, Ashmem, HDF, HMDFS, etc.).
- **Status & Notes:** Completed.
    - Created `openharmony.config` with settings for Hilog, Hievent, Blackbox, Binder, HDF, AccessTokenID, HMDFS, Unified Collection, Hyperhold, and HCK.
    - Updated `deviceinfo` to include `openharmony.config` in `deviceinfo_kernel_defconfig`.
    - Appended `hardware=x23 ohos.boot.sn=0a20230726rpi` to `deviceinfo_kernel_cmdline`.

### 2.3 Apply Release Adaptation Patches (Manual Porting)
- [x] **Action:** Port OpenHarmony 6.1 kernel adaptations to the Volla kernel.
- **Deliverable:** Kernel source updated with required OHOS system-level adaptations.
- **Status & Notes:** Completed (Manual Porting from `kernel/linux/linux-5.10`).
    - **Drivers:** Copied `accesstokenid` and staging drivers (`hilog`, `hievent`, `hisysevent`, `zerohung`, `hungtask`, `blackbox`) from OHOS 6.1 tree.
    - **Token Support:** Patched `sched.h` and `fork.c` to add `token` and `ftoken` fields to `task_struct`.
    - **Binder:** Enhanced binder driver with AccessTokenID support, transaction tracking (`async_from_pid`, `async_from_tid`, `timestamp`), and new ioctls (`BINDER_FEATURE_SET`, `BINDER_GET_ACCESS_TOKEN`).
    - **Build:** Updated top-level `Makefile` KBUILD_CFLAGS and added missing UIDs (`NWEBSPAWN_UID`, `GLOBAL_MEMMGR_UID`) to `uidgid.h`.

### 2.4 Apply HDF (Hardware Driver Foundation) Patches
- [x] **Action:** Apply the HDF framework patches to the kernel source and configure platform drivers.
- **Deliverable:** Kernel source includes HDF support and necessary platform drivers.
- **Status & Notes:** Completed.
    - Applied HDF patches via `patch_hdf.sh` (with manual fix for `hid.h`).
    - **Symlinks:** Created absolute relative symlinks for `khdf`, `framework`, `inner_api`, and `include/hdf` to correctly point to the OHOS project root.
    - **Config:** Copied `hdf_config` from `vendor/oniro/x23` reference.
    - **Fixes:** Patched `hdf_usb_pnp_manage.h` to define `true`/`false` macros for preprocessor compatibility.

### 2.5 Build and Package Kernel/Modules
- [x] **Action:** Execute the kernel build script and package the resulting modules.
- **Command:**
  ```bash
  # Inside kernel/linux/volla-vidofnir
  PRODUCT_PATH=vendor/oniro/hybris_generic ./build.sh -b build-dir -k
  ```
- **Deliverable:** `boot.img`, `dtbo.img`, `vendor_boot.img` and `modules.tar.gz`.
- **Status & Notes:** Completed.
    - **Environment:** Added binutils (`as`, `ld`, `ar`, etc.) and `c++` to `ALLOWED_HOST_TOOLS` in `build.sh`.
    - **Tooling:** Manually built `hc-gen` to resolve C++ build issues in restricted path.
    - **Compatibility:** Patched `make-dtboimage.sh`, `make-bootimage.sh`, and `mkdtboimg.py` for Python 3 compatibility (replaced `python2` with `python`, `xrange` with `range`, and used `bytearray`).
    - **Success:** Kernel successfully compiled and artifacts collected in `kernel/linux/volla-vidofnir/out/`.

### 2.6 Kernel Build Reproduction
- [x] **Action:** Generate patches and automation scripts for reproduction.
- **Deliverable:** Patches and scripts in `device/board/oniro/hybris_generic/kernel/x23/`.
- **Status & Notes:** Completed.
    - Generated patches for `volla-vidofnir`, `build`, `kernel-volla-mt6789`, and `libufdt`.
    - Created `patch.sh` to automate the entire patching process, including symlinks and external component setup.
    - Created `build.sh` to automate patching and building in one command.
    - Usage: `./device/board/oniro/hybris_generic/kernel/x23/build.sh` from the project root.

### 2.7 Kernel Verification & Deployment
- [x] **Action:** Deploy the built kernel images and modules to the Volla X23 and verify functionality.
- **Commands:**
  ```bash
  # Use the automated deployment script:
  ./device/board/oniro/hybris_generic/utils/deploy-kernel.sh
  ```
- **Deliverable:** Verified kernel running on Volla X23.
- **Verification Steps:**
    - **Boot:** Device successfully boots into host OS (Ubuntu Touch) with new kernel.
    - **Drivers:** Verified presence of mandatory device nodes:
      - `/dev/access_token_id`: Present (crw------- 1 root root 10, 126)
      - `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder`: Present (symlinks to binderfs)
    - **Version:** `uname -a` confirms custom build (e.g., `5.10.209-ga4ec076d798b`).
- **Status & Notes:** Completed. Automated via `deploy-kernel.sh`. `/proc/transaction_proc` was not found in the initial boot but basic driver functionality is confirmed.
