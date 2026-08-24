# KMI contract — stock MediaTek modules

`stock-module-versions.txt` is the union of every `(crc, symbol)` pair the
Plinius' **stock** prebuilt kernel modules expect: the `__versions` tables of
the vendor_boot module overlay plus `vendor_dlkm` and `system_dlkm`
(447 modules).  `build_kernel.sh` diffs our `Module.symvers` against it and
fails the build if the OHOS adaptation ever changes the CRC of a symbol a
stock module imports — which would make that module refuse to load.

It is build metadata (symbol names + CRCs), not a blob, so it is tracked here
rather than under the gitignored `halium-blobs/`.  Regenerate it only when
Volla ships new stock modules: extract the `.ko` files from the current
`halium_vendor_dlkm_a.img` / `halium_system_dlkm_a.img` / vendor_boot ramdisk
and concatenate their `__versions` sections.
