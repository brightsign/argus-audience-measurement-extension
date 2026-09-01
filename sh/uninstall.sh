#!/bin/bash
#
# Robust uninstaller for the BrightSign NPU Argus extension.
#
# With no arguments, handles both the current extension name (npu_argus)
# and the legacy name (npu_gaze) that some older installs used before the
# rename, so it is safe to run regardless of which version is on the
# device. Also closes the dm-verity "verified" mapping (if present) before
# lvremove, which is required or lvremove fails with
# "Logical volume ... is in use" and the extension is left stuck.
#
# Usage: uninstall.sh [extension-name ...]
#   If one or more names are given, ONLY those names are uninstalled
#   (the npu_argus/npu_gaze defaults are not touched). This lets the
#   script be reused for any BrightSign extension, e.g.:
#     uninstall.sh vidireports

set +e

if [ "$(id -u)" -ne 0 ]; then
    echo "Error: this script must be run as root" 1>&2
    exit 1
fi

# Names to try: explicit args if given, otherwise current + legacy defaults.
if [ "$#" -gt 0 ]; then
    names="$*"
else
    names="npu_argus npu_gaze"
fi


any_found=0

for name in ${names}; do
    mount_dir="/var/volatile/bsext/ext_${name}"
    lv_bsext="/dev/mapper/bsext_${name}"
    lv_bsos="/dev/mapper/bsos-ext_${name}"
    lv_bsos_verified="/dev/mapper/bsos-ext_${name}-verified"

    found=0
    [ -d "${mount_dir}" ] && found=1
    [ -b "${lv_bsext}" ] && found=1
    [ -b "${lv_bsos}" ] && found=1
    [ -b "${lv_bsos_verified}" ] && found=1

    if [ "${found}" -eq 0 ]; then
        continue
    fi
    any_found=1

    echo "=== Uninstalling extension '${name}' ==="

    # Stop the extension if its init script is present.
    if [ -x "${mount_dir}/bsext_init" ]; then
        echo "Stopping extension..."
        "${mount_dir}/bsext_init" stop
    fi
    # Belt-and-braces: kill anything still referencing the mount point.
    pkill -9 -f "ext_${name}/" 2>/dev/null || true

    # Close the dm-verity verified mapping first -- lvremove will fail
    # with the LV "in use" until this is removed.
    if [ -b "${lv_bsos_verified}" ]; then
        echo "Closing dm-verity mapping bsos-ext_${name}-verified..."
        veritysetup close "bsos-ext_${name}-verified"
    fi

    # Unmount the extension, retrying lazily if something still has it busy.
    if mountpoint -q "${mount_dir}" 2>/dev/null; then
        echo "Unmounting ${mount_dir}..."
        if ! umount "${mount_dir}" 2>/dev/null; then
            echo "  Busy, retrying with fuser -k + lazy umount..."
            fuser -k "${mount_dir}" 2>/dev/null
            sleep 1
            umount -l "${mount_dir}" 2>/dev/null
        fi
    fi
    if [ -d "${mount_dir}" ] && ! mountpoint -q "${mount_dir}" 2>/dev/null; then
        rmdir "${mount_dir}" 2>/dev/null || rm -rf "${mount_dir}"
    fi

    # Remove the LVM volume (try every naming convention we've used).
    for lv in "${lv_bsext}" "${lv_bsos}"; do
        if [ -b "${lv}" ]; then
            echo "Removing logical volume ${lv}..."
            lvremove --yes "${lv}"
        fi
    done

    # Clean up any stale device-mapper nodes left behind.
    rm -f "${lv_bsext}" "${lv_bsos}" "${lv_bsos_verified}"

    echo "=== Done with '${name}' ==="
    echo
done

if [ "${any_found}" -eq 0 ]; then
    echo "No matching extension (${names}) found on this device -- nothing to do."
    echo "Check 'mount | grep bsext' and 'lvs' to see what's actually installed."
fi

sync
echo "Uninstall complete. A reboot is recommended to fully clear device-mapper state."
# reboot