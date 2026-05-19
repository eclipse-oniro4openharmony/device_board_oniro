# Phase 12: User-File Access via `sharefs`

> **Legacy (LXC-era) document.** The sections below the "Native-boot
> resolution" describe the original OHOS-as-LXC-container path and its
> bind-mount workaround, which is **no longer used**. For current status
> start at [README.md](README.md).

## Native-boot resolution (2026-05-18) — the proper fix, DONE

The `sharefs` kernel driver is now **ported and built into the Volla X23
kernel**, so `storage_daemon`'s stock JSON-driven mount flow gives normal
apps a working `/storage/Users` view. No bind-mount workaround, no
userspace patches to the mount logic.

Four changes — all reproducible from a clean checkout:

1. **`sharefs` kernel driver.** The OHOS `kernel/linux/linux-5.10`
   reference tree already ships a 5.10-native `sharefs` (the X23 kernel
   is also 5.10, so **no VFS-API port was needed** — all 8 source files
   compiled unmodified). The 5.10 `sharefs` has **no `access_token_id`
   dependency** — just standard VFS + `configfs`. It ships as
   `kernel/x23/patches/kernel-source/sharefs.patch` (adds `fs/sharefs/` +
   wires `fs/Kconfig`/`fs/Makefile`), applied by `build_kernel.sh`;
   `kernel/x23/config/openharmony.config` sets `CONFIG_SHARE_FS=y`
   (built-in, so it is registered before `storage_daemon` mounts).

1b. **`CONFIG_SHAREFS_SUPPORT_OVERRIDE=y`** (also in `openharmony.config`).
   Without it, `sharefs_permission()` (`fs/sharefs/inode.c`) enforces
   sharefs's per-app isolation model: every level-1 dir under the mount
   (`Download`, `Documents`, `Desktop`, …) is presented as mode `0550`
   owned by uid `<userId>*200000`. A normal app such as VLC (no
   `FILE_ACCESS_COMMON_DIR`, uid `<userId>*200000+appId`) is therefore
   "other" and **cannot even traverse into a picked file's directory**.
   `file_api`'s `OpenByFileDataUri` (`open.cpp`) then sees
   `access(realPath)!=0` for the `file://docs/...` URI and misroutes the
   open to the MediaLibrary DataShare (`datashare:///media`), which
   rejects a non-media URI (`-13`) — the app reports "No such file or
   directory". With `SUPPORT_OVERRIDE`, `sharefs_permission()` returns 0
   and access defers to the lower ext4 perms (`0771` dirs, `0644` files —
   both world-traversable/readable). This is the OHOS-intended setting
   for device types without per-app file isolation and matches this
   build's single-user dev posture. Symptom this fixed: file picker shows
   the file and returns its URI fine, but the picked video/file won't
   open/play.

2. **`const.distributed_file_property.enabled=false`** in
   `foundation/filemanagement/dfs_service/services/distributed_file.para`.
   This was the non-obvious blocker. `storage_daemon::MountHmdfs()`
   (`mount_manager.cpp:556`) checks `SupportHmdfs()`, which reads that
   `const.` param. While it was `true`, `MountHmdfs()` attempted the real
   `-t hmdfs` mount, which fails `ENODEV` (no hmdfs driver) and **aborts
   the entire per-user mount sequence** at `MountFileSystem():548` — so
   `MountSharefs()` and `MountAppdata()` (which produce the docs `sharefs`
   mounts) never ran. With `false`, `MountHmdfs()` takes the
   `LocalMount()` path: plain bind mounts only (no hmdfs driver needed),
   feeding `/storage/media/<id>/local` from `/data/service/el2/<id>/hmdfs/
   account`, after which `MountSharefs` + `MountAppdata` run normally.
   The param had to change in `distributed_file.para` itself — it is a
   `const.` param and the param service (`CheckParamValue`,
   `param_manager.c:617`) rejects re-setting a `const.` once loaded, so a
   later `/sys_prod` vendor `.para` cannot override the `/system` one.

