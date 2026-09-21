#!/usr/bin/env bash
# waydroid-patches.sh — prove that patches.conf matches the pinned images.
#
# The Waydroid container needs two files of the upstream images changed by a
# handful of instruction words.  The table, and the reasoning behind every
# word, is device/soc/oniro/hybris_generic/waydroid/data/patches.conf; waydroidd
# applies it on the device, on every container start, to the images it has just
# mounted.  Nothing patched is built, stored or pushed from the host any more.
#
# What is left for the host is the check this script does: replay the same
# table against the sha256-pinned images and compare pristine and patched
# results with the `sha256` lines of patches.conf.  waydroidd only asserts the
# original words, so this is the stronger test — run it whenever
# images.manifest or patches.conf changes:
#
#   waydroid-images.sh fetch && waydroid-patches.sh
#
# To re-derive a patch for a new image build, follow the "Re-derive" notes in
# patches.conf, then put the new sha256 pair (printed below on a mismatch) in.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE_ROOT="$(cd "$SCRIPT_DIR/../../../../../.." && pwd)"
BLOB_DIR="$TREE_ROOT/halium-blobs/waydroid"
CONF="$TREE_ROOT/device/soc/oniro/hybris_generic/waydroid/data/patches.conf"

die() { echo "ERROR: $*" >&2; exit 1; }

command -v debugfs >/dev/null || die "debugfs not found (apt install e2fsprogs)"
[ -f "$CONF" ] || die "no $CONF"
for i in system vendor; do
    [ -f "$BLOB_DIR/$i.img" ] || die "no $BLOB_DIR/$i.img — run waydroid-images.sh fetch first"
done

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT

# Container path -> (image, path inside that image): vendor.img is mounted at
# /vendor, everything else comes from system.img (whose root is the rootfs).
python3 - "$CONF" "$BLOB_DIR" "$WORK" <<'PY'
import hashlib, os, struct, subprocess, sys

conf, blob_dir, work = sys.argv[1:4]
files, order = {}, []
for n, raw in enumerate(open(conf, encoding='utf-8'), 1):
    f = raw.split('#', 1)[0].split()
    if not f:
        continue
    kw, name = f[0], f[1]
    if kw == 'file':
        files[name] = {'mode': f[2], 'path': f[3], 'words': [], 'sha': None}
        order.append(name)
    elif kw == 'word':
        files[name]['words'].append(tuple(int(x, 16) for x in f[2:5]))
    elif kw == 'sha256':
        files[name]['sha'] = (f[2], f[3])
    else:
        sys.exit(f'{conf}:{n}: unknown keyword {kw}')

sha = lambda b: hashlib.sha256(b).hexdigest()
bad = 0
for name in order:
    e = files[name]
    if e['path'].startswith('/vendor/'):
        img, inner = 'vendor.img', e['path'][len('/vendor'):]
    else:
        img, inner = 'system.img', e['path']
    out = os.path.join(work, name)
    subprocess.run(['debugfs', '-R', f'dump {inner} {out}',
                    os.path.join(blob_dir, img)],
                   check=True, stderr=subprocess.DEVNULL)
    if not os.path.getsize(out):
        sys.exit(f'{name}: debugfs could not extract {inner} from {img}')
    d = bytearray(open(out, 'rb').read())
    pristine = sha(d)
    for off, old, new in e['words']:
        cur = struct.unpack_from('<I', d, off)[0]
        if cur != old:
            sys.exit(f'{name}: at {off:#x} expected {old:#010x}, found '
                     f'{cur:#010x} — the image changed, re-derive the patch')
        struct.pack_into('<I', d, off, new)
    patched = sha(d)
    ok = e['sha'] == (pristine, patched)
    bad += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {name}: {len(e['words'])} word(s), "
          f"mode {e['mode']}, {img}:{inner}")
    if not ok:
        print(f'     patches.conf: sha256 {name} {pristine} {patched}')
sys.exit(1 if bad else 0)
PY
echo "patches.conf matches the pinned images"
