#!/sbin/sh
#
# Keep the hardware watchdog fed for as long as recovery is up.
#
# The board's watchdog is running by the time userspace exists, and this kernel
# is built CONFIG_WATCHDOG_NOWAYOUT=y, so it cannot be switched off again: the
# only thing keeping the board alive is somebody writing to /dev/watchdog
# before it expires. Android has watchdogd for exactly that, started from
# init.rc and running the whole time the system is up. Recovery has nothing of
# the sort, so a recovery left sitting on its menu resets itself -- and the
# giveaway is a console log that simply stops, with no "reboot: Restarting
# system" anywhere, because nothing asked for the reboot.
#
# The descriptor is held open for the life of the script rather than reopened
# for each write. Reopening works, but it makes every single kick depend on an
# open succeeding, and an open is also what would start the timer if it were
# not already running.
#
# Never fails the boot: a recovery on a board with no watchdog device should
# still come up.

LOG_TAG="watchdog-kick"

[ -c /dev/watchdog ] || { echo "$LOG_TAG: no /dev/watchdog, nothing to feed"; exit 0; }

exec 3> /dev/watchdog

echo "$LOG_TAG: feeding /dev/watchdog every 5s"

while true; do
    echo -n 1 >&3 2>/dev/null || true
    sleep 5
done