3. **Picker fix re-gated.** The 12.1 MediaLibrary `CheckUnlockScene`
   bypass in `os_account_interface.cpp::SendToStorageAccountStart` was
   gated on `getenv("container")` — an LXC-era env var absent under
   native boot. Re-gated on `getenv("OHOS_NATIVE_BOOT")` (set by the
   chainload). Without it the OS account stays `Verified:false` and
   MediaLibrary self-kills, breaking the file picker before it even
   appears.

Resulting on-device mount chain (verified):
`/data/service/el2/100/hmdfs/account/files/Docs` (real ext4) → bind →
`/storage/media/100/local/files/Docs` → `-t sharefs` →
`/mnt/user/100/currentUser/other` → bind →
`/mnt/user/100/sharefs/docs/currentUser` (the view normal apps get at
`/storage/Users/currentUser`). `/proc/filesystems` lists `sharefs`;
`/config/sharefs/<bundle>` is populated. **End-to-end verified**: VLC
(`org.oniroproject.vlc`) picks a file from `/storage/Users` and plays it.

Everything below is the retired LXC-era workaround, kept for history.

---

## Context

OpenHarmony exposes `/storage/Users` (the user-visible "Internal storage" root) to sandboxed apps via two separate mounts declared in `/system/etc/sandbox/appdata-sandbox.json`:

| Mount flag                 | Source (host)                                    | Sandbox path      | Who gets it                                  |
|----------------------------|--------------------------------------------------|-------------------|----------------------------------------------|
| `FILE_CROSS_APP`           | `/mnt/user/<userId>/nosharefs/docs`              | `/storage/Users`  | Privileged extensions only (`ExternalFileManager`) |
| `FILE_ACCESS_COMMON_DIR`   | `/mnt/user/<userId>/sharefs/docs`                | `/storage/Users`  | Normal apps (requires `ohos.permission.FILE_ACCESS_COMMON_DIR`) |

On a stock OHOS device, `storage_daemon` bridges the two by stacking the in-kernel **`sharefs`** filesystem (`kernel/linux/linux-6.6/fs/sharefs/`) on top of `nosharefs/docs` to produce `sharefs/docs`. `sharefs` behaves like a tiny stacking overlay (similar to overlayfs) but with **per-URI permission checks hooked into VFS ops** — it consults the caller's token-id and the set of URIs the UriPermMgr has granted, and filters readdir / lookup results accordingly.

The end result on stock OHOS:

- The system file picker (running as the privileged `ExternalFileManager`) enumerates the full tree via `nosharefs/docs`.
- When the picker returns `file://docs/...` URIs to an app, `UriPermMgr` records the grant.
- The app then reads those URIs through its `sharefs/docs` view, which the kernel `sharefs` driver filters down to exactly the files that were granted.

## The problem on `hybris_generic`

The Volla X23 and Volla Tablet (mimir) run a Halium/MediaTek **5.10** kernel. That kernel tree has **no `sharefs` driver**:

```
$ cat /proc/filesystems
nodev   sysfs
nodev   tmpfs
...
nodev   fuse
        fuseblk
nodev   fusectl
# no sharefs
```

Consequently `storage_daemon`'s `MountSharefs()` (`foundation/filemanagement/storage_service/services/storage_daemon/user/src/mount_manager.cpp:590`) silently fails or no-ops, and `/mnt/user/100/sharefs/docs/` stays an empty plain-ext4 stub. Every normal app that holds `FILE_ACCESS_COMMON_DIR` (including VLC, media apps, document editors, etc.) sees an empty `/storage/Users` no matter what the picker granted.

Concrete symptom: pick a file with the system file picker → VLC receives `file://docs/storage/Users/currentUser/Videos/I_am_legend.mp4` → `fs.open` / `fopen` returns `ENOENT` because `/storage/Users/currentUser/Videos/` is empty inside VLC's sandbox.

Additional symptom observed: `storage_daemon`'s `PrepareDir` pass creates `{no,}sharefs/docs{,/currentUser}` as `mode 0711 root:root` (source: `storage_user_path.json`), so even the privileged `ExternalFileManager` extension (uid 20010039) can traverse but not list the directory — the file picker also showed an empty Internal Storage until perms were relaxed.

## Current workaround (deployed)

