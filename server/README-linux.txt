MAMEUI Netplay                                            Relay Server


                   BUILDING THE RELAY ON LINUX


Abstract

   The relay server is a standalone UDP program. It shares only the
   wire format with the emulator and needs nothing from MAME, so it
   builds on a small VPS in seconds.

Table of Contents

   1. What is in this directory
   2. Checking the box can build it
   3. Building
   4. Running
   5. Opening the port
   6. Pointing the peers at it
   7. Limits worth knowing

1.  What is in this directory

   +---------------------------+------------------------------------+
   | File                      | Role                               |
   +===========================+====================================+
   | netplay_server.cpp        | the relay                          |
   +---------------------------+------------------------------------+
   | netplay/netplay_packet.h  | the wire format, shared with MAME  |
   | netplay/netplay_packet.cpp|                                    |
   | netplay/netplay_types.h   |                                    |
   +---------------------------+------------------------------------+
   | Makefile                  | the build                          |
   +---------------------------+------------------------------------+
   | build.sh                  | same build, for a box without make |
   +---------------------------+------------------------------------+

   The three files under netplay/ are COPIES of src/emu/netplay in the
   MAMEUI tree. Both programs compile the same source so that one
   definition of the wire format serves both; a copy that goes stale is
   two builds disagreeing, which appears as every packet being counted
   malformed and no session ever starting. Re-stage after changing them.

2.  Checking the box can build it

   Three commands. All three must answer:

       g++ --version
       make --version
       echo '#include <sys/socket.h>' | g++ -x c++ -fsyntax-only -

   The third is the one worth running: it proves the C library headers
   are installed, which a compiler alone does not. On Debian 13 the one
   package that supplies all of it is:

       sudo apt update
       sudo apt install build-essential

   Nothing else is needed - no zlib, no pthreads, no boost. The server
   does not compress and does not thread.

   Memory: two translation units at -O2, a few MB of peak compiler
   residency. 512 MB is ample. The running server holds one fixed
   client table and a single datagram buffer.

3.  Building

       make

   leaves ./netplay_server beside the sources. Without make:

       sh build.sh

   NETPLAY_STANDALONE is required and both paths set it. It is what
   makes netplay_types.h define u8..u64 for itself instead of expecting
   MAME's emu.h. A build without it fails on a missing emu.h.

4.  Running

       ./netplay_server -port 7000 -players 2

   It runs forever, recycles the session when everyone leaves, and
   prints a line per join and per departure. Output is unbuffered, so
   it is safe to redirect to a file or read over ssh.

   To keep it up after logout, either

       nohup ./netplay_server -port 7000 -players 2 > relay.log 2>&1 &

   or a systemd unit, which is the better answer on a VPS and is not
   included here.

5.  Opening the port

   IT IS UDP. A rule written for TCP passes nothing and looks correct
   while doing it. Both layers have to allow it:

   o the host firewall - ufw allow 7000/udp, or the nftables
     equivalent
   o the provider's own firewall or security group, which is separate
     from the host and is the usual reason a correct ufw rule still
     drops everything

   Confirm from your own machine before blaming the game:

       nc -u -z -v <host> 7000

6.  Pointing the peers at it

   Each peer sets NETPLAY_HOST to the VPS address and keeps the same
   port. In B:\nplay\A\A.bat and B.bat:

       set "NETPLAY_HOST=203.0.113.10"
       set "NETPLAY_PORT=7000"

   Leave NETPLAY_SLOT different per peer, as it already is.

   RAISE NETPLAY_DELAY FIRST. Both batch files ship at 1, which is the
   clamped minimum and has no tolerance for jitter at all - it is a
   localhost setting. Over a real link start at 6 and come down only if
   it feels sluggish. Smooth beats low latency; notes/protocol.txt
   1.6.4 is the argument.

7.  Limits worth knowing

   o IPv4 ONLY. The socket is AF_INET and addresses are sockaddr_in,
     so an IPv6-only host will not work. Dual-stack is fine - use the
     v4 address.
   o The session starts only when -players peers have joined, and a
     peer silent for 15 seconds is dropped.
   o The relay understands INPUT, UICMD, RESYNC_REQ, RESYNC and CHAT
     and forwards them verbatim. Anything else it counts as malformed,
     which is also how a version mismatch between the two ends shows
     up.
