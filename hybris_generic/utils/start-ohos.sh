#!/bin/bash

LOG_FILE="/home/phablet/openharmony/start-ohos.log"
ROOTFS="/home/phablet/openharmony/rootfs"
LXC_CONFIG="/var/lib/lxc/openharmony/config"
echo "Starting OpenHarmony deployment..." > $LOG_FILE

# ── Detect Halium version ────────────────────────────────────────────────
# Halium 13 has the @2.3 composer binary; Halium 12 has @2.1 only.
HALIUM_VERSION=12
if [ -f /vendor/bin/hw/android.hardware.graphics.composer@2.3-service ]; then
    HALIUM_VERSION=13
fi
echo "Detected Halium $HALIUM_VERSION" | tee -a $LOG_FILE

# ── Stop lightdm ─────────────────────────────────────────────────────────
if systemctl list-unit-files | grep -q lightdm.service; then
    if systemctl is-active --quiet lightdm; then
        echo "Lightdm is active. Stopping lightdm..." | tee -a $LOG_FILE
        systemctl stop lightdm
    else
        echo "Lightdm is not active." | tee -a $LOG_FILE
    fi
fi

# Stop other services that might conflict
echo "Stopping other potential conflicting services..." | tee -a $LOG_FILE
for svc in android-tools-adbd sensors-hal sensors-service sensors-hal-1-0; do
    if systemctl list-unit-files | grep -q ${svc}.service; then
        echo "Stopping $svc..." | tee -a $LOG_FILE
        systemctl stop $svc
    fi
done

# ── Build system/build.prop from Android partition props ─────────────────
# libhybris property cache reads /system/build.prop to resolve ro.hardware.egl
# and ro.board.platform. The android partitions are bind-mounted on the host.
echo "Generating system/build.prop from Android partition build props..." | tee -a $LOG_FILE
mkdir -p ${ROOTFS}/system
if [ -f /vendor/build.prop ] && [ -f /system/build.prop ]; then
    cat /vendor/build.prop /system/build.prop > ${ROOTFS}/system/build.prop
    echo "system/build.prop created from /vendor/build.prop + /system/build.prop." | tee -a $LOG_FILE
elif [ -f /vendor/build.prop ]; then
    cat /vendor/build.prop > ${ROOTFS}/system/build.prop
    echo "system/build.prop created from /vendor/build.prop only." | tee -a $LOG_FILE
else
    echo "WARNING: Android build.prop files not found; system/build.prop not updated." | tee -a $LOG_FILE
fi

# Fix non-standard ro.hardware.egl values (e.g., "meow" on Volla Tablet)
if grep -q 'ro.hardware.egl=meow' ${ROOTFS}/system/build.prop 2>/dev/null; then
    sed -i 's/ro.hardware.egl=meow/ro.hardware.egl=mali/' ${ROOTFS}/system/build.prop
    echo "  Fixed ro.hardware.egl=meow -> mali" | tee -a $LOG_FILE
fi

# ── Stop conflicting WiFi daemons ────────────────────────────────────────
# OHOS runs its own wpa_supplicant + wifi_hal_service; host and Android WiFi
# daemons must not compete for wlan0 / nl80211 control.
echo "Stopping conflicting WiFi daemons..." | tee -a $LOG_FILE

# Host wpa_supplicant (Ubuntu Touch NetworkManager-controlled)
if systemctl is-active --quiet wpa_supplicant 2>/dev/null; then
    echo "  Stopping and masking host wpa_supplicant..." | tee -a $LOG_FILE
    systemctl stop wpa_supplicant
    systemctl mask wpa_supplicant
fi
# Tell NetworkManager to release WiFi so it doesn't restart wpa_supplicant
if command -v nmcli >/dev/null 2>&1; then
    nmcli radio wifi off 2>/dev/null || true
fi

# Android WiFi daemons — use setprop ctl.stop so Android init doesn't respawn them
if /usr/bin/lxc-info -n android -s 2>/dev/null | grep -q "RUNNING"; then
    for daemon in wificond wlan_assistant; do
        echo "  Stopping android $daemon via ctl.stop..." | tee -a $LOG_FILE
        /usr/bin/lxc-attach -n android -- setprop ctl.stop $daemon 2>/dev/null || true
    done
fi
echo "WiFi daemons stopped." | tee -a $LOG_FILE

# ── Ensure android composer is running ───────────────────────────────────
# Auto-detect composer binary: prefer @2.3 (Halium 13), fall back to @2.1 (Halium 12)
if [ -f /vendor/bin/hw/android.hardware.graphics.composer@2.3-service ]; then
    COMPOSER_BIN="/vendor/bin/hw/android.hardware.graphics.composer@2.3-service"
elif [ -f /vendor/bin/hw/android.hardware.graphics.composer@2.1-service ]; then
    COMPOSER_BIN="/vendor/bin/hw/android.hardware.graphics.composer@2.1-service"
else
    COMPOSER_BIN=""
fi
echo "Checking android LXC container and composer service..." | tee -a $LOG_FILE
if /usr/bin/lxc-info -n android -s 2>/dev/null | grep -q "RUNNING"; then
    if ! /usr/bin/lxc-attach -n android -- pgrep -f "graphics.composer" > /dev/null 2>&1; then
        if [ -n "$COMPOSER_BIN" ]; then
            echo "Starting android composer service ($COMPOSER_BIN)..." | tee -a $LOG_FILE
            /usr/bin/lxc-attach -n android -- ${COMPOSER_BIN} &
            # Give the service time to register with hwservicemanager
            sleep 2
            echo "Android composer service started." | tee -a $LOG_FILE
        else
            echo "WARNING: No composer binary found and composer not running." | tee -a $LOG_FILE
        fi
    else
        echo "Android composer service already running." | tee -a $LOG_FILE
    fi
else
    echo "WARNING: android LXC container is not running. Composer service not started." | tee -a $LOG_FILE
    echo "         Run 'lxc-start -n android' first if display output is needed." | tee -a $LOG_FILE
fi

# ── Prevent systemd-logind from acting on power button ───────────────────
# The OHOS container handles power key events via multimodalinput; logind must not act on them.
echo "Configuring systemd-logind to ignore power key..." | tee -a $LOG_FILE
mkdir -p /run/systemd/logind.conf.d
printf "[Login]\nHandlePowerKey=ignore\nHandlePowerKeyLongPress=ignore\n" \
    > /run/systemd/logind.conf.d/ohos-container.conf
systemctl kill -s HUP systemd-logind
echo "logind power key handling disabled." | tee -a $LOG_FILE

# ── Start the OpenHarmony container ──────────────────────────────────────
echo "Starting OpenHarmony container..." | tee -a $LOG_FILE
/usr/bin/lxc-start -n openharmony -F 2>&1 | tee -a $LOG_FILE
