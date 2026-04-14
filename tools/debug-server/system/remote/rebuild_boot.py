#!/usr/bin/env python
# Rebuild boot.img with a new ramdisk and set multirom trampoline version.
#
# Usage:
#   micropython rebuild_boot.py <boot.img> <new_ramdisk.gz> <tr_ver> <output>
#   python     rebuild_boot.py <boot.img> <new_ramdisk.gz> <tr_ver> <output>
#
# Compatible with Python 2.6+, Python 3.x, and MicroPython.
import struct
import sys

boot = open(sys.argv[1], 'rb').read()
new_rd = open(sys.argv[2], 'rb').read()
tr_ver = int(sys.argv[3])
out_path = sys.argv[4]

ks = struct.unpack_from('<I', boot, 8)[0]
rs = struct.unpack_from('<I', boot, 16)[0]
ss = struct.unpack_from('<I', boot, 24)[0]
ps = struct.unpack_from('<I', boot, 36)[0]
ds = struct.unpack_from('<I', boot, 40)[0]


def pa(s):
    return ((s + ps - 1) // ps) * ps


k_off = ps
r_off = k_off + pa(ks)
s_off = r_off + pa(rs)
d_off = s_off + pa(ss)

# Update header: new ramdisk size + trampoline version
hdr = bytearray(boot[:ps])
struct.pack_into('<I', hdr, 16, len(new_rd))
name = 'tr_ver%d' % tr_ver
for i in range(16):
    if i < len(name):
        hdr[48 + i] = ord(name[i])
    else:
        hdr[48 + i] = 0

# Write segments directly to file
f = open(out_path, 'wb')
f.write(bytes(hdr))
f.write(boot[k_off:k_off + ks])
f.write(b'\x00' * (pa(ks) - ks))
f.write(new_rd)
f.write(b'\x00' * (pa(len(new_rd)) - len(new_rd)))
if ds > 0:
    f.write(boot[d_off:d_off + ds])
    f.write(b'\x00' * (pa(ds) - ds))
total = f.tell()
f.close()
print('OK size=%d tr_ver=%d' % (total, tr_ver))
