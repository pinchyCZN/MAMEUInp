// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_manager.h

    Owns the netplay session: the translation table, the input ring, and
    the per-frame sequencing. Three hooks in ioport_manager::frame_update,
    and nothing else in MAME knows netplay exists.

    LOCKSTEP WITH A FIXED INPUT DELAY. Rollback reuses this wire format and
    only changes client speculation, so it is deliberately not here yet -
    building it first means debugging two unknowns at once. [open]

    THE DELAY IS AT LEAST ONE FRAME, and that is structural rather than a
    tuning choice. A player's word spans ports, so a word injected into the
    first port would need bits not yet captured from the last. With a delay
    of one or more, inject reads a word completed on an earlier frame and
    capture builds one for a later frame, so both stay per-port inside
    MAME's existing loop. notes/protocol.txt 1.6 counts the same frame as
    tier 1's minimum added input delay.

    Transport is not here yet either. Loopback feeds the local player's own
    word back in after the delay, which exercises capture, the ring and
    injection without a socket. [open]

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_MANAGER_H
#define MAME_EMU_NETPLAY_NETPLAY_MANAGER_H

#pragma once

#include "netplay/netplay_inputmap.h"
#include "netplay/netplay_types.h"

#include <cstdio>
#include <memory>


namespace netplay {

class manager
{
public:
	// ---- ioport_manager::frame_update hooks ----
	//
	// CAPTURE BEFORE INJECT within one port's iteration, or capture reads
	// back the remote values inject just wrote. All three are no-ops unless
	// a session is running, so the cost when netplay is off is one pointer
	// test per port.
	static void port_capture(ioport_port &port);
	static void port_inject(ioport_port &port);
	static void frame_end(running_machine &machine);

	frame_t frame() const { return m_frame; }
	player_t local_slot() const { return m_local; }
	unsigned delay() const { return m_delay; }

	// public only because std::unique_ptr holds one; construction stays
	// private so a session can only come from open_session()
	~manager();

private:
	// Enough for any delay worth having: at 60 Hz, 128 frames is 2.1 s, and
	// a lockstep session that is two seconds behind has already stalled.
	static constexpr unsigned RING = 128;

	struct slot
	{
		bool        complete;
		input_word  words[MAX_PLAYERS];
	};

	manager(running_machine &machine, player_t local, player_t players,
			unsigned delay, bool selftest);
	bool setup(const char *logpath);

	static manager *active();
	static void open_session(running_machine &machine);

	slot &at(frame_t frame) { return m_ring[frame % RING]; }

	void publish(frame_t frame, player_t player, input_word word);
	void check_selftest();
	void on_exit();

	running_machine &   m_machine;
	inputmap            m_map;

	frame_t             m_frame;
	player_t            m_local;
	player_t            m_players;
	unsigned            m_delay;

	input_word          m_pending;      // local word being built for m_frame + m_delay
	input_word          m_echo;         // what injection actually put back, for selftest
	u32                 m_local_mask;   // digital bits owned by LOCAL_SLOT

	slot                m_ring[RING];

	bool                m_selftest;
	u64                 m_selftest_fail;
	FILE *              m_log;
};

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_MANAGER_H
