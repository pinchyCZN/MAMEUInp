#!/usr/bin/env python3
# license:BSD-3-Clause
# copyright-holders:MAMEUI Netplay
#
# Read the netplay status dump written by src/emu/netplay/netplay_manager.cpp.
#
#     set NETPLAY_STATUS=B:\nplay\A\status.txt      (per peer)
#     python tools/netplay_status.py B:/nplay/A/status.txt
#     python tools/netplay_status.py B:/nplay/*/status.txt --watch
#
# WHY A FILE AND NOT A PROTOCOL. The dump is one-way and the contract is
# "key=value lines plus a '# end' marker", so there is nothing to negotiate:
# no handshake, no request, no schema. A wedged peer cannot be asked what it
# is doing, and every netplay bug so far has been an ordering question rather
# than a wrong value, so what is worth externalising is state.
#
# A reader can catch a partial write, since the emulator rewrites the file in
# place. The '# end' marker is the validity test; without it the snapshot is
# discarded rather than half-reported.

import argparse
import glob
import os
import sys
import time


def read_status(path):
    """Return a dict, or None if the file is absent or mid-write."""
    try:
        with open(path, encoding='ascii', errors='replace') as fh:
            text = fh.read()
    except OSError:
        return None

    if '# end' not in text:
        return None            # caught mid-rewrite

    out = {}
    for line in text.split(chr(10)):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if '=' not in line:
            continue
        k, v = line.split('=', 1)
        out[k] = v
    return out


def diagnose(st):
    """The one line worth reading: what is this peer doing, and why."""
    if st.get('gated') == '1':
        return 'WAITING TO START - the server has not released the session yet'
    if st.get('net_paused') == '1':
        return 'PAUSED by a netplay command (press the pause key to resume)'
    if st.get('frame_complete') != '1':
        missing = st.get('missing', '')
        held = int(st.get('held_polls', '0') or 0)
        return ('STALLED waiting for %s - about %d ms so far'
                % (missing or 'input', held))
    return 'running'


def show(path, st, now):
    name = os.path.basename(os.path.dirname(os.path.abspath(path))) or path
    if st is None:
        print('%-10s  no readable snapshot (%s)' % (name, path))
        return

    age = now - int(st.get('time', '0') or 0)
    stale = '  STALE %ds' % age if age > 5 else ''

    print('%-10s  frame %-8s %s%s'
          % (name, st.get('frame', '?'), diagnose(st), stale))
    print('            slot %s of %s   delay %s   batch %s   budget %s   %s'
          % (st.get('slot', '?'), st.get('players', '?'), st.get('delay', '?'),
             st.get('batch', '?'), st.get('budget', '?'),
             st.get('transport', '?')))
    print('            have %s of %s      stalls %s   stall_frames %s   waits %s'
          % (st.get('have', '?'), st.get('need', '?'), st.get('stalls', '?'),
             st.get('stall_frames', '?'), st.get('waits', '?')))
    print('            link sent %s  recv %s  dropped %s  malformed %s'
          % (st.get('link_sent', '?'), st.get('link_recv', '?'),
             st.get('link_dropped', '?'), st.get('malformed', '?')))
    if st.get('cmd_pending') == '1':
        print('            pending: %s at frame %s'
              % (st.get('cmd', '?'), st.get('cmd_frame', '?')))

    ring = st.get('ring', '')
    if ring:
        # Where the Cs stop is where the session is stuck.
        ready = len(ring) - len(ring.lstrip('C'))
        print('            ring %s  (%d frame%s of input in hand)'
              % (ring, ready, '' if ready == 1 else 's'))
    print('')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('paths', nargs='+',
                    help='status files, globs accepted')
    ap.add_argument('--watch', action='store_true',
                    help='refresh until interrupted')
    ap.add_argument('--interval', type=float, default=1.0)
    args = ap.parse_args()

    expanded = []
    for p in args.paths:
        hits = sorted(glob.glob(p))
        expanded.extend(hits if hits else [p])

    while True:
        now = int(time.time())
        if args.watch:
            print(chr(27) + '[2J' + chr(27) + '[H', end='')
            print('netplay status  %s' % time.strftime('%H:%M:%S'))
            print('')
        for p in expanded:
            show(p, read_status(p), now)
        if not args.watch:
            return 0
        time.sleep(args.interval)


if __name__ == '__main__':
    sys.exit(main())
