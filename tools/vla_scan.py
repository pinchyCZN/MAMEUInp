#!/usr/bin/env python
"""
Find variable-length arrays - declarations whose size is not a constant.

    cd src/osd/winui && python ../../../tools/vla_scan.py
    python tools/vla_scan.py PATH ...

VLAs are a GCC extension and not C++. MSVC rejects each with C2131 and
then reports a cascade of dependent errors - a later s[4] = 0 becomes
C3863, an array passed to an overload set becomes ambiguous - so 18
declarations produced roughly 25 messages. See notes/upstream-edits.txt
1.5.

THIS DOES NOT GUESS THE SHAPE, and that is the point. Two earlier
hand-written greps missed VLAs: the first looked only for alloca, the
second only for sizes spelled strlen/.size()/.length(), and the real
code also wrote char t2[nFilenameLength]. Here every TYPE NAME[EXPR]
declaration is matched and then EXPR is tested for constness by
requiring every identifier in it to be ALL_CAPS. A lowercase identifier
in an array bound is a variable.

With no PATH, scans *.cpp and *.h in the current directory.
"""

import glob
import io
import re
import sys

ENC = "latin-1"   # byte-exact; MAME sources are not all UTF-8

DECL = re.compile(
    r'^(?P<ind>\s*)(?P<type>(?:const\s+)?(?:unsigned\s+)?'
    r'(?:char|TCHAR|WCHAR|wchar_t|int|BYTE|LPSTR|LPTSTR|LPWSTR|DWORD|UINT|'
    r'short|long|float|double|bool|u?int(?:8|16|32|64)_t))\s+'
    r'(?P<name>[A-Za-z_]\w*)\s*\[(?P<expr>[^\]]*)\]\s*(?P<tail>[;,=].*)$')


def const_expr(e):
    """True if the bracket expression is a compile-time constant."""
    e = e.strip()
    if not e:
        return True                       # char x[] = "..."
    for ident in re.findall(r'[A-Za-z_]\w*', e):
        if ident != ident.upper():        # lowercase -> a variable
            return False
    return True


def main(argv):
    paths = argv[1:] or sorted(glob.glob("*.cpp") + glob.glob("*.h"))
    hits = 0
    for fn in paths:
        try:
            fp = io.open(fn, encoding=ENC, errors="replace")
        except IOError as e:
            sys.stderr.write("%s: %s\n" % (fn, e))
            continue
        with fp:
            for i, line in enumerate(fp, 1):
                m = DECL.match(line.rstrip("\r\n"))
                if m and not const_expr(m.group("expr")):
                    print("%s:%d: %s[%s]" % (
                        fn, i, m.group("name"), m.group("expr").strip()))
                    hits += 1
    print("%d non-constant array declaration(s)" % hits)
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
