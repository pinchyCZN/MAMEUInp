#!/usr/bin/env python3
# license:BSD-3-Clause
# copyright-holders:MAMEUI Netplay
#
# What did the coinop subtarget actually leave out?
#
# coinop.flt is arcade.flt minus every driver source flagged
# MACHINE_MECHANICAL - notes/coinop-subtarget.txt. That says which SOURCE
# FILES went; this says which MACHINES, by name, which is the list a person
# wants when a game they expected is missing.
#
#     python tools/gen_dropped_list.py                 to stdout
#     python tools/gen_dropped_list.py B:/path/out.txt
#
# IT ALSO REPORTS THE LEAKS, which is half the point. Two kinds survive the
# filter:
#
#   o a file upstream never flagged mechanical at all. bfm_sc4.cpp defines
#     MACHINE_FLAGS as MACHINE_NOT_WORKING|MACHINE_CLICKABLE_ARTWORK with no
#     MECHANICAL bit, so its 3119 Scorpion 4 fruit machines are invisible to
#     a flag-driven filter. Nothing is wrong with the filter; the flag is
#     simply not set.
#
#   o a file whose machines are declared with GAMEL. THE LAST ARGUMENT OF
#     GAMEL IS THE LAYOUT, NOT THE FLAGS, so reading flags at [-1] - which
#     mech_scan.py does - tests the layout name and never matches. Every
#     mechanical GAMEL machine is missed. segaufo.cpp, seven crane machines,
#     is the clearest case.
#
# This file resolves GAMEL at [-2] and is therefore the corrected view.
# mech_scan.py still has the original behaviour; the two disagree on
# purpose until that is fixed.

import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mech_scan                                            # noqa: E402

ROOT = mech_scan.ROOT
DRVDIR = mech_scan.DRVDIR
MECH = mech_scan.MECH


def flags_and_name(macro, a):
    # GAME/COMP/CONS/SYST: ... company, fullname, flags
    # GAMEL:               ... company, fullname, flags, layout
    if macro == "GAMEL":
        if len(a) < 4:
            return None, None
        return a[-2], a[-3]
    if len(a) < 3:
        return None, None
    return a[-1], a[-2]


def unquote(s):
    if s is None:
        return "?"
    s = s.strip()
    m = re.findall(r'"((?:[^"\\]|\\.)*)"', s)
    if m:
        return "".join(m).replace('\\"', '"')
    return s


def scan(path):
    """Returns (total, [(shortname, fullname, mechanical)]).

    FOLLOWS #include OF A .hxx IN THE SAME DIRECTORY, because four fruit
    platforms keep their entire game list there and have no GAME() in the
    .cpp at all - bfm_sc5sw.hxx alone is 1674 machines. Counting only the
    .cpp reports those files as empty.
    """
    t = io.open(path, encoding="utf-8", errors="replace").read()

    for inc in re.findall(r'#include\s+"([a-z0-9_]+\.hxx)"', t):
        side = os.path.join(os.path.dirname(path), inc)
        if os.path.exists(side):
            t += "\n" + io.open(side, encoding="utf-8",
                                errors="replace").read()

    t = re.sub(r"\[ \t]*\r?\n", " ", mech_scan.strip_comments(t))

    wrappers = {m.group(1)
                for m in re.finditer(
                    r"#define\s+([A-Za-z_]\w*)\(([^)]*)\)(.*)", t)
                if re.search(r"\b(%s)\s*\(" % "|".join(mech_scan.BASE),
                             m.group(3))}
    inv = re.compile(r"\b(%s)\s*\("
                     % "|".join(list(mech_scan.BASE) + sorted(wrappers)))
    dfn = re.compile(r"#define\s+([A-Za-z_]\w*)(\([^)]*\))?[ \t]+(.*)")

    events = [(m.start(), "def", m) for m in dfn.finditer(t)]
    events += [(m.start(), "inv", m) for m in inv.finditer(t)]
    events.sort(key=lambda e: e[0])

    obj, func, out = {}, {}, []
    for _, kind, m in events:
        if kind == "def":
            if m.group(2) is None:
                obj[m.group(1)] = m.group(3).strip()
            else:
                func[m.group(1)] = (m.group(2)[1:-1], m.group(3).strip())
            continue

        macro = m.group(1)
        if macro in mech_scan.BASE:
            a, _ = mech_scan.args_of(t, m.end())
            if len(a) < 9:
                continue
            flags, name = flags_and_name(macro, a)
            short = a[1].strip()
        else:
            if macro not in func:
                continue
            params, body = func[macro]
            call, _ = mech_scan.args_of(t, m.end())
            bm = re.search(r"\b(%s)\s*\(" % "|".join(mech_scan.BASE), body)
            if not bm:
                continue
            base = bm.group(1)
            ba, _ = mech_scan.args_of(body, bm.end())
            if len(ba) < 9:
                continue
            flags, name = flags_and_name(base, ba)
            plist = [x.strip() for x in params.split(",")]
            # a wrapper usually forwards its own parameters positionally
            def thru(v):
                if v is not None and v in plist and len(call) == len(plist):
                    return call[plist.index(v)]
                return v
            flags, name = thru(flags), thru(name)
            short = thru(ba[1].strip()) if len(ba) > 1 else "?"

        if flags is None:
            continue
        mech = bool(MECH.search(mech_scan.expand(flags, obj)))
        out.append((unquote(short), unquote(name), mech))
    return len(out), out


def flt_sources(path):
    src = io.open(path, encoding="utf-8", errors="replace").read()
    return {l.strip() for l in src.split("\n")
            if re.match(r"^[a-z0-9_]+\.cpp$", l.strip())}


