# hybris_generic SELinux policy

Policy added on top of the OHOS policy (`selinux_adapter_build_path` in
`vendor/oniro/hybris_generic/config.json`):

| dir | what |
|-----|------|
| `halium/` | The Halium (Android) container as one permissive island: `halium_init` (Android init), `halium_domain` (every other Android process), `halium_file`, `halium_property`. |
| `halium/system/halium_compat*.te` | **Generated.** Every Android type name of the Plinius and X23 Halium policies as an alias of one of the four types, and the exec transition Android init needs. |
| `androidd/` | The OHOS daemon that provisions the container and execs Android init into `halium_init`. |
| `upstream_gaps/` | Domains named by OHOS 7.0 service cfgs that have no policy in the 7.0 tree. |

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

Permissive (`selinux_adapter_enforce = false`): the policy is loaded and
OHOS services run in their domains, and denials are audited
(`dmesg | grep avc`).
