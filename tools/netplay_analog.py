#!/usr/bin/env python3
# license:BSD-3-Clause
# copyright-holders:MAMEUI Netplay
#
# How wide does an analog axis have to be on the wire?
#
# protocol.txt 1.6.1 fixed the word at 32 digital bits plus four 8-bit analog
# slots from a 26-driver sample. No lightgun driver was in that sample, and the
# slots were never filled, so the analog half of the format is unmeasured. This
# measures it across the whole driver set.
#
# ROM-FREE, because it reads -listxml rather than a running machine.
# info.cpp output_ports emits one <analog mask="N"/> per analog field, and
# output_input emits one <control> per (player, control type) with the
# PORT_MINMAX range and the default sensitivity. Between them that is every
# number the wire format needs, for every driver, without owning a single ROM.
#
#     mamecoinopui.exe -listxml > coinop.xml        (run from run/agent, rule 11)
#     python tools/netplay_analog.py coinop.xml
#     python tools/netplay_analog.py coinop.xml --csv analog.csv
#     python tools/netplay_analog.py coinop.xml --driver area51
#
# WHAT A MASK MEANS HERE. field.mask() is the GAME-VISIBLE width - the bits
# ioport_port::read() can deliver to the driver - so it bounds how much
# resolution is worth transmitting. It is not the width of what MAME stores:
# analog_field::m_accum is an s32 in device space, and ioport.cpp record_port
# writes it as a full s32. The mask tells us what a peer can observe; m_accum
# is what a peer has to agree on. Both figures are reported.
#
# LIMITATION: <control> aggregates by control type, so a lightgun's X and Y
# collapse into one entry and per-player axis attribution is inferred from the
# total axis count rather than read off. Drivers where that inference is not
# clean are listed under "ragged" and need the runtime dump
# (tools/netplay_portdump.lua) to settle. The axis WIDTHS below are exact
# either way, because every axis emits its own mask.

import argparse
import collections
import re
import sys

# ioport.cpp analog_field::analog_field, the switch on field.type():
#   absolute, never wraps        stick paddle pedal lightgun
#   relative, m_wraps = true     dial trackball mouse
#   absolute, wraps if PORT_WRAPS   positional
# A wrapping axis is the hard case: frame_update skips apply_min_max when
# m_wraps, so m_accum is a running total with no bound and no fixed-width
# quantisation of it is safe.
ABSOLUTE = ("stick", "paddle", "pedal", "lightgun")
WRAPPING = ("dial", "trackball", "mouse")
MAYBE_WRAPS = ("positional",)

# How many axes one <control type=...> can stand for, from the case labels in
# info.cpp that share a type string.
AXES_PER_TYPE = {
    "stick": 3,         # AD_STICK_X, _Y, _Z
    "paddle": 2,        # PADDLE, PADDLE_V
    "pedal": 3,         # PEDAL, PEDAL2, PEDAL3
    "lightgun": 2,      # LIGHTGUN_X, _Y
    "positional": 2,    # POSITIONAL, POSITIONAL_V
    "dial": 2,          # DIAL, DIAL_V
    "trackball": 2,     # TRACKBALL_X, _Y
    "mouse": 2,         # MOUSE_X, _Y
}

RE_MACHINE = re.compile(r'<machine name="([^"]+)"(.*)>')
RE_ATTR = re.compile(r'(\w+)="([^"]*)"')
RE_INPUT = re.compile(r'<input players="(\d+)"')
RE_CONTROL = re.compile(r'<control (.*?)/>')
RE_ANALOG = re.compile(r'<analog mask="(\d+)"/>')


def mask_bits(mask):
    # Masks are contiguous runs, so a popcount is the width. Counting set bits
    # rather than using bit_length() keeps a shifted mask honest.
    return bin(mask).count("1")


def span_bits(lo, hi):
    # Bits to hold the values lo..hi inclusive. hi-lo rather than hi-lo+1:
    # 256 distinct values fit in 8 bits, and bit_length() of 256 is 9.
    d = hi - lo
    return d.bit_length() if d > 0 else 1


