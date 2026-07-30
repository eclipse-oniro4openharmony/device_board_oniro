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
| `build_kernel.sh` | clone (pinned SHAs) → reset tree → apply `ohos-adaptation.patch` + `hmdfs.patch` + `config/openharmony.config` → Halium build (`./build.sh -b workdir -k`) → **KMI guard** → artifacts in `kernel/linux/volla-ansuz/out/` |
| `build_kernel.sh --baseline` | pristine (no-OHOS) build; records `workdir/baseline-Module.symvers`; its images are the UT-compatible set used for the P2 gates |
| `build_init_boot_chainload.sh` | splices `launcher/init-chainload.sh` (`@HYBRIS_DEVICE@`→`ansuz`, halium init kept as `/init.halium`) into init_boot; repacks vendor_boot with the OHOS cmdline (`ohos.boot.hardware=ansuz lsm=selinux`) |
| `build_super_img.sh` | lpmake super: OHOS system/vendor/sys_prod/chip_prod + halium system/vendor/**vendor_dlkm/system_dlkm** (EROFS) from `halium-blobs/ansuz/` |
| `config/openharmony.config` | stage-1 fragment: ACCESS_TOKENID, HILOG, HIEVENT, BINDER_SENDER_INFO, HMDFS |
| `patches/kernel-source/ohos-adaptation.patch` | all kernel changes (new drivers + binder token ioctls + wiring); regenerate with `git diff` from the kernel tree |
| `patches/kernel-source/hmdfs.patch` | `fs/hmdfs/` + two anchor hunks in `fs/Kconfig` / `fs/Makefile`; disjoint from `ohos-adaptation.patch`, which owns only `drivers/` + `include/` |

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
* **New self-contained subsystems are free.** hmdfs adds `fs/hmdfs/`
  plus two anchor hunks and touches no struct or export a stock module
  sees: 0 CRC changes, only 3 new tracepoint exports (2026-07-30).
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

**hmdfs (2026-07-30)** took the same 6.6 source but a different shape:
it is 31k lines across 57 files, so instead of editing call sites it
carries `fs/hmdfs/hmdfs_compat.h`, force-included by its Makefile
(`ccflags-y += -include $(srctree)/$(src)/hmdfs_compat.h`). Every entry
is a version-guarded token rename, which keeps the rest of `fs/hmdfs/`
byte-identical to the OHOS tree — re-syncing is a plain re-copy:

| 6.6 spelling | 6.1 spelling | since |
|---|---|---|
| `struct mnt_idmap *` | `struct user_namespace *` | 6.3 |
| `nop_mnt_idmap` | `init_user_ns` | 6.3 |
| `renamedata.{old,new}_mnt_idmap` | `.{old,new}_mnt_userns` | 6.3 |
| `crypto_completion_t` arg `void *` | `struct crypto_async_request *` | 6.4 |
| `copy_splice_read` | `generic_file_splice_read` | 6.5 |
| `kernel_tmpfile_open` | `vfs_tmpfile_open` | 6.5 |
| `inode->__i_ctime` | `inode->i_ctime` | 6.6 |

Before adding a row, grep `include/` to confirm the 6.6 name is wholly
absent from the target kernel — that is the only thing making it safe to
alias a struct tag globally.

Four spots the shim could not reach, edited in place — the first with
the same `LINUX_VERSION_CODE` guard style OHOS already uses in these
files:

* `hmdfs_dentryfile.c` — `<linux/filelock.h>` only exists from 6.6, when
  the file-locking declarations were split out of `<linux/fs.h>`.
* `comm/transport.c` — `aeadcipher_cb()`'s first parameter is spelled
  via the `hmdfs_crypto_cb_req` alias above; the body already casts to
  the concrete type, so one body serves both kernels.
* `inode_local.c` — `__lookup_nosensitive()` is callerless in the OHOS
  tree (5.10 and 6.6 alike), so `CONFIG_WERROR` needs `__maybe_unused`
  on it. Kept rather than deleted, to stay close to the reference.
* `hmdfs_trace.h` — an upstream bug, not a version delta:
  `hmdfs_do_readpages_cloud_end`'s `TP_printk` passed three arguments to
  a two-specifier format. The missing `ret:%d` was added.

`CONFIG_HMDFS_FS_ENCRYPTION` stays off: it `depends on TLS` and only
covers the cross-device socket transport, which a single-device port
never uses.

Staged later (same 6.6 source, port when the OHOS userspace needs
them): sharefs, hyperhold/zswapd, HDF, hisysevent, blackbox,
hungtask, zerohung, HCK.