### 1. LXC bind: make `sharefs/docs` a view of `nosharefs/docs`

`device/board/oniro/hybris_generic/utils/lxc/config` adds:

```
lxc.mount.entry = /home/phablet/openharmony/rootfs/mnt/user/100/nosharefs/docs mnt/user/100/sharefs/docs none bind,create=dir,optional 0 0
```

This runs at LXC mount phase, before `storage_daemon` starts. `storage_daemon::MountSharefs()` has `IsPathMounted(dst) → return E_OK` as its first check (`mount_manager.cpp:601`), so our pre-bind survives the daemon's own mount attempt. Normal apps now see the same files as the picker.

### 2. Relax perms on the docs dirs

Patched `foundation/filemanagement/storage_service/services/storage_daemon/storage_user_path.json`: changed `mode: "0711"` → `"0755"` on the four entries:

- `/mnt/user/<userId>/nosharefs/docs`
- `/mnt/user/<userId>/nosharefs/docs/currentUser`
- `/mnt/user/<userId>/sharefs/docs`
- `/mnt/user/<userId>/sharefs/docs/currentUser`

`storage_daemon`'s `PrepareDir` reads this JSON on every boot, so the fix persists through container restarts without needing a chmod loop in `start-ohos.sh`.

Deployed copy lives at `/home/phablet/openharmony/rootfs/system/etc/storage_daemon/storage_user_path.json`.

## Trade-off accepted

With the bind mount in place, every app holding `FILE_ACCESS_COMMON_DIR` sees **all** files under `/storage/Users`, not just the URIs that were actually granted to it. `UriPermMgr` is bypassed because the in-kernel filtering that would enforce it is absent. This matches the current security posture of the build (LXC perimeter, not per-app isolation) and is acceptable for bring-up, but it **is** a regression vs. stock OHOS.

Concretely:

- VLC can open any file under `/storage/Users/currentUser/`, not just the one the user picked.
- `currentUser/.Recent` and any future per-app sub-trees are visible to every app.
- Apps that *should* be scoped to a narrow `dec-paths` set (e.g. `ohos.permission.READ_WRITE_DOWNLOAD_DIRECTORY`) no longer are — they see the full tree.

## Proper fix: port `sharefs` to the 5.10 kernel

The correct long-term solution is to port OHOS's `sharefs` kernel driver onto the Volla X23 / mimir Halium-5.10 kernel. This is the same pattern used in Phase 2 for `hilog`, `hievent`, `accesstokenid`, `blackbox`, and the binder AccessTokenID extension.

### Source

`kernel/linux/linux-6.6/fs/sharefs/` in the OHOS tree — about 10 files:

- `Kconfig`, `Makefile`
- `sharefs.h`, `super.c` — filesystem registration, super-block
- `inode.c`, `file.c`, `dentry.c`, `lookup.c`, `main.c` — VFS ops
- `authentication.c` / `authentication.h` — **the critical piece**: consults OHOS token-id and URI-grant state to filter `readdir` / `lookup`
- `config.c` — ioctl / sysfs for pushing the URI-grant policy from `storage_daemon` / `UriPermMgr`

### Porting work

1. **VFS API drift (5.10 → 6.6)**. The VFS signatures `sharefs` hooks into have churned:
   - `struct user_namespace *` → `struct mnt_idmap *` in inode ops (6.3+).
   - `iget_locked`, `follow_link` / `get_link` signatures.
   - `d_revalidate`, `d_delete` dentry-op signature tweaks.
   - readdir context (`filldir_t`) changes.
   Expect each file to need a handful of API shims; reference Phase 2's patches for a similar-scope example.

2. **Dependencies on other OHOS kernel additions**.
   - `authentication.c` calls into the `access_token_id` driver (already ported in Phase 2).
   - May reach into `hmfs` or `hmdfs` helpers — check and stub / port as needed.
   - Some util macros live in `include/linux/sharefs/` — pull those in too.

3. **Kconfig / defconfig**.
   - Add `CONFIG_SHAREFS=y` to both `device/board/oniro/hybris_generic/kernel/x23/patches/config.diff` and the mimir equivalent.
   - Wire the new directory into `fs/Kconfig` / `fs/Makefile`.

4. **Rebuild + deploy** via `device/board/oniro/hybris_generic/kernel/x23/build.sh` and the mimir counterpart. Verify with `cat /proc/filesystems | grep sharefs` after boot.

5. **Revert the workaround** in `utils/lxc/config`:
   - Remove the `nosharefs/docs → sharefs/docs` bind entry.
   - `storage_daemon::MountSharefs()` will now succeed (`mount -t sharefs ... /mnt/user/<uid>/sharefs/docs`) and the per-URI filtering comes back.
   - Verify an app without a URI grant cannot list a file it shouldn't see, and that picker-granted files remain accessible.

6. **Keep the `storage_user_path.json` perm change** (`0711` → `0755`). Even with `sharefs` in place, the sandboxed `ExternalFileManager` still needs `r-x` on the parent dirs to enumerate them via `nosharefs`. The perm relax is orthogonal to `sharefs` and should persist.

### Effort estimate

- Pure-`sharefs` porting: ~1–2 days for someone familiar with the X23 kernel tree (Phase-2-style work: API shims, no algorithmic design).
- Token-id glue and URI-policy ioctl: another ~0.5–1 day to validate the auth path still works end-to-end against the 5.10 accesstokenid driver we've already ported.
- Testing: ~0.5 day — need at least one app with `FILE_ACCESS_COMMON_DIR` (e.g. VLC) and one privileged enumerator (file picker / ExternalFileManager) to confirm the full flow.

### When to do this

Low priority for bring-up: the current workaround is fully functional for single-user development use. Upgrade to the proper fix if/when:

- We need real per-app file isolation (multi-user scenarios, dev → production transition).
- A pre-installed app has to operate on a narrow `dec-paths` set without seeing the rest of `/storage/Users`.
- We pick up an OHOS update that changes `storage_daemon`'s mount logic in a way the `IsPathMounted` early-exit no longer covers.

## Files touched by the workaround

| File | Change |
|------|--------|
| `device/board/oniro/hybris_generic/utils/lxc/config` | Add `lxc.mount.entry` binding `nosharefs/docs` → `sharefs/docs` |
| `foundation/filemanagement/storage_service/services/storage_daemon/storage_user_path.json` | `mode: "0711"` → `"0755"` on the four `{no,}sharefs/docs{,/currentUser}` entries |
| `device/board/oniro/hybris_generic/utils/start-ohos.sh` | (Previously added chmod loop — removed after JSON patch replaced the need) |

## 12.1 Normal-app file picker (VLC "Open File") — downstream of the same hmdfs gap

