#!/bin/bash
#
# mocha-remote — remote kernel development tool for Xiaomi Mi Pad (mocha)
#
# Usage:
#   mocha-remote cmd "dmesg | tail -50"    — execute command on device
#   mocha-remote dmesg                     — shortcut for dmesg
#   mocha-remote flash boot.img            — flash boot.img to LNX partition + reboot
#   mocha-remote kexec boot.img            — kexec boot (no flash, like fastboot boot)
#   mocha-remote push local.file /tmp/     — upload file to device
#   mocha-remote shell                     — interactive shell (telnet)
#   mocha-remote wait                      — wait for device to come online
#
# Environment:
#   MOCHA_IP=192.168.10.77   (override device IP)
#   MOCHA_PORT=8080          (override HTTP port)
#   MOCHA_WORK_DIR=/sdcard/tmp (override temp dir on device)
#
# Requires: curl, telnet (for shell command)
# Device must be running remote-server.sh (httpd on port 8080)

set -euo pipefail

# --- Mocha hardware constants ---
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEVICE="${MOCHA_IP:-192.168.10.77}"
PORT="${MOCHA_PORT:-8080}"
TELNET_PORT=2323
BOOT_PARTITION="/dev/block/platform/sdhci-tegra.3/by-name/LNX"
KEXEC_MEM_MIN="0x85000000"
KEXEC_MEM_MAX="0xd0000000"
WORK_DIR="${MOCHA_WORK_DIR:-/sdcard/tmp}"

die() { echo "ERROR: $*" >&2; exit 1; }

# Build full URL for device HTTP endpoint
url() { echo "http://${DEVICE}:${PORT}$1"; }

# Execute command on device via /cgi-bin/cmd, return output
# Usage: remote_cmd "dmesg | tail -5"
remote_cmd() {
    local result
    result=$(curl -s --fail --connect-timeout 5 --max-time 300 -X POST -d "$*" "$(url /cgi-bin/cmd)") || return 1
    echo "$result"
}

# Send command without waiting for response (for reboot/kexec -e)
# Usage: remote_cmd_async "reboot"
remote_cmd_async() {
    curl -s --connect-timeout 5 --max-time 3 -X POST -d "$*" "$(url /cgi-bin/cmd)" 2>/dev/null || true
}

# Upload local file to device via /cgi-bin/upload
# Usage: remote_upload /local/path /remote/path
remote_upload() {
    local src="$1" dest="$2"
    local encoded_dest
    encoded_dest=$(printf '%s' "$dest" | sed 's/ /%20/g;s/!/%21/g;s/#/%23/g;s/&/%26/g;s/=/%3D/g')
    curl -s --fail -X POST --data-binary "@${src}" \
        "$(url /cgi-bin/upload?path=${encoded_dest})" || die "Upload failed"
}

# Poll device until HTTP server responds or timeout
# Usage: wait_for_device [timeout_seconds]
wait_for_device() {
    local timeout="${1:-60}"
    local elapsed=0
    echo -n "[*] Waiting for device at $DEVICE..." >&2
    while [ $elapsed -lt "$timeout" ]; do
        if curl -s --connect-timeout 2 -X POST -d "echo ok" "$(url /cgi-bin/cmd)" 2>/dev/null | grep -q ok; then
            echo " online (${elapsed}s)" >&2
            return 0
        fi
        sleep 3
        elapsed=$((elapsed + 3))
        echo -n "." >&2
    done
    echo " timeout" >&2
    return 1
}

# Execute arbitrary command on device
cmd_exec() {
    remote_cmd "$*"
}

# Show kernel log (dmesg). Optional: pass grep filter or dmesg flags
cmd_dmesg() {
    remote_cmd "dmesg ${*:---color=never}"
}

