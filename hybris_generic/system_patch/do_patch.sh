#!/bin/bash
# Copyright (C) 2025 Huawei Inc.
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
#
# Apply every .patch under ./patches/<repo_path>/ via `git am`, so each
# patch becomes a real commit in the target repository.
#
# Each repo's pre-patch and post-patch HEAD SHA is recorded in
# .patch_state so undo_patch.sh can verify nothing has changed since
# before it rolls anything back. Pair with undo_patch.sh.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")"; pwd)"
PATCH_DIR="$SCRIPT_DIR/patches"
SOURCE_ROOT_DIR="$(cd "$PATCH_DIR/../../../../../../"; pwd)"
STATE_FILE="$SCRIPT_DIR/.patch_state"

# Collect every directory that directly contains at least one .patch file,
# deduped and sorted so application order is deterministic.
mapfile -t repo_dirs < <(
    find "$PATCH_DIR" -type f -name "*.patch" -printf '%h\n' | sort -u
)

if [[ ${#repo_dirs[@]} -eq 0 ]]; then
    echo "No patches found under $PATCH_DIR"
    exit 0
fi

if [[ -s "$STATE_FILE" ]]; then
    echo "ERROR: $STATE_FILE already exists — patches appear to be applied." >&2
    echo "       Run undo_patch.sh first (or delete the file if it is stale)." >&2
    exit 1
fi

# Pre-flight: every target must be a clean git repo before we touch anything.
for dir in "${repo_dirs[@]}"; do
    repo_rel="$(realpath --relative-to="$PATCH_DIR" "$dir")"
    repo_path="$SOURCE_ROOT_DIR/$repo_rel"

    if [[ ! -d "$repo_path/.git" && ! -f "$repo_path/.git" ]]; then
        echo "ERROR: $repo_path is not a git repository" >&2
        exit 1
    fi
    if [[ -n "$(git -C "$repo_path" status --porcelain)" ]]; then
        echo "ERROR: $repo_rel has uncommitted changes — commit or stash them first" >&2
        exit 1
    fi
done

: > "$STATE_FILE"

for dir in "${repo_dirs[@]}"; do
    repo_rel="$(realpath --relative-to="$PATCH_DIR" "$dir")"
    repo_path="$SOURCE_ROOT_DIR/$repo_rel"

    mapfile -t patches < <(find "$dir" -maxdepth 1 -type f -name "*.patch" | sort)

    pre_sha="$(git -C "$repo_path" rev-parse HEAD)"
    echo "Applying ${#patches[@]} patch(es) in $repo_rel (base ${pre_sha:0:12})"
    for p in "${patches[@]}"; do
        echo "  git am $p"
        if ! git -C "$repo_path" am "$p"; then
            git -C "$repo_path" am --abort || true
            echo "ERROR: failed to apply $p in $repo_rel" >&2
            echo "       this repo was reset to ${pre_sha:0:12}; already-patched" >&2
            echo "       repos remain applied — run undo_patch.sh to roll them back." >&2
            exit 1
        fi
    done
    post_sha="$(git -C "$repo_path" rev-parse HEAD)"
    printf '%s\t%s\t%s\n' "$repo_rel" "$pre_sha" "$post_sha" >> "$STATE_FILE"
done

echo "Done. State recorded in $STATE_FILE"
