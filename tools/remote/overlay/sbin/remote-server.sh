#!/system/bin/sh
export PATH="/sbin:/system/bin:/system/xbin:$PATH"
mkdir -p /tmp 2>/dev/null
mount -t tmpfs tmpfs /tmp -o size=64m 2>/dev/null
exec /sbin/busybox httpd -f -p 8080 -h /sbin -c /dev/null
