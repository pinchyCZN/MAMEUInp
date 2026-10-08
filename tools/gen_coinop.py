#!/usr/bin/env python
"""
Generate the 'coinop' subtarget: arcade minus mechanical machines.

    python tools/gen_coinop.py            write the pair
    python tools/gen_coinop.py --check    regenerate and diff, write nothing

Writes:
    src/mame/coinop.flt              driver filter  (makelist.py input)
    scripts/target/mame/coinop.lua   target script  (genie input)

Both are required: the .flt decides which drivers reach drivlist.cpp, the
.lua decides which sources are compiled. The .flt must stay a subset of
what the .lua compiles or the link fails on missing GAME_EXTERNs.

Re-run this after moving to a different MAME tag; it derives everything
from that tag's arcade.flt and arcade.lua.

Policy, in one place so it is reviewable:

  DROP    every driver file that is fully mechanical (mech_scan.py 'full'),
          plus the mixed fruit/gambling and pinball platforms, plus the four
          .hxx-only fruit platforms.
  KEEP    six mixed files that are genuine arcade hardware carrying a few
          mechanical entries. Dropping them wholesale would cost real games
          (Sonic Blast Man, the itech8 bowlers, World PK Soccer).

The whole 'pinball' group block is excised rather than just its drivers:
its 14 support devices (genpin, wpc*, decodmd*, s11c_bg, bally audio) are
included by nothing that survives, so leaving them compiles orphan objects.
Verified by scanning every surviving src/mame source for their headers.

Filter comments must use '//'. makelist.py treats a leading '#' as an
import directive and tries to open it as a filename.
"""

import argparse, io, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FLT_OUT = os.path.join(ROOT, "src", "mame", "coinop.flt")
LUA_OUT = os.path.join(ROOT, "scripts", "target", "mame", "coinop.lua")

KEEP_MIXED = {"itech8.cpp", "taito_b.cpp", "segac2.cpp",
              "m107.cpp", "namcond1.cpp", "tapatune.cpp"}
HXX_ONLY = {"bfm_ad5.cpp", "bfm_sc5.cpp", "maygay1b.cpp", "mpu5.cpp"}
STALE = {"unkfr.cpp"}          # in upstream arcade.flt; file does not exist

# ---- console systems added ON TOP of arcade ----
#
# arcade.flt does not carry these at all, so they are an ADD rather than
# a keep. One driver entry each - the games come from a cart file at
# runtime, not from the driver list. notes/console-roadmap.txt
CONSOLE_FLT = ("megadriv.cpp", "snes.cpp")

# Buses arcade.lua ships COMMENTED OUT. Uncommenting is the whole of it
# for two of them; SNES_CTRL is absent from arcade.lua entirely because
# no arcade driver wants it, so it has to be inserted.
# GAMEBOY comes along for the ride: BUSES["SNES"] pulls in bus/snes/sgb.cpp,
# the Super Game Boy cart, which needs the Game Boy cart slot and the DMG
# PPU. Without them the link fails with 19 unresolved gb_cart_slot_/
# dmg_ppu_ symbols.
CONSOLE_BUSES_ON = ("MEGADRIVE", "SNES", "GAMEBOY")
CONSOLE_VIDEOS_ADD = ('VIDEOS["GB_LCD"] = true',)

# ...and the Game Boy APU with it, for the same sgb.cpp reason.
CONSOLE_SOUNDS_ADD = ('SOUNDS["GB_SOUND"] = true',)
CONSOLE_BUSES_ADD = ("SNES_CTRL",)

# Source the two drivers need that arcade does not already compile.
# machine/snes.cpp and includes/megadriv.h are already in, via nss.cpp
# and megatech.cpp. Anchored on a line that is already in the right
# project group, so the files land in a group that is linked.
CONSOLE_FILES = (
    ('src/mame/drivers/nss.cpp",',
     ('src/mame/drivers/snes.cpp",',
      'src/mame/includes/snes.h",',
      'src/mame/machine/snescx4.cpp",',
      'src/mame/machine/snescx4.h",')),
    ('src/mame/drivers/megatech.cpp",',
     ('src/mame/drivers/megadriv.cpp",',
      # megadriv.cpp instantiates the 32X and Sega CD devices even for a
      # plain cartridge machine, so leaving them out compiles and then
      # fails at link with 36 unresolved sega_32x_/sega_segacd_ symbols.
      'src/mame/machine/mega32x.cpp",',
      'src/mame/machine/mega32x.h",',
      'src/mame/machine/megacd.cpp",',
      'src/mame/machine/megacd.h",',
      'src/mame/machine/megacdcd.cpp",',
      'src/mame/machine/megacdcd.h",')),
)


def read(path):
    s = io.open(path, encoding="utf-8", newline="").read()
    nl = "\r\n" if s.count("\r\n") else "\n"
    return s.replace("\r\n", "\n").split("\n"), nl


def drop_set():
    import mech_scan
    full, mixed = [], []
    for fn in sorted(os.listdir(mech_scan.DRVDIR)):
        if not fn.endswith(".cpp"):
            continue
        total, mech = mech_scan.scan_file(os.path.join(mech_scan.DRVDIR, fn))
        if not total or mech == 0:
            continue
        (full if mech == total else mixed).append(fn)
    src, _ = read(os.path.join(ROOT, "src/mame/arcade.flt"))
    arcade = {l.strip() for l in src
              if re.match(r"^[a-z0-9_]+\.cpp$", l.strip())}
    drop = (set(full) | (set(mixed) - KEEP_MIXED)) & arcade
    return drop | (HXX_ONLY & arcade)


