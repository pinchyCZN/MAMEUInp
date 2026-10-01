#!/usr/bin/env python3
# license:BSD-3-Clause
# copyright-holders:MAMEUI Netplay
#
# TEST 2 analyser: did the savestate round-trip bit-identically?
#
# Reads a -netplay_hashlog CSV produced under tools/netplay_roundtrip.lua and
# decides whether the hash sequence after the load REPLAYS the sequence after
# the save. If it does, save/load is exact and rollback and resync are both
# viable. If it does not, the design is lockstep-only.
#
#     python tools/netplay_rtcheck.py rt.csv --save 600 --load 1200
#
# The alignment offset is discovered, not assumed: handle_saveload() can slip
# by an unknown number of frames when m_scheduler.can_save() is false, and the
# Lua end-of-frame callback need not align 1:1 with frame_update. See the
# header of netplay_roundtrip.lua.
#
# A run of identical hashes would make any offset match, so the number of
# DISTINCT hashes in the compared window is reported and gates the verdict.
# Without that guard a static screen passes this test trivially.

import argparse
import sys

MIN_RUN = 60        # consecutive matching frames needed to call it a replay
MIN_DISTINCT = 8    # distinct hashes needed for the window to mean anything
SEARCH = 150        # frames either side of (load - save) to search for d


def read_log(path):
    meta, series = {}, {}
    with open(path, newline='') as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith('#'):
                if '=' in line:
                    k, v = line.lstrip('# ').split('=', 1)
                    meta[k.strip()] = v.strip()
                continue
            if line.startswith('frame,'):
                continue
            parts = line.split(',')
            if len(parts) != 2:
                continue
            try:
                series[int(parts[0])] = parts[1]
            except ValueError:
                continue
    return meta, series


def longest_run(series, lo, hi, d):
    """Longest consecutive i in [lo, hi] with series[i] == series[i - d]."""
    best = cur = 0
    total = 0
    for i in range(lo, hi + 1):
        a, b = series.get(i), series.get(i - d)
        if a is not None and b is not None and a == b:
            cur += 1
            total += 1
            best = max(best, cur)
        else:
            cur = 0
    return best, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csvfile')
    ap.add_argument('--save', type=int, required=True,
                    help='SAVE_FRAME from netplay_roundtrip.lua')
    ap.add_argument('--load', type=int, required=True,
                    help='LOAD_FRAME from netplay_roundtrip.lua')
    args = ap.parse_args()

    meta, series = read_log(args.csvfile)
    if not series:
        print('FAIL  no hash rows in %s' % args.csvfile)
        return 2
    if meta.get('interval') not in (None, '1'):
        print('FAIL  interval=%s; the check is frame-by-frame, rerun with '
              '-netplay_hashinterval 1' % meta['interval'])
        return 2

    frames = sorted(series)
    first, last = frames[0], frames[-1]
    nominal = args.load - args.save

    print('log    %s' % args.csvfile)
    print('       machine=%s entries=%s layout=%s'
          % (meta.get('machine', '?'), meta.get('entries', '?'),
             meta.get('layout', '?')))
    print('       frames %d..%d (%d rows)' % (first, last, len(series)))
    print('hint   save=%d load=%d nominal offset=%d'
          % (args.save, args.load, nominal))

    # the window to compare: everything logged after the load hint
    lo = args.load + 1
    hi = last
    if hi - lo < MIN_RUN:
        print('FAIL  only %d frames logged after the load hint; run longer'
              % max(0, hi - lo))
        return 2

    best = (0, 0, None)
    for d in range(max(1, nominal - SEARCH), nominal + SEARCH + 1):
        run, total = longest_run(series, lo, hi, d)
        if run > best[0]:
            best = (run, total, d)
    run, total, d = best

    # vacuity guard: a static screen matches at every offset
    window = [series[i] for i in range(lo, hi + 1) if i in series]
    distinct_post = len(set(window))
    src = [series[i] for i in range(args.save + 1, args.save + 1 + len(window))
           if i in series]
    distinct_src = len(set(src))

    # did the load take effect at all? if it silently failed, the machine kept
    # evolving and the post-load hashes are values never seen before
    seen_before = set(series[i] for i in frames if i <= args.load)
    novel = sum(1 for h in window if h not in seen_before)

    print('match  best offset d=%s longest run=%d frames, %d matching total'
          % (d, run, total))
    print('       distinct hashes: %d in the compared window, %d in the '
          'source window' % (distinct_post, distinct_src))
    print('       %d of %d post-load hashes never appear before the load'
          % (novel, len(window)))

    if min(distinct_post, distinct_src) < MIN_DISTINCT:
        print('')
        print('INCONCLUSIVE  the compared window is nearly static (%d/%d '
              'distinct).' % (distinct_post, distinct_src))
        print('              Any offset matches a constant sequence. Pick '
              'save/load frames')
        print('              during actual gameplay, not a title or attract '
              'screen.')
        return 3

    if run >= MIN_RUN:
        print('')
        print('PASS  the post-load sequence replays the post-save sequence '
              'for %d' % run)
        print('      consecutive frames at offset %d (nominal %d, slip %+d).'
              % (d, nominal, d - nominal))
        print('      Savestate save/load round-trips exactly. Rollback and '
              'resync are')
        print('      viable; see notes/protocol.txt.')
        return 0

    if novel == len(window):
        print('')
        print('FAIL  every post-load hash is new, and nothing replays. The '
              'load did not')
        print('      take effect, or it produced a state unrelated to the '
              'save. Check the')
        print('      console for "Unable to load" / "Unable to save", and '
              'confirm the')
        print('      driver carries MACHINE_SUPPORTS_SAVE.')
        return 1

    print('')
    print('FAIL  best replay run is only %d frames (need %d). Save/load does '
          'not' % (run, MIN_RUN))
    print('      round-trip exactly: some state is outside the registered '
          'set, or is')
    print('      restored differently than it was saved. Rollback and resync '
          'are NOT')
    print('      viable until this is understood.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
