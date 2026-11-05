# hybris_generic SELinux policy

Policy added on top of the OHOS policy (`selinux_adapter_build_path` in
`vendor/oniro/hybris_generic/config.json`):

| dir | what |
|-----|------|
| `halium/` | The Halium (Android) container as one permissive island: `halium_init` (Android init), `halium_domain` (every other Android process), `halium_file`, `halium_property`, `halium_device`. |
| `halium/system/halium_compat*.te` | **Generated.** Every Android type name of the Plinius and X23 Halium policies as an alias of one of the five types, and the exec transition Android init needs. |
| `androidd/` | The OHOS daemon that provisions the container and execs Android init into `halium_init` (permissive, like the container). |
| `hybris/` | `hybris_client`: the OHOS domains that load Android libraries through libhybris (HDI hosts, render_service, bootanimation, av_codec_service), Halium's binder, the MediaTek nodes the VDIs use, binderfs labels. |
| `chainload/` | What the chainload changes: `/dev` is the ramdisk's devtmpfs (init relabels kernel-created nodes, ueventd fixes existing ones), binderfs and devpts mounted from the board cfgs, `/dev/.ohos-cmdline` over `/proc/cmdline`. |
| `daemons/` | Oniro daemons: `wmtdetect` (wmtdetect-init), `location_timezone`, `usb_udc_bind`; the `ohos.hybris.*` parameters. |
| `config/` | Product configuration Oniro installs on sys_prod (and the board's ressched config on /vendor), readable where it is used. |
| `upstream_gaps/` | Domains named by OHOS 7.0 service cfgs that have no policy in the 7.0 tree. |
| `debug/` | `su` (hdc's root shell) permissive on root builds. |
| `*.json` | Whitelists and baselines `selinux_check` reads from every policy dir. |

## Why aliases

There is one policy in the kernel, the OHOS one. Android init, its services
and the files on the Halium partitions and APEXes carry Android contexts
(xattrs, `seclabel`, `property_contexts`). A context naming an unknown type
is invalid, even in permissive mode: `setexeccon()` and
`security_compute_create()` fail with EINVAL, so Android init starts no
service, and its property area setup aborts. Aliases make every Android
context valid without importing Android's rules. The 65 Android names that
OHOS also defines (`init`, `kernel`, `ueventd`, `system_file`, ...) keep
their OHOS meaning.

Android's own object managers (servicemanager, hwservicemanager, the
property service) check classes the OHOS policy does not define
(`service_manager`, `hwservice_manager`, `property_service`). The product
builds the policy with `--handle-unknown=allow` (system_patch
`base/security/selinux_adapter` 0003), so those checks pass instead of
failing closed.

androidd keeps Android's labeling inside the container (`selinux_overlays()`
in `launcher/androidd.c`): Halium's domains are permissive, so their relabels
would go through, and sysfs and the nodes bound in from OHOS's `/dev` are
shared objects. Android's file_contexts entries for them become `<<none>>`,
and init.rc's `seclabel u:r:ueventd:s0` (also an OHOS domain) becomes
`halium_domain`.

## Platform notes

* `/data` created before the policy existed is entirely `unlabeled`: a phone
  moving to this image needs a userdata wipe (app data labels come from the
  bundle manager at install time; `restorecon` cannot recreate them).
* `/apex` (`halium_apex.img`) is labeled at pack time by
  `kernel/ansuz/build_super_img.sh` (e2fsdroid); `mke2fs -d` writes no
  labels.
* Collecting every denial of a boot needs the printk ratelimit off (kauditd
  prints through it): build the vendor_boot with
  `OHOS_CMDLINE_DEBUG="log_buf_len=16M sysctl.kernel.printk_ratelimit=0"`.
* `enforcing=0` on the OHOS cmdline (the same variable) overrides the
  config file: the way back from an enforcing image that does not boot.
* OHOS's `restorecon` takes one path and no `-R`.

## Regenerating `halium_compat*.te`

Needed when the OHOS policy gains a type with an Android name (the build
fails with a re-declaration), or when a Halium image changes.

1. Build a stock OHOS policy (without this directory) to get its CILs:
   `system.cil`, `vendor.cil`, `min.cil` (run
   `base/security/selinux_adapter/scripts/build_policy.py` with
   `--policy_dir_list base/security/selinux_adapter/sepolicy/ohos_product`).
2. Extract the Android CILs of each device's Halium images:
   `/system/etc/selinux/plat_sepolicy.cil` from `halium_system_a.img`,
   `/etc/selinux/vendor_sepolicy.cil` and `plat_pub_versioned.cil` from
   `halium_vendor_a.img` (`debugfs -R "dump <path> <out>"` for ext4,
   `fsck.erofs --extract` for erofs, or `hdc file recv /android/...`).
3. Run:

   ```
   python3 tools/gen_halium_compat.py \
       --ohos-cil system.cil --ohos-cil vendor.cil --ohos-cil min.cil \
       --android-cil <ansuz>/plat_sepolicy.cil --android-cil <ansuz>/vendor_sepolicy.cil \
       --android-cil <ansuz>/plat_pub_versioned.cil \
       --android-cil <x23>/plat_sepolicy.cil --android-cil <x23>/vendor_sepolicy.cil \
       --android-cil <x23>/plat_pub_versioned.cil \
       -o halium/system/halium_compat.te \
       --transition-output halium/system/halium_compat_transition.te
   ```

After a boot, `dmesg | grep 'is not valid'` lists labels on disk that no
input policy defines; add them to `EXTRA_FILE_TYPES` in the generator.

## State

Permissive (`selinux_adapter_enforce = false`): the policy is loaded,
OHOS services run in their domains, denials are audited
(`dmesg | grep avc`).

Enforcing at runtime (`echo 1 > /sys/fs/selinux/enforce` after boot) was
tested on the Plinius Plus on a fresh /data: camera preview and capture,
the shutter sound through the MediaTek audio HAL, WiFi and Bluetooth on,
sensors, Settings and the Control Center all work.

Booting enforcing does not work yet: the boot loops (init's
critical-service panic about 67 s in).  storage_daemon's `sdc filecrypt
init_main_user` cannot reach samgr, the per-user /data directories are not
created, and later every service falls back from /dev/binderfs/binder to
the /dev/binder link.  Enforcing from the pre-init job onwards boots fine,
so the cause sits between the policy load and pre-init (the services
started there: ueventd, samgr, storage_daemon, hdf_devmgr, appspawn, ...).
No log of that window survived the loops yet: pstore keeps a stale console,
and no /data logger ran before the failure.
