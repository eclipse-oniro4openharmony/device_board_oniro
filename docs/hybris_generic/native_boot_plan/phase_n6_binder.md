# Phase N6 — Binder Device Management

**Status:** ✅ Source-side complete (2026-04-30)

Provision separate `binder` context-manager devices for OHOS (host) and Android (guest), with `hwbinder` and `vndbinder` shared across both.

---

## N6.1 — Mount binderfs in init.x23.cfg ✅

Already shipped in Phase N3.3. The relevant pre-init commands:

```
mkdir /dev/binderfs 0755 root root
mount binder binder /dev/binderfs nodev,noexec,nosuid
symlink /dev/binderfs/binder    /dev/binder
symlink /dev/binderfs/hwbinder  /dev/hwbinder
symlink /dev/binderfs/vndbinder /dev/vndbinder
```

**Kernel support verified (2026-04-30, on-device):**

```
$ cat /proc/config.gz | gunzip | grep -E "BINDERFS|BINDER_DEVICES"
CONFIG_ANDROID_BINDERFS=y
CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder,anbox-binder,anbox-hwbinder,anbox-vndbinder"
```

Multiple binder context-managers are already proven in this kernel — both anbox and ohos namespaces use distinct contexts today via the LXC bind-mount path, and our N4.2 launcher adds an `android-binder` context to the same kernel. **No kernel rebuild required.**

---

## N6.2 — Allocation strategy ✅

**Plan adjustment from N0:** the original plan migrated OHOS samgr to the *default* `/dev/binder` and gave Android a dedicated `android-binder`. We keep OHOS on the existing `ohos-binder` device (or default `binder`, the choice is OHOS samgr's discretion via its existing config) and only add `android-binder` for the guest. This keeps every existing OHOS binder client wire-compatible with the LXC build.

**Resulting binder visibility:**

| Process | `/dev/binder` (= what file) | `/dev/hwbinder` | `/dev/vndbinder` |
|---|---|---|---|
| OHOS samgr (host PID 1) | `/dev/binderfs/binder` (default) | `/dev/binderfs/hwbinder` | `/dev/binderfs/vndbinder` |
| Android servicemanager (in androidd's PID NS) | `/dev/binderfs/android-binder` (bind-mounted as `/dev/binder` inside the Android NS) | same hwbinder kernel object as OHOS | same vndbinder |

OHOS and Android servicemanagers register as context manager on **different** kernel binder objects (`binder` vs `android-binder`), so there is no collision.

`hwbinder` and `vndbinder` are shared — that's the entire point: hwservicemanager registers once on `hwbinder`, and OHOS-side hybris-VDI binder clients can call into it.

---

## N6.3 — C device-creation utility ✅

Replaces the existing Python script (`/home/phablet/openharmony/create_ohos_binder.py`) with a function in `androidd.c`:

```c
static int create_binderfs_device(const char *name)
{
    int fd = open(BINDERFS_CONTROL, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    struct binderfs_device dev = {0};
    strncpy(dev.name, name, sizeof(dev.name) - 1);
    int rc = ioctl(fd, BINDER_CTL_ADD, &dev);
    int saved_errno = errno;
    close(fd);
    if (rc < 0 && saved_errno == EEXIST) return 0;
    return rc;
}
```

Idempotent: `EEXIST` is treated as success so a launcher restart works. Confirmed in Phase N4.2 source.

The Python script `/home/phablet/openharmony/create_ohos_binder.py` is **superseded** by this C function in native boot. It can stay on the device for the LXC build (no removal needed) — the two paths are independent.

---

## N6.4 — Symlinks for OHOS ✅

Already shipped in Phase N3.3 (above). OHOS sees:
- `/dev/binder    -> /dev/binderfs/binder`
- `/dev/hwbinder  -> /dev/binderfs/hwbinder`
- `/dev/vndbinder -> /dev/binderfs/vndbinder`

These symlinks are created by `init.x23.cfg` pre-init, before any service that opens binder runs.

---

## Plan adjustments emitted by N6

1. **OHOS keeps using `ohos-binder` (or default `binder`) — does NOT need to migrate to anything new.** Only Android's binder is "new" (the `android-binder` context).
2. **Idempotent device creation** — `EEXIST` is success, so restart-after-crash works.
3. **`create_ohos_binder.py` is not removed** — the LXC build still uses it; native boot just doesn't run it.
4. **Kernel config: nothing to add.** `CONFIG_ANDROID_BINDERFS=y` is already on. The kernel cmdline `CONFIG_ANDROID_BINDER_DEVICES` is irrelevant once binderfs is enabled — devices are created via ioctl, not via the kernel cmdline static list.

## Tasks status

- ✅ **N6.1** — binderfs mounted in `init.x23.cfg` pre-init (Phase N3.3)
- ✅ **N6.2** — allocation strategy: OHOS keeps existing devices; Android gets new `android-binder`
- ✅ **N6.3** — `create_binderfs_device()` in C in `androidd.c` (Phase N4.2)
- ✅ **N6.4** — OHOS symlinks shipped (Phase N3.3)

## Next phase entry condition

N7 needs: kernel UDC node identification (✅ from N0 — `musb-hdrc`), USB configfs setup precedent (✅ — `/vendor/etc/init/init.usb.configfs.cfg` exists). Move forward to N7.
