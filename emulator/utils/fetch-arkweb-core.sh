#!/bin/bash
#
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License").
#
# Fetch the x86_64 ArkWeb engine (ArkWebCore.hap) for the emulator_x86_64
# emulator into base/web/webview/ohos_nweb/prebuilts/x86_64/.
#
# OpenHarmony 7.0 ships ArkWebCore only for arm/arm64. On x86_64 the stock
# build installs the arm64 HAP, whose engine cannot load, so every Web
# component (Browser, in-app WebViews) stays blank. The webview system_patch
# makes x86_64 builds take prebuilts/x86_64/ArkWebCore.hap, and gn stops with
# an error while the HAP is missing.
#
# The HAP is a source build of Chromium M144 / ArkWeb from the OpenHarmony-TPC
# OpenHarmony-7.0-Release branches, made by harmony-contrib/ohos-qemu, and is
# rehosted unchanged and SHA256-pinned on a device_soc_oniro release (the
# release notes and assets have the source pins and build record).
#
# Usage:
#   fetch-arkweb-core.sh               download, verify and install
#   fetch-arkweb-core.sh --from FILE   verify and install a local copy
#
# One-time per tree. Run it again after a pin bump.

set -euo pipefail

VERSION=7.0.0.40
URL="https://github.com/eclipse-oniro4openharmony/device_soc_oniro/releases/download/arkwebcore-x86_64-${VERSION}/ArkWebCore-x86_64-${VERSION}.hap"
SHA256=ff7e549fb6cfa585a577cb2fdd4d4944f920c64f1a90e31b157ddc617b2006c4

ROOT_DIR=$(cd "$(dirname "$0")/../../../../.." && pwd)
WEBVIEW=${ROOT_DIR}/base/web/webview
DEST_DIR=${WEBVIEW}/ohos_nweb/prebuilts/x86_64
DEST=${DEST_DIR}/ArkWebCore.hap

src=""
case "${1:-}" in
  --from) src=${2:?--from needs a file} ;;
  -h|--help) sed -n '7,26p' "$0"; exit 0 ;;
  "") ;;
  *) echo "unknown argument: $1" >&2; exit 2 ;;
esac

[ -d "${WEBVIEW}/ohos_nweb" ] || { echo "not an OpenHarmony tree: ${WEBVIEW} missing" >&2; exit 1; }

if [ -f "${DEST}" ] && echo "${SHA256}  ${DEST}" | sha256sum -c --status; then
  echo "ArkWebCore x86_64 ${VERSION} already in place: ${DEST}"
  exit 0
fi

mkdir -p "${DEST_DIR}"
tmp=$(mktemp "${DEST_DIR}/.ArkWebCore.hap.XXXXXX")
trap 'rm -f "${tmp}"' EXIT

if [ -n "${src}" ]; then
  cp "${src}" "${tmp}"
else
  echo "Downloading ${URL}"
  # curl --retry does not retry a connection reset mid-transfer (error 56) and
  # --retry-all-errors needs curl 7.71, so loop here and resume the partial file.
  for try in 1 2 3 4 5; do
    curl -fL --retry 3 --progress-bar -C - -o "${tmp}" "${URL}" && break
    [ "${try}" -lt 5 ] || { echo "download failed" >&2; exit 1; }
    echo "retrying (${try}/5)" >&2
    sleep 3
  done
fi

if ! echo "${SHA256}  ${tmp}" | sha256sum -c --status; then
  echo "SHA256 mismatch: expected ${SHA256}, got $(sha256sum "${tmp}" | cut -d' ' -f1)" >&2
  exit 1
fi
chmod 0644 "${tmp}"
mv "${tmp}" "${DEST}"
trap - EXIT

# Keep the fetched binary out of the webview repo's git status.
exclude=$(git -C "${WEBVIEW}" rev-parse --git-path info/exclude 2>/dev/null || true)
if [ -n "${exclude}" ]; then
  case "${exclude}" in /*) ;; *) exclude=${WEBVIEW}/${exclude} ;; esac
  mkdir -p "$(dirname "${exclude}")"
  grep -qxF "/ohos_nweb/prebuilts/x86_64/" "${exclude}" 2>/dev/null || \
    echo "/ohos_nweb/prebuilts/x86_64/" >> "${exclude}"
fi

echo "Installed ArkWebCore x86_64 ${VERSION}: ${DEST}"