def driver_names(flt):
    """The driver set a filter selects, from MAME's OWN makelist.py.

    THE SPINE OF THE LIST, because it is the same tool the build uses and
    therefore cannot disagree with what was compiled. The macro parser
    above supplies titles, but it resolves about two thirds of the
    declarations - wrapper macros defeat it - so it must not decide
    membership.
    """
    import subprocess
    out = subprocess.run(
            [sys.executable,
             os.path.join(ROOT, "scripts/build/makelist.py"),
             os.path.join(ROOT, "src/mame/mame.lst"),
             os.path.join(ROOT, flt)],
            capture_output=True, text=True, errors="replace")
    return set(re.findall(r"GAME_EXTERN\(([^)]+)\)", out.stdout))


def main():
    arcade = flt_sources(os.path.join(ROOT, "src/mame/arcade.flt"))
    coinop = flt_sources(os.path.join(ROOT, "src/mame/coinop.flt"))
    dropped = sorted(arcade - coinop)

    gone = driver_names("src/mame/arcade.flt") - driver_names(
            "src/mame/coinop.flt")

    drop_rows, leak_rows = [], []
    titles = {}                 # shortname -> (fullname, source)

    for fn in dropped:
        p = os.path.join(DRVDIR, fn)
        if not os.path.exists(p):
            drop_rows.append((fn, 0, []))
            continue
        total, games = scan(p)
        for short, name, _ in games:
            titles.setdefault(short, (name, fn))
        drop_rows.append((fn, total, games))

    drop_total = len(gone)
    named = sum(1 for s in gone if s in titles)

    # group the authoritative set by the source its title came from, so a
    # name we could not resolve still appears rather than vanishing
    by_src = {}
    for s in sorted(gone):
        src = titles.get(s, (None, "(source not resolved)"))[1]
        by_src.setdefault(src, []).append(s)

    for fn in sorted(coinop):
        p = os.path.join(DRVDIR, fn)
        if not os.path.exists(p):
            continue
        total, games = scan(p)
        mech = [g for g in games if g[2]]
        if mech:
            leak_rows.append((fn, total, mech))

    out = []
    w = out.append
    w("MAMEUI coinop subtarget - machines excluded as mechanical")
    w("Generated by tools/gen_dropped_list.py. Do not hand-edit.")
    w("See notes/coinop-subtarget.txt for how the set is derived.")
    w("")
    w("The coinop driver set is MAME's arcade set minus every driver source")
    w("flagged MACHINE_MECHANICAL - pinball, fruit, slot, crane, redemption")
    w("and other machines with moving parts. This is what went.")
    w("")
    w("SUMMARY")
    w("  driver sources in arcade.flt : %d" % len(arcade))
    w("  driver sources in coinop.flt : %d" % len(coinop))
    w("  driver sources dropped       : %d" % len(dropped))
    w("  machines dropped             : %d" % drop_total)
    w("  of those, title resolved     : %d" % named)
    w("")
    w("  The machine count is makelist.py's, the same tool the build uses.")
    w("  Titles come from parsing the GAME macros, which does not resolve")
    w("  every wrapper macro - an unresolved one is listed by short name.")
    w("")
    w("=" * 72)
    w("1. DROPPED SOURCE FILES")
    w("=" * 72)
    w("")
    for fn, total, _ in drop_rows:
        w("  %-28s %5d machine(s) parsed" % (fn, total))
    w("")
    w("=" * 72)
    w("2. EVERY DROPPED MACHINE")
    w("=" * 72)
    for src in sorted(by_src):
        w("")
        w("---- %s (%d) ----" % (src, len(by_src[src])))
        for s in by_src[src]:
            name = titles.get(s, (None, None))[0]
            w("  %-12s %s" % (s, name if name else ""))
    w("")
    w("=" * 72)
    w("3. STILL IN THE BUILD BUT FLAGGED MECHANICAL")
    w("=" * 72)
    w("")
    w("Six files are kept on purpose - genuine arcade hardware carrying a")
    w("few mechanical entries (notes/coinop-subtarget.txt 4). The rest are")
    w("leaks: their machines are declared with GAMEL, whose LAST argument")
    w("is the layout rather than the flags, so the flag test in")
    w("mech_scan.py reads the wrong field and never matches.")
    w("")
    nleak = 0
    for fn, total, mech in leak_rows:
        nleak += len(mech)
        w("---- %s (%d of %d mechanical) ----" % (fn, len(mech), total))
        for short, name, _ in mech:
            w("  %-12s %s" % (short, name))
        w("")
    w("  mechanical machines still in the build: %d" % nleak)
    w("")
    w("=" * 72)
    w("4. NOT CAUGHT BY A FLAG FILTER AT ALL")
    w("=" * 72)
    w("")
    w("bfm_sc4.cpp defines")
    w("    #define MACHINE_FLAGS MACHINE_NOT_WORKING|MACHINE_CLICKABLE_ARTWORK")
    w("with no MECHANICAL bit, so its Scorpion 4 fruit machines carry no")
    w("flag for the filter to find. They are Bell-Fruit AWP gambling")
    w("cabinets - Deal or No Deal, Daylight Robbery and so on - marked NOT")
    w("WORKING, and what shows on screen is the artwork layout's reel and")
    w("LCD panel rather than an emulated game. Dropping them is a one-line")
    w("addition to the generator, not a filter fix.")
    w("")

    text = "\n".join(out) + "\n"
    if len(sys.argv) > 1:
        d = os.path.dirname(os.path.abspath(sys.argv[1]))
        if d and not os.path.isdir(d):
            os.makedirs(d)
        io.open(sys.argv[1], "w", encoding="ascii",
                errors="replace", newline="\r\n").write(text)
        print("wrote %s  (%d dropped machines, %d mechanical still in)"
              % (sys.argv[1], drop_total, nleak))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
