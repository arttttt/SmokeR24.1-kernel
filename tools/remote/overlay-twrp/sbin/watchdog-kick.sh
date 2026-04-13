#!/sbin/sh
#
# Kick hardware watchdog to prevent reboot in TWRP recovery.
# Writes to /dev/watchdog every 10 seconds.
# Never exits with error — TWRP continues normally if watchdog is unavailable.

LOG_TAG="watchdog-kick"

[ -e /dev/watchdog ] || { echo "$LOG_TAG: /dev/watchdog not found, exiting"; exit 0; }

echo "$LOG_TAG: starting watchdog kicker"

while true; do
    echo V > /dev/watchdog 2>/dev/null || true
    sleep 10
done
