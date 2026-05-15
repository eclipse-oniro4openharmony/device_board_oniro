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
# Roll back the commits created by do_patch.sh, driven by the
# .patch_state file it wrote (repo<TAB>pre_sha<TAB>post_sha per line).
#
# For each repo this verifies, BEFORE touching anything, that:
#   * the working tree is clean (reset --hard would discard dirty work);
#   * HEAD still equals the recorded post-patch SHA (i.e. nothing was
#     committed or amended on top since do_patch.sh ran).
# If any repo has diverged the script aborts without changing a thing.
# When all checks pass it resets each repo to its recorded pre-patch SHA.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")"; pwd)"
PATCH_DIR="$SCRIPT_DIR/patches"
SOURCE_ROOT_DIR="$(cd "$PATCH_DIR/../../../../../../"; pwd)"
STATE_FILE="$SCRIPT_DIR/.patch_state"

if [[ ! -s "$STATE_FILE" ]]; then
    echo "ERROR: no $STATE_FILE — do_patch.sh has not been run (nothing to undo)" >&2
    exit 1
fi

# Pass 1 — verify every repo; mutate nothing.
errors=0
while IFS=$'\t' read -r repo_rel pre_sha post_sha; do
    [[ -z "$repo_rel" ]] && continue
    repo_path="$SOURCE_ROOT_DIR/$repo_rel"

    if [[ ! -d "$repo_path/.git" && ! -f "$repo_path/.git" ]]; then
        echo "ERROR: $repo_path is not a git repository" >&2
        errors=1; continue
    fi
    if [[ -n "$(git -C "$repo_path" status --porcelain)" ]]; then
        echo "ERROR: $repo_rel has uncommitted changes — reset --hard would discard them" >&2
        errors=1; continue
    fi
    cur="$(git -C "$repo_path" rev-parse HEAD)"
    if [[ "$cur" != "$post_sha" ]]; then
        echo "ERROR: $repo_rel HEAD has moved since do_patch.sh ran" >&2
        echo "       expected ${post_sha:0:12}, found ${cur:0:12}" >&2
        echo "       (new commits or an amend on top — resolve manually)" >&2
        errors=1; continue
    fi
done < "$STATE_FILE"

if [[ "$errors" -ne 0 ]]; then
    echo "Aborting — no repositories were changed." >&2
    exit 1
fi

# Pass 2 — all clear, roll back to each recorded pre-patch SHA.
while IFS=$'\t' read -r repo_rel pre_sha post_sha; do
    [[ -z "$repo_rel" ]] && continue
    repo_path="$SOURCE_ROOT_DIR/$repo_rel"
    echo "Reverting $repo_rel to ${pre_sha:0:12}"
    git -C "$repo_path" reset --hard "$pre_sha"
done < "$STATE_FILE"

rm -f "$STATE_FILE"
echo "Done. $STATE_FILE removed."
