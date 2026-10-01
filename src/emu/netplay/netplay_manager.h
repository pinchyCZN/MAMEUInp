// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_manager.h

    Owns the netplay session: the translation table, the input ring, the
    outgoing batch, and the per-frame sequencing. Four hooks in
    ioport_manager::frame_update, and nothing else in MAME knows netplay
    exists.

    LOCKSTEP WITH A FIXED INPUT DELAY. Rollback reuses this wire format and
    only changes client speculation, so it is deliberately not here yet -
    building it first means debugging two unknowns at once. [open]

    THE DELAY IS AT LEAST ONE FRAME, and that is structural rather than a
    tuning choice. A player's word spans ports, so a word injected into the
    first port would need bits not yet captured from the last. With a delay
    of one or more, inject reads a word completed on an earlier frame and
    capture builds one for a later frame, so both stay per-port inside
    MAME's existing loop.

    THE DELAY IS LOCAL. m_delay decides only which future frame this peer's
    own capture lands on, and emulated state depends solely on the contents
    of a ring slot, so peers may run different delays and stay
    bit-identical. notes/protocol.txt 1.6.3.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_MANAGER_H
#define MAME_EMU_NETPLAY_NETPLAY_MANAGER_H

#pragma once

#include "netplay/netplay_inputmap.h"
#include "netplay/netplay_packet.h"
#include "netplay/netplay_transport.h"
#include "netplay/netplay_types.h"

#include <cstdio>
#include <memory>


namespace netplay {

class manager
{
public:
	// ---- ioport_manager::frame_update hooks ----
	//
	// frame_begin holds the frame until its input has arrived; the other
	// three run per port and at end of frame. CAPTURE BEFORE INJECT within
	// one port's iteration, or capture reads back the remote values inject
	// just wrote. All four are no-ops unless a session is running.
	static void frame_begin(running_machine &machine);
	static void port_capture(ioport_port &port);
	static void port_inject(ioport_port &port);
	static void frame_end(running_machine &machine);

	// public only because std::unique_ptr holds one; construction stays
	// private so a session can only come from open_session()
	~manager();

	frame_t frame() const { return m_frame; }
	player_t local_slot() const { return m_local; }
	unsigned delay() const { return m_delay; }

private:
	// Enough for any delay worth having: at 60 Hz, 128 frames is 2.1 s, and
	// a lockstep session two seconds behind has already failed.
	static constexpr unsigned RING = 128;

	// The tier ladder tops out at 6 frames per packet (protocol.txt 1.6);
	// 8 leaves headroom without making the buffers interesting.
	static constexpr unsigned MAX_BATCH = 8;

	// A wait this long is not jitter. Ending it runs a frame on incomplete
	// input, which is a desync and what resync exists for, so it is counted
	// rather than hidden. [open]
	static constexpr unsigned STALL_LIMIT = 240;

	struct slot
	{
		frame_t     stamp;      // which frame these words belong to
		u8          have;       // bit p-1 set once player p's word arrived
		input_word  words[MAX_PLAYERS];
	};

	manager(running_machine &machine, player_t local, player_t players,
			unsigned delay, unsigned batch, bool redundant, bool selftest);
	bool setup(const char *logpath, std::unique_ptr<transport> link);

	static manager *active();
	static void open_session(running_machine &machine);

	slot &at(frame_t frame) { return m_ring[frame % RING]; }
	void touch(frame_t frame);
	bool complete(frame_t frame);

	void publish(frame_t frame, player_t player, input_word word);
	void batch_add(input_word word, frame_t frame);
	void batch_flush();
	void poll_link();
	void wait_for_input();
	void check_selftest();
	void on_exit();

	running_machine &           m_machine;
	inputmap                    m_map;
	std::unique_ptr<transport>  m_link;

	frame_t                     m_frame;
	player_t                    m_local;
	player_t                    m_players;
	unsigned                    m_delay;
	unsigned                    m_batch;        // tier K, frames per packet
	bool                        m_redundant;
	u8                          m_need;         // slots that must arrive

	input_word                  m_pending;      // local word for m_frame + m_delay
	input_word                  m_echo;         // what inject put back, for selftest
	u32                         m_local_mask;

	slot                        m_ring[RING];

	// the outgoing batch, and the previous one for redundancy
	input_word                  m_out[MAX_BATCH];
	frame_t                     m_out_base;
	unsigned                    m_out_n;
	input_word                  m_prev[MAX_BATCH];
	frame_t                     m_prev_base;
	unsigned                    m_prev_n;

	// Accounting. The sizing work in protocol.txt 1.6.3 is these numbers:
	// stalls and stall_frames are what a delay is chosen to drive to zero.
	u64                         m_stalls;
	u64                         m_stall_frames;
	u64                         m_starved;
	u64                         m_malformed;

	bool                        m_selftest;
	u64                         m_selftest_fail;
	FILE *                      m_log;
};

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_MANAGER_H
