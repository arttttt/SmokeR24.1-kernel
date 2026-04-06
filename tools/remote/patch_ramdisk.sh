#!/bin/bash
#
# Patch ramdisk with WiFi remote debug server.
# Uses overlay from tools/remote/overlay/ and downloads binaries if needed.
#
# Usage:
#   ./tools/remote/patch_ramdisk.sh <ramdisk.img|gz> [output]
#
# If output is omitted, patches in-place (with .orig backup).
# Binaries (busybox, kexec) are cached in tools/remote/.cache/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVERLAY_DIR="${SCRIPT_DIR}/overlay"
CACHE_DIR="${SCRIPT_DIR}/.cache"

BUSYBOX_URL="https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-armv7l"

RAMDISK="${1:?Usage: $0 <ramdisk.img|gz> [output]}"
OUTPUT="${2:-}"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -f "$RAMDISK" ] || die "Ramdisk not found: $RAMDISK"
[ -d "$OVERLAY_DIR" ] || die "Overlay dir not found: $OVERLAY_DIR"

mkdir -p "$CACHE_DIR"

# --- Download/cache binaries ---
fetch_busybox() {
    if [ ! -f "$CACHE_DIR/busybox" ]; then
        echo "[*] Downloading busybox..."
        curl -L -o "$CACHE_DIR/busybox" "$BUSYBOX_URL"
        chmod 755 "$CACHE_DIR/busybox"
    fi
    echo "[*] busybox: $CACHE_DIR/busybox"
}

fetch_kexec() {
    if [ -f "${SCRIPT_DIR}/bin/kexec-armv7l" ]; then
        cp "${SCRIPT_DIR}/bin/kexec-armv7l" "$CACHE_DIR/kexec"
        chmod 755 "$CACHE_DIR/kexec"
        echo "[*] kexec: ${SCRIPT_DIR}/bin/kexec-armv7l"
    else
        echo "[!] kexec not found in bin/ (optional, skipping)"
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

    # Save original cpio file list (preserves order and no ./ prefix)
    gzip -dc "$RAMDISK" | cpio -t 2>/dev/null > /tmp/_orig_cpio_list.txt

    # Add busybox
    cp "$CACHE_DIR/busybox" sbin/busybox
    chmod 755 sbin/busybox
    ln -sf busybox sbin/telnetd

    # Add micropython if available
    if [ -f "${SCRIPT_DIR}/bin/micropython-armv7l" ]; then
        cp "${SCRIPT_DIR}/bin/micropython-armv7l" sbin/micropython
        chmod 755 sbin/micropython
    fi

    # Add kexec if available
    if [ -f "$CACHE_DIR/kexec" ]; then
        cp "$CACHE_DIR/kexec" sbin/kexec
        chmod 755 sbin/kexec
    fi

    # Apply overlay (init.remote.rc, CGI scripts, remote-server.sh)
    cp -R "$OVERLAY_DIR"/* .
    chmod 755 sbin/cgi-bin/cmd sbin/cgi-bin/upload sbin/remote-server.sh

    # Add import to init rc
    local init_rc=""
    if [ -f init.tn8.rc ]; then
        init_rc=init.tn8.rc
    elif [ -f init.mocha.rc ]; then
        init_rc=init.mocha.rc
    fi

    if [ -n "$init_rc" ]; then
        # Clean old imports
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

    # Build new file list: original files + overlay files + extra binaries
    local new_files
    new_files=$(cd "$workdir" && find sbin/busybox sbin/telnetd $(cd "$OVERLAY_DIR" && find . -mindepth 1 | sed 's|^\./||') $([ -f sbin/kexec ] && echo sbin/kexec) -prune 2>/dev/null | sort -u)

    # Build file list for cpio: original order + new files appended
    {
        cat /tmp/_orig_cpio_list.txt
        echo "$new_files"
    } > /tmp/_patched_cpio_list.txt

    # Repack: use python to invoke cpio correctly (macOS cpio mangles ./ prefix)
    local output_file
    if [ -n "$OUTPUT" ]; then
        output_file="$OUTPUT"
    else
        cp "$RAMDISK" "${RAMDISK}.orig"
        echo "[*] Original backed up to ${RAMDISK}.orig"
        output_file="$RAMDISK"
    fi

    WORKDIR="$workdir" OUTPUT_FILE="$output_file" python3 << 'PYEOF'
import gzip, os, struct

def cpio_header(name, stat_info, content_len, is_symlink=False):
    """Create cpio newc format header with uid=0, gid=0 (root)"""
    mode = stat_info.st_mode
    nlink = 2 if os.path.isdir(os.path.join(workdir, name)) else 1
    namesize = len(name) + 1  # include null terminator
    h = "070701"                          # magic
    h += "%08X" % stat_info.st_ino
    h += "%08X" % mode
    h += "%08X" % 0                       # uid = root
    h += "%08X" % 0                       # gid = root
    h += "%08X" % nlink
    h += "%08X" % int(stat_info.st_mtime)
    h += "%08X" % content_len
    h += "%08X" % 0                       # devmajor
    h += "%08X" % 0                       # devminor
    h += "%08X" % 0                       # rdevmajor
    h += "%08X" % 0                       # rdevminor
    h += "%08X" % namesize
    h += "%08X" % 0                       # check
    return h.encode('ascii')

def pad4(n):
    return (4 - (n % 4)) % 4

workdir = os.environ['WORKDIR']
filelist = open('/tmp/_patched_cpio_list.txt').read().strip().split('\n')

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

# TRAILER
trailer = "TRAILER!!!"
hdr = "070701" + "00000000" * 12 + "%08X" % (len(trailer) + 1) + "00000000"
entry = hdr.encode() + trailer.encode() + b'\0'
entry += b'\0' * pad4(len(entry))
out += entry

with gzip.open(os.environ['OUTPUT_FILE'], 'wb') as f:
    f.write(bytes(out))
print(f"Packed {len(filelist)} entries, uid=0:gid=0")
PYEOF
    rm -f /tmp/_orig_cpio_list.txt /tmp/_patched_cpio_list.txt

    local size
    size=$(stat -f%z "$output_file" 2>/dev/null || stat -c%s "$output_file" 2>/dev/null)
    echo "[+] Patched ramdisk: $output_file ($size bytes)"
}

# --- Main ---
echo "=== Mocha ramdisk patcher ==="
fetch_busybox
fetch_kexec
patch_ramdisk

echo ""
echo "Next: build boot.img with this ramdisk, or use:"
echo "  mocha-remote.sh kexec boot.img"
echo "  mocha-remote.sh flash boot.img"
