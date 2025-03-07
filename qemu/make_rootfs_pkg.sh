#!/usr/bin/env bash

# Parse arguments
while [[ "$#" -gt 0 ]]; do
    case $1 in
        --output) output="$2"; shift ;;
        --input) input="$2"; shift ;;
        *) echo "Unknown parameter passed: $1"; exit 1 ;;
    esac
    shift
done

# Ensure input and output are set
if [[ -z "$output" || -z "$input" ]]; then
    echo "Usage: $0 --output <output_path> --input <input_path>"
    exit 1
fi

# Check if input directories exist
if [[ ! -d "${input}/root" || ! -d "${input}/system" || ! -d "${input}/vendor" ]]; then
    echo "One or more input directories do not exist."
    exit 1
fi

# Execute the required commands
rm -rf ${input}/ohos-rootfs*
cp -r ${input}/root/ ohos-rootfs/
cp -r ${input}/system ohos-rootfs/
cp -r ${input}/vendor/ ohos-rootfs/
tar -cvf ${output} -C ohos-rootfs .
rm -rf ${input}/ohos-rootfs*