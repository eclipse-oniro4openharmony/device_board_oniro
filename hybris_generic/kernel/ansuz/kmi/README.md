# KMI contract — stock MediaTek modules

`stock-module-versions.txt` is the union of every `(crc, symbol)` pair the
Plinius' **stock** prebuilt kernel modules expect: the `__versions` tables of
the vendor_boot module overlay plus `vendor_dlkm` and `system_dlkm`
(486 modules with bootstrap v3 and port repo f8c7581).  `build_kernel.sh`
diffs our `Module.symvers` against it and fails the build if the OHOS
adaptation ever changes the CRC of a symbol a stock module imports — which
would make that module refuse to load.

It is build metadata (symbol names + CRCs), not a blob, so it is tracked here
rather than under the gitignored `halium-blobs/`.  Regenerate it whenever the
stock modules change (a new bootstrap pin in `pull-halium-blobs.sh` or a new
`ANSUZ_REPO_SHA` in `build_kernel.sh`):

```bash
bash device/board/oniro/hybris_generic/kernel/ansuz/kmi/gen-stock-module-versions.sh
```

It extracts the `.ko` files from `halium_vendor_dlkm_a.img`,
`halium_system_dlkm_a.img` and the port repo's `vendor-ramdisk-overlay`, and
writes the union of their `__versions` tables.
