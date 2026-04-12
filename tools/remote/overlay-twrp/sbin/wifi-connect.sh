#!/sbin/sh
#
# Best-effort WiFi connection using saved Android credentials.
#
# Mounts /system for wpa_supplicant binary and /data for saved WiFi
# networks. Never exits with error code — logs failures via kernel
# log and exits 0 so TWRP recovery continues normally.
#
# Requires: busybox (udhcpc), wpa_supplicant (from /system)

WPA_CONF="/data/misc/wifi/wpa_supplicant.conf"
WPA_BIN="/system/bin/wpa_supplicant"
IFACE="wlan0"
LOG_TAG="wifi-connect"

log() { echo "$LOG_TAG: $*"; }

# Log error and exit cleanly — never crash recovery
abort() { log "SKIP: $*"; exit 0; }

mount_if_needed() {
    local mnt="$1"
    mountpoint -q "$mnt" && return 0
    mount "$mnt" 2>/dev/null || mount -o ro "$mnt" 2>/dev/null || return 1
}

# --- Mount partitions ---
mount_if_needed /system || abort "/system mount failed"
mount_if_needed /data   || abort "/data mount failed"

# --- Validate prerequisites ---
[ -x "$WPA_BIN" ]  || abort "wpa_supplicant not found at $WPA_BIN"
[ -f "$WPA_CONF" ] || abort "WiFi config not found at $WPA_CONF"
grep -q "network=" "$WPA_CONF" || abort "No saved networks in $WPA_CONF"

# --- Bring up interface ---
log "bringing up $IFACE"
ifconfig "$IFACE" up 2>/dev/null || abort "ifconfig $IFACE up failed"
sleep 1

# --- Start wpa_supplicant (try nl80211 first, fall back to wext) ---
log "starting wpa_supplicant"
"$WPA_BIN" -B -i"$IFACE" -c"$WPA_CONF" -Dnl80211 2>/dev/null || \
"$WPA_BIN" -B -i"$IFACE" -c"$WPA_CONF" -Dwext 2>/dev/null    || \
    abort "wpa_supplicant failed to start"

# --- Wait for association (up to 15s) ---
log "waiting for association..."
connected=0
n=0
while [ "$n" -lt 15 ]; do
    if wpa_cli -i"$IFACE" status 2>/dev/null | grep -q "wpa_state=COMPLETED"; then
        connected=1
        break
    fi
    sleep 1
    n=$((n + 1))
done
[ "$connected" = "1" ] || abort "WiFi association timed out after 15s"

# --- DHCP ---
log "requesting DHCP lease"
/sbin/busybox udhcpc -i "$IFACE" -s /sbin/udhcpc.script -t 5 -n -q 2>/dev/null || \
    abort "udhcpc failed"

IP=$(ifconfig "$IFACE" 2>/dev/null | grep "inet addr" | awk -F: '{print $2}' | awk '{print $1}')
log "connected: ${IP:-unknown}"
log "telnet $IP 2323 | http://$IP:8080"
