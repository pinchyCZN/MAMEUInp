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
#include "netplay/netplay_resync.h"
#include "netplay/netplay_transport.h"
#include "netplay/netplay_types.h"

#include <cstdio>
#include <deque>
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

	// Whether a session is CONFIGURED, which is true before one is open.
	static bool configured();

	// The frame whose input was just applied, for stamping diagnostics.
	// False when no session owns this machine, leaving the caller to use
	// its own clock. Callers run after frame_end, hence m_frame - 1.
	static bool stamp_frame(running_machine &machine, u64 &frame);

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

	// Called from running_machine::run between timeslices - the only point
	// MAME itself saves or loads at. Resync capture and restore happen
	// here; see the comment at the call site.
	static void safe_point(running_machine &machine);

	// A guest asking the host for its machine state. Deliberate only -
	// see the hotkey note in ui.cpp. Returns false when there is nothing
	// to ask (no session, or this peer IS the host).
	static bool request_resync(running_machine &machine);

	// ---- chat ----
	//
	// The manager owns the text and the timing; the frontend owns the
	// drawing. src/emu must not include src/frontend, and draw_text_full
	// is a mame_ui_manager method, so the split is not a preference.

	// Lines held, and how long one stays up. Six lines is what fits the
	// corner without covering the playfield.
	static constexpr unsigned CHAT_LINES = 6;
	static constexpr unsigned CHAT_SECONDS = 12;
	static constexpr unsigned CHAT_FADE_SECONDS = 2;

	struct chat_line
	{
		const char *text;
		u8          alpha;      // 255 solid, falling to 0 as it expires
	};

	// True while a line is being typed. The caller must then route every
	// keystroke to chat_keys and act on nothing else.
	static bool chat_composing(running_machine &machine);

	// Opens the compose line. False when no session is running, so the
	// key keeps its normal meaning outside netplay.
	static bool chat_begin(running_machine &machine);

	// Consumes typed characters, Enter, Escape and Backspace.
	static void chat_keys(running_machine &machine);

	// Sampled once per RENDERED frame, from update_and_render, because
	// that runs whatever owns the UI and whether or not the machine is
	// paused. The input hook does not: it stops with the scheduler, so a
	// key pressed while a menu is up would still look new afterwards.
	static void sample_ui_keys(running_machine &machine);

	// True only on the frame Enter went down. Replaces
	// input_manager::code_pressed_once for the chat keys - see the
	// comment on sample_ui_keys in the .cpp.
	static bool enter_edge(running_machine &machine);

	// Oldest first. Returns how many were written, at most max.
	static unsigned chat_view(running_machine &machine, chat_line *out,
			unsigned max);

	// The line being typed, or nullptr when not composing.
	static const char *chat_prompt(running_machine &machine);

	// public only because std::unique_ptr holds one; construction stays
	// private so a session can only come from open_session()
	~manager();

	frame_t frame() const { return m_frame; }
	player_t local_slot() const { return m_local; }
	unsigned delay() const { return m_delay; }

