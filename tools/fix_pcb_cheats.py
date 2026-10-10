#!/usr/bin/env python3
# license:BSD-3-Clause
# copyright-holders:MAMEUI Netplay
#
# Check and repair the device tags in MAME cheat files.
#
# A cheat addresses memory as <name>.<space><size>@<addr>, and <name> is
# resolved by an EXACT, root-relative subdevice lookup - debugcpu.cpp
# expression_get_device is root_device().subdevice(lowercase(tag)), with no
# search of subtags. So a driver that holds its board in a subdevice wants
# "mainpcb:maincpu", and a cheat written against the flat ":maincpu" fails:
#
#     error parsing cheat expression "maincpu.pb@080CA7=03" (unknown symbol)
#
# THAT MESSAGE IS MISLEADING AND COSTS AN HOUR IF BELIEVED. express.cpp
# parse_symbol_or_number wraps parse_memory_operator in a try/catch that
# discards the real error - INVALID_MEMORY_NAME - and falls through to
# parsing the text as a symbol, which then fails as "unknown symbol". On any
# <name>.<space><size>@ expression it almost always means the tag is wrong.
#
#     python tools/fix_pcb_cheats.py --check B:/cheat/cheat.zip
#     python tools/fix_pcb_cheats.py B:/cheat/cheat.zip --dry-run
#     python tools/fix_pcb_cheats.py B:/cheat/cheat.zip
#     python tools/fix_pcb_cheats.py B:/abcop.xml
#
# TWO INVENTORIES, BOTH OUT OF THE EMULATOR, because the two namespaces are
# separate and a cheat picks between them with its space letter. Generate
# them beside the archive, from the same binary the cheats will run under:
#
#     mamecoinopui.exe -listdevices > B:/cheat/devices.txt
#     mamecoinopui.exe -listxml | grep -E "<machine name=|region=" \
#             > B:/cheat/regions.txt
#
# Measured: 3 s and 8 s for ~19300 drivers. No ROMs and no emulation.
#
# WHAT IS FIXED AUTOMATICALLY, AND WHAT IS ONLY REPORTED. A device tag is
# rewritten only when the name does not resolve AND exactly one real device
# has the same leaf name - "maincpu" against a machine owning only
# "mainpcb:maincpu" is mechanical. Everything else is reported:
#
#   o  REGION references (space "m") are never rewritten. Old cheat files
#      say "user1" and modern MAME renamed many regions to descriptive
#      names, so the correction is a per-driver judgement between gfxrom,
#      maincpu, adpcm:cpu and so on. Guessing would silently point a cheat
#      at the wrong ROM.
#   o  AMBIGUOUS leaf names are left alone. gprider is a twin cabinet with
#      a full "subpcb", so "maincpu" matches two devices and this refuses
#      to choose; arescue and f1en carry a "slavepcb" the same way.
#   o  A tag with no candidate at all is reported, not invented.

import io
import os
import re
import shutil
import sys
import zipfile
from collections import Counter, defaultdict

# express.cpp parse_memory_operator: optional l/p, then the space, then the
# size. One character means the size alone and the space defaults to p.
SPACES = "pdi3orm"
SIZES = "bwdq"
REF = re.compile(r"(?<![:\w.])([A-Za-z0-9_:]+)\.([A-Za-z0-9]{1,3})@")


def decode(mid):
    """(space, size) for the characters between the dot and the @, or None."""
    mid = mid.lower()
    if len(mid) == 1:
        return ("p", mid[0]) if mid[0] in SIZES else None
    if len(mid) == 2:
        return (mid[0], mid[1]) if (mid[0] in SPACES
                                    and mid[1] in SIZES) else None
    if mid[0] in "lp" and mid[1] in SPACES and mid[2] in SIZES:
        return (mid[1], mid[2])
    return None


def refs(text):
    """Every memory reference as (tag, space), tag lowercased, ':' stripped."""
    out = []
    for m in REF.finditer(text):
        d = decode(m.group(2))
        if d is not None:
            out.append((m.group(1).lower().lstrip(":"), d[0]))
    return out


