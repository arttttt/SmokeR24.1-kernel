#!/bin/bash
#
# Patch ramdisk with WiFi remote debug server.
#
# Supports two modes:
#   Android (default) — services start on sys.boot_completed=1
#   TWRP (--twrp)     — services start on boot, WiFi via wpa_supplicant
#
# Requires: python3, cpio, curl (for busybox download on first run)
# Binaries: tools/remote/bin/kexec-armv7l, tools/remote/bin/micropython-armv7l

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVERLAY_DIR="${SCRIPT_DIR}/overlay"
OVERLAY_TWRP_DIR="${SCRIPT_DIR}/overlay-twrp"
CACHE_DIR="${SCRIPT_DIR}/.cache"
BIN_DIR="${SCRIPT_DIR}/bin"

BUSYBOX_URL="https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-armv7l"

MODE="android"
RAMDISK=""
OUTPUT=""

# ---- Output helpers ----

die() {
    echo "ERROR: $1" >&2
    [ -n "${2:-}" ] && echo "  hint: $2" >&2
    exit 1
}

usage() {
    local fd=1
    local code=0
    if [ "${1:-}" = "error" ]; then
        fd=2
        code=1
    fi

    cat >&$fd <<'EOF'
Usage: patch_ramdisk.sh [OPTIONS] <ramdisk.gz> [output]

Patch a ramdisk with WiFi remote debug server overlay.

Options:
  --twrp       Patch for TWRP recovery (WiFi via wpa_supplicant from
               /system, credentials from /data/misc/wifi/wpa_supplicant.conf)
  -h, --help   Show this help

Arguments:
  ramdisk.gz   Gzipped cpio ramdisk to patch (extracted from boot.img)
  output       Output file path (default: in-place, original saved as .orig)

Modes:
  Android (default):
    Services start on sys.boot_completed=1 property trigger.
    WiFi is managed by Android framework.

  TWRP (--twrp):
    Services start on boot trigger. WiFi is brought up by
    wifi-connect.sh which mounts /system and /data, uses
    wpa_supplicant with saved credentials, and runs udhcpc.
    If WiFi fails, TWRP continues normally — services are
    still running but unreachable until WiFi is up.

Examples:
  ./tools/remote/patch_ramdisk.sh ramdisk.gz
  ./tools/remote/patch_ramdisk.sh --twrp ramdisk.gz /tmp/patched.gz
EOF
    exit $code
}

# ---- Argument parsing ----

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -h|--help) usage ;;
            --twrp)    MODE="twrp"; shift ;;
            -*)        die "Unknown option: $1" "See --help for usage" ;;
            *)
                if [ -z "$RAMDISK" ]; then
                    RAMDISK="$1"
                elif [ -z "$OUTPUT" ]; then
                    OUTPUT="$1"
                else
                    die "Too many arguments: $1" "See --help for usage"
                fi
                shift
                ;;
        esac
    done

    [ -n "$RAMDISK" ] || usage error
    [ -f "$RAMDISK" ] || die "Ramdisk not found: $RAMDISK" "Check the path and try again"
    [ -d "$OVERLAY_DIR" ] || die "Overlay dir not found: $OVERLAY_DIR" "Run from the kernel source root"

    if [ "$MODE" = "twrp" ] && [ ! -d "$OVERLAY_TWRP_DIR" ]; then
        die "TWRP overlay dir not found: $OVERLAY_TWRP_DIR" "Expected at tools/remote/overlay-twrp/"
    fi

    # Resolve to absolute path
    RAMDISK="$(cd "$(dirname "$RAMDISK")" && pwd)/$(basename "$RAMDISK")"
    [ -n "$OUTPUT" ] && OUTPUT="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
}

# ---- Prepare overlay binaries ----

prepare_overlay() {
    mkdir -p "$CACHE_DIR"

    # Busybox: download if needed
    if [ ! -f "$CACHE_DIR/busybox" ]; then
        echo "[*] Downloading busybox..."
        curl -L -o "$CACHE_DIR/busybox" "$BUSYBOX_URL" \
            || die "Failed to download busybox" "Check internet connection"
    fi
    cp "$CACHE_DIR/busybox" "$OVERLAY_DIR/sbin/busybox"
    chmod 755 "$OVERLAY_DIR/sbin/busybox"
    ln -sf busybox "$OVERLAY_DIR/sbin/telnetd"
    echo "[*] busybox: ready"

    # Kexec
    if [ -f "$BIN_DIR/kexec-armv7l" ]; then
        cp "$BIN_DIR/kexec-armv7l" "$OVERLAY_DIR/sbin/kexec"
        chmod 755 "$OVERLAY_DIR/sbin/kexec"
        echo "[*] kexec: ready"
    fi

    # Micropython
    if [ -f "$BIN_DIR/micropython-armv7l" ]; then
        cp "$BIN_DIR/micropython-armv7l" "$OVERLAY_DIR/sbin/micropython"
        chmod 755 "$OVERLAY_DIR/sbin/micropython"
        echo "[*] micropython: ready"
    fi
}

