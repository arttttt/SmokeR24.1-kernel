#!/bin/bash
#
# Patch ramdisk with WiFi remote debug server.
# Uses overlay from tools/remote/overlay/ and binaries from tools/remote/bin/.
#
# Usage:
#   ./tools/remote/patch_ramdisk.sh <ramdisk.img|gz> [output]
#
# Parameters:
#   ramdisk.img|gz  — gzipped cpio ramdisk to patch (from boot.img)
#   output          — output file path (optional, default: in-place with .orig backup)
#
# Output: patched gzipped cpio ramdisk with remote server overlay injected
#
# What it does:
#   - Copies busybox, kexec, micropython into overlay/sbin/
#   - Extracts ramdisk, merges overlay files, adds init.remote.rc import
#   - Repacks as gzipped cpio with uid=0:gid=0
#
# Requires: python3, curl (for busybox download on first run)
# Binaries: tools/remote/bin/kexec-armv7l, tools/remote/bin/micropython-armv7l

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVERLAY_DIR="${SCRIPT_DIR}/overlay"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BIN_DIR="${SCRIPT_DIR}/bin"

BUSYBOX_URL="https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-armv7l"

RAMDISK="${1:?Usage: $0 <ramdisk.img|gz> [output]}"
OUTPUT="${2:-}"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -f "$RAMDISK" ] || die "Ramdisk not found: $RAMDISK"
[ -d "$OVERLAY_DIR" ] || die "Overlay dir not found: $OVERLAY_DIR"

mkdir -p "$CACHE_DIR"

# --- Prepare overlay: copy binaries into overlay/sbin/ ---
prepare_overlay() {
    # Busybox: download if needed
    if [ ! -f "$CACHE_DIR/busybox" ]; then
        echo "[*] Downloading busybox..."
        curl -L -o "$CACHE_DIR/busybox" "$BUSYBOX_URL"
    fi
    cp "$CACHE_DIR/busybox" "$OVERLAY_DIR/sbin/busybox"
    chmod 755 "$OVERLAY_DIR/sbin/busybox"
    ln -sf busybox "$OVERLAY_DIR/sbin/telnetd"
    echo "[*] busybox: ready"

    # Kexec: from bin/
    if [ -f "$BIN_DIR/kexec-armv7l" ]; then
        cp "$BIN_DIR/kexec-armv7l" "$OVERLAY_DIR/sbin/kexec"
        chmod 755 "$OVERLAY_DIR/sbin/kexec"
        echo "[*] kexec: ready"
    fi

    # Micropython: from bin/
    if [ -f "$BIN_DIR/micropython-armv7l" ]; then
        cp "$BIN_DIR/micropython-armv7l" "$OVERLAY_DIR/sbin/micropython"
        chmod 755 "$OVERLAY_DIR/sbin/micropython"
        echo "[*] micropython: ready"
    fi
}