def load_devices(path):
    """machine -> set of full device tags, from -listdevices."""
    head = re.compile(r"^Driver (\S+) \(")
    real = defaultdict(set)
    cur, stack = None, []
    for line in io.open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\r\n")
        h = head.match(line)
        if h:
            cur, stack = h.group(1), []
            continue
        if cur is None or not line.startswith("   "):
            continue
        name = line.strip().split()[0] if line.strip() else ""
        if not name:
            continue
        # "   <root>" is level 0 and two spaces per level below it. A level
        # 1 device is the FIRST tag component, so it sits at stack index 0 -
        # truncating to [level:] instead of [level-1:] leaves a sibling in
        # place and yields nonsense like "audiocpu:maincpu".
        level = (len(line) - len(line.lstrip(" ")) - 3) // 2
        if name == "<root>":
            stack = []
            continue
        if level < 1:
            continue
        del stack[level - 1:]
        stack.append(name)
        real[cur].add(":".join(stack).lower())
    return real


def load_regions(path):
    """machine -> set of region names, from -listxml."""
    mach = re.compile(r'<machine name="([^"]+)"(.*)$')
    reg = re.compile(r'region="([^"]+)"')
    out = defaultdict(set)
    cur, skip = None, True
    for line in io.open(path, encoding="utf-8", errors="replace"):
        m = mach.search(line)
        if m:
            cur = m.group(1)
            skip = 'isdevice="yes"' in m.group(2)
            continue
        if cur is None or skip:
            continue
        for r in reg.findall(line):
            out[cur].add(r.lower())
    return out


def inventories(zip_or_xml, dev_override, reg_override):
    base = os.path.dirname(os.path.abspath(zip_or_xml))
    dev = dev_override or os.path.join(base, "devices.txt")
    rgn = reg_override or os.path.join(base, "regions.txt")
    for p in (dev, rgn):
        if not os.path.isfile(p):
            print("missing inventory: %s" % p)
            print("")
            print("generate both beside the archive:")
            print("  mamecoinopui.exe -listdevices > devices.txt")
            print('  mamecoinopui.exe -listxml | grep -E "<machine name=|'
                  'region=" > regions.txt')
            return None, None
    return load_devices(dev), load_regions(rgn)


def verdict(stem, devices, regions):
    """(bad, fixes) for one machine's cheat text.

    KEYED BY (tag, space), NOT BY TAG. The same name can appear in both
    namespaces in one file - "maincpu.pb@" is a device and "maincpu.mw@" is
    a region - and collapsing them made the verdict depend on set iteration
    order, so the reported counts moved between runs.
    """
    def judge(text):
        bad, fixes = {}, {}
        for tag, space in sorted(set(refs(text))):
            if space == "m":
                if tag not in regions.get(stem, ()):
                    bad[(tag, "region")] = []
                continue
            if tag in devices.get(stem, ()):
                continue
            leaf = tag.split(":")[-1]
            cand = sorted(d for d in devices.get(stem, ())
                          if d.split(":")[-1] == leaf)
            bad[(tag, "device")] = cand
            if len(cand) == 1:
                fixes[tag] = cand[0]
        return bad, fixes
    return judge


def rewrite(text, fixes):
    """Substitute per match, deciding on that match's own space letter.

    A BLIND "tag." SUBSTITUTION WOULD CORRUPT REGION REFERENCES. markham
    says sub.mb@ and others say sub.pb@; rewriting the text wholesale would
    move a region reference onto a device tag that cannot serve it.
    """
    if not fixes:
        return text, 0
    hits = [0]

    def rep(m):
        tag = m.group(1).lower().lstrip(":")
        d = decode(m.group(2))
        if (d is None) or (d[0] == "m") or (tag not in fixes):
            return m.group(0)
        hits[0] += 1
        return fixes[tag] + "." + m.group(2) + "@"

    return REF.sub(rep, text), hits[0]