# ---- Ramdisk extraction ----

extract_ramdisk() {
    local workdir="$1"

    echo "[*] Extracting ramdisk..."
    cd "$workdir"
    gzip -dc "$RAMDISK" | cpio -idm 2>/dev/null

    # Save original file list for repack
    gzip -dc "$RAMDISK" | cpio -t 2>/dev/null > "$workdir/_orig_cpio_list.txt"
}

# ---- Apply overlay files ----

apply_overlay() {
    local workdir="$1"

    # Shared overlay (always)
    cp -R "$OVERLAY_DIR"/* "$workdir/"
    chmod 755 "$workdir/sbin/cgi-bin"/* \
              "$workdir/sbin/remote-server.sh" \
              "$workdir/sbin/rebuild_boot.sh" \
              "$workdir/sbin/rebuild_boot.py" 2>/dev/null || true

    # TWRP overlay (overwrites init.remote.rc, adds wifi-connect.sh + udhcpc.script)
    if [ "$MODE" = "twrp" ]; then
        cp -R "$OVERLAY_TWRP_DIR"/* "$workdir/"
        chmod 755 "$workdir/sbin/wifi-connect.sh" \
                  "$workdir/sbin/udhcpc.script" \
                  "$workdir/sbin/watchdog-kick.sh" 2>/dev/null || true
        echo "[*] TWRP overlay applied"
    fi
}

# ---- Inject import into init rc ----

inject_import() {
    local workdir="$1"
    local init_rc=""

    if [ "$MODE" = "twrp" ]; then
        # TWRP: use init.rc
        [ -f "$workdir/init.rc" ] || die "init.rc not found in ramdisk" \
            "This doesn't look like a TWRP ramdisk"
        init_rc="init.rc"
    else
        # Android: find device-specific rc that is actually imported
        # init.rc imports init.${ro.hardware}.rc which is init.mocha.rc or init.tn8.rc
        for rc in init.mocha.rc init.tn8.rc; do
            if [ -f "$workdir/$rc" ] && grep -q "${rc%.rc}" "$workdir/init.rc" 2>/dev/null; then
                init_rc="$rc"
                break
            fi
        done
        # Fallback: just check file existence
        if [ -z "$init_rc" ]; then
            for rc in init.mocha.rc init.tn8.rc; do
                if [ -f "$workdir/$rc" ]; then
                    init_rc="$rc"
                    break
                fi
            done
        fi
        [ -n "$init_rc" ] || die "No device init rc found in ramdisk" \
            "Expected init.mocha.rc or init.tn8.rc"
    fi

    cd "$workdir"

    # Remove any existing remote imports
    sed -i.tmp '/init\.telnetd\.rc/d; /init\.remote\.rc/d' "$init_rc"
    rm -f "${init_rc}.tmp"

    # Insert after last existing import (or at top)
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
}

# ---- Repack ramdisk as gzipped cpio ----

repack_ramdisk() {
    local workdir="$1"

    # Determine output path
    local output_file
    if [ -n "$OUTPUT" ]; then
        output_file="$OUTPUT"
    else
        cp "$RAMDISK" "${RAMDISK}.orig"
        echo "[*] Original backed up to ${RAMDISK}.orig"
        output_file="$RAMDISK"
    fi

    # Build merged file list: original + new
    local new_files
    new_files=$(cd "$workdir" && find . -mindepth 1 | sed 's|^\./||' | sort \
        | comm -23 - <(sort "$workdir/_orig_cpio_list.txt"))

    {
        cat "$workdir/_orig_cpio_list.txt"
        echo "$new_files"
    } > "$workdir/_patched_cpio_list.txt"

    WORKDIR="$workdir" OUTPUT_FILE="$output_file" python3 << 'PYEOF'
import gzip, os

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

# ---- Main orchestration ----

patch_ramdisk() {
    local workdir
    workdir=$(mktemp -d)
    trap "rm -rf '$workdir'" EXIT

    extract_ramdisk "$workdir"
    apply_overlay "$workdir"
    inject_import "$workdir"
    repack_ramdisk "$workdir"
}

main() {
    parse_args "$@"

    echo "=== Mocha ramdisk patcher (mode: $MODE) ==="
    prepare_overlay

    patch_ramdisk

    local output_file="${OUTPUT:-${RAMDISK}}"
    local size
    size=$(stat -f%z "$output_file" 2>/dev/null || stat -c%s "$output_file" 2>/dev/null)
    echo "[+] Patched ramdisk: $output_file ($size bytes)"
    echo ""
    echo "Next: build boot.img with this ramdisk, or use:"
    echo "  mocha-remote.sh kexec boot.img"
    echo "  mocha-remote.sh flash boot.img"
}

main "$@"
