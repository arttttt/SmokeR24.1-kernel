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

set -euo pipefail

# --- Mocha hardware constants ---
DEVICE="${MOCHA_IP:-192.168.10.77}"
PORT="${MOCHA_PORT:-8080}"
TELNET_PORT=2323
BOOT_PARTITION="/dev/block/platform/sdhci-tegra.3/by-name/LNX"
KEXEC_MEM_MIN="0x85000000"
KEXEC_MEM_MAX="0xd0000000"
UPLOAD_DIR="/data/local/tmp"

die() { echo "ERROR: $*" >&2; exit 1; }

url() { echo "http://${DEVICE}:${PORT}$1"; }

remote_cmd() {
    local result
    result=$(curl -s --fail --connect-timeout 5 --max-time 300 -X POST -d "$*" "$(url /cgi-bin/cmd)") || return 1
    echo "$result"
}

# Fire-and-forget: send command, don't wait for response
remote_cmd_async() {
    curl -s --connect-timeout 5 --max-time 3 -X POST -d "$*" "$(url /cgi-bin/cmd)" 2>/dev/null || true
}

remote_upload() {
    local src="$1" dest="$2"
    local encoded_dest
    encoded_dest=$(python3 -c "import urllib.parse; print(urllib.parse.quote('$dest'))")
    curl -s --fail -X POST --data-binary "@${src}" \
        "$(url /cgi-bin/upload?path=${encoded_dest})" || die "Upload failed"
}

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

cmd_exec() {
    remote_cmd "$*"
}

cmd_dmesg() {
    remote_cmd "dmesg ${*:---color=never}"
}

