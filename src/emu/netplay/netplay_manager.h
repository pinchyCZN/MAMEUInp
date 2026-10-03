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
#include <string>


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

	// Originates a frame-stamped UI command. Returns true when netplay has
	// taken the request, in which case the caller must NOT act locally -
	// the command applies on every peer including this one, at the stamped
	// frame. Returns false with no session, so MAME behaves normally.
	static bool ui_request(running_machine &machine, u8 cmd);

	// True while a session is running. The UI uses it to refuse the keys
	// that cannot be made safe by stamping them with a frame - see
	// notes/netplay-hooks.txt 6.
	static bool session_active(running_machine &machine);

	// Slot 1 is the host. Savestate load and save are the host's alone:
	// a load replaces machine state under one peer, and the plan is for the
	// host to save, rehost later and have guests resync to it.
	static bool is_host(running_machine &machine);

	// Every UI action netplay refuses, and the single place that decides.
	// Kept here rather than spread across ui.cpp so the policy is
	// auditable in one read and the upstream edit stays one block.
	// notes/netplay-hooks.txt 6.
	enum ui_lock : u8
	{
		LOCK_FRAMESKIP,
		LOCK_THROTTLE,
		LOCK_FASTFORWARD,
		LOCK_GFXVIEWER,
		LOCK_SAVESTATE,
		LOCK_LOADSTATE
	};

	// nullptr when the action is allowed, otherwise the reason to show the
	// player. Returns nullptr for everything when no session is running, so
	// single-player MAME behaves exactly as it always did.
	static const char *ui_locked(running_machine &machine, u8 what);

	// Called once per presentation frame from video_manager::frame_update,
	// which unlike the ioport hooks still runs while the machine is PAUSED.
	// A peer sitting in a MAME menu produces no frames, so none of the
	// other hooks run and it stops talking entirely - and a server that
	// tracks liveness then drops it. Observed: both peers opening the
	// savestate prompt killed the session.
	static void idle_update(running_machine &machine);

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

	// Hold this long, in poll iterations of about a millisecond, before
	// pausing and pumping the display. Short stalls are ordinary jitter and
	// pausing for them flickers the screen.
	static constexpr unsigned PUMP_AFTER = 100;

	// Waiting for the other peer's process to finish launching is a wait
	// of seconds, not milliseconds, and it happens exactly once. udp_link
	// waits a millisecond at a time, so this is about 20 s.
	static constexpr unsigned JOIN_STALL_LIMIT = 20000;

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
	void resend_last();
	void poll_link();
	void wait_for_input();
	bool gated_now() const;
	const char *missing_text();
	void test_pause_tick();
	void apply_ui_command();
	void send_uicmd(u8 cmd);
	void check_selftest();
	void on_exit();
	void write_status();

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
	u64                         m_malformed;

	// Set once a remote player's INPUT has actually been filed. Not the
	// transport's receive count: that includes the handshake reply, and
	// counting it cancels the initial alignment budget the moment the
	// server answers - seconds before the other peer has even launched.
	bool                        m_got_remote;
	unsigned                    m_stall_limit;
	unsigned                    m_join_limit;

	// A frame-stamped UI command waiting to take effect, and the netplay
	// pause state. NOT machine().pause(): a machine paused that way stops
	// the scheduler, so ioport_manager::frame_update never runs and the
	// RESUME could never be seen. Holding the frame in wait_for_input
	// instead keeps the link polled and the display pumped.
	frame_t                     m_cmd_frame;
	u8                          m_cmd;
	bool                        m_cmd_pending;
	bool                        m_net_paused;

	// DIAGNOSTIC. A UI command normally comes from a keypress, which a
	// headless test cannot produce, so NETPLAY_TEST_PAUSE_AT asks for one
	// at a given frame and NETPLAY_TEST_PAUSE_HOLD asks for the matching
	// resume. The point of the frame stamp is that it is exact, and that
	// is not something to ship untested.
	frame_t                     m_test_pause_at;
	unsigned                    m_test_pause_hold;
	u64                         m_test_held;

	// Hold bookkeeping. A held session sends nothing of its own - no frame
	// completes, so nothing is captured - and a server that tracks liveness
	// would drop everyone. m_held_polls paces both the keepalive and the
	// warning the player sees.
	u64                         m_held_polls;
	u64                         m_waits;
	char                        m_missing[96];

	// Where to dump the status readout, and how long since the last one.
	// A wedged session cannot be asked what it is doing, and every bug so
	// far has been an ordering question rather than a wrong value, so the
	// useful thing to externalise is state, not memory.
	std::string                 m_status_path;
	u64                         m_status_due;
	u64                         m_idle_ticks;

	bool                        m_selftest;
	u64                         m_selftest_fail;
	FILE *                      m_log;
};

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_MANAGER_H
