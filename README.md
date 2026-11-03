MAMEUI 0.210 with Netplay
=========================

A fork of [Robbbert/mameui](https://github.com/Robbbert/mameui) at tag `tag210`
that adds deterministic, server-mediated netplay to MAME's own frontend, plus a
standalone relay server to go with it.

Conceptually this is Kaillera: every client sends its local input for frame N to
a server, the server redistributes the complete input set for frame N, and every
instance steps in lockstep. The difference is that it is built on MAME's own
determinism guarantees rather than against them, and that the settings which
actually decide whether a session feels good are exposed instead of hidden
behind a connection-quality label.

> **Status.** Lockstep with a fixed input delay works, including resync from a
> savestate, chat, loss tolerance and a live connection readout. Rollback is
> not implemented, and neither are late join or spectators. See
> [What is not done](#what-is-not-done).


Contents
--------

* [Why rebuild Kaillera](#why-rebuild-kaillera)
* [The two knobs that matter](#the-two-knobs-that-matter)
* [Surviving packet loss](#surviving-packet-loss)
* [Keeping two machines bit-identical](#keeping-two-machines-bit-identical)
* [Resync from a savestate](#resync-from-a-savestate)
* [The lobby](#the-lobby)
* [The relay server](#the-relay-server)
* [Other changes to MAMEUI](#other-changes-to-mameui)
* [What is not done](#what-is-not-done)
* [Building](#building)
* [Licensing](#licensing)


Why rebuild Kaillera
--------------------

The lockstep model is correct and does not need replacing. What it needs is the
parts Kaillera got wrong or never had:

| Kaillera | Here |
|---|---|
| one "connection quality" dropdown, input delay bundled behind it | delay and batch are separate, both user-visible |
| a lost UDP datagram wedges the session | every input word is sent in several datagrams, with a retransmission request behind that |
| peers cold-boot and hope | the host can hand its machine state to a peer mid-session |
| a desync is silent and permanent | a per-frame state hash travels with the input stream |
| a pre-filtered game list that silently omits sets you nearly have | the join button audits your set and names the file that is wrong |
| settings mismatches become "all parties must use the same speed" | session settings travel in the handshake, so a mismatch is unrepresentable |
| closed protocol, dead servers | plain UDP, documented byte for byte in `notes/protocol.txt`, relay builds on a small VPS in seconds |

MAME is deterministic for a fixed binary, configuration and ROM set - the proof
is that `-record` and `-playback` work at all. The netplay layer inherits that
guarantee by hooking the same chokepoint the recorder uses,
`ioport_manager::frame_update`. Nothing else in MAME knows netplay exists.


The two knobs that matter
-------------------------

**Input delay (D)** is how many frames ahead of the current frame a peer
publishes its own input. It is not a buffer sitting beside the network jitter
budget - it *is* the budget. With delay D, a peer's input for frame N was
captured D frames earlier, so the network has that long to deliver it.

**Batch (K)** is how many frames of input ride in one datagram. Raising it cuts
packet rate, which is the only thing it does.

The trap, and the reason both are exposed:

```
effective jitter budget = (D - K + 1) frames
```

A sender cannot transmit a batch until it has captured every frame in it, so the
first frame of each batch waits for the other K-1. **Batching spends the budget;
it does not provide it.** Kaillera's later versions reduced D while leaving K at
its highest rung, which collapses the budget to one frame - about 17 ms - and
that is exactly what a session configured for smoothness stuttering looks like,
with no knob the player can reach for.

Sizing is arithmetic rather than a label. At roughly 60 Hz a frame is 16.77 ms:

```
D >= ceil( (one_way_delay + jitter_margin) / 16.77ms ) + K - 1
```

| Link | K | Covers | D | Felt lag |
|---|---|---|---|---|
| LAN | 1 | 10 ms | 1 | 17 ms |
| excellent | 2 | 25 ms | 3 | 50 ms |
| good | 3 | 45 ms | 5 | 84 ms |
| average | 4 | 70 ms | 8 | 134 ms |
| low | 5 | 100 ms | 10 | 168 ms |
| bad | 6 | 140 ms | 14 | 235 ms |

Two consequences worth knowing:

**Delay is per peer, not per session for correctness.** Emulated state depends
only on the *contents* of a ring slot, which is the same set of words on every
peer, so D decides nothing but which future frame a peer's own capture lands on.
A player on a bad link can raise their own delay and only they pay the lag. The
lobby still equalises it by default, because the startup priming assumes it
matches, and because a low-delay player reacts faster than a high-delay one -
which is a fairness preference, not a correctness one.

**Changing delay mid-session is free.** No peer but the owner ever writes those
slots, so there is no resync and no protocol event; the artifact is a few frames
of held input.

### Smoothness over latency, deliberately

Where the two conflict this design spends latency. A stutter is worse than
200 ms of lag.

That is a choice and the games decide it. Modern emulator netplay is tuned for
fighting games, where the shared state is two human opponents: a late input
costs one move against a player who also missed it. In a co-op game the shared
state is a world full of AI reacting to player input, so a lost input means a
character doing the wrong thing in a world that responds to it, and the
consequences compound instead of cancelling.

An in-game overlay reports delay, batch, the computed budget in frames and
milliseconds, the percentage of frames that waited on the network, typical/p95/
max wait, and a suggested delay when the current one is not holding.


Surviving packet loss
---------------------

Input has no retransmission by default: a frame's word cannot be skipped and no
peer may invent one, so **one lost datagram stalls every peer for good**. That
was measured here - 11122 datagrams sent, 11121 received, both peers wedged
permanently on the single frame that went missing.

So each word is sent in several separate datagrams. Depth N survives a burst of
N-1 consecutive losses, with no round trip and no stall - the data simply
arrives in time. Depth is derived rather than configured, because the last copy
that is any use has to leave before the frame is needed:

```
depth <= (D - 1) / K + 1
```

Measured on a two-peer session at delay 10, batch 1, with loss injected at the
sending socket: bursts of 1, 3 and 9 consecutive drops were *invisible* - in
sync, zero network waits - and a burst of 10 deadlocked, which is the N-1
boundary exactly. The cost is bandwidth: about 58 kbit/s per peer at depth 10
instead of 23.

Repetition rather than an erasure code, because the payload is 8 bytes against
28 bytes of IP and UDP header. A parity shard costs a whole datagram; a
duplicated word costs eight bytes.

Past that, a stalled peer asks for what it is missing. Only the receiver knows
what that is, so this cannot be replaced by senders resending their recent
window unprompted.

Note the interaction: **delay buys smoothness and loss tolerance in the same
direction**, and batching spends both in the same proportion. Raising D on a bad
link is doubly right. At delay 1 the ceiling is depth 1, so a delay-1 session
cannot be protected from a single lost datagram by any amount of configuration.
It is a localhost setting.


Keeping two machines bit-identical
----------------------------------

Lockstep is only as good as the agreement about what both machines are. Most of
the work in this fork is here rather than in the networking.

**The handshake verifies rather than trusts.** A peer sends a digest of its
input layout - the structural walk of every port and field - plus a digest of
its DIP, configuration and analog values, and the driver name. A mismatch is
refused with a specific reason, not a generic failure.

**The host pushes its settings and the guest adopts them.** DIP switches,
configuration ports and analog sensitivity travel in a `CONFIG` message,
identified by position in the ordered port walk, so no strings go over the wire.
The digest travels even when the host is set not to share values - otherwise a
differently configured guest has no way to find out and plays on desynced.

**The emulated clock is pinned.** Wall-clock time reaches a MAME machine through
one door, and every RTC device seeds from it at start, so the session base time
is fixed before any device starts.

**Frameskip is locked, because it is not presentation-only.**
`video_manager::skip_this_frame()` is public and several driver families branch
on it into state that is itself save-stated - `cave.cpp` banks its sprite RAM
only when not skipping, `gaelco3d.cpp` resets its polygon count only when not
skipping, `namcos22.cpp` save-states the skip flag directly. Peers at different
frameskip therefore hold different machine state on those drivers, and any
state-hash comparison diverges even where the game plays on regardless. The key
is refused during a session; agreeing on it at join time is a defined handshake
refusal that is not yet wired up.

**The UI splits what it can make safe from what it cannot.** Frameskip,
throttle, fast-forward, the graphics viewer and manual save/load state are
refused while a session is live, each with a reason shown to the player rather
than a dead key. Pause and soft reset are *not* refused - they are stamped with
a frame, broadcast, and applied on every peer at that same frame, which is the
only way a pause can be anything but a desync.

**The startup screens are suppressed.** Every peer holds the others until it
dismisses the machine-information and ROM-warning screens, and `skip_gameinfo`
is a per-install setting, so it cannot be relied on at the far end.

**Quitting takes a confirmation.** `confirm_quit` defaults to off, and Escape is
also the key that leaves the state and settings menus - so one stray press ended
the game for everybody.

**Analog axes are injected properly.** A remote player's analog input cannot be
pushed through the digital path, because `analog_field::read` applies analog
after digital and overwrites its own mask. Analog fields gained an explicit
forced value, honoured in `read` and in `crosshair_read`, so a remote player's
crosshair moves too.

**Desync detection comes before rollback, not after.** Without a per-frame state
hash exchanged and compared, every later bug is invisible. A hash of every
registered save-state entry rides as a trailer on the input stream, and two
options exist for offline diagnosis:

```
-netplay_hashlog FILE        log a per-frame hash of emulated state
-netplay_hashinterval N      hash every Nth frame (large states)
```

One sampled frame is not enough, and the reason is worth stating: input that
diverges is not visible in the hash on the frame it is applied, only on the
next, because it has not reached RAM yet. Compare runs of consecutive frames.


Resync from a savestate
-----------------------

A peer that has diverged is brought back onto the host's timeline - the host is
authoritative by definition, being the peer everyone else agreed to match.

The host snapshots at frame S, transit costs M frames of wall clock, and there
are exactly two things you can do with a state from the past in the present:
apply it to everybody, so correct peers hiccup to fix the broken one, or apply
it to the diverged peer alone and have it replay inputs S..S+M to catch up. Any
design that claims to avoid both is hiding one of them.

This takes the second. The peer that broke pays; the peers that did not break
observe nothing at all. Control messages travel in band on the UDP session, the
bulk state out of band. Either end can cancel a transfer in progress.

The guest asks deliberately with a hotkey, or the host can push unsolicited - an
unsolicited state is its own notification. The host can also snapshot
periodically, keeping the last N.

One session switch decides whether any of this is used, because "can this driver
reload its own state" is a per-driver fact that `MACHINE_SUPPORTS_SAVE` does not
predict. With it off, peers cold-boot and lockstep alone carries the session,
which is all Kaillera ever had.

**This is most of rollback.** In-band frame-tagged state plus "load it and
replay the inputs from there" *is* rollback, triggered rarely over a long window
instead of constantly over a short one. Rollback then becomes shrinking the
window and calling it more often, against machinery already proven by use -
rather than arriving untested, in its hardest configuration, with no way to tell
a rollback bug from a determinism bug.


The lobby
---------

**File -> Netplay** in the frontend. The host picks a system, the room sees it,
guests join, the host starts, and every peer launches into the session.

Three things are different from how Kaillera did this:

**Auditing is the join button.** A host may pick any system, audited or not;
selecting it audits that one system and reports what is missing. Clicking join
audits that set on the joiner's machine and, if anything is wrong, says *which
file*. The common case is one wrong ROM in an otherwise working set, and being
told which one is the difference between fixing it and giving up. This is
deliberately not a pre-filtered list - a list that silently omits systems you
nearly have is the behaviour being replaced.

**Session settings are not negotiated, they are delivered.** The host's delay,
batch, player count and savestate policy are handed to every joiner in the
server's `WELCOME`, so a mismatch is unrepresentable rather than checked.

**Host policy is visible.** The dialog shows every setting; a joiner can edit
almost none of it, and can see why.

The lobby also carries a text chat, which continues in game - six lines in the
corner, fading out, with local input suppressed while you are typing.

The frontend additions are their own files (`netplayui.cpp`, `netplaypick.cpp`,
`netplaystate.cpp`): a netplay dialog, a game picker built on MAMEUI's own
picker control with header sorting and clone grouping, and a savestate picker
with thumbnails for choosing which state to host from.


The relay server
----------------

`server/` holds a single-file relay. It compiles the *same* wire-format sources
the emulator does rather than reimplementing them, because two copies of a
packet layout is how two programs come to disagree about one.

It is a relay, not an authority: an `INPUT` packet from one peer is forwarded
verbatim to every other, since the packet already declares which slot it belongs
to. It assigns slots, checks the handshake digests it is given, and releases
`START` once the session is full. It does **not** synthesise input for a late
player - that was considered and deferred, because it costs the late player
their input, which is the wrong currency for these games.

One UDP socket for the session, one TCP port for the lobby, both on the same
port number. Up to 10 players.

```
netplay_server -port 7000 -players 2
```

`-players` only matters for direct UDP connections; a lobby host sets the count
when it creates the game, which overrides it. The relay builds on Linux with
`make` in seconds and needs nothing from MAME - see `server/README-linux.txt`.


Other changes to MAMEUI
-----------------------

Changes in this fork that are not netplay, listed because they are useful on
their own. Every upstream file touched is in `notes/upstream-edits.txt`, which
is the rebase checklist.

**It builds with MSVC.** MAMEUI's WinUI frontend had only ever been built with
MinGW; the build scripts carried a configuration block for `mingw*-gcc` and
nothing for MSVC. Six changes fix that, none of which alters behaviour under
MinGW. The one that costs a day if you hit it yourself: genie resolves the
executable to `mainCRTStartup`, which finds stock MAME's `main()` rather than
MAMEUI's `wWinMain` - **it links clean and runs as plain MAME with no
frontend**, with no diagnostic at all. The rest are 18 variable-length arrays,
missing `<malloc.h>` and `<io.h>` includes, two functions defined in a header, a
one-definition-rule violation, and `NOWERROR=1` not actually disabling `/WX`.

**Frontend fixes:**

* The two icon-view toolbar buttons are gone - they hang the app on a full
  driver set - and a saved icon view is clamped to List at startup, since
  restoring one hangs before any menu is reachable.
* The toolbar search box is anchored so it stops following the window edge, and
  gained four radio buttons choosing which column it filters on. Previously it
  searched full name and directory together, so a directory match appeared under
  a machine-name search.
* The Directories dialog no longer re-audits. It called `UpdateGameList`, which
  flags every driver unknown, so editing rompath order rescanned the whole list
  on every edit.
* One corrupt screenshot no longer crashes on a single click: `LoadDIB` left its
  error code stale across extension passes and dereferenced an empty pointer.
* A cheat file with bad expressions no longer blocks startup. WinUI boxes every
  error-channel message, so six bad expressions meant six modal dialogs before
  the game would run; a per-cheat failure is now a warning.
* The default list mode was 5, one past the last valid view, so a fresh install
  could not select Grouped.

**An upstream DRC stack overflow.** `drc_frontend::describe_one` recurses to
describe the delay slots of a branch, and when a delay slot is itself a branch
the next pc comes from the *previous* description's target - which can be an
address already in the chain, so it recurses on the same address until the stack
is gone. `MAX_STACK_DEPTH` does not bound it; that constant guards a different
structure. Observed on `rsgun` (SH-2) immediately after loading a savestate.
Fixed by taking the redirection only when it advances. Untouched upstream as of
this writing.

**Mega Drive and SNES cartridges in the arcade frontend.** `megadriv` and `snes`
are in the machine list with working cart slots and both play, which took bus
and video dependencies rather than new code. The driver list went from 19150 to
19324 - 174 entries including regional variants.

**A trimmed driver set.** `SUBTARGET=coinop` is arcade minus mechanical,
generated from that tag's own `arcade.flt` and `arcade.lua` by
`tools/gen_coinop.py` so it can be regenerated against another tag rather than
hand-maintained.


What is not done
----------------

Stated plainly, because an honest gap is more useful than a confident guess.

* **Rollback.** The resync machinery above is the groundwork; client speculation
  is not written.
* **Late join and spectators.** Structurally this is a resync where the transit
  gap is the whole session, so the pieces exist, but it is out of scope until
  lockstep is fully dialled in.
* **The adaptive control law for delay.** Raising and lowering D is free and the
  overlay already suggests a value, but nothing moves it automatically. An
  adaptive jitter buffer is a solved problem in VoIP and the same arithmetic
  applies.
* **Cheat propagation.** A cheat writes emulated memory, so one peer enabling it
  alone desyncs at once. Wanted; it is a control message, not an input-path
  change.
* **Blocking the Tab menu per item** rather than wholesale, so remapping stays
  available while DIP and configuration changes do not.
* **Refusing a frameskip, cheat or `-drc` mismatch at the handshake.** The
  refusal reason exists in the protocol and nothing sends it yet; the frameskip
  *key* is locked during a session, but two peers that started at different
  settings are not stopped from trying.
* **A stalled peer stalls everyone.** It stops running frames, so it stops
  capturing, so it stops publishing. Lockstep has no answer to this short of
  server-side input synthesis, which is deliberately refused. Sizing the delay
  so it does not happen is the whole strategy.
* **Driver coverage is sampled, not swept.** The analog axis widths, the per
  player bit budget, and which drivers reload their own state are measured over
  tens of drivers, not the whole set. The handshake refuses what does not fit
  either way.

The design notes in `notes/` carry the detail, each claim tagged `[verified]`,
`[inferred]`, `[upstream]` or `[guess]`:

| | |
|---|---|
| `notes/protocol.txt` | the wire format, byte for byte, with the measurements |
| `notes/determinism.txt` | what perturbs emulated state, and the evidence |
| `notes/netplay-hooks.txt` | where MAME is hooked and what it guarantees there |
| `notes/netplay-frame-flow.txt` | one session traced frame by frame |
| `notes/netplay-ui.txt` | the frontend and lobby design |
| `notes/upstream-edits.txt` | every upstream file changed - the rebase checklist |
| `notes/build-config.txt` | toolchain quirks and their evidence |


Building
--------

Windows only - the WinUI frontend will not compile on Unix. MSVC 2017 (`v141`)
or MinGW, and Python 3 on `PATH`.

```
make vs2017 OSD=winui TARGET=mame SUBTARGET=coinop PTR64=1 NOWERROR=1
```

That generates the solution under `build/projects/winui/mamecoinop/vs2017/` and
compiles nothing. `SUBTARGET=arcade` for the stock arcade set, `OSD=messui` for
the MESS variant.

Precompiled headers are forcibly off for MSVC in this vintage and `PRECOMPILE`
cannot override it, so every driver re-parses `emu.h` and a full build takes
hours. For work on the netplay layer itself, a `SOURCES=` build derives the
needed CPU, sound and video devices from the named drivers and compiles in
minutes:

```
make vs2017 OSD=winui TARGET=mame PTR64=1 NOWERROR=1 OPTIMIZE=0 \
     SOURCES=src/mame/drivers/cps1.cpp SUBTARGET=netplay
```

**Build both peers the same way.** Same compiler, same `OPTIMIZE` level - do not
pair an `OPTIMIZE=0` peer with a release one. Floating-point code generation
differs between levels, which would be a desync rather than a performance
difference. This one is reasoned rather than measured, and it is cheap to
respect.

The relay server builds separately and needs none of the above:

```
cd server && make
```


Licensing
---------

MAME is BSD-3-Clause. The WinUI portion has its own licence in
`docs/winui_license.txt`. The netplay layer, the relay server and the notes are
our own work product under the same BSD-3-Clause terms.

ROM images are not redistributed and are excluded from version control.

* MAME content: <https://github.com/mamedev/mame/blob/master/README.md>
* MAME licence particulars and disclaimers:
  <https://github.com/mamedev/mame/blob/master/LICENSE.md>
* WinUI portion:
  <https://github.com/Robbbert/mameui/blob/master/docs/winui_license.txt>
* [Official MAME Development Team Site](http://mamedev.org/)
* [MAMEUI site](http://www.mameui.info/)