def entries(path):
    """[(key, text)] for a zip or a lone xml, key is the driver short name."""
    if path.lower().endswith(".zip"):
        z = zipfile.ZipFile(path, "r")
        out = []
        for info in z.infolist():
            if info.filename.lower().endswith(".xml"):
                stem = os.path.splitext(os.path.basename(info.filename))[0]
                out.append((info.filename, stem,
                            z.read(info.filename).decode("ascii", "replace")))
        z.close()
        return out
    stem = os.path.splitext(os.path.basename(path))[0]
    return [(path, stem,
             io.open(path, "rb").read().decode("ascii", "replace"))]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = [a for a in sys.argv[1:] if a.startswith("--")]
    check = "--check" in opts
    dry = "--dry-run" in opts
    dev_override = reg_override = None
    for o in opts:
        if o.startswith("--devices="):
            dev_override = o.split("=", 1)[1]
        if o.startswith("--regions="):
            reg_override = o.split("=", 1)[1]
    if not args:
        print("usage: fix_pcb_cheats.py [--check] [--dry-run] "
              "<cheat.zip | game.xml>")
        return 2
    path = args[0]

    devices, regions = inventories(path, dev_override, reg_override)
    if devices is None:
        return 2
    print("machines with devices  : %d" % len(devices))
    print("machines with regions  : %d" % len(regions))

    rows = entries(path)
    print("cheat xml entries      : %d" % len(rows))

    absent, clean = 0, 0
    allbad = defaultdict(Counter)       # (tag, kind, candidates) -> files
    fixable = {}                        # name -> (text, fixes, count)
    for name, stem, text in rows:
        if stem not in devices:
            absent += 1
            continue
        bad, fixes = verdict(stem, devices, regions)(text)
        if not bad:
            clean += 1
            continue
        for (tag, kind), cand in bad.items():
            allbad[(tag, kind, tuple(cand))][stem] += 1
        if fixes:
            new, n = rewrite(text, fixes)
            fixable[name] = (new.encode("ascii", "replace"), fixes, n)

    print("machine not in build   : %d" % absent)
    print("all tags resolve       : %d" % clean)
    print("files with bad tags    : %d" % len({s for c in allbad.values()
                                               for s in c}))
    print("  of those, auto-fixable: %d files" % len(fixable))
    print("")
    print("%-20s %-7s %-26s %s" % ("written in cheat", "space", "real tag",
                                   "files"))
    print("-" * 92)
    for (tag, kind, cand), who in sorted(allbad.items(),
                                         key=lambda kv: -len(kv[1])):
        if kind == "region":
            fix = "(region, report only)"
        elif len(cand) == 1:
            fix = cand[0]
        elif cand:
            fix = "AMBIGUOUS: " + ",".join(cand)
        else:
            fix = "(no device by that leaf name)"
        names = sorted(who)
        print("%-20s %-7s %-26s %4d  %s" % (
            tag, kind, fix, len(names),
            " ".join(names[:5]) + ("..." if len(names) > 5 else "")))

    if check or not fixable:
        return 0

    print("")
    print("applying %d file(s), %d expression(s)"
          % (len(fixable), sum(n for _, _, n in fixable.values())))
    if dry:
        print("(dry run, nothing written)")
        return 0

    if not path.lower().endswith(".zip"):
        io.open(path, "wb").write(fixable[path][0])
        print("rewrote %s" % path)
        return 0

    # REWRITE WHOLE, PRESERVING EVERY OTHER ENTRY BYTE FOR BYTE, its
    # timestamp and its compression method. 18k files go through here and
    # only a few dozen should differ afterwards.
    zin = zipfile.ZipFile(path, "r")
    tmp = path + ".new"
    zout = zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED)
    copied = 0
    for info in zin.infolist():
        data = fixable.get(info.filename, (None,))[0]
        if data is None:
            data = zin.read(info.filename)
            copied += 1
        out = zipfile.ZipInfo(info.filename, date_time=info.date_time)
        out.compress_type = info.compress_type
        out.external_attr = info.external_attr
        zout.writestr(out, data)
    zout.close()
    zin.close()
    shutil.move(tmp, path)
    print("rewrote %s" % path)
    print("  entries: %d changed, %d copied" % (len(fixable), copied))
    return 0


if __name__ == "__main__":
    sys.exit(main())
