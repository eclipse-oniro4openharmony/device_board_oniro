#!/bin/bash

# Function to check if lightdm service is active
check_lightdm() {
    systemctl is-active --quiet lightdm
}

# Wait until lightdm service is running
until check_lightdm; do
    echo "Waiting for lightdm to start..."
    sleep 1
done

# Lightdm is running, wait for 1 second
echo "Lightdm is active. Waiting for 1 second..."
sleep 1

# Stop lightdm
echo "Stopping lightdm..."
systemctl stop lightdm

# Set property to stop vendor.hwcomposer-2-3
echo "Stopping vendor.hwcomposer-2-3..."
setprop ctl.stop vendor.hwcomposer-2-3

# Start the OpenHarmony container
echo "Starting OpenHarmony container..."
exec /usr/bin/lxc-start -n openharmony -F