# --- Patch ramdisk ---
patch_ramdisk() {
    local workdir
    workdir=$(mktemp -d)
    trap "rm -rf '$workdir'" EXIT

    echo "[*] Extracting ramdisk..."
    cd "$workdir"
    gzip -dc "$RAMDISK" | cpio -idm 2>/dev/null

    # Save original cpio file list
    gzip -dc "$RAMDISK" | cpio -t 2>/dev/null > "$workdir/_orig_cpio_list.txt"

    # Apply overlay — everything in overlay/ goes into ramdisk
    cp -R "$OVERLAY_DIR"/* .
    chmod 755 sbin/cgi-bin/* sbin/remote-server.sh sbin/rebuild_boot.sh sbin/rebuild_boot.py 2>/dev/null

    # Add import to init rc
    local init_rc=""
    if [ -f init.tn8.rc ]; then
        init_rc=init.tn8.rc
    elif [ -f init.mocha.rc ]; then
        init_rc=init.mocha.rc
    fi

    if [ -n "$init_rc" ]; then
        sed -i.tmp '/init\.telnetd\.rc/d; /init\.remote\.rc/d' "$init_rc"
        rm -f "${init_rc}.tmp"

        local last_import
        last_import=$(grep -n '^import' "$init_rc" | tail -1 | cut -d: -f1)
        if [ -n "$last_import" ]; then
            sed -i.tmp "${last_import}a\\
import init.remote.rc" "$init_rc"
        else
            sed -i.tmp '1i\
import init.remote.rc' "$init_rc"
        fi
        rm -f "${init_rc}.tmp"
        echo "[*] Added import to $init_rc"
    fi

    # Inject LD_PRELOAD for ISP wrapper into mediaserver service
    if grep -q '^service media /system/bin/mediaserver' init.rc 2>/dev/null; then
        sed -i.tmp 's|^service media /system/bin/mediaserver$|&\
    setenv LD_PRELOAD /system/lib/isp_wrapper.so|' init.rc
        rm -f init.rc.tmp
        echo "[*] Injected LD_PRELOAD into mediaserver service"
    fi

    # Build file list: original + new files (auto-detected)
    local new_files
    new_files=$(cd "$workdir" && find . -mindepth 1 | sed 's|^\./||' | sort | comm -23 - <(sort "$workdir/_orig_cpio_list.txt"))

    # Repack
    local output_file
    if [ -n "$OUTPUT" ]; then
        output_file="$OUTPUT"
    else
        cp "$RAMDISK" "${RAMDISK}.orig"
        echo "[*] Original backed up to ${RAMDISK}.orig"
        output_file="$RAMDISK"
    fi

    {
        cat "$workdir/_orig_cpio_list.txt"
        echo "$new_files"
    } > "$workdir/_patched_cpio_list.txt"

    WORKDIR="$workdir" OUTPUT_FILE="$output_file" python3 << 'PYEOF'
import gzip, os, struct

def cpio_header(name, stat_info, content_len):
    mode = stat_info.st_mode
    nlink = 2 if os.path.isdir(os.path.join(workdir, name)) else 1
    namesize = len(name) + 1
    h = "070701"
    h += "%08X" % stat_info.st_ino
    h += "%08X" % mode
    h += "%08X" % 0
    h += "%08X" % 0
    h += "%08X" % nlink
    h += "%08X" % int(stat_info.st_mtime)
    h += "%08X" % content_len
    h += "%08X" % 0
    h += "%08X" % 0
    h += "%08X" % 0
    h += "%08X" % 0
    h += "%08X" % namesize
    h += "%08X" % 0
    return h.encode('ascii')

def pad4(n):
    return (4 - (n % 4)) % 4

workdir = os.environ['WORKDIR']
filelist = open(os.environ['WORKDIR'] + '/_patched_cpio_list.txt').read().strip().split('\n')

out = bytearray()
for name in filelist:
    path = os.path.join(workdir, name)
    if not os.path.exists(path) and not os.path.islink(path):
        continue
    st = os.lstat(path)
    if os.path.islink(path):
        target = os.readlink(path).encode()
        hdr = cpio_header(name, st, len(target))
        entry = hdr + name.encode() + b'\0'
        entry += b'\0' * pad4(len(entry))
        entry += target
        entry += b'\0' * pad4(len(entry))
    elif os.path.isdir(path):
        hdr = cpio_header(name, st, 0)
        entry = hdr + name.encode() + b'\0'
        entry += b'\0' * pad4(len(entry))
    else:
        with open(path, 'rb') as f:
            data = f.read()
        hdr = cpio_header(name, st, len(data))
        entry = hdr + name.encode() + b'\0'
        entry += b'\0' * pad4(len(entry))
        entry += data
        entry += b'\0' * pad4(len(entry))
    out += entry

trailer = "TRAILER!!!"
hdr = "070701" + "00000000" * 12 + "%08X" % (len(trailer) + 1) + "00000000"
entry = hdr.encode() + trailer.encode() + b'\0'
entry += b'\0' * pad4(len(entry))
out += entry

with gzip.open(os.environ['OUTPUT_FILE'], 'wb') as f:
    f.write(bytes(out))
print(f"Packed {len(filelist)} entries, uid=0:gid=0")
PYEOF
    rm -f "$workdir/_orig_cpio_list.txt" "$workdir/_patched_cpio_list.txt"
}

# --- Main ---
echo "=== Mocha ramdisk patcher ==="
prepare_overlay
patch_ramdisk

local_out="${OUTPUT:-${RAMDISK}}"
local_size=$(stat -f%z "$local_out" 2>/dev/null || stat -c%s "$local_out" 2>/dev/null)
echo "[+] Patched ramdisk: $local_out ($local_size bytes)"
echo ""
echo "Next: build boot.img with this ramdisk, or use:"
echo "  mocha-remote.sh kexec boot.img"
echo "  mocha-remote.sh flash boot.img"