def build(drop):
    src, nl = read(os.path.join(ROOT, "src/mame/arcade.flt"))
    body = [l for l in src if l.strip() not in drop and l.strip() not in STALE]
    hdr = ["// coinop.flt - arcade.flt minus every driver source flagged",
           "// MACHINE_MECHANICAL (pinball, fruit/slot, other mechanical).",
           "// Generated by tools/gen_coinop.py - do not hand-edit.",
           "// %d driver sources removed; stale unkfr.cpp dropped."
           % len(drop),
           "// NOTE: comments must use // - makelist.py reads # as import.",
           ""]
    flt = nl.join(hdr + body)

    lua, nl2 = read(os.path.join(ROOT, "scripts/target/mame/arcade.lua"))
    a = next(i for i, l in enumerate(lua)
             if 'createMAMEProjects(_target, _subtarget, "pinball")' in l)
    b = next(i for i in range(a + 1, len(lua))
             if "createMAMEProjects(" in lua[i])
    while b - 1 > a and (lua[b - 1].strip() == ""
                         or lua[b - 1].lstrip().startswith("--")):
        b -= 1
    lua = lua[:a] + lua[b:]
    kept = []
    for l in lua:
        m = re.search(r'src/mame/drivers/([A-Za-z0-9_]+\.cpp)"', l)
        if m and m.group(1) in drop:
            continue
        # linkProjects_mame_* names every group as a library. Excising the
        # pinball group without this leaves links { "pinball" } behind and
        # the build dies at LNK1104: cannot open file 'pinball.lib'.
        if l.strip() == '"pinball",':
            continue
        kept.append(l)
    # BOTH entry points must be renamed. main.lua calls
    # createProjects_mame_<subtarget> AND linkProjects_mame_<subtarget>;
    # missing the second one fails at generate time with
    # "attempt to call a nil value", not at parse time.
    banner = "--   MAME target makefile"
    kept = [l.replace("createProjects_mame_arcade",
                      "createProjects_mame_coinop")
             .replace("linkProjects_mame_arcade",
                      "linkProjects_mame_coinop")
             .replace(banner, banner + " (coinop: arcade minus mech)")
            for l in kept]
    # ---- console systems on top ----
    body2 = flt.split(nl)
    at = max(i for i, l in enumerate(body2)
             if re.match(r"^[a-z0-9_]+\.cpp$", l.strip()))
    body2[at + 1:at + 1] = sorted(CONSOLE_FLT)
    flt = nl.join(body2)

    out = []
    for l in kept:
        st = l.strip()
        for b in CONSOLE_BUSES_ON:
            if st == '--BUSES["%s"] = true' % b:
                l = l.replace("--BUSES", "BUSES")
        out.append(l)
        # AFTER the uncomment above, so test the ORIGINAL text: st still
        # holds the commented form at this point.
        if st == '--BUSES["SNES"] = true':
            for b in CONSOLE_BUSES_ADD:
                out.append('BUSES["%s"] = true' % b)
        if st == 'VIDEOS["SEGA315_5124"] = true':
            out.extend(CONSOLE_VIDEOS_ADD)
        if st == 'SOUNDS["SN76496"] = true':
            out.extend(CONSOLE_SOUNDS_ADD)
        for anchor, adds in CONSOLE_FILES:
            if anchor in l:
                indent = l[:len(l) - len(l.lstrip())]
                for a in adds:
                    out.append(indent + 'MAME_DIR .. "' + a)
    kept = out

    return flt, nl2.join(kept)


def main():
    ap = argparse.ArgumentParser(description="Generate the coinop subtarget.")
    ap.add_argument("--check", action="store_true",
                    help="regenerate and compare; exit 1 if different")
    ns = ap.parse_args()

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    drop = drop_set()
    flt, lua = build(drop)
    print("dropping %d driver sources" % len(drop))

    if ns.check:
        bad = 0
        for path, want in ((FLT_OUT, flt), (LUA_OUT, lua)):
            have = io.open(path, encoding="utf-8", newline="").read() \
                   if os.path.exists(path) else None
            ok = have == want
            print("  %-34s %s" % (os.path.relpath(path, ROOT),
                                  "matches" if ok else "DIFFERS"))
            bad += 0 if ok else 1
        return 1 if bad else 0

    for path, data in ((FLT_OUT, flt), (LUA_OUT, lua)):
        io.open(path, "w", encoding="utf-8", newline="").write(data)
        print("  wrote %s" % os.path.relpath(path, ROOT))

    for fn in ("createProjects_mame_coinop", "linkProjects_mame_coinop"):
        assert ("function %s(" % fn) in lua, "missing entry point: " + fn
    assert "mame_arcade" not in lua, "stale mame_arcade reference remains"
    assert '"pinball",' not in lua, "stale pinball library link remains"
    n = len([l for l in flt.split(chr(10))
             if re.match(r"^[a-z0-9_]+\.cpp$", l.strip())])
    print("coinop.flt: %d driver entries" % n)
    print("verify with: python scripts/build/makelist.py "
          "src/mame/mame.lst src/mame/coinop.flt > NUL")
    return 0


if __name__ == "__main__":
    sys.exit(main())
