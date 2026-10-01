#!/usr/bin/env python
"""
Measure MAME savestate sizes - compressed and uncompressed.

    python tools/sta_size.py run/sta
    python tools/sta_size.py path/to/file.sta ...

Prints, per driver, the file size (what a resync would put on the wire)
and the inflated size (what a rollback snapshot costs in RAM), because
the two differ by between 4x and 39x depending on the driver and they
drive different parts of the protocol. See notes/protocol.txt 2.4.

A .sta file is a 0x20-byte uncompressed header followed by one zlib
stream - save_manager::write_file does file.compress(FCOMPRESS_NONE) for
the header then FCOMPRESS_MEDIUM for the payload. ram_state::save()
writes the same payload with NO compression, so the inflated size here is
also ram_state::get_size(), which is sum(m_typesize * m_typecount) over
every registered save_item plus the header. That figure is fixed for a
given driver and MAME version; the compressed figure varies with what is
in memory at the moment of the save.

To produce a state without touching your settings:

    mamecoinopui.exe <driver> -noreadconfig -rompath roms -autosave \\
        -str 15 -nothrottle -video none -sound none \\
        -state_directory <scratch dir>
"""

import glob
import io
import os
import sys
import zlib

HEADER_SIZE = 0x20


def inflated_size(path):
    """Uncompressed payload + header, or None if it cannot be inflated."""
    data = io.open(path, "rb").read()
    body = data[HEADER_SIZE:]
    for wbits in (15, -15):                 # zlib-wrapped, then raw deflate
        try:
            return len(zlib.decompress(body, wbits)) + HEADER_SIZE
        except zlib.error:
            continue
    return None


def collect(args):
    paths = []
    for a in args:
        if os.path.isdir(a):
            paths += sorted(glob.glob(os.path.join(a, "*", "*.sta")))
            paths += sorted(glob.glob(os.path.join(a, "*.sta")))
        else:
            paths.append(a)
    return paths


def main(argv):
    paths = collect(argv[1:] or ["run/sta"])
    if not paths:
        sys.stderr.write("no .sta files found\n")
        return 2

    rows = []
    for p in paths:
        name = os.path.basename(os.path.dirname(p))
        if name in ("sta", "", "."):
            name = os.path.splitext(os.path.basename(p))[0]
        rows.append((name, os.path.getsize(p), inflated_size(p)))

    print("%-14s %13s %15s %8s" % ("driver", "compressed", "uncompressed",
                                   "ratio"))
    print("-" * 54)
    for name, comp, raw in sorted(rows, key=lambda r: -(r[2] or 0)):
        if raw is None:
            print("%-14s %10d KB %15s %8s" % (name, comp // 1024, "?", "-"))
        else:
            print("%-14s %10d KB %12.2f MB %7.1fx" % (
                name, comp // 1024, raw / 1048576.0, raw / float(comp)))

    ok = [r for r in rows if r[2]]
    if len(ok) > 1:
        print("-" * 54)
        lo, hi = min(r[2] for r in ok), max(r[2] for r in ok)
        print("uncompressed %.2f - %.2f MB (%.0fx spread)"
              % (lo / 1048576.0, hi / 1048576.0, hi / float(lo)))
        lo, hi = min(r[1] for r in ok), max(r[1] for r in ok)
        print("compressed   %d KB - %d KB (%.0fx spread)"
              % (lo // 1024, hi // 1024, hi / float(lo)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
