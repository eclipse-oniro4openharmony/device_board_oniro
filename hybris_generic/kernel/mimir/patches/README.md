# Volla Tablet (mimir) kernel patches

All modifications to the Volla Tablet (MT8781) kernel build live here and
in `../config/`. `../build_kernel.sh` is their **single owner** — it
applies everything in this directory during the build. A clean
`bash build_kernel.sh` from a fresh checkout reproduces the kernel.

`system_patch/` deliberately does **not** touch the kernel: the kernel-port
repo and the MT8781 source are not OHOS-checkout repos (one is cloned, the
other downloaded, both by `build_kernel.sh`), so `system_patch/do_patch.sh`
— which `git am`s into existing checkout repos — cannot manage them.

## Two target trees

| Subdir | Target tree | When it exists |
|---|---|---|
| `port-repo/` | the `volla-mimir` kernel-port repo (`kernel/linux/volla-mimir/`) | cloned by `build_kernel.sh` (git) |
| `kernel-source/` | the MT8781 kernel source (`…/downloads/android_kernel_volla_mt8781`) | downloaded by the Halium build, then copied to a fresh temp tree per build |

`../config/openharmony.config` is the kernel defconfig fragment, copied
into the source as `arch/arm64/configs/openharmony.config`.

## port-repo/

Applied to `$KERNEL_TREE` with `patch -N` (the repo persists between
builds, so `-N` no-ops cleanly when already applied).

| Patch | Patches | Why |
|---|---|---|
| `deviceinfo.patch` | `deviceinfo` | adds `openharmony.config` to the defconfig list + OHOS kernel cmdline params |
| `build-tools.patch` | `build/` (Halium build tools) | widens `ALLOWED_HOST_TOOLS`; `python2` → `python` |
| `libufdt.patch` | `build-dir/downloads/libufdt` | libufdt build fix |

## kernel-source/

Applied to the **fresh per-build temp copy** of the kernel source. That
copy is recreated every build, so a plain `patch -p1` always applies to a
clean tree — a failure is a real error (the build aborts).

| Patch | Patches | Why |
|---|---|---|
| `ohos-adaptation.patch` | `drivers/`, `include/`, `kernel/fork.c`, … | OHOS kernel drivers: hilog, accesstokenid, blackbox, hievent, binder token-id, … |
| `hdf.patch` (+ `hdf_patch.sh`) | HDF driver framework | applied via the helper script — it also symlinks/copies the in-tree HDF repos |

To add a kernel-source patch, drop a `*.patch` here — `build_kernel.sh`
globs the directory (skipping `hdf.patch`, which the helper script owns).
Patches apply in glob (alphabetical) order; prefix with `NN-` if a
specific order is required.

> The X23 (`kernel/x23/`) additionally carries a `sharefs.patch` here that
> ports the OHOS `sharefs` filesystem. mimir does not — if the tablet's
> file picker needs `/storage/Users` access for normal apps, the same
> port applies (see `kernel/x23/patches/kernel-source/sharefs.patch` and
> `docs/hybris_generic/legacy_sharefs_user_files.md`).
