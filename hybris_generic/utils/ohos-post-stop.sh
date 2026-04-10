#!/bin/sh
#
# lxc.hook.post-stop for the openharmony container.
#
# OHOS init's reboot plugin writes "poweroff" or "reboot" to /ohos-host-action
# (a bind mount of /run/ohos-host-action on the host) before invoking the
# Linux reboot() syscall. Inside an LXC PID namespace the kernel turns that
# syscall into plain termination of container-init instead of an actual host
# power action, so we have to finish the job here on the host side.
#
# On clean shutdown-from-menu this script translates the flag into a
# systemctl poweroff / systemctl reboot. On any other stop path (manual
# lxc-stop, container crash, host shutdown tearing the container down) the
# flag file is empty and this script exits 0 so the host is not perturbed.
#
# The script always clears the flag after reading it so a subsequent container
# respawn cannot re-trigger a stale action.

FLAG=/run/ohos-host-action

[ -r "$FLAG" ] || exit 0

action=$(cat "$FLAG" 2>/dev/null)
: > "$FLAG" 2>/dev/null || true

case "$action" in
    poweroff)
        logger -t ohos-post-stop "OHOS requested poweroff, invoking systemctl poweroff"
        exec systemctl poweroff
        ;;
    reboot)
        logger -t ohos-post-stop "OHOS requested reboot, invoking systemctl reboot"
        exec systemctl reboot
        ;;
    "")
        # Empty flag — container stopped for a reason other than a user power action.
        ;;
    *)
        logger -t ohos-post-stop "Unknown action in $FLAG: '$action' (ignored)"
        ;;
esac

exit 0