# Upload local file to device
# Usage: cmd_push <local-file> <remote-path>
# If remote-path ends with /, appends local filename
cmd_push() {
    [ $# -ge 2 ] || die "Usage: mocha-remote push <local-file> <remote-path>"
    local src="$1" dest="$2"
    [ -f "$src" ] || die "File not found: $src"
    [[ "$dest" == */ ]] && dest="${dest}$(basename "$src")"
    echo "[*] Uploading $(basename "$src") -> $dest"
    remote_upload "$src" "$dest"
}

# Flash boot.img to LNX partition + multirom trampoline inject + reboot
# Uses /cgi-bin/flash if available, otherwise fallback via upload + cmd
# Usage: cmd_flash <boot.img> [--no-inject]
cmd_flash() {
    [ $# -ge 1 ] || die "Usage: mocha-remote flash <boot.img> [--no-inject]"
    local img="$1"
    local no_inject=0
    [ "${2:-}" = "--no-inject" ] && no_inject=1
    [ -f "$img" ] || die "File not found: $img"
    local size
    size=$(stat -f%z "$img" 2>/dev/null || stat -c%s "$img" 2>/dev/null)

    ensure_tools

    local has_cgi
    has_cgi=$(remote_cmd "[ -f /sbin/cgi-bin/flash ] && echo yes || echo no")

    local result
    if [ "$has_cgi" = "yes" ]; then
        echo "[*] Uploading + flashing $(basename "$img") ($size bytes)..."
        result=$(curl -s --fail --max-time 300 -X POST --data-binary "@${img}" \
            "$(url /cgi-bin/flash?part=LNX)") || die "Flash failed"
    else
        echo "[*] CGI not available, using fallback..."
        echo "[*] Uploading $(basename "$img") ($size bytes)..."
        remote_upload "$img" "${WORK_DIR}/boot.img" || die "Upload failed"
        remote_upload "${SCRIPT_DIR}/overlay/sbin/cgi-bin/flash" "${WORK_DIR}/flash.sh" || die "Upload flash script failed"
        result=$(remote_cmd "cat ${WORK_DIR}/boot.img | QUERY_STRING=part=LNX REQUEST_METHOD=POST CONTENT_LENGTH=\$(/sbin/busybox stat -c%s ${WORK_DIR}/boot.img) /system/bin/sh ${WORK_DIR}/flash.sh 2>&1")
        remote_cmd "rm -f ${WORK_DIR}/flash.sh ${WORK_DIR}/boot.img"
    fi

    echo "$result"
    if echo "$result" | grep -q "INJECT_FAILED\|FLASH_FAILED"; then
        die "Flash failed: check output above"
    fi
    echo "$result" | grep -q "FLASH_OK" || die "Flash failed: no FLASH_OK in output"

    echo "[*] Rebooting..."
    remote_cmd_async "reboot"
    echo "[+] Done. Device is rebooting."
}

# Ensure micropython + rebuild_boot.py are on device (for trampoline inject)
# Installs to /system/bin/ if not found in ramdisk
ensure_tools() {
    remote_cmd "mkdir -p ${WORK_DIR}"
    local has_flash
    has_flash=$(remote_cmd "[ -f /sbin/cgi-bin/flash ] && echo yes || echo no")
    if [ "$has_flash" != "yes" ]; then
        echo "[*] CGI scripts not found, installing tools..."
        remote_cmd "mount -o remount,rw /system"
        remote_upload "${SCRIPT_DIR}/bin/micropython-armv7l" "/system/bin/micropython"
        remote_upload "${SCRIPT_DIR}/overlay/sbin/rebuild_boot.py" "/system/bin/rebuild_boot.py"
        remote_cmd "chmod 755 /system/bin/micropython /system/bin/rebuild_boot.py"
    fi
}

# Kexec-hardboot: load kernel from boot.img and reboot into it (no flash)
# boot.img: sends to /cgi-bin/kexec which extracts and loads all components
# zImage: uploads components separately, extracts ramdisk from LNX
# Usage: cmd_kexec <boot.img|zImage> [dtb]
cmd_kexec() {
    [ $# -ge 1 ] || die "Usage: mocha-remote kexec <boot.img|zImage> [dtb]"
    local img="$1"
    local dtb_arg="${2:-}"
    [ -f "$img" ] || die "File not found: $img"
    local size
    size=$(stat -f%z "$img" 2>/dev/null || stat -c%s "$img" 2>/dev/null)

    # Detect if input is boot.img
    local magic
    magic=$(dd if="$img" bs=8 count=1 2>/dev/null)
    if [[ "$magic" == *"ANDROID"* ]] && [ -z "$dtb_arg" ]; then
        local has_cgi
        has_cgi=$(remote_cmd "[ -f /sbin/cgi-bin/kexec ] && echo yes || echo no")

        if [ "$has_cgi" = "yes" ]; then
            echo "[*] Uploading boot.img ($size bytes) to kexec CGI..."
            local result
            result=$(curl -s --fail --max-time 60 -X POST --data-binary "@${img}" \
                "$(url /cgi-bin/kexec)") || die "Kexec failed"
            echo "$result"
        else
            echo "[*] CGI not available, using fallback..."
            remote_cmd "mkdir -p ${WORK_DIR}"
            remote_upload "$img" "${WORK_DIR}/boot.img"
            remote_upload "${SCRIPT_DIR}/overlay/sbin/cgi-bin/kexec" "${WORK_DIR}/kexec.sh"
            remote_cmd "/system/bin/sh ${WORK_DIR}/kexec.sh < ${WORK_DIR}/boot.img 2>&1"
            remote_cmd "rm -f ${WORK_DIR}/kexec.sh ${WORK_DIR}/boot.img"
        fi
        echo "[+] Kexec sent. Device is rebooting."
        return
    fi

    # Raw zImage path — upload components separately
    echo "[*] Using raw zImage"
    local zimage_file="$img"

    echo "[*] Uploading zImage ($size bytes)..."
    remote_upload "$zimage_file" "${WORK_DIR}/kexec_zImage"

    # Ramdisk from LNX
    echo "[*] Extracting ramdisk from LNX partition on device..."
    remote_cmd 'BB=/sbin/busybox;LNX='"${BOOT_PARTITION}"';D='"${WORK_DIR}"';dd if=$LNX of=$D/hdr bs=2048 count=1 2>/dev/null;KS=$($BB od -A n -t u4 -j 8 -N 4 $D/hdr|$BB tr -d " ");RS=$($BB od -A n -t u4 -j 16 -N 4 $D/hdr|$BB tr -d " ");PS=$($BB od -A n -t u4 -j 36 -N 4 $D/hdr|$BB tr -d " ");KP=$(((KS+PS-1)/PS*PS));RO=$((PS+KP));dd if=$LNX bs=$PS skip=$((RO/PS)) count=$(((RS+PS-1)/PS)) 2>/dev/null|$BB head -c $RS >$D/kexec_ramdisk;rm $D/hdr;echo "ramdisk: $RS bytes"'

    # DTB
    local dtb_flag=""
    if [ -n "$dtb_arg" ]; then
        [ -f "$dtb_arg" ] || die "DTB not found: $dtb_arg"
        echo "[*] Uploading DTB..."
        remote_upload "$dtb_arg" "${WORK_DIR}/kexec_dtb"
        dtb_flag="--dtb=${WORK_DIR}/kexec_dtb"
    fi

    # cmdline
    local cmdline
    cmdline=$(remote_cmd "cat /proc/cmdline")

    # Load + exec
    local kexec_cmd="kexec --load-hardboot --mem-min=${KEXEC_MEM_MIN} --mem-max=${KEXEC_MEM_MAX}"
    kexec_cmd+=" --boardname=mocha --initrd=${WORK_DIR}/kexec_ramdisk"
    [ -n "$dtb_flag" ] && kexec_cmd+=" ${dtb_flag}"
    [ -n "$cmdline" ] && kexec_cmd+=" --append=\"${cmdline}\""
    kexec_cmd+=" ${WORK_DIR}/kexec_zImage"

    echo "[*] Loading kexec..."
    remote_cmd "$kexec_cmd 2>&1"

    echo "[*] Executing kexec..."
    remote_cmd_async "kexec -e"
    echo "[+] Kexec sent. Device is rebooting."
}

# Interactive telnet shell to device
cmd_shell() {
    echo "[*] Connecting to $DEVICE:$TELNET_PORT..."
    telnet "$DEVICE" "$TELNET_PORT"
}

# Wait for device to come online
# Usage: cmd_wait [timeout_seconds]
cmd_wait() {
    wait_for_device "${1:-60}"
}

# --- Main ---
[ $# -ge 1 ] || {
    cat >&2 <<USAGE
mocha-remote — remote kernel dev tool for Xiaomi Mi Pad

Usage: mocha-remote <command> [args...]

Commands:
  cmd <command>       Execute shell command on device
  dmesg [flags]       Show kernel log
  push <file> <path>  Upload file to device
  flash <boot.img>    Flash boot.img to LNX + reboot
  kexec <boot.img>    Kexec boot (like fastboot boot, no flash)
  shell               Interactive telnet shell
  wait [timeout]      Wait for device to come online

Device: $DEVICE:$PORT (set MOCHA_IP / MOCHA_PORT to override)
USAGE
    exit 1
}

CMD="$1"; shift
case "$CMD" in
    cmd)    cmd_exec "$@" ;;
    dmesg)  cmd_dmesg "$@" ;;
    push)   cmd_push "$@" ;;
    flash)  cmd_flash "$@" ;;
    kexec)  cmd_kexec "$@" ;;
    shell)  cmd_shell ;;
    wait)   cmd_wait "$@" ;;
    *)      die "Unknown command: $CMD" ;;
esac