**Symptom (2026-04-16):** In a normal app that calls `picker.DocumentViewPicker().select(...)` (e.g. VLC's `OPEN_FILE_SERVICE` flow), tapping "Open File" does nothing. The picker never appears; `com.ohos.filepicker` spawns, reaches `FilePickerUIExtAbility.onSessionCreate`, then hangs and is killed by AMS with `LIFECYCLE_TIMEOUT`.

**Why Settings / `ExternalFileManager` picker worked already:** that flow enumerates `/mnt/user/100/nosharefs/docs` directly and doesn't touch MediaLibrary at all. VLC's picker goes through `com.ohos.filepicker`'s `FilePickerUIExtAbility`, whose `onSessionCreate → getMediaLibrary()` spawns `com.ohos.medialibrary.medialibrarydata` and connects to `datashare:///media`. That path was broken the whole time in our build — nobody had exercised it until VLC was installed.

**Root cause (two chained checks in `foundation/multimedia/media_library/frameworks/innerkitsimpl/medialibrary_data_extension/src/media_datashare_ext_ability.cpp::CheckUnlockScene`):**

1. **`IsStartBeforeUserUnlock()` → true** because `account_iam`'s `isVerified` for user 100 was `false`. On stock OHOS, `OsAccountInterface::SendToStorageAccountStart` → `UnlockUser` → `StorageManager::PrepareStartUser` → `storage_daemon::StartUser` tries to `mount(.., "hmdfs", ..) /mnt/hmdfs/100/account`. Our Halium 5.10 kernel has no `hmdfs` driver (same class as the `sharefs` problem above), so the mount fails with `ENODEV` and `StartUser` returns error `13600721`. `SendToStorageAccountStart` then keeps `isUserUnlocked = false`, never calls `SetIsVerified(true)`, and the runtime `verifiedAccounts_` map stays empty. MediaLibrary sees this, logs `Killing self caused by booting before unlocking`, and calls `KillApplicationSelf()`. The picker hangs on `datashare:///media` connect and AMS kills it on `LIFECYCLE_TIMEOUT`.

2. **`MediaFileUtils::IsDirectory(ROOT_MEDIA_DIR)` → false** (where `ROOT_MEDIA_DIR = "/storage/cloud/files/"`). On stock OHOS, `cloud_service` creates / mounts `/storage/cloud/<userId>/files/` at boot. We have no cloud service. Even after fixing (1), MediaLibrary still self-killed with `Killing self caused by media path unmounted`.

**Fix 1 — account verified in container mode.** Patched `base/account/os_account/services/accountmgr/src/osaccount/os_account_interface.cpp::SendToStorageAccountStart` to force `isUserUnlocked = true` when `getenv("container") != nullptr`, mirroring the existing `#else isUserUnlocked = true` fallback for builds without `HAS_STORAGE_PART`. This restores the `SetIsVerified(true)` + `SetIsLoggedIn(true)` auto-path (user 100 has no credential → no PIN check to bypass). Rebuilt `accountmgr` via `--build-target accountmgr`; the interesting binary is `libaccountmgr.z.so` at `out/hybris_generic/account/os_account/` (the `packages/phone/` copy is stale after `--build-target` — same gotcha documented in the auto-memory for `--fast-rebuild`). After patch: `acm dump -i 100` reports `Verified: true`.

**Fix 2 — `/storage/cloud/<userId>/files` auto-created.** Added an entry to `foundation/filemanagement/storage_service/services/storage_daemon/storage_user_path.json`:

```json
{
    "path": "/storage/cloud/<userId>/files",
    "mode": "0711",
    "uid": 1008,
    "gid": 1008
}
```

`storage_daemon`'s `PrepareUserDirs` reads this JSON on user activation and creates the directory via `PrepareDir`. No source patch, no cloud-service emulation needed — just the directory shell is enough to satisfy `IsDirectory(ROOT_MEDIA_DIR)`. Files end up on plain ext4 (no cloud sync), which matches the "no cloud service" reality.

**Trade-off:** MediaLibrary now starts cleanly but `/storage/cloud/files/` is an empty plain-ext4 directory, not a cloud-synced mount. Any cloud-sync features (upload/download, cross-device media) silently no-op, which is the same posture as every other cloud-adjacent feature in this build.

**Not fixed by these patches:** the underlying `UnlockUser` failure itself. `storage_daemon::StartUser` still fails with `ENODEV` on the hmdfs mount and `PrepareStartUser` still returns `13600721`. Any feature that *actually* depends on the hmdfs mount (distributed files, hmdfs cross-device shared media, `/data/storage/el2/distributedfiles`) remains unusable until the proper kernel-port fix — same overall shape as the `sharefs` TODO above.

### Files touched (12.1)

| File | Change |
|------|--------|
| `base/account/os_account/services/accountmgr/src/osaccount/os_account_interface.cpp` | `SendToStorageAccountStart`: force `isUserUnlocked = true` when `getenv("container") != nullptr` |
| `foundation/filemanagement/storage_service/services/storage_daemon/storage_user_path.json` | Add `/storage/cloud/<userId>/files` entry (mode 0711, uid/gid 1008) |

## Related

- Phase 2 (`legacy_kernel_adaptation.md`) — the reference for how we port OHOS kernel drivers onto the Halium kernel.
- Phase 6.14 — `appdata-sandbox.json` deployment context (nweb sandbox config fix, same file family).
- 12.1 shares the same underlying kernel-driver gap as the main Phase 12 work (`sharefs` there, `hmdfs` here) — both fall out of running stock OHOS storage code on Halium 5.10 without the OHOS-side kernel drivers.
