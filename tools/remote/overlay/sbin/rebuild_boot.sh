#!/sbin/busybox sh
# Rebuild boot.img with a new ramdisk and set multirom trampoline version.
# Pure shell+dd fallback for rebuild_boot.py (no python/micropython needed).
#
# Usage:
#   rebuild_boot.sh <boot.img> <new_ramdisk.gz> <tr_ver> <output>
#
# Parameters:
#   boot.img        — original Android boot.img to use as template
#   new_ramdisk.gz  — replacement ramdisk (gzipped cpio)
#   tr_ver          — multirom trampoline version number (integer)
#   output          — output boot.img path
#
# Output: "OK size=<N> tr_ver=<N>" on success
#
# Same function as rebuild_boot.py but implemented with busybox dd+od.
# Requires: busybox (od, dd, tr, stat, truncate, printf)

BB=/sbin/busybox
[ $# -ne 4 ] && { echo "Usage: $0 <boot.img> <new_ramdisk.gz> <tr_ver> <output>"; exit 1; }
BOOT="$1"
NEW_RD="$2"
TR_VER="$3"
OUT="$4"
[ ! -f "$BOOT" ] && { echo "ERROR: $BOOT not found"; exit 1; }
[ ! -f "$NEW_RD" ] && { echo "ERROR: $NEW_RD not found"; exit 1; }

# Parse header
KS=$($BB od -A n -t u4 -j 8 -N 4 "$BOOT" | $BB tr -d " ")
RS=$($BB od -A n -t u4 -j 16 -N 4 "$BOOT" | $BB tr -d " ")
SS=$($BB od -A n -t u4 -j 24 -N 4 "$BOOT" | $BB tr -d " ")
PS=$($BB od -A n -t u4 -j 36 -N 4 "$BOOT" | $BB tr -d " ")
DS=$($BB od -A n -t u4 -j 40 -N 4 "$BOOT" | $BB tr -d " ")

pa() { echo $(( ($1 + PS - 1) / PS * PS )); }

K_OFF=$PS
R_OFF=$(( K_OFF + $(pa $KS) ))
S_OFF=$(( R_OFF + $(pa $RS) ))
D_OFF=$(( S_OFF + $(pa $SS) ))

NEW_RS=$($BB stat -c%s "$NEW_RD")
NEW_R_PAD=$(( $(pa $NEW_RS) - NEW_RS ))

# 1. Copy header (first page)
dd if="$BOOT" of="$OUT" bs=$PS count=1 2>/dev/null

# 2. Patch ramdisk size in header (offset 16, little-endian u32)
# Write 4 bytes at offset 16
printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
    $((NEW_RS & 0xFF)) \
    $(((NEW_RS >> 8) & 0xFF)) \
    $(((NEW_RS >> 16) & 0xFF)) \
    $(((NEW_RS >> 24) & 0xFF)))" | \
    dd of="$OUT" bs=1 seek=16 count=4 conv=notrunc 2>/dev/null

# 3. Write tr_ver in name field (offset 48, 16 bytes)
printf "%-16s" "tr_ver$TR_VER" | dd of="$OUT" bs=1 seek=48 count=16 conv=notrunc 2>/dev/null

# 4. Append kernel (page-aligned)
dd if="$BOOT" bs=1 skip=$K_OFF count=$KS 2>/dev/null >> "$OUT"
K_PAD=$(( $(pa $KS) - KS ))
[ $K_PAD -gt 0 ] && dd if=/dev/zero bs=1 count=$K_PAD 2>/dev/null >> "$OUT"

# 5. Append new ramdisk (page-aligned)
cat "$NEW_RD" >> "$OUT"
[ $NEW_R_PAD -gt 0 ] && dd if=/dev/zero bs=1 count=$NEW_R_PAD 2>/dev/null >> "$OUT"

# 6. Append dt if exists (page-aligned)
if [ "$DS" -gt 0 ]; then
    dd if="$BOOT" bs=1 skip=$D_OFF count=$DS 2>/dev/null >> "$OUT"
    D_PAD=$(( $(pa $DS) - DS ))
    [ $D_PAD -gt 0 ] && dd if=/dev/zero bs=1 count=$D_PAD 2>/dev/null >> "$OUT"
fi

TOTAL=$($BB stat -c%s "$OUT")
echo "OK size=$TOTAL tr_ver=$TR_VER"
