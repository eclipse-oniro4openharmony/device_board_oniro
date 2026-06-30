#!/bin/bash

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

# Get the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Set the root directory (5 levels up from script directory)
ROOT_DIR="$SCRIPT_DIR/../../../../../"
ROOT_DIR="$(cd "$ROOT_DIR" && pwd)"

MODE="generate"
for arg in "$@"; do
    case "$arg" in
        -l|--list-dirty)
            MODE="list-dirty"
            ;;
        -h|--help)
            cat <<EOF
Usage: $(basename "$0") [OPTION]

With no option, generate git format-patch files for commits that exist
locally but not on any upstream remote branch, for every (non-oniro) git
repo under the OpenHarmony source tree.

Options:
  -l, --list-dirty   Instead of generating patches, list repositories with
                     uncommitted changes in the working tree or index.
  -h, --help         Show this help and exit.
EOF
            exit 0
            ;;
        *)
            echo "Unknown option: $arg (see --help)" >&2
            exit 2
            ;;
    esac
done

# Create the patches directory only when actually generating patches
PATCHES_DIR="$SCRIPT_DIR/patches"
if [[ "$MODE" == "generate" ]]; then
    mkdir -p "$PATCHES_DIR"
    echo "root directory: $ROOT_DIR"
    echo "patches directory: $PATCHES_DIR"
else
    echo "root directory: $ROOT_DIR"
fi

# Generate git format-patch files for all commits in the repo that exist
# in the local branch but not on any upstream remote branch.
#
# This produces one .patch file per commit, in the standard git format-patch
# format (with From: header, Subject:, diffstat, and diff) so that the patches
# can be replayed with git am.
generate_patch() {
    repo_path=$1
    root_dir=$2
    patches_base_dir=$3

    # Find commits that are local but not yet pushed to any remote branch.
    # --not --remotes excludes all commits reachable from any remote ref.
    local_commits=$(git -C "$repo_path" log HEAD --not --remotes --oneline 2>/dev/null)

    if [[ -z "$local_commits" ]]; then
        echo "No local (unpushed) commits in $repo_path"
        return
    fi

    # Get the relative path from ROOT_DIR to the repository
    rel_path=$(realpath --relative-to="$root_dir" "$repo_path")

    # Create subdirectory structure in patches directory
    patch_subdir="$patches_base_dir/$rel_path"
    mkdir -p "$patch_subdir"

    # Count local commits to build the format-patch range
    commit_count=$(echo "$local_commits" | wc -l)

    echo "Found $commit_count local commit(s) in $repo_path — generating patches in $patch_subdir"

    # Remove old patch files for this repo before regenerating
    rm -f "$patch_subdir"/*.patch

    # Generate one .patch file per local commit using git format-patch.
    # --no-numbered prevents a leading "0001-" prefix when there is only one
    # commit; --numbered (the default for multiple commits) keeps the prefix
    # so patches apply in the right order.
    git -C "$repo_path" format-patch \
        --output-directory "$patch_subdir" \
        "HEAD~${commit_count}..HEAD" 2>/dev/null

    if [[ $? -eq 0 ]]; then
        echo "Patch file(s) generated in: $patch_subdir"
        ls "$patch_subdir"/*.patch 2>/dev/null | while read -r f; do
            echo "  $f"
        done
    else
        echo "ERROR: format-patch failed for $repo_path"
    fi
}

# Report repos whose working tree or index has uncommitted changes.
# Prints one relative path per dirty repo.
list_dirty() {
    repo_path=$1
    root_dir=$2

    if [[ -z "$(git -C "$repo_path" status --porcelain 2>/dev/null)" ]]; then
        return
    fi

    realpath --relative-to="$root_dir" "$repo_path"
}

# Export the functions for use with find -exec
export -f generate_patch
export -f list_dirty

# Export variables for use with find -exec
export ROOT_DIR
export PATCHES_DIR

# Find all git repositories in ROOT_DIR.
# Exclude the .repo directory and the oniro device/board/soc/vendor trees
# (those are our own repos, not upstream repos being patched), and the
# kernel/linux/volla-* port repos — those are transient build clones owned
# by device/board/oniro/hybris_generic/kernel/*/build_kernel.sh, not
# OHOS-checkout repos that system_patch should manage.
if [[ "$MODE" == "list-dirty" ]]; then
    find "$ROOT_DIR" -name ".git" \( -type d -o -type l \) \
        ! -path "$ROOT_DIR/.repo/*" \
        ! -path "$ROOT_DIR/device/board/oniro/*" \
        ! -path "$ROOT_DIR/device/soc/oniro/*" \
        ! -path "$ROOT_DIR/vendor/oniro/*" \
        ! -path "$ROOT_DIR/kernel/linux/volla-*" \
        -exec bash -c 'list_dirty "$(dirname "{}")" "$ROOT_DIR"' \;
else
    find "$ROOT_DIR" -name ".git" \( -type d -o -type l \) \
        ! -path "$ROOT_DIR/.repo/*" \
        ! -path "$ROOT_DIR/device/board/oniro/*" \
        ! -path "$ROOT_DIR/device/soc/oniro/*" \
        ! -path "$ROOT_DIR/vendor/oniro/*" \
        ! -path "$ROOT_DIR/kernel/linux/volla-*" \
        -exec bash -c 'generate_patch "$(dirname "{}")" "$ROOT_DIR" "$PATCHES_DIR"' \;

    echo ""
    echo "All patch files are saved in the patches directory: $PATCHES_DIR"
fi
