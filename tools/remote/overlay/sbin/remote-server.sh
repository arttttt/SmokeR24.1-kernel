#!/system/bin/sh
# Start the HTTP CGI server for remote kernel development.
#
# Usage: remote-server.sh (started by init via init.remote.rc)
#
# Runs busybox httpd on port 8080 with /sbin as document root.
# CGI scripts in /sbin/cgi-bin/ handle: cmd, upload, flash, kexec, dump.
# Runs in foreground (-f) so init can manage the service lifecycle.
#
# Requires: busybox (httpd applet)

export PATH="/sbin:/system/bin:/system/xbin:$PATH"
mkdir -p /sdcard/tmp 2>/dev/null
exec /sbin/busybox httpd -f -p 8080 -h /sbin -c /dev/null
