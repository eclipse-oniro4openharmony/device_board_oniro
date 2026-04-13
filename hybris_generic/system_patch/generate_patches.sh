#!/bin/bash

# Get the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Set the root directory (5 levels up from script directory)
ROOT_DIR="$SCRIPT_DIR/../../../../../"
ROOT_DIR="$(cd "$ROOT_DIR" && pwd)"

# Create the patches directory if it doesn't exist (in same directory as script)
PATCHES_DIR="$SCRIPT_DIR/patches"
mkdir -p "$PATCHES_DIR"

echo "root directory: $ROOT_DIR"
echo "patches directory: $PATCHES_DIR"

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

# Export the function for use with find -exec
export -f generate_patch

# Export variables for use with find -exec
export ROOT_DIR
export PATCHES_DIR

# Find all git repositories in ROOT_DIR and generate patch files.
# Exclude the .repo directory and the oniro device/board/soc/vendor trees
# (those are our own repos, not upstream repos being patched).
find "$ROOT_DIR" -name ".git" \( -type d -o -type l \) \
    ! -path "$ROOT_DIR/.repo/*" \
    ! -path "$ROOT_DIR/device/board/oniro/*" \
    ! -path "$ROOT_DIR/device/soc/oniro/*" \
    ! -path "$ROOT_DIR/vendor/oniro/*" \
    -exec bash -c 'generate_patch "$(dirname "{}")" "$ROOT_DIR" "$PATCHES_DIR"' \;

echo ""
echo "All patch files are saved in the patches directory: $PATCHES_DIR"
