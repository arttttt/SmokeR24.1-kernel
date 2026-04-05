#!/bin/bash
#
# Patch ramdisk to add remote debugging server over WiFi.
# Usage: ./patch_ramdisk_telnetd.sh <ramdisk.img|ramdisk.gz> <busybox-armv7l> [kexec-binary]
#
# Adds:
#   /sbin/busybox           — static ARM busybox binary
#   /sbin/kexec             — kexec binary (optional, from multirom)
#   /sbin/remote-server.sh  — HTTP server + CGI setup script
#   /sbin/cgi-bin/cmd       — execute commands via HTTP
#   /sbin/cgi-bin/upload    — upload files via HTTP
#   init.remote.rc          — service definitions (telnetd + httpd)
#
# Services:
#   telnetd on port 2323    — interactive shell
#   httpd   on port 8080    — HTTP API for commands, uploads, kexec, flash
#
# Use with: tools/remote/mocha-remote.sh (client-side)

set -euo pipefail

RAMDISK="$1"
BUSYBOX="$2"
KEXEC="${3:-}"

if [ ! -f "$RAMDISK" ] || [ ! -f "$BUSYBOX" ]; then
    echo "Usage: $0 <ramdisk.img|gz> <busybox-armv7l> [kexec-binary]"
    exit 1
fi

WORKDIR=$(mktemp -d)
BACKUP="${RAMDISK}.bak.$(date +%s)"

echo "[*] Backing up $RAMDISK -> $BACKUP"
cp "$RAMDISK" "$BACKUP"

echo "[*] Extracting ramdisk..."
cd "$WORKDIR"
gzip -dc "$RAMDISK" | cpio -id 2>/dev/null

echo "[*] Adding busybox to /sbin/busybox"
cp "$BUSYBOX" sbin/busybox
chmod 755 sbin/busybox

if [ -n "$KEXEC" ] && [ -f "$KEXEC" ]; then
    echo "[*] Adding kexec to /sbin/kexec"
    cp "$KEXEC" sbin/kexec
    chmod 755 sbin/kexec
fi

echo "[*] Creating CGI scripts"
mkdir -p sbin/cgi-bin

cat > sbin/cgi-bin/cmd << 'CGI'
#!/system/bin/sh
echo "Content-Type: text/plain"
echo ""
if [ "$REQUEST_METHOD" = "POST" ]; then
    read -n "$CONTENT_LENGTH" CMD
else
    CMD=$(echo "$QUERY_STRING" | /sbin/busybox sed 's/cmd=//;s/+/ /g;s/%20/ /g;s/%2F/\//g;s/%7C/|/g;s/%26/\&/g;s/%3B/;/g;s/%3D/=/g;s/%2D/-/g;s/%22/"/g;s/%27/'"'"'/g;s/%0A/\n/g')
fi
[ -z "$CMD" ] && { echo "Usage: POST command or GET ?cmd=command"; exit 0; }
eval "$CMD" 2>&1
CGI

cat > sbin/cgi-bin/upload << 'CGI'
#!/system/bin/sh
echo "Content-Type: text/plain"
echo ""
DEST=$(echo "$QUERY_STRING" | /sbin/busybox sed 's/path=//;s/%2F/\//g;s/%20/ /g')
[ -z "$DEST" ] && { echo "ERROR: specify ?path=/tmp/boot.img"; exit 1; }
/sbin/busybox cat > "$DEST"
SIZE=$(/sbin/busybox ls -l "$DEST" | /sbin/busybox awk '{print $5}')
echo "OK: $DEST ($SIZE bytes)"
CGI

chmod 755 sbin/cgi-bin/cmd sbin/cgi-bin/upload

echo "[*] Creating remote-server.sh"
cat > sbin/remote-server.sh << 'SCRIPT'
#!/system/bin/sh
# Setup and start HTTP server for remote access
export PATH="/sbin:/system/bin:/system/xbin:$PATH"

# Create tmpfs workspace
mkdir -p /tmp 2>/dev/null
mount -t tmpfs tmpfs /tmp -o size=64m 2>/dev/null

# Start httpd with CGI
exec /sbin/busybox httpd -f -p 8080 -h /sbin -c /dev/null
SCRIPT
chmod 755 sbin/remote-server.sh

echo "[*] Creating init.remote.rc"
cat > init.remote.rc << 'INITRC'
# Remote debugging services over WiFi
# telnet: interactive shell on port 2323
# httpd:  HTTP API on port 8080 (used by mocha-remote.sh)

service telnetd /sbin/busybox telnetd -l /system/bin/sh -b 0.0.0.0 -p 2323 -F
    class late_start
    disabled
    oneshot
    seclabel u:r:su:s0

service remotehttpd /sbin/remote-server.sh
    class late_start
    disabled
    oneshot
    seclabel u:r:su:s0

on property:sys.boot_completed=1
    start telnetd
    start remotehttpd
INITRC

# Remove old init.telnetd.rc if present
rm -f init.telnetd.rc

# Find the right init rc to add import (tn8 for SmokeR24.1, mocha for Stock)
INIT_RC=""
if [ -f init.tn8.rc ]; then
    INIT_RC=init.tn8.rc
elif [ -f init.mocha.rc ]; then
    INIT_RC=init.mocha.rc
elif [ -f init.rc ]; then
    INIT_RC=init.rc
fi

if [ -n "$INIT_RC" ]; then
    # Remove old telnetd import if present
    sed -i.tmp '/init.telnetd.rc/d' "$INIT_RC"
    rm -f "${INIT_RC}.tmp"

    if ! grep -q 'init.remote.rc' "$INIT_RC"; then
        echo "[*] Adding import to $INIT_RC"
        LAST_IMPORT=$(grep -n '^import' "$INIT_RC" | tail -1 | cut -d: -f1)
        if [ -n "$LAST_IMPORT" ]; then
            sed -i.tmp "${LAST_IMPORT}a\\
import init.remote.rc" "$INIT_RC"
        else
            sed -i.tmp '1i\
import init.remote.rc' "$INIT_RC"
        fi
        rm -f "${INIT_RC}.tmp"
    else
        echo "[*] import already present in $INIT_RC"
    fi
else
    echo "[!] WARNING: No suitable init rc found, import not added"
fi

echo "[*] Repacking ramdisk..."
find . | cpio -o -H newc 2>/dev/null | gzip > "$RAMDISK"

echo "[*] Cleaning up"
rm -rf "$WORKDIR"

echo "[+] Done! Ramdisk patched: $RAMDISK"
echo "[+] Backup: $BACKUP"
echo ""
echo "Services after boot:"
echo "  telnet <device-ip> 2323         — interactive shell"
echo "  mocha-remote.sh cmd 'dmesg'     — HTTP API (port 8080)"
echo "  mocha-remote.sh kexec boot.img  — kexec boot (like fastboot boot)"
echo "  mocha-remote.sh flash boot.img  — flash + reboot (like fastboot flash)"
