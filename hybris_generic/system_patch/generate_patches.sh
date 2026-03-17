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

# Function to check for uncommitted changes and generate patch files
generate_patch() {
    repo_path=$1
    root_dir=$2
    patches_base_dir=$3

    # Check if there are uncommitted changes
    if [[ -n $(git -C "$repo_path" status --porcelain) ]]; then
        # Get the relative path from ROOT_DIR to the repository
        rel_path=$(realpath --relative-to="$root_dir" "$repo_path")
        
        # Create subdirectory structure in patches directory
        patch_subdir="$patches_base_dir/$rel_path"
        mkdir -p "$patch_subdir"
        
        # Generate a unique patch filename based on the repo path
        repo_name=$(basename "$repo_path")
        patch_file="$patch_subdir/${repo_name}_$(date +%Y%m%d%H%M%S).patch"

        # Generate the patch file
        git -C "$repo_path" diff > "$patch_file"
        
        # Check if the patch file is empty and remove it if so
        if [[ ! -s "$patch_file" ]]; then
            rm "$patch_file"
            echo "No changes to generate patch for $repo_path (empty diff)"
        else
            echo "Patch file generated: $patch_file"
        fi
    else
        echo "No uncommitted changes in $repo_path"
    fi
}

# Export the function for use with find -exec
export -f generate_patch

# Export variables for use with find -exec
export ROOT_DIR
export PATCHES_DIR

# Find all git repositories in ROOT_DIR and generate patch files
find "$ROOT_DIR" -name ".git" \( -type d -o -type l \) -exec bash -c 'generate_patch "$(dirname "{}")" "$ROOT_DIR" "$PATCHES_DIR"' \;

echo "All patch files are saved in the patches directory: $PATCHES_DIR"

