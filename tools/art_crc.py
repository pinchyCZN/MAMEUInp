#!/usr/bin/env python3
# license:BSD-3-Clause
# copyright-holders:MAMEUI Netplay
#
# Find art files MAMEUI can open but not decode.
#
# A MISSING image is harmless - every open fails, LoadDIB gives up and the
# missing-art bitmap is drawn. A CORRUPT BUT READABLE one is what hurts: the
# open succeeds, the decode fails, and before the screenshot.cpp fix that
# crashed the frontend on a single click in the machine list. The fix stops
# the crash; it does not make a bad image display, so the files still want
# finding.
#
#     python tools/art_crc.py "N:/games/MAME/Extras/snap"
#     python tools/art_crc.py "N:/games/MAME/Extras"        (walks subdirs)
#     python tools/art_crc.py <dir> --delete
#     python tools/art_crc.py <dir> --quiet
#
# Reads .zip archives in place, so snap.zip is checked without unpacking.
#
# WHAT COUNTS AS BAD IS WHAT MAME REJECTS, not what a viewer tolerates.
# png.cpp verifies EVERY chunk CRC, ancillary ones included, and returns
# PNGERR_FILE_CORRUPT on the first mismatch - so a wrong tEXt CRC kills an
# image whose pixels are perfectly fine, and most viewers show it anyway.
# That is exactly the outr2st.png case: valid IHDR and IDAT, one bad tEXt.

import binascii
import io
import os
import struct
import sys
import zipfile

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
JPEG_MAGIC = b"\xff\xd8\xff"
EXTS = (".png", ".jpg", ".jpeg")


def check_png(data):
    """(ok, detail). Mirrors png.cpp: every chunk CRC must match."""
    if not data.startswith(PNG_MAGIC):
        return False, "not a PNG despite the extension"
    off = len(PNG_MAGIC)
    seen_ihdr = seen_idat = seen_iend = False
    while off + 8 <= len(data):
        ln, typ = struct.unpack(">I4s", data[off:off + 8])
        if off + 12 + ln > len(data):
            return False, "truncated in %s chunk" % typ.decode("latin1", "replace")
        body = data[off + 8:off + 8 + ln]
        want = struct.unpack(">I", data[off + 8 + ln:off + 12 + ln])[0]
        if (binascii.crc32(typ + body) & 0xffffffff) != want:
            return False, "bad CRC on %s chunk" % typ.decode("latin1", "replace")
        if typ == b"IHDR":
            seen_ihdr = True
        elif typ == b"IDAT":
            seen_idat = True
        elif typ == b"IEND":
            seen_iend = True
            break
        off += 12 + ln
    if not seen_ihdr:
        return False, "no IHDR"
    if not seen_idat:
        return False, "no image data"
    if not seen_iend:
        return False, "no IEND, file ends early"
    return True, ""


def check_jpeg(data):
    """Shallow: libjpeg is lenient, so only catch what it cannot start on."""
    if not data.startswith(JPEG_MAGIC):
        return False, "not a JPEG despite the extension"
    if not data.rstrip(b"\x00").endswith(b"\xff\xd9"):
        return False, "no end-of-image marker, probably truncated"
    return True, ""


def check(name, data):
    if len(data) == 0:
        return False, "zero bytes"
    ext = os.path.splitext(name)[1].lower()
    if ext == ".png":
        return check_png(data)
    return check_jpeg(data)


def walk(root):
    """Yields (label, name, data_getter) for every art file under root."""
    for base, _dirs, files in os.walk(root):
        for f in sorted(files):
            full = os.path.join(base, f)
            low = f.lower()
            if low.endswith(EXTS):
                yield full, f, (lambda p=full: io.open(p, "rb").read()), None
            elif low.endswith(".zip"):
                try:
                    z = zipfile.ZipFile(full)
                except Exception as e:
                    print("  %-58s UNREADABLE ZIP: %s" % (full, e))
                    continue
                for info in z.infolist():
                    if info.filename.lower().endswith(EXTS):
                        yield (("%s!%s" % (full, info.filename)),
                               info.filename,
                               (lambda z=z, n=info.filename: z.read(n)),
                               full)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    delete = "--delete" in sys.argv
    quiet = "--quiet" in sys.argv
    if not args:
        print("usage: art_crc.py <art dir> [--delete] [--quiet]")
        return 2
    root = args[0]
    if not os.path.isdir(root):
        print("not a directory: %s" % root)
        return 2

    total = 0
    bad = []
    for label, name, get, inzip in walk(root):
        total += 1
        try:
            data = get()
        except Exception as e:
            bad.append((label, "unreadable: %s" % e, inzip))
            continue
        ok, why = check(name, data)
        if not ok:
            bad.append((label, why, inzip))
            if not quiet:
                print("  BAD  %-56s %s" % (label, why))

    print("")
    print("checked : %d art file(s) under %s" % (total, root))
    print("bad     : %d" % len(bad))

    if bad and quiet:
        for label, why, _ in bad:
            print("  %-58s %s" % (label, why))

    # DELETING ONLY TOUCHES LOOSE FILES. Rewriting an archive to drop one
    # entry is a different and riskier operation, and a bad entry inside a
    # zip is reported so it can be dealt with deliberately.
    if delete and bad:
        print("")
        loose = [b for b in bad if b[2] is None]
        inzip = [b for b in bad if b[2] is not None]
        for label, _why, _ in loose:
            try:
                os.remove(label)
                print("  deleted %s" % label)
            except Exception as e:
                print("  could NOT delete %s: %s" % (label, e))
        if inzip:
            print("  %d bad entr(ies) left inside archives, not touched:"
                  % len(inzip))
            for label, why, _ in inzip:
                print("    %-56s %s" % (label, why))
    elif bad:
        print("")
        print("re-run with --delete to remove the loose ones")

    return 0


if __name__ == "__main__":
    sys.exit(main())
