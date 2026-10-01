#!/usr/bin/env python3
# license:BSD-3-Clause
# copyright-holders:MAMEUI Netplay
#
# How many bits does a per-player logical input word actually need?
#
# protocol.txt 1.1 settles that the wire payload is a per-player LOGICAL word,
# not MAME's port-shaped record_port structure. That leaves the word's width
# as the one unmeasured number every bandwidth figure in protocol.txt 1.6.1
# depends on.
#
# This reads the CSVs written by tools/netplay_portdump.lua, resolves the
# numeric field.type values against the ioport_type enum in src/emu/ioport.h,
# and counts the distinct logical inputs one player can own.
#
#     mamecoinopui.exe <driver> -autoboot_script tools/netplay_portdump.lua
#     python tools/netplay_wordsize.py portdump/*.csv
#
# Routing follows protocol.txt 1.2, which needs two rules rather than one:
#
#     joysticks and buttons    the player comes from field.player
#     start and coin           the player is encoded in the TYPE, so
#                              IPT_START2 and IPT_COIN2 belong to player 2
#
# LIMITATION: ioport_field::type_class() resolves through
# manager().type_group(), whose data lives in inpttype.h and is not
# reproduced here. This classifies by enum RANGE instead, which agrees with
# type_class for the controller and start/coin types that matter for the word
# and does not attempt the keyboard, config or dipswitch classes. Fields it
# cannot place are reported as unclassified rather than silently dropped -
# protocol.txt 1.2 requires the table to be total over digital fields, so a
# non-zero count there is a gap in the format, not a rounding error.

import argparse
import os
import re
import sys
import collections

IOPORT_H = os.path.join('src', 'emu', 'ioport.h')


def parse_enum(path):
    """Return {name: value} for the ioport_type enum."""
    text = open(path, encoding='latin-1').read()
    start = text.index('enum ioport_type')
    body = text[start:text.index('};', start)]
    out, nxt = {}, 0
    for raw in body.split(chr(10)):
        line = raw.split('//')[0].strip()
        m = re.match(r'^(IPT_[A-Z0-9_]+)\s*(=\s*(-?\d+))?\s*,?$', line)
        if not m:
            continue
        name = m.group(1)
        if m.group(3) is not None:
            nxt = int(m.group(3))
        out[name] = nxt
        nxt += 1
    return out


def read_dump(path):
    rows, meta = [], {}
    with open(path, encoding='latin-1') as fh:
        for line in fh:
            line = line.rstrip(chr(10))
            if line.startswith('#'):
                for tok in line.lstrip('# ').split():
                    if '=' in tok:
                        k, v = tok.split('=', 1)
                        meta[k] = v
                continue
            if line.startswith('port,'):
                continue
            p = line.split(',')
            if len(p) < 8 or p[1] == '<PORT TOTAL>':
                continue
            rows.append({
                'port': p[0], 'field': p[1], 'player': int(p[2]),
                'type': int(p[3]), 'mask': int(p[4], 16),
                'analog': p[5] == 'true',
            })
    return meta, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csvfiles', nargs='+')
    ap.add_argument('--ioport', default=IOPORT_H)
    args = ap.parse_args()

    E = parse_enum(args.ioport)
    rev = {}
    for n, v in E.items():
        rev.setdefault(v, n)

    # Per-player blocks, taken from the enum's own FIRST/LAST markers rather
    # than hardcoded: DIGITAL_JOYSTICK, MAHJONG, HANAFUDA, GAMBLING. Mahjong
    # alone is 29 types, which is the case protocol.txt 1.2 cites from
    # Kaillera and the one that sets the word width.
    blocks = []
    for name, v in E.items():
        if name.endswith('_FIRST'):
            stem = name[:-len('_FIRST')]
            last = stem + '_LAST'
            if last in E:
                blocks.append((stem, v, E[last]))
    # IPT_UI_* is a per-player-shaped block by accident of the enum: it is the
    # frontend's own keys and no driver assigns it to a port. Excluded so it
    # cannot inflate a future count.
    blocks = [b for b in blocks if b[0] != 'IPT_UI']
    player_types = set()
    for _stem, lo, hi in blocks:
        player_types.update(range(lo + 1, hi))
    # buttons run contiguously from IPT_BUTTON1 and carry PORT_PLAYER
    btn = sorted(v for n, v in E.items() if re.match(r'^IPT_BUTTON\d+$', n))
    player_types.update(btn)
    # per-player by TYPE
    starts = {E['IPT_START%d' % i]: i for i in range(1, 11)
              if 'IPT_START%d' % i in E}
    coins = {E['IPT_COIN%d' % i]: i for i in range(1, 13)
             if 'IPT_COIN%d' % i in E}

    def owner(r):
        """(player, logical-id) or (None, reason)."""
        t = r['type']
        if t in starts:
            return starts[t], 'START'
        if t in coins:
            return coins[t], 'COIN'
        if t in player_types:
            # field.player is 0-based
            return r['player'] + 1, rev.get(t, 'T%d' % t)
        return None, rev.get(t, 'T%d' % t)

    print('per-player enum blocks: %s'
          % ', '.join('%s(%d)' % (b[0].replace('IPT_', ''), b[2] - b[1] - 1)
                      for b in sorted(blocks, key=lambda x: x[1])))
    print('action buttons: %d' % len(btn))
    print('')
    w = ('+----------+---------+----------+--------+--------+'
         '--------+--------------+')
    print(w)
    print('| driver   | players | max      | bits   | bytes  | shared | '
          'unclassified |')
    print('|          |         | per-plyr | needed | needed | fields | '
          'fields       |')
    print('+==========+=========+==========+========+========+'
          '========+==============+')

    worst = (0, None, None)
    rows_out = []
    for path in args.csvfiles:
        meta, rows = read_dump(path)
        drv = meta.get('driver', os.path.basename(path))
        per = collections.defaultdict(set)
        shared, unk = [], []
        for r in rows:
            if r['analog']:
                continue                    # analog is not in the word
            pl, lid = owner(r)
            if pl is None:
                # a named non-player digital field: service, tilt, freeze
                shared.append((r['field'], lid))
            else:
                per[pl].add(lid)
        nplayers = len(per)
        maxper = max((len(v) for v in per.values()), default=0)
        bits = maxper
        nbytes = (bits + 7) // 8
        rows_out.append((drv, nplayers, maxper, bits, nbytes,
                         len(shared), len(unk), per, shared))
        if maxper > worst[0]:
            worst = (maxper, drv, per)
        print('| %-8s | %7d | %8d | %6d | %6d | %6d | %12d |'
              % (drv, nplayers, maxper, bits, nbytes, len(shared), len(unk)))
    print(w)

    print('')
    print('widest per-player word: %s needs %d logical inputs -> %d byte(s)'
          % (worst[1], worst[0], (worst[0] + 7) // 8))
    print('')
    for drv, npl, maxper, bits, nb, nsh, nunk, per, shared in rows_out:
        print('%s:' % drv)
        for pl in sorted(per):
            print('    player %d (%2d): %s'
                  % (pl, len(per[pl]),
                     ' '.join(sorted(x.replace('IPT_', '')
                                     for x in per[pl]))))
        if shared:
            print('    shared/non-player digital (%d): %s'
                  % (len(shared),
                     ' '.join(sorted(set(l.replace('IPT_', '')
                                         for _, l in shared)))))
        print('')
    return 0


if __name__ == '__main__':
    sys.exit(main())
