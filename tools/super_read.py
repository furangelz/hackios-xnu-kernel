#!/usr/bin/env python3
"""Read a logical partition (e.g. vendor_a) out of the phone's "super" through the xnuloader's
"oem readpart super OFFSET LENGTH" (read only), using the dynamic-partition metadata
(Android liblp, metadata_format.h). Usage: super_read.py NAME OUT [--list]

The phone must be in the loader (xl enter). This is how the 9 Pro's GPU firmware was got: its
Android (a user build) does not let adb read /vendor/firmware, so vendor_a (EROFS) is read here
and the files taken out of it with erofs-utils' fsck.erofs --extract. What comes out is the
vendor's and the phone's: it goes into fw-gpu/ and never into git."""
import os
import struct
import subprocess
import sys
import tempfile

WINDOW = 64 << 20
SECTOR = 512


def window(offset, length, scratch):
    """length bytes of super at offset (both multiples of 4096), through the loader"""
    out = subprocess.run(["fastboot", "oem", "readpart", "super", "%x" % offset, "%x" % length],
                         capture_output=True, text=True, timeout=120)
    if "OKAY" not in out.stdout + out.stderr:
        sys.exit("readpart failed at %#x: %s" % (offset, out.stdout + out.stderr))
    out = subprocess.run(["fastboot", "get_staged", scratch], capture_output=True, text=True, timeout=300)
    with open(scratch, "rb") as f:
        data = f.read()
    if len(data) != length:
        sys.exit("get_staged gave %d bytes, not %d: %s" % (len(data), length, out.stdout + out.stderr))
    return data


def main():
    name, dest = sys.argv[1], sys.argv[2]
    listing = "--list" in sys.argv
    scratch = tempfile.mktemp(prefix="super-", dir=os.path.dirname(os.path.abspath(dest)))
    head = window(0, 1 << 20, scratch)
    magic, size = struct.unpack_from("<II", head, 4096)
    if magic != 0x616C4467:
        sys.exit("no LP geometry at 4096 (magic %#x)" % magic)
    max_size, slots, block_size = struct.unpack_from("<III", head, 4096 + 8 + 32)
    print("geometry: metadata %#x bytes x %d slots, logical blocks of %d" % (max_size, slots, block_size))
    meta_at = 4096 + 2 * 4096                       # slot 0
    if meta_at + max_size > len(head):
        head = window(0, (meta_at + max_size + 0xfff) & ~0xfff, scratch)
    m = head[meta_at:]
    magic, major, minor, header_size = struct.unpack_from("<IHHI", m, 0)
    if magic != 0x414C5030:
        sys.exit("no LP metadata header (magic %#x)" % magic)
    tables_size, = struct.unpack_from("<I", m, 12 + 32)
    desc = struct.unpack_from("<12I", m, 12 + 32 + 4 + 32)
    tables = m[header_size:header_size + tables_size]
    parts, exts = desc[0:3], desc[3:6]
    extents = []
    for i in range(exts[1]):
        n, ttype, tdata, src = struct.unpack_from("<QIQI", tables, exts[0] + i * exts[2])
        extents.append((n, ttype, tdata, src))
    found = None
    for i in range(parts[1]):
        raw = tables[parts[0] + i * parts[2]:parts[0] + (i + 1) * parts[2]]
        pname = raw[:36].split(b"\0")[0].decode()
        attrs, first, count, group = struct.unpack_from("<IIII", raw, 36)
        size = sum(extents[first + k][0] for k in range(count)) * SECTOR
        if listing:
            print("  %-24s %6d MB in %d extent(s)" % (pname, size >> 20, count))
        if pname == name:
            found = [extents[first + k] for k in range(count)]
    if listing or found is None:
        if found is None:
            print("no partition %s" % name)
        os.unlink(scratch)
        return
    total = sum(e[0] for e in found) * SECTOR
    print("%s: %d bytes in %d extent(s); reading" % (name, total, len(found)))
    with open(dest, "wb") as f:
        for n, ttype, tdata, src in found:
            if ttype != 0 or src != 0:
                sys.exit("extent kind %d on device %d: not handled" % (ttype, src))
            at, left = tdata * SECTOR, n * SECTOR
            while left > 0:
                take = min(left, WINDOW)
                f.write(window(at, take, scratch))
                at += take
                left -= take
                print("  %d MB left" % (left >> 20), flush=True)
    os.unlink(scratch)
    print("written: %s" % dest)


main()
