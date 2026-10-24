#!/usr/bin/env python3
"""Read a whole partition of the phone (e.g. modem_a, a FAT image) through the xnuloader's
"oem readpart NAME OFFSET LENGTH" (read only) in 64 MB windows. Usage: part_read.py NAME OUT [BYTES]

How much is read: BYTES if given, else the size a FAT boot sector at the start says (the modem
partition, NON-HLOS, is FAT: /vendor/firmware_mnt on the phone's Android), else windows until the
loader refuses one. The phone must be in the loader (xl enter). What comes out is the vendor's and
the phone's: it never goes into git (the 9 Pro's audio DSP image: modem_a, image/adsp.*)."""
import os
import struct
import subprocess
import sys
import tempfile

WINDOW = 64 << 20
BLOCK = 4096


def window(name, offset, length, scratch):
    """length bytes of the partition at offset (both multiples of 4096), or None if the loader refuses"""
    out = subprocess.run(["fastboot", "oem", "readpart", name, "%x" % offset, "%x" % length],
                         capture_output=True, text=True, timeout=120)
    if "OKAY" not in out.stdout + out.stderr:
        return None
    out = subprocess.run(["fastboot", "get_staged", scratch], capture_output=True, text=True, timeout=300)
    with open(scratch, "rb") as f:
        data = f.read()
    if len(data) != length:
        sys.exit("get_staged gave %d bytes, not %d: %s" % (len(data), length, out.stdout + out.stderr))
    return data


def fat_size(head):
    """the size a FAT12/16/32 boot sector gives the file system, or 0"""
    if head[510:512] != b"\x55\xaa":
        return 0
    bytes_per_sector, = struct.unpack_from("<H", head, 11)
    total16, = struct.unpack_from("<H", head, 19)
    total32, = struct.unpack_from("<I", head, 32)
    if bytes_per_sector not in (512, 1024, 2048, 4096):
        return 0
    return bytes_per_sector * (total16 or total32)


def main():
    name, dest = sys.argv[1], sys.argv[2]
    scratch = tempfile.mktemp(prefix="part-", dir=os.path.dirname(os.path.abspath(dest)))
    want = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
    try:
        head = window(name, 0, 1 << 20, scratch)
        if head is None:
            sys.exit("the loader does not read %s" % name)
        if not want:
            want = fat_size(head)
            if want:
                print("%s: FAT, %d bytes (%d MB)" % (name, want, want >> 20))
        size = (want + BLOCK - 1) // BLOCK * BLOCK if want else 0
        done = 0
        with open(dest, "wb") as out:
            while not size or done < size:
                length = WINDOW if not size else min(WINDOW, size - done)
                data = window(name, done, length, scratch)
                if data is None:
                    if size:
                        sys.exit("the loader refused %#x bytes at %#x" % (length, done))
                    break
                out.write(data)
                done += length
                print("%s: %d MB" % (name, done >> 20), flush=True)
            if want:
                out.truncate(want)
    finally:
        if os.path.exists(scratch):
            os.unlink(scratch)
    print("%s: %d bytes in %s" % (name, want or done, dest))


if __name__ == "__main__":
    main()
