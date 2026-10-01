// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_simlink.h

    A transport that is a simulated network. Datagrams go in, come back out
    some frames later, sometimes out of order, sometimes not at all.

    WHY THIS EXISTS AT ALL. Everything interesting about a lockstep
    protocol is a timing property: whether the input for frame N arrives
    before frame N runs. Testing that over a real socket needs two
    machines, a link with the latency you want to study, and patience, and
    it is not reproducible. This runs the real wire format through the real
    manager with a link whose delay, jitter and loss are set by the test,
    so the arithmetic in notes/protocol.txt 1.6.3 becomes measurable.

    It echoes a peer's own packets back at a different player slot, which
    makes a one-process two-player session against a remote peer that
    happens to mirror the local one. The manager is not aware of this: it
    parses a packet declaring slot 2 and files it like any other.

    Time is counted in FRAMES, not milliseconds. The manager ticks it once
    per frame, so the unit matches the one the delay budget is expressed
    in and there is no wall clock to make a test irreproducible.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_SIMLINK_H
#define MAME_EMU_NETPLAY_NETPLAY_SIMLINK_H

#pragma once

#include "netplay/netplay_transport.h"

#include <deque>
#include <vector>


namespace netplay {

class simlink : public transport
{
public:
	// delay and jitter in FRAMES, loss in percent. echo_as is the player
	// slot outgoing packets come back as; 0 echoes them unchanged.
	simlink(unsigned delay, unsigned jitter, unsigned loss_pct,
			player_t echo_as, u64 seed);

	void send(const u8 *data, size_t len) override;
	size_t recv(u8 *buf, size_t cap) override;
	void tick() override;
	bool wait() override;
	const char *name() const override { return "simlink"; }

	u64 sent_count() const override { return m_sent; }
	u64 recv_count() const override { return m_recv; }
	u64 drop_count() const override { return m_dropped; }

	// how many times wait() advanced the clock, i.e. how long the manager
	// spent stalled, in frames
	u64 wait_ticks() const { return m_wait_ticks; }

private:
	struct datagram
	{
		u64     release;                // tick at which it becomes visible
		size_t  len;
		u8      data[MAX_DATAGRAM];
	};

	u64 next_random();

	std::deque<datagram>    m_flight;
	u64                     m_now;
	unsigned                m_delay;
	unsigned                m_jitter;
	unsigned                m_loss_pct;
	player_t                m_echo_as;
	u64                     m_rng;

	u64                     m_sent;
	u64                     m_recv;
	u64                     m_dropped;
	u64                     m_wait_ticks;
};

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_SIMLINK_H