cmd_push() {
    [ $# -ge 2 ] || die "Usage: mocha-remote push <local-file> <remote-path>"
    local src="$1" dest="$2"
    [ -f "$src" ] || die "File not found: $src"
    [[ "$dest" == */ ]] && dest="${dest}$(basename "$src")"
    echo "[*] Uploading $(basename "$src") -> $dest"
    remote_upload "$src" "$dest"
}

cmd_flash() {
    [ $# -ge 1 ] || die "Usage: mocha-remote flash <boot.img> [--no-inject]"
    local img="$1"
    local no_inject=0
    [ "${2:-}" = "--no-inject" ] && no_inject=1
    [ -f "$img" ] || die "File not found: $img"
    local size
    size=$(stat -f%z "$img" 2>/dev/null || stat -c%s "$img" 2>/dev/null)
    local remote_path="${UPLOAD_DIR}/boot_flash.img"

    echo "[*] Uploading $(basename "$img") ($size bytes)..."
    remote_upload "$img" "$remote_path"

    echo "[*] Flashing to $BOOT_PARTITION..."
    remote_cmd "dd if=$remote_path of=$BOOT_PARTITION bs=4096 && sync"

    # Inject multirom trampoline if multirom is installed
    if [ "$no_inject" -eq 0 ]; then
        echo "[*] Checking for MultiROM..."
        local has_mrom
        has_mrom=$(remote_cmd "[ -f /data/media/0/multirom/trampoline ] && echo yes || echo no")
        if [ "$has_mrom" = "yes" ]; then
            echo "[*] Injecting MultiROM trampoline..."
            inject_multirom_trampoline
        fi
    fi

    echo "[*] Rebooting..."
    remote_cmd_async "reboot"
    echo "[+] Done. Device is rebooting."
}

inject_multirom_trampoline() {
    remote_cmd 'BB=/sbin/busybox;TMP=/data/local/tmp/mrom_inject;MROM=/data/media/0/multirom;PART='"${BOOT_PARTITION}"';rm -rf $TMP;mkdir -p $TMP/rd;dd if=$PART of=$TMP/boot.img bs=4096 2>/dev/null;KS=$($BB od -A n -t u4 -j 8 -N 4 $TMP/boot.img|$BB tr -d " ");RS=$($BB od -A n -t u4 -j 16 -N 4 $TMP/boot.img|$BB tr -d " ");PS=$($BB od -A n -t u4 -j 36 -N 4 $TMP/boot.img|$BB tr -d " ");KP=$(((KS+PS-1)/PS*PS));RO=$((PS+KP));dd if=$TMP/boot.img bs=1 skip=$RO count=$RS of=$TMP/ramdisk.gz 2>/dev/null;cd $TMP/rd;$BB gzip -d -c $TMP/ramdisk.gz|$BB cpio -i 2>/dev/null;[ ! -f main_init ]&&mv init main_init;cp $MROM/trampoline init;chmod 750 init;rm -f sbin/ueventd sbin/watchdogd;$BB ln -s ../main_init sbin/ueventd;$BB ln -s ../main_init sbin/watchdogd;[ -f $MROM/mrom.fstab ]&&cp $MROM/mrom.fstab .;$BB find .|$BB cpio -o -H newc 2>/dev/null|$BB gzip >$TMP/ramdisk_new.gz;TR_VER=$($MROM/trampoline -v 2>/dev/null||echo 27);cd $TMP;python /sbin/rebuild_boot.py $TMP/boot.img $TMP/ramdisk_new.gz $TR_VER $TMP/boot_injected.img;dd if=$TMP/boot_injected.img of=$PART bs=4096 2>/dev/null&&sync;rm -rf $TMP;echo INJECT_OK'
}

cmd_kexec() {
    [ $# -ge 1 ] || die "Usage: mocha-remote kexec <boot.img|zImage> [dtb]"
    local img="$1"
    local dtb_arg="${2:-}"
    [ -f "$img" ] || die "File not found: $img"

    local zimage_file="" dtb_file="" ramdisk_file="" cleanup=""

    # Detect if input is boot.img or raw zImage
    local magic
    magic=$(head -c 8 "$img" | cat -v)
    if [[ "$magic" == *"ANDROID"* ]]; then
        echo "[*] Detected boot.img, extracting zImage + ramdisk + DTB..."
        local tmpdir
        tmpdir=$(mktemp -d /tmp/mocha_kexec.XXXXXX)
        cleanup="$tmpdir"
        python3 -c "
import struct, sys
with open('$img', 'rb') as f:
    d = f.read()
ks = struct.unpack_from('<I', d, 8)[0]
rs = struct.unpack_from('<I', d, 16)[0]
ss = struct.unpack_from('<I', d, 24)[0]
ps = struct.unpack_from('<I', d, 36)[0]
ds = struct.unpack_from('<I', d, 40)[0]
def pages(sz): return ((sz + ps - 1) // ps) * ps
k_off = ps
r_off = k_off + pages(ks)
s_off = r_off + pages(rs)
d_off = s_off + pages(ss)
with open('$tmpdir/zImage', 'wb') as f: f.write(d[k_off:k_off+ks])
if rs > 0:
    with open('$tmpdir/ramdisk.gz', 'wb') as f: f.write(d[r_off:r_off+rs])
if ds > 0:
    with open('$tmpdir/dt.img', 'wb') as f: f.write(d[d_off:d_off+ds])
print(f'zImage: {ks} bytes, ramdisk: {rs} bytes, DTB: {ds} bytes')
"
        zimage_file="$tmpdir/zImage"
        [ -f "$tmpdir/ramdisk.gz" ] && ramdisk_file="$tmpdir/ramdisk.gz"
        [ -f "$tmpdir/dt.img" ] && dtb_file="$tmpdir/dt.img"
    else
        echo "[*] Using raw zImage"
        zimage_file="$img"
    fi

    # Override DTB if explicitly provided
    if [ -n "$dtb_arg" ]; then
        [ -f "$dtb_arg" ] || die "DTB not found: $dtb_arg"
        dtb_file="$dtb_arg"
        echo "[*] Using explicit DTB: $dtb_arg"
    fi

    # Upload zImage
    echo "[*] Uploading zImage ($(stat -f%z "$zimage_file" 2>/dev/null || stat -c%s "$zimage_file") bytes)..."
    remote_upload "$zimage_file" "${UPLOAD_DIR}/kexec_zImage"

    # Ramdisk: upload from boot.img, or extract from LNX partition on device
    local initrd_flag=""
    if [ -n "$ramdisk_file" ]; then
        echo "[*] Uploading ramdisk ($(stat -f%z "$ramdisk_file" 2>/dev/null || stat -c%s "$ramdisk_file") bytes)..."
        remote_upload "$ramdisk_file" "${UPLOAD_DIR}/kexec_ramdisk"
        initrd_flag="--initrd=${UPLOAD_DIR}/kexec_ramdisk"
    else
        echo "[*] Extracting ramdisk from LNX partition on device..."
        remote_cmd 'LNX='"${BOOT_PARTITION}"' D='"${UPLOAD_DIR}"' && dd if=$LNX of=$D/lnx.img bs=4096 2>/dev/null && KS=$(busybox od -A n -t u4 -j 8 -N 4 $D/lnx.img | busybox tr -d " ") && RS=$(busybox od -A n -t u4 -j 16 -N 4 $D/lnx.img | busybox tr -d " ") && PS=$(busybox od -A n -t u4 -j 36 -N 4 $D/lnx.img | busybox tr -d " ") && KP=$(( (KS + PS - 1) / PS * PS )) && RO=$(( PS + KP )) && dd if=$D/lnx.img of=$D/kexec_ramdisk bs=1 skip=$RO count=$RS 2>/dev/null && rm -f $D/lnx.img && echo "ramdisk: $RS bytes from LNX"'
        initrd_flag="--initrd=${UPLOAD_DIR}/kexec_ramdisk"
    fi

    # Upload DTB if available
    local dtb_flag=""
    if [ -n "$dtb_file" ]; then
        echo "[*] Uploading DTB ($(stat -f%z "$dtb_file" 2>/dev/null || stat -c%s "$dtb_file") bytes)..."
        remote_upload "$dtb_file" "${UPLOAD_DIR}/kexec_dtb"
        dtb_flag="--dtb=${UPLOAD_DIR}/kexec_dtb"
    fi

    [ -n "$cleanup" ] && rm -rf "$cleanup"

    # Grab current cmdline from device (bootloader passes critical params)
    echo "[*] Reading device cmdline..."
    local cmdline
    cmdline=$(remote_cmd "cat /proc/cmdline")
    echo "[*] cmdline: ${cmdline:0:80}..."

    # Build kexec command
    local kexec_cmd="kexec --load-hardboot --mem-min=${KEXEC_MEM_MIN} --mem-max=${KEXEC_MEM_MAX}"
    kexec_cmd+=" --boardname=mocha"
    [ -n "$initrd_flag" ] && kexec_cmd+=" ${initrd_flag}"
    [ -n "$dtb_flag" ] && kexec_cmd+=" ${dtb_flag}"
    [ -n "$cmdline" ] && kexec_cmd+=" --append=\"${cmdline}\""
    kexec_cmd+=" ${UPLOAD_DIR}/kexec_zImage"

    echo "[*] Loading kernel via kexec-hardboot..."
    remote_cmd "$kexec_cmd 2>&1"

    echo "[*] Executing kexec..."
    remote_cmd_async "kexec -e"

    echo "[*] Waiting for device to come back..."
    sleep 5
    if wait_for_device 60; then
        echo "[+] Kexec boot successful."
    else
        echo "[!] Device did not come back. Check device screen."
    fi
}

cmd_shell() {
    echo "[*] Connecting to $DEVICE:$TELNET_PORT..."
    telnet "$DEVICE" "$TELNET_PORT"
}

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
