#!/usr/bin/env python
"""
Re-pad RFC table rows so each | row matches the +---+ border above it.

    python tools/tblfix.py PATH ...

asciify.py --check reports these but deliberately will not repair them.
This does the mechanical part: it adjusts padding inside the LAST cell of
a row, never the text. A row whose content genuinely does not fit still
has to be rewrapped by hand, and is reported instead of mangled.
"""
import io, re, sys

BORDER = re.compile(r"^(\s*)\+[-=+]+\+\s*$")


def fix(path):
    s = io.open(path, encoding="utf-8", newline="").read()
    nl = "\r\n" if s.count("\r\n") else "\n"
    lines = s.replace("\r\n", "\n").split("\n")
    width, changed, stuck = None, 0, []
    for i, ln in enumerate(lines):
        if BORDER.match(ln):
            width = len(ln.rstrip())
            continue
        if width is None or not ln.lstrip().startswith("|"):
            continue
        cur = ln.rstrip()
        if len(cur) == width:
            continue
        if len(cur) < width:
            lines[i] = cur[:-1] + " " * (width - len(cur)) + "|"
            changed += 1
        else:
            over = len(cur) - width
            body = cur[:-1]
            trimmed = re.sub(r" {%d,}$" % (over + 1),
                             lambda m: m.group(0)[over:], body)
            if trimmed == body:
                stuck.append(i + 1)
                continue
            lines[i] = trimmed + "|"
            changed += 1
    io.open(path, "w", encoding="utf-8", newline="").write(nl.join(lines))
    print("%s: %d row(s) re-padded" % (path, changed))
    for n in stuck:
        print("  line %d: too wide, no padding to remove - rewrap by hand"
              % n)
    return 1 if stuck else 0


if __name__ == "__main__":
    sys.exit(max(fix(p) for p in sys.argv[1:]) if len(sys.argv) > 1 else 2)
