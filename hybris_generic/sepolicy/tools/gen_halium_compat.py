#!/usr/bin/env python3
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""Generate halium_compat.te: every Android (Halium) SELinux type name as an
alias of one of a few Oniro types.

Why: the kernel has a single SELinux policy, and it is the OHOS one.  The
Halium container's Android init, its services and the files on the Halium
partitions and APEXes all carry Android context strings (xattrs, seclabels,
property_contexts).  A context naming a type the loaded policy does not
know is invalid: setexeccon() and security_compute_create() fail with
EINVAL, so Android init cannot start a single service, and
__system_property_area_init() aborts on the failed fsetxattr.

Declaring each Android type name as an alias keeps every Android context
valid without importing Android's rules:
  * Android domains        -> halium_domain
  * Android property types -> halium_property
  * everything else        -> halium_file
Names that already exist in the OHOS policy (65 on 7.0: init, kernel,
ueventd, system_file, vendor_file, ...) keep their OHOS meaning and are left
out.

Usage:
  gen_halium_compat.py --ohos-cil system.cil --ohos-cil vendor.cil \
      --ohos-cil min.cil --android-cil plat_sepolicy.cil \
      --android-cil vendor_sepolicy.cil [...] \
      -o halium/system/halium_compat.te \
      --transition-output halium/system/halium_compat_transition.te

The --ohos-cil inputs are the intermediate CILs of a stock OHOS policy build
(out/.../selinux_adapter/{system,vendor,min}.cil, built WITHOUT this
directory).  Feed the Android CILs of every device the image serves
(Plinius/ansuz and X23): /system/etc/selinux/plat_sepolicy.cil and
/vendor/etc/selinux/vendor_sepolicy.cil of each Halium image, plus
system_ext/product/odm CILs if present.
"""

import argparse
import re
import sys

ONIRO_TYPES = {"halium_init", "halium_domain", "halium_property", "halium_file"}
VERSION_SUFFIX = re.compile(r"^(.+)_[0-9]+_[0-9]+$")
# Labels no Halium policy defines but that are on disk: the persist and
# firmware partitions were formatted by the stock VollaOS system, whose
# vendor policy differs from Halium's ("SELinux: Context ... is not valid
# (left unmapped)" in dmesg after the policy load).
EXTRA_FILE_TYPES = {"firmware_file", "persist_file"}


def cil_names(path, keyword):
    pat = re.compile(r"^\(" + keyword + r" ([^ ()]+)\)")
    names = set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = pat.match(line)
            if m:
                names.add(m.group(1))
    return names


def attr_members(paths, attrs):
    """Members of Android attributes (plain lists only; that is how
    Android's compiled CIL spells domain/property_type)."""
    members = set()
    pat = re.compile(r"^\(typeattributeset ([^ ]+) \((.*)\)\)\s*$")
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for line in f:
                m = pat.match(line)
                if not m or m.group(1) not in attrs:
                    continue
                body = m.group(2)
                if "(" in body:  # and/not/all expressions: not used for these
                    continue
                members.update(body.split())
    return members


def unversion(name, known):
    m = VERSION_SUFFIX.match(name)
    if m and m.group(1) in known:
        return m.group(1)
    return name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ohos-cil", action="append", required=True)
    ap.add_argument("--android-cil", action="append", required=True)
    ap.add_argument("-o", "--output", required=True,
                    help="aliases (system policy dir: CIL typealiases may "
                    "not be re-declared, and public files land in both "
                    "system.cil and vendor.cil)")
    ap.add_argument("--transition-output", required=True,
                    help="halium_init exec transition (system policy dir: it "
                    "names OHOS types the vendor policy does not have)")
    args = ap.parse_args()

    ohos = set()
    for p in args.ohos_cil:
        for kw in ("type", "typeattribute", "typealias"):
            ohos |= cil_names(p, kw)

    android_types = set()
    android_aliases = {}
    for p in args.android_cil:
        android_types |= cil_names(p, "type")
        with open(p, encoding="utf-8") as f:
            for line in f:
                m = re.match(r"^\(typealiasactual ([^ ]+) ([^ ()]+)\)", line)
                if m:
                    android_aliases[m.group(1)] = m.group(2)
    # Versioned compat types (foo_34_0) are attributes in Android, not types
    # anyone labels with; drop them.
    android_types = {t for t in android_types
                     if unversion(t, android_types) == t} | EXTRA_FILE_TYPES

    domains = {unversion(t, android_types)
               for t in attr_members(args.android_cil, {"domain", "domain_34_0"})}
    props = {unversion(t, android_types)
             for t in attr_members(args.android_cil,
                                   {"property_type", "system_property_type",
                                    "vendor_property_type"})}

    buckets = {"halium_domain": [], "halium_property": [], "halium_file": []}
    skipped = sorted((android_types | set(android_aliases)) & (ohos | ONIRO_TYPES))
    for name in sorted(android_types | set(android_aliases)):
        if name in ohos or name in ONIRO_TYPES:
            continue
        actual = android_aliases.get(name, name)
        if actual in domains:
            buckets["halium_domain"].append(name)
        elif actual in props:
            buckets["halium_property"].append(name)
        else:
            buckets["halium_file"].append(name)

    with open(args.output, "w", encoding="utf-8") as out:
        out.write("# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.\n"
                  "# SPDX-License-Identifier: Apache-2.0\n"
                  "#\n"
                  "# GENERATED by tools/gen_halium_compat.py -- do not edit by hand.\n"
                  "# Android (Halium) type names as aliases of the Oniro halium_* types,\n"
                  "# so that Halium's Android contexts are valid under the OHOS policy.\n"
                  "# Android names that the OHOS policy already defines (kept as OHOS):\n")
        for i in range(0, len(skipped), 8):
            out.write("#   " + " ".join(skipped[i:i + 8]) + "\n")
        for target, names in buckets.items():
            out.write("\n# %d names\n" % len(names))
            out.write("typealias %s alias {\n" % target)
            for n in names:
                out.write("    %s\n" % n)
            out.write("};\n")
        # Android init refuses to start a service whose computed context
        # equals its own ("no domain transition"), even when permissive, and
        # an executable can carry any Android label, an OHOS-colliding one
        # (ueventd is system_file) or none at all (unlabeled).
    exec_like = ["halium_file", "unlabeled"] + [
        n for n in skipped if n not in domains and n in android_types]
    with open(args.transition_output, "w", encoding="utf-8") as out:
        out.write("# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.\n"
                  "# SPDX-License-Identifier: Apache-2.0\n"
                  "#\n"
                  "# GENERATED by tools/gen_halium_compat.py -- do not edit by hand.\n"
                  "# Every exec by Halium's Android init lands in halium_domain.\n")
        out.write("type_transition halium_init {\n")
        for n in exec_like:
            out.write("    %s\n" % n)
        out.write("}:process halium_domain;\n")
    print("domains %d, properties %d, files %d, kept-OHOS %d" % (
        len(buckets["halium_domain"]), len(buckets["halium_property"]),
        len(buckets["halium_file"]), len(skipped)), file=sys.stderr)


if __name__ == "__main__":
    main()