class Driver(object):
    def __init__(self, name, attrs):
        self.name = name
        self.source = attrs.get("sourcefile", "?")
        self.cloneof = attrs.get("cloneof")
        self.isdevice = (attrs.get("isdevice") == "yes")
        self.runnable = (attrs.get("runnable", "yes") == "yes")
        self.players = 0
        self.controls = []      # dicts from <control>
        self.masks = []         # one int per analog field

    @property
    def axes(self):
        return len(self.masks)

    @property
    def analog_controls(self):
        return [c for c in self.controls if c["type"] in AXES_PER_TYPE]

    @property
    def analog_players(self):
        # info.cpp output_input emits player= ONLY when nplayer > 1, so a
        # one-player driver has analog controls with no player attribute.
        # Reading that as "no player" put every single-player gun game -
        # timecris, golgo13 - at zero axes per player.
        who = sorted(set(int(c["player"]) for c in self.analog_controls
                         if c.get("player")))
        if not who and self.analog_controls:
            return [1]
        return who

    def widest_mask(self):
        return max((mask_bits(m) for m in self.masks), default=0)

    def wraps(self):
        return any(c["type"] in WRAPPING for c in self.analog_controls)

    def maybe_wraps(self):
        return any(c["type"] in MAYBE_WRAPS for c in self.analog_controls)

    def axes_per_player(self):
        # Inferred, not read: see the LIMITATION note at the top.
        n = len(self.analog_players)
        if n == 0:
            return 0, False
        if self.axes % n != 0:
            return (self.axes + n - 1) // n, True
        return self.axes // n, False

    def digital_estimate(self):
        # A BOUND, NOT A MEASUREMENT. One bit per joystick direction, one per
        # button, plus start and coin. inputmap::build() computes the real
        # figure (m_logical.size()) from the live port list at session start;
        # this only has to be good enough to sanity-check a word size.
        bits = 2                                    # start + coin
        for c in self.controls:
            t = c["type"]
            if t == "joy":
                bits += 4
            elif t == "doublejoy":
                bits += 8
            elif t == "triplejoy":
                bits += 12
            try:
                bits += int(c.get("buttons", 0) or 0)
            except ValueError:
                pass
        return bits


def parse(path):
    drivers = []
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.strip()
            if s.startswith("<machine "):
                m = RE_MACHINE.match(s)
                if m:
                    cur = Driver(m.group(1), dict(RE_ATTR.findall(m.group(2))))
                continue
            if cur is None:
                continue
            if s.startswith("</machine>"):
                drivers.append(cur)
                cur = None
                continue
            if s.startswith("<input "):
                m = RE_INPUT.search(s)
                if m:
                    cur.players = int(m.group(1))
                continue
            if s.startswith("<control "):
                m = RE_CONTROL.search(s)
                if m:
                    cur.controls.append(dict(RE_ATTR.findall(m.group(1))))
                continue
            if s.startswith("<analog "):
                m = RE_ANALOG.search(s)
                if m:
                    cur.masks.append(int(m.group(1)))
                continue
    return drivers