private:
	// Enough for any delay worth having: at 60 Hz, 128 frames is 2.1 s, and
	// a lockstep session two seconds behind has already failed.
	// Must cover the largest lag a resync can open. The guest rewinds its
	// frame counter to the state's frame and replays forward, so every
	// frame between there and the present needs its words STILL in the
	// ring. A transfer plus the replay can span a couple of seconds, so
	// 128 frames was far too few. 1024 is about 17 seconds and costs
	// 1024 x (4 + 1 + 80) bytes, which is nothing.
	static constexpr unsigned RING = 1024;

	// The tier ladder tops out at 6 frames per packet (protocol.txt 1.6);
	// 8 leaves headroom without making the buffers interesting.
	static constexpr unsigned MAX_BATCH = 8;

	// Redundancy depth: how many separate datagrams carry a given frame's
	// word. Depth 1 is no redundancy and one lost packet ends the session.
	//
	// 16 rather than something larger because depth x batch words have to
	// fit one datagram: 16 x 8 = 128 words is 1036 bytes against
	// MAX_DATAGRAM's 1200. The deadline clamp below almost always bites
	// first anyway.
	static constexpr unsigned REDUNDANCY_MAX = 16;

	// Retransmission request cadence, in hold-loop polls. transport::wait
	// is about a millisecond, so these are roughly milliseconds.
	//
	// FIRST is short because the cost of a request is a round trip and
	// that is the whole latency being added; REPEAT is longer than one
	// round trip on a typical link so a reply gets a chance to arrive,
	// but short enough that a lost request or lost reply is retried
	// quickly. Both the request and its answer are single datagrams and
	// can themselves be lost, which is why this repeats at all.
	static constexpr unsigned NAK_FIRST = 8;
	static constexpr unsigned NAK_REPEAT = 50;

	// When to tell the player a wait has stopped being normal, in poll
	// iterations of about a millisecond - so about three seconds. By then
	// redundancy has not covered it and several retransmission requests
	// have gone unanswered, which means the link is down or the peer is
	// gone, not that a packet was lost.
	//
	// THE WAIT DOES NOT END. Running a frame on incomplete input would
	// desync silently and permanently; this only puts a message on the
	// screen so a frozen picture has a reason attached to it.
	static constexpr unsigned STALL_LIMIT = 3000;

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
			unsigned delay, unsigned batch, bool selftest);
	bool setup(const char *logpath, std::unique_ptr<transport> link);

	static manager *active();
	static void open_session(running_machine &machine);

	slot &at(frame_t frame) { return m_ring[frame % RING]; }
	void touch(frame_t frame);
	bool complete(frame_t frame);

	void publish(frame_t frame, player_t player, input_word word);
	void batch_add(input_word word, frame_t frame);
	void batch_flush();

	// Sends one INPUT datagram holding the contiguous run of OUR OWN
	// words that ends at `newest`, at most `span` frames long, shortened
	// to whatever the ring actually holds. `fresh` is how many of those
	// frames are new, and only sets FLAG_REDUNDANT truthfully. Returns
	// the frames sent.
	unsigned send_window(frame_t newest, unsigned span, unsigned fresh);

	// Publishes our own word for every frame after m_published up to and
	// including `through`, repeating `word` across any gap. THE one rule
	// for what goes on the wire - see the comment at the frame_end call
	// site for why a second mechanism must not exist.
	void publish_up_to(frame_t through, input_word word);

	// Ask the peers for the words this peer is missing, and answer such a
	// request from another peer. notes/protocol.txt 1.7.4
	void send_nak();
	void answer_nak(frame_t frame, player_t from, u8 want, u8 count);
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
	void send_resync_req();
	void serve_resync(const resync_req_payload &req);
	void take_resync_chunk(frame_t state_frame, const resync_payload &p,
			const u8 *chunk, size_t chunk_len);
	void apply_resync();
	void pump_resync_send();
	void service_resync();
	void take_snapshot();
	void send_nak(u8 about, u8 reason);
	void send_config();
	void take_config(u64 digest, u16 flags, const config_rec *recs,
			unsigned count);
	void take_nak(u8 about, u8 reason);
	void do_capture();
	void test_corrupt();

	running_machine &           m_machine;
	inputmap                    m_map;
	std::unique_ptr<transport>  m_link;

	frame_t                     m_frame;
	player_t                    m_local;
	player_t                    m_players;
	unsigned                    m_delay;
	unsigned                    m_batch;        // tier K, frames per packet
	unsigned                    m_depth;        // datagrams per word

	// Does this session use savestates at all? One switch over resync
	// serving, asking, the periodic snapshot and the state push, because
	// they all rest on the same thing: that this driver can reload its
	// own state. determinism.txt 6.2 shows that is per driver and that
	// MACHINE_SUPPORTS_SAVE does not predict it, so it has to be a
	// decision somebody makes rather than something inferred.
	bool                        m_savestates;

	// Does the host hand its DIP, config and analog settings to guests?
	// On by default: joining somebody's game and playing it with their
	// settings is what every other multiplayer game does, and the
	// alternative is a session that will not start and a player who
	// cannot tell which switch is wrong. Off restores the refusal.
	bool                        m_send_config;
	bool                        m_config_done;
	bool                        m_config_checked;
	u8                          m_need;         // slots that must arrive

	input_word                  m_pending;      // local word for m_frame + m_delay
	input_word                  m_echo;         // what inject put back, for selftest
	u32                         m_local_mask;

	slot                        m_ring[RING];

	// the outgoing batch, and the previous one for redundancy
	input_word                  m_out[MAX_BATCH];
	frame_t                     m_out_base;
	unsigned                    m_out_n;

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
	u64                         m_naks_sent;
	u64                         m_naks_answered;
	u64                         m_naks_served;
	char                        m_missing[96];

	// Where to dump the status readout, and how long since the last one.
	// A wedged session cannot be asked what it is doing, and every bug so
	// far has been an ordering question rather than a wrong value, so the
	// useful thing to externalise is state, not memory.
	std::string                 m_status_path;
	u64                         m_status_due;
	u64                         m_idle_ticks;

	// ---- resync ----
	//
	// NOBODY PAUSES. The guest stays in lockstep throughout: a desync is
	// divergent state, not divergent frame numbers, so input keeps being
	// exchanged and the guest keeps publishing its own words. That is what
	// makes the catch-up a replay out of its OWN ring rather than
	// something that has to be fetched.
	state_blob                  m_blob;         // sending or receiving
	u32                         m_resync_id;
	bool                        m_resync_wanted;
	bool                        m_resync_serving;
	frame_t                     m_resync_frame;
	u64                         m_resync_nag;
	std::deque<unsigned>        m_send_queue;   // chunk indices still to go

	// Capture and apply happen at frame_begin only, so m_resync_frame
	// always means "state as of the START of this frame" on both sides.
	// poll_link runs from frame_begin AND frame_end, so doing the work
	// where the packet arrives would make that frame number mean
	// before-the-frame on one path and after-it on the other.
	bool                        m_req_pending;
	resync_req_payload          m_req;
	bool                        m_resync_ready;

	// The id of the transfer most recently applied. An unsolicited chunk
	// carrying it is a straggler from a transfer already finished with,
	// and must not be allowed to start a new one.
	u32                         m_resync_done_id;

	// Host side: snapshot every m_snap_period frames, keeping
	// m_snap_keep of them, and push state to a guest once when the
	// session started from one.
	frame_t                     m_snap_period;
	unsigned                    m_snap_keep;
	unsigned                    m_snap_next;
	frame_t                     m_snap_due;
	bool                        m_push_wanted;
	bool                        m_push_done;
	bool                        m_snap_pending;
	bool                        m_capture_pending;
	unsigned                    m_unsafe;       // safe points can_save refused

	// Highest frame whose local word has gone out; 0 before the first.
	// See frame_end - this is what makes a hole in the stream impossible.
	frame_t                     m_published;
	bool                        m_own_hole;     // reported once
	bool                        m_stall_warned;
	bool                        m_refused_reported;

	// Chat. osd_ticks drives the expiry, which is safe only because none
	// of this reaches emulated state - see determinism.txt 2.2 for the
	// one door wall-clock time is allowed through, and this is not it.
	struct chat_entry
	{
		char        text[CHAT_MAX_TEXT + 24];   // room for the "P1: " prefix
		osd_ticks_t shown;
	};
	chat_entry                  m_chat[CHAT_LINES];
	unsigned                    m_chat_n;       // total ever added
	// Enter, tracked by us rather than by input_manager. One edge shared
	// by opening and sending, so a key held across both cannot do both.
	bool                        m_enter_prev;
	bool                        m_enter_edge;

	bool                        m_chat_open;
	char                        m_chat_edit[CHAT_MAX_TEXT + 1];
	unsigned                    m_chat_len;
	// DIAGNOSTIC. Opens the compose box at a frame and sends it HOLD
	// frames later, so a test can check that nothing is published while
	// it is open. See the header comment on the other NETPLAY_TEST vars.
	frame_t                     m_test_chat_at;

	frame_t                     m_test_resync_every;
	frame_t                     m_test_chat_hold;
	char                        m_chat_prompt[CHAT_MAX_TEXT + 8];

	void chat_push(player_t from, const char *text);
	void chat_send();


	// Replaying already-known input to close the gap. Capture and publish
	// are suppressed while this is set: those frames were captured and
	// published once already, and re-reading the live controls would
	// invent input the other peer never saw.
	bool                        m_replaying;
	frame_t                     m_replay_until;
	bool                        m_catchup;
	u64                         m_resyncs;

	// DIAGNOSTIC. A resync can only be shown to work by breaking the
	// session first: NETPLAY_TEST_CORRUPT_AT scribbles on a save entry at
	// that frame, NETPLAY_TEST_RESYNC_AT asks for a state at that one. The
	// hash logs then have to diverge and converge again, which is the only
	// evidence that recovery happened rather than nothing happening.
	frame_t                     m_test_corrupt_at;
	frame_t                     m_test_resync_at;

	bool                        m_selftest;
	u64                         m_selftest_fail;
	FILE *                      m_log;
};

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_MANAGER_H
