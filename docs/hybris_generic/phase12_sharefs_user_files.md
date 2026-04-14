# Phase 12: User-File Access via `sharefs` — Current Workaround & Proper Fix

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

## Related

- Phase 2 (`phase2_kernel_adaptation.md`) — the reference for how we port OHOS kernel drivers onto the Halium kernel.
- Phase 6.14 — `appdata-sandbox.json` deployment context (nweb sandbox config fix, same file family).
