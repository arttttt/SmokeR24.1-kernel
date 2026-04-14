#!/bin/bash
#
# Install remote debug server to /system partition.
#
# Usage:
#   install.sh adb          Install via ADB (default)
#   install.sh wifi         Install via existing WiFi server (mocha-remote)
#
# Supports:
#   Android 4.4 (MIUI) — starts via sysinit /system/etc/init.d/
#   Android 7+  (LOS)  — starts via /system/etc/init/remote.rc
#
# Requires: curl (for busybox download on first run)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SYSTEM_DIR="${SCRIPT_DIR}/system"
BIN_DIR="${SCRIPT_DIR}/bin"
BUSYBOX_URL="https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-armv7l"

MODE="${1:-adb}"

die() { echo "ERROR: $*" >&2; exit 1; }

# --- Transport layer ---

_push() { :; }
_cmd() { :; }

setup_adb() {
    adb get-state >/dev/null 2>&1 || die "No ADB device found"
    echo "[*] Transport: ADB"
    _push() { adb push "$1" "$2" >/dev/null; }
    _cmd()  { adb shell "$*"; }
    # Try adb root + remount
    adb root >/dev/null 2>&1 || true
    sleep 1
    adb remount >/dev/null 2>&1 || true
}

setup_wifi() {
    local mocha="${SCRIPT_DIR}/../remote/mocha-remote.sh"
    [ -f "$mocha" ] || die "mocha-remote.sh not found at $mocha"
    echo "[*] Transport: WiFi (mocha-remote)"
    _push() { "$mocha" push "$1" "$2"; }
    _cmd()  { "$mocha" cmd "$*"; }
}

# --- Prepare binaries ---

prepare_bin() {
    mkdir -p "$BIN_DIR"

    # Busybox
    if [ ! -f "$BIN_DIR/busybox" ]; then
        echo "[*] Downloading busybox..."
        curl -L -o "$BIN_DIR/busybox" "$BUSYBOX_URL" \
            || die "Failed to download busybox"
    fi

    # Check kexec + micropython from old tools/remote/bin/
    local old_bin="${SCRIPT_DIR}/../remote/bin"
    [ ! -f "$BIN_DIR/kexec" ] && [ -f "$old_bin/kexec-armv7l" ] && \
        cp "$old_bin/kexec-armv7l" "$BIN_DIR/kexec"
    [ ! -f "$BIN_DIR/micropython" ] && [ -f "$old_bin/micropython-armv7l" ] && \
        cp "$old_bin/micropython-armv7l" "$BIN_DIR/micropython"
}

# --- Install ---

install() {
    echo "=== Installing remote debug server to /system ==="

    echo "[*] Remounting /system rw..."
    _cmd "mount -o remount,rw /system" || true

    echo "[*] Creating directories..."
    _cmd "mkdir -p /system/remote/cgi-bin /system/etc/init.d /system/etc/init"

    # Init files
    echo "[*] Installing init scripts..."
    _push "${SYSTEM_DIR}/etc/init.d/99remote"  "/system/etc/init.d/99remote"
    _push "${SYSTEM_DIR}/etc/init/remote.rc"    "/system/etc/init/remote.rc"

    # Server + CGI
    echo "[*] Installing server + CGI..."
    _push "${SYSTEM_DIR}/remote/server.sh"      "/system/remote/server.sh"
    _push "${SYSTEM_DIR}/remote/index.html"     "/system/remote/index.html"
    _push "${SYSTEM_DIR}/remote/rebuild_boot.py" "/system/remote/rebuild_boot.py"
    for cgi in cmd upload flash kexec dump; do
        _push "${SYSTEM_DIR}/remote/cgi-bin/${cgi}" "/system/remote/cgi-bin/${cgi}"
    done

    # Binaries
    echo "[*] Installing binaries..."
    _push "$BIN_DIR/busybox" "/system/remote/busybox"
    [ -f "$BIN_DIR/kexec" ] && \
        _push "$BIN_DIR/kexec" "/system/remote/kexec"
    [ -f "$BIN_DIR/micropython" ] && \
        _push "$BIN_DIR/micropython" "/system/remote/micropython"

    # Permissions
    echo "[*] Setting permissions..."
    _cmd "chmod 755 /system/remote/busybox /system/remote/server.sh /system/remote/rebuild_boot.py"
    _cmd "chmod 755 /system/remote/cgi-bin/cmd /system/remote/cgi-bin/upload /system/remote/cgi-bin/flash /system/remote/cgi-bin/kexec /system/remote/cgi-bin/dump"
    _cmd "chmod 755 /system/etc/init.d/99remote"
    _cmd "[ -f /system/remote/kexec ] && chmod 755 /system/remote/kexec || true"
    _cmd "[ -f /system/remote/micropython ] && chmod 755 /system/remote/micropython || true"

    # Remount ro
    echo "[*] Remounting /system ro..."
    _cmd "mount -o remount,ro /system" || true

    # Detect Android version
    local sdk
    sdk=$(_cmd "getprop ro.build.version.sdk" | tr -d '\r\n ')
    echo "[*] Android SDK: ${sdk:-unknown}"

    if [ "${sdk:-0}" -ge 24 ]; then
        echo "[+] LOS/Android 7+: /system/etc/init/remote.rc (auto-loaded by init)"
    else
        echo "[+] MIUI/Android 4.x: /system/etc/init.d/99remote (via sysinit)"
    fi

    echo ""
    echo "[+] Done. Reboot to start, or run now:"
    echo "    /system/etc/init.d/99remote"
}

# --- Main ---

case "$MODE" in
    adb)  setup_adb ;;
    wifi) setup_wifi ;;
    -h|--help)
        echo "Usage: install.sh [adb|wifi]"
        echo "  adb   — install via ADB USB (default)"
        echo "  wifi  — install via existing WiFi server (mocha-remote)"
        exit 0
        ;;
    *)    die "Unknown mode: $MODE (use 'adb' or 'wifi')" ;;
esac

prepare_bin
install