def report(drivers, parents_only):
    pool = [d for d in drivers
            if not d.isdevice and d.runnable
            and (not parents_only or d.cloneof is None)]
    analog = [d for d in pool if d.axes > 0]

    print("drivers scanned      : %d" % len(pool))
    print("with an analog axis  : %d (%.1f%%)"
          % (len(analog), 100.0 * len(analog) / max(1, len(pool))))
    print("")

    # ---- axis width, the number the word has to carry ----
    width = collections.Counter()
    for d in analog:
        for m in d.masks:
            width[mask_bits(m)] += 1
    total_axes = sum(width.values())

    print("AXIS WIDTH (game-visible, from field.mask)")
    print("  bits  axes   share  cumulative")
    run = 0
    for b in sorted(width):
        run += width[b]
        print("  %4d  %5d  %5.1f%%  %5.1f%%"
              % (b, width[b], 100.0 * width[b] / total_axes,
                 100.0 * run / total_axes))
    print("  total %5d axes across %d drivers" % (total_axes, len(analog)))
    print("")

    # ---- axes per driver and per player ----
    per_drv = collections.Counter(d.axes for d in analog)
    print("AXES PER DRIVER")
    for n in sorted(per_drv):
        print("  %2d axes : %5d drivers" % (n, per_drv[n]))
    print("")

    per_ply = collections.Counter()
    ragged = []
    for d in analog:
        n, odd = d.axes_per_player()
        per_ply[n] += 1
        if odd:
            ragged.append(d)
    print("AXES PER PLAYER (inferred)")
    for n in sorted(per_ply):
        print("  %2d axes/player : %5d drivers" % (n, per_ply[n]))
    print("  ragged (axes not divisible by analog players): %d" % len(ragged))
    for d in ragged[:12]:
        print("      %-12s %2d axes / %d players  %s"
              % (d.name, d.axes, len(d.analog_players), d.source))
    print("")

    # ---- by control type: how wide, and does it wrap ----
    by_type = {}
    for d in analog:
        for c in d.analog_controls:
            t = c["type"]
            e = by_type.setdefault(t, {"n": 0, "span": 0, "who": "",
                                       "sens": set()})
            e["n"] += 1
            if c.get("minimum") and c.get("maximum"):
                sb = span_bits(int(c["minimum"]), int(c["maximum"]))
                if sb > e["span"]:
                    e["span"] = sb
                    e["who"] = d.name
            if c.get("sensitivity"):
                e["sens"].add(int(c["sensitivity"]))

    print("BY CONTROL TYPE")
    print("  type        count  wraps  widest span  worst driver  sensitivity")
    for t in sorted(by_type):
        e = by_type[t]
        w = "yes" if t in WRAPPING else ("maybe" if t in MAYBE_WRAPS else "no")
        sens = sorted(e["sens"])
        srange = ("%d-%d" % (sens[0], sens[-1])) if sens else "-"
        print("  %-11s %5d  %-5s  %7d bits  %-12s  %s"
              % (t, e["n"], w, e["span"], e["who"], srange))
    print("")

    # ---- what m_accum itself can hold, which is what has to agree ----
    #
    # analog_field::frame_update stores apply_inverse_sensitivity(rawvalue)
    # for an absolute axis, which is rawvalue * 100 / m_sensitivity over a
    # raw range of INPUT_ABSOLUTE_MIN..MAX = -65536..65536 (input.h). So a
    # LOW sensitivity is what makes m_accum large, and the lowest default in
    # the set decides the width of the field that has to carry it.
    lo_sens = None
    lo_who = ""
    for d in analog:
        for c in d.analog_controls:
            if not c.get("sensitivity"):
                continue
            s = int(c["sensitivity"])
            if s > 0 and ((lo_sens is None) or (s < lo_sens)):
                lo_sens = s
                lo_who = "%s (%s)" % (d.name, c["type"])
    print("m_accum RANGE (absolute axes: raw * 100 / sensitivity)")
    print("  raw range                 : %d .. %d" % (-65536, 65536))
    if lo_sens:
        reach = (65536 * 100) // lo_sens
        print("  lowest default sensitivity: %d   %s" % (lo_sens, lo_who))
        print("  so m_accum reaches        : +/- %d = %d bits + sign"
              % (reach, reach.bit_length()))
    print("  wrapping axes are not bounded at all - frame_update skips")
    print("  apply_min_max when m_wraps, so m_accum is a running total.")
    print("")

    # ---- the wrapping set, which cannot be quantised ----
    wrap = [d for d in analog if d.wraps()]
    mw = [d for d in analog if d.maybe_wraps()]
    print("WRAPPING AXES (m_accum unbounded - needs s32, not a quantised field)")
    print("  drivers with dial/trackball/mouse : %d" % len(wrap))
    print("  drivers with positional           : %d (wraps only w/ PORT_WRAPS)"
          % len(mw))
    print("")

    # ---- worst case word, option B ----
    # THE DIGITAL HALF IS NOT IN QUESTION and is deliberately not estimated
    # here. inputmap::build() counts it exactly at session start and refuses
    # above DIGITAL_BITS, so 32 is both the measured worst case
    # (protocol.txt 1.6.1) and the enforced ceiling. The new number is the
    # analog cost, which is what this reports.
    #
    # Per-player axis width uses that driver's WIDEST mask for every one of
    # its axes, so each figure is an upper bound rather than a sum of the
    # real widths: listxml gives masks without a player, so the two cannot
    # be joined here (see LIMITATION).
    print("ANALOG COST PER PLAYER (upper bound: axes/player x widest mask)")
    print("")
    rows = []
    for d in analog:
        ap, _ = d.axes_per_player()
        if ap == 0:
            continue
        wm = d.widest_mask()
        rows.append((ap * wm, ap * 32, d, ap, wm))
    rows.sort(key=lambda r: (-r[1], -r[0]))
    print("  driver        src                 ax  w    at mask   at s32")
    for bits_mask, bits_s32, d, ap, wm in rows[:14]:
        print("  %-12s  %-17s  %2d  %2d  %5d b   %5d b"
              % (d.name, d.source.replace(".cpp", ""), ap, wm,
                 bits_mask, bits_s32))
    print("")
    mm = max(r[0] for r in rows)
    ms = max(r[1] for r in rows)
    print("  worst analog at mask width : %3d bits (%2d bytes)"
          % (mm, (mm + 7) // 8))
    print("  worst analog at s32        : %3d bits (%2d bytes)"
          % (ms, (ms + 7) // 8))
    print("")
    print("  whole word = digital + analog, digital capped at 32 by")
    print("  inputmap::fits(). So the worst word is %d or %d bits"
          % (32 + mm, 32 + ms))
    print("  (%d or %d bytes) against the 8 bytes sent today."
          % ((32 + mm + 7) // 8, (32 + ms + 7) // 8))
    print("")
    med = sorted(r[1] for r in rows)[len(rows) // 2]
    print("  MEDIAN analog driver at s32: %d bits, so a typical word is" % med)
    print("  32 + %d = %d bits (%d bytes)."
          % (med, 32 + med, (32 + med + 7) // 8))


def detail(drivers, name):
    for d in drivers:
        if d.name != name:
            continue
        print("driver   : %s   (%s)" % (d.name, d.source))
        print("players  : %d" % d.players)
        print("controls :")
        for c in d.controls:
            bits = ""
            if c.get("minimum") and c.get("maximum"):
                bits = "  span %d..%d = %d bits" % (
                    int(c["minimum"]), int(c["maximum"]),
                    span_bits(int(c["minimum"]), int(c["maximum"])))
            print("    type=%-11s player=%-2s buttons=%-2s sens=%-4s%s"
                  % (c.get("type", "?"), c.get("player", "-"),
                     c.get("buttons", "-"), c.get("sensitivity", "-"), bits))
        print("analog axes: %d" % d.axes)
        for m in d.masks:
            print("    mask 0x%04x = %d bits" % (m, mask_bits(m)))
        ap, odd = d.axes_per_player()
        print("axes/player: %d%s" % (ap, "  (RAGGED)" if odd else ""))
        print("digital estimate: %d bits" % d.digital_estimate())
        return
    print("no such driver: %s" % name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xml", help="output of mame -listxml")
    ap.add_argument("--csv", help="write one row per analog driver")
    ap.add_argument("--driver", help="detail for one driver")
    ap.add_argument("--all", action="store_true",
                    help="include clones (default: parents only)")
    args = ap.parse_args()

    drivers = parse(args.xml)

    if args.driver:
        detail(drivers, args.driver)
        return 0

    if args.csv:
        with open(args.csv, "w", encoding="ascii", newline="\n") as f:
            f.write("driver,source,players,axes,axes_per_player,widest_mask_bits,"
                    "wraps,digital_estimate,word_bits_mask,word_bits_s32\n")
            for d in drivers:
                if d.isdevice or not d.runnable or d.axes == 0:
                    continue
                if not args.all and d.cloneof is not None:
                    continue
                ap_, _ = d.axes_per_player()
                dig = d.digital_estimate()
                wm = d.widest_mask()
                f.write("%s,%s,%d,%d,%d,%d,%s,%d,%d,%d\n"
                        % (d.name, d.source, d.players, d.axes, ap_, wm,
                           "yes" if d.wraps() else "no", dig,
                           dig + ap_ * wm, dig + ap_ * 32))
        print("wrote %s" % args.csv)

    report(drivers, not args.all)
    return 0


if __name__ == "__main__":
    sys.exit(main())
