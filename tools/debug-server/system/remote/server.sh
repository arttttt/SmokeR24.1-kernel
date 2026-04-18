#!/system/bin/sh
# Start the HTTP CGI server for remote kernel development.
#
# Runs busybox httpd on port 8080 with /system/remote as document root.
# CGI scripts in /system/remote/cgi-bin/ handle: cmd, upload, download, flash, kexec, dump.

export PATH="/system/remote:/system/bin:/system/xbin:$PATH"
mkdir -p /sdcard/tmp 2>/dev/null
exec /system/remote/busybox httpd -f -p 8080 -h /system/remote -c /dev/null
