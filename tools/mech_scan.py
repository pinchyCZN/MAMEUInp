#!/usr/bin/env python
"""
Resolve MACHINE_MECHANICAL per driver source file.

    python tools/mech_scan.py                 summary + per-class counts
    python tools/mech_scan.py --list full     one filename per line
    python tools/mech_scan.py --list mixed    files with mech AND non-mech games

Classifies every src/mame/drivers/*.cpp as:
    full    every GAME() in the file carries MACHINE_MECHANICAL
    mixed   some do, some do not
    clean   none do

A naive grep for MACHINE_MECHANICAL is wrong: the flags argument is
routinely a per-file macro. mpu4.cpp reads 11/258 unresolved and 257/258
resolved. The indirections that matter:

    #define GAME_FLAGS ...          26 files
    #define MACHINE_FLAGS ...       sam.cpp, bfm_sc4.cpp, bfmsys85.cpp
    #define MACHINE_IS_PINBALL ...  whitestar.cpp only
    #define GAME_CUSTOM(...) GAME(..., GAME_FLAGS)   7 files, REDEFINED
                                    repeatedly mid-file

So the macro table is tracked sequentially by source position, not
collected up front. naomi.cpp, namcos23.cpp and cv1k.cpp also define
GAME_FLAGS but without MECHANICAL - a "file defines GAME_FLAGS" rule
would delete Naomi, which is the check to keep this honest.

Four fruit-machine platforms keep their game lists in .hxx includes and
declare nothing in the .cpp: bfm_ad5, bfm_sc5, maygay1b, mpu5. They are
reported as having no declarations; treat them as mechanical by hand.
"""

import argparse, io, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRVDIR = os.path.join(ROOT, "src", "mame", "drivers")
BASE = ("GAME", "GAMEL", "COMP", "CONS", "SYST")
MECH = re.compile(r"\b(MACHINE_MECHANICAL|MACHINE_IS_SKELETON_MECHANICAL)\b")
HXX_ONLY = ("bfm_ad5.cpp", "bfm_sc5.cpp", "maygay1b.cpp", "mpu5.cpp")


def strip_comments(t):
    t = re.sub(r"/\*.*?\*/", lambda m: chr(10) * m.group(0).count(chr(10)),
               t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def args_of(t, i):
    """Split the argument list that starts just past an open paren."""
    d, j = 1, i
    while j < len(t) and d:
        if t[j] == "(":
            d += 1
        elif t[j] == ")":
            d -= 1
        j += 1
    out, d, cur = [], 0, ""
    for ch in t[i:j - 1]:
        if ch in "([":
            d += 1
        elif ch in ")]":
            d -= 1
        if ch == "," and d == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out, j


def expand(expr, obj, depth=8):
    for _ in range(depth):
        new = re.sub(r"\b([A-Za-z_][A-Za-z0-9_]*)\b",
                     lambda m: obj.get(m.group(1), m.group(1)), expr)
        if new == expr:
            break
        expr = new
    return expr


def scan_file(path):
    t = io.open(path, encoding="utf-8", errors="replace").read()
    t = re.sub(r"\[ \t]*\r?\n", " ", strip_comments(t))

    wrappers = {m.group(1)
                for m in re.finditer(
                    r"#define\s+([A-Za-z_]\w*)\(([^)]*)\)(.*)", t)
                if re.search(r"\b(%s)\s*\(" % "|".join(BASE), m.group(3))}
    inv = re.compile(r"\b(%s)\s*\(" % "|".join(list(BASE) + sorted(wrappers)))
    dfn = re.compile(r"#define\s+([A-Za-z_]\w*)(\([^)]*\))?[ \t]+(.*)")

    events = [(m.start(), "def", m) for m in dfn.finditer(t)]
    events += [(m.start(), "inv", m) for m in inv.finditer(t)]
    events.sort(key=lambda e: e[0])

    obj, func, total, mech = {}, {}, 0, 0
    for _, kind, m in events:
        if kind == "def":
            if m.group(2) is None:
                obj[m.group(1)] = m.group(3).strip()
            else:
                func[m.group(1)] = (m.group(2)[1:-1], m.group(3).strip())
            continue
        macro = m.group(1)
        if macro in BASE:
            a, _ = args_of(t, m.end())
            if len(a) < 9:
                continue
            flags = a[-1]
        else:
            if macro not in func:
                continue
            params, body = func[macro]
            call, _ = args_of(t, m.end())
            bm = re.search(r"\b(%s)\s*\(" % "|".join(BASE), body)
            if not bm:
                continue
            ba, _ = args_of(body, bm.end())
            if len(ba) < 9:
                continue
            flags = ba[-1]
            plist = [x.strip() for x in params.split(",")]
            if flags in plist and len(call) == len(plist):
                flags = call[plist.index(flags)]
        total += 1
        if MECH.search(expand(flags, obj)):
            mech += 1
    return total, mech


def main():
    ap = argparse.ArgumentParser(
        description="Resolve MACHINE_MECHANICAL per driver.")
    ap.add_argument("--list", choices=("full", "mixed", "clean", "nodecl"))
    ns = ap.parse_args()

    cls = {"full": [], "mixed": [], "clean": [], "nodecl": []}
    tg = tm = 0
    for fn in sorted(os.listdir(DRVDIR)):
        if not fn.endswith(".cpp"):
            continue
        total, mech = scan_file(os.path.join(DRVDIR, fn))
        tg += total
        tm += mech
        if not total:
            cls["nodecl"].append(fn)
        elif mech == 0:
            cls["clean"].append(fn)
        elif mech == total:
            cls["full"].append(fn)
        else:
            cls["mixed"].append(fn)

    if ns.list:
        out = cls[ns.list]
        if ns.list == "nodecl":
            out = [f for f in out if f in HXX_ONLY] or out
        sys.stdout.write("\n".join(out) + "\n")
        return 0

    print("driver files scanned  : %d" % sum(len(v) for v in cls.values()))
    print("game declarations     : %d" % tg)
    print("flagged MECHANICAL    : %d" % tm)
    for k in ("full", "mixed", "clean", "nodecl"):
        print("  %-8s %5d" % (k, len(cls[k])))
    print("\nsanity (must all be 0 mech):")
    for f in ("cps1.cpp", "neogeo.cpp", "naomi.cpp", "namcos23.cpp",
              "cv1k.cpp"):
        t, m = scan_file(os.path.join(DRVDIR, f))
        ok = "OK" if m == 0 else "*** FAIL ***"
        print("  %-14s %d/%d %s" % (f, m, t, ok))
    print("sanity (must be fully mechanical):")
    for f in ("mpu4.cpp", "sam.cpp", "whitestar.cpp"):
        t, m = scan_file(os.path.join(DRVDIR, f))
        ok = "OK" if m and m >= t - 1 else "*** FAIL ***"
        print("  %-14s %d/%d %s" % (f, m, t, ok))
    return 0


if __name__ == "__main__":
    sys.exit(main())
