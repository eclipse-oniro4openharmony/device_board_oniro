# kernel/ansuz — Volla Phone Plinius (ansuz) kernel + image builders

The ansuz kernel is the **generic GKI `android14-6.1-halium`** tree
(UBports `kernel-android-common`) built with the Halium generic
adaptation build tools driven by the volla-ansuz port repo — there is no
per-device vendor kernel tree (contrast: `kernel/x23`). OHOS drivers are
applied as one additive patch. See
`docs/hybris_generic/plinius_port_plan.md` (plan) and
`plinius_recon.md` (device facts).

## Layout

| File | Purpose |
|---|---|
| `build_kernel.sh` | clone (pinned SHAs) → reset tree → apply `ohos-adaptation.patch` + `config/openharmony.config` → Halium build (`./build.sh -b workdir -k`) → **KMI guard** → artifacts in `kernel/linux/volla-ansuz/out/` |
| `build_kernel.sh --baseline` | pristine (no-OHOS) build; records `workdir/baseline-Module.symvers`; its images are the UT-compatible set used for the P2 gates |
| `build_init_boot_chainload.sh` | splices `launcher/init-chainload.sh` (`@HYBRIS_DEVICE@`→`ansuz`, halium init kept as `/init.halium`) into init_boot; repacks vendor_boot with the OHOS cmdline (`ohos.boot.hardware=ansuz lsm=selinux`) |
| `build_super_img.sh` | lpmake super: OHOS system/vendor/sys_prod/chip_prod + halium system/vendor/**vendor_dlkm/system_dlkm** (EROFS) from `halium-blobs/ansuz/` |
| `config/openharmony.config` | stage-1 fragment: ACCESS_TOKENID, HILOG, HIEVENT, BINDER_SENDER_INFO |
| `patches/kernel-source/ohos-adaptation.patch` | all kernel changes (new drivers + binder token ioctls + wiring); regenerate with `git diff` from the kernel tree |

## KMI discipline (the one hard rule)

Stock prebuilt MTK modules (vendor_boot overlay + vendor_dlkm +
system_dlkm) must keep loading: CONFIG_MODVERSIONS CRCs are the
contract. Therefore:

* **task_struct is never touched** — access_tokenid stores tokens in an
  external RCU hash (`access_token_store.c`) keyed by task pointer, fed
  by the `sched_process_fork`/`sched_process_free` tracepoints. Binder
  (builtin) reads them via `include/linux/ohos_access_token.h`.
* Additions must not change any existing exported symbol's type
  expansion. Exception that proved benign: `struct binder_transaction`
  gained fields, shifting CRCs of `binder_alloc_copy_from_buffer` and
  `__traceiter_binder_transaction_received` — **no stock module imports
  either** (verified against all 400+ stock .ko `__versions` tables).
* The guard in `build_kernel.sh` checks our `Module.symvers` against
  `halium-blobs/ansuz/recon/stock-module-versions.txt` (the dumped
  union of every stock-module expectation) and fails the build on any
  mismatch. Re-dump that file if Volla ever ships new stock modules.
* Gate P2-G (2026-07-23, PASSED): UT boots on the OHOS-patched kernel
  with all 447 stock modules loaded and `/dev/access_token_id` present.

## 6.6→6.1 porting notes

Driver sources come from the in-tree OHOS `kernel/linux/linux-6.6` (all
OHOS drivers already merged there — much closer than the X23's 5.10
set). Deltas needed for 6.1: two-arg `class_create`, `iter_iov(i)` →
`i->iov`, NWEBSPAWN_UID defined driver-local, hilog dynamic-major
fallback (245 collides with lirc on this GKI).

Staged later (same 6.6 source, port when the OHOS userspace needs
them): hmdfs, sharefs, hyperhold/zswapd, HDF, hisysevent, blackbox,
hungtask, zerohung, HCK.
