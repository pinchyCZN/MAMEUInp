// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_manager.cpp

    See netplay_manager.h.

***************************************************************************/

#include "emu.h"
#include "netplay/netplay_manager.h"
#include <ctime>
#include "uiinput.h"   // NETPLAY: ui_input_manager is incomplete via emu.h
#include "emuopts.h"  // NETPLAY: emu_options, for -state detection
#include "screen.h"   // NETPLAY: screen_device_iterator for thumbnails
#include "main.h"      // NETPLAY: emulator_info::draw_user_interface
#include "osdepend.h"  // NETPLAY: osd_interface::update
#include "netplay/netplay_hash.h"
#include "netplay/netplay_transport.h"

#include <cstdlib>
#include <cstring>


namespace netplay {

namespace {

// One session per machine. MAMEUI runs machines sequentially in-process, so
// this is torn down by the exit notifier rather than assumed to outlive a run.
std::unique_ptr<manager> s_instance;
running_machine *s_owner = nullptr;

// A whitespace-only value is unset. MAME writes an unset string option into
// an ini as the key padded with spaces, so a value read back that way is
// blanks rather than empty, and testing only the first byte treats it as real.
bool unset(const char *v)
{
	if (v == nullptr)
		return true;
	for ( ; *v != 0; v++)
		if ((*v != 0x20) && (*v != 0x09))
			return false;
	return true;
}

unsigned env_uint(const char *name, unsigned def)
{
	const char *const v = std::getenv(name);
	if (unset(v))
		return def;
	const long n = std::strtol(v, nullptr, 10);
	return (n < 0) ? def : unsigned(n);
}

bool env_flag(const char *name)
{
	return !unset(std::getenv(name));
}

} // anonymous namespace


//-------------------------------------------------
//  construction
//-------------------------------------------------

manager::manager(running_machine &machine, player_t local, player_t players,
		unsigned delay, unsigned batch, bool selftest)
	: m_machine(machine)
	, m_map()
	, m_link()
	, m_frame(0)
	, m_local(local)
	, m_players(players)
	, m_delay(delay)
	, m_batch(batch)
	, m_depth(1)
	, m_savestates(true)
	, m_send_config(true)
	, m_config_done(false)
	, m_config_checked(false)
	, m_need(0)
	, m_pending(0)
	, m_echo(0)
	, m_local_mask(0)
	, m_out_base(0)
	, m_out_n(0)
	, m_stalls(0)
	, m_stall_frames(0)
	, m_malformed(0)
	, m_got_remote(false)
	, m_stall_limit(STALL_LIMIT)
	, m_join_limit(JOIN_STALL_LIMIT)
	, m_cmd_frame(0)
	, m_cmd(0)
	, m_cmd_pending(false)
	, m_net_paused(false)
	, m_test_pause_at(0)
	, m_test_pause_hold(0)
	, m_test_held(0)
	, m_held_polls(0)
	, m_waits(0)
	, m_naks_sent(0)
	, m_naks_answered(0)
	, m_naks_served(0)
	, m_status_path()
	, m_status_due(0)
	, m_idle_ticks(0)
	, m_blob()
	, m_resync_id(0)
	, m_resync_wanted(false)
	, m_resync_serving(false)
	, m_resync_frame(0)
	, m_resync_nag(0)
	, m_req_pending(false)
	, m_req()
	, m_resync_ready(false)
	, m_resync_done_id(0)
	, m_snap_period(0)
	, m_snap_keep(10)
	, m_snap_next(0)
	, m_snap_due(0)
	, m_push_wanted(false)
	, m_push_done(false)
	, m_snap_pending(false)
	, m_capture_pending(false)
	, m_unsafe(0)
	, m_published(0)
	, m_own_hole(false)
	, m_stall_warned(false)
	, m_refused_reported(false)
	, m_chat_n(0)
	, m_enter_prev(false)
	, m_enter_edge(false)
	, m_chat_open(false)
	, m_chat_len(0)
	, m_test_chat_at(0)
	, m_test_resync_every(0)
	, m_test_chat_hold(0)
	, m_replaying(false)
	, m_replay_until(0)
	, m_catchup(false)
	, m_resyncs(0)
	, m_test_corrupt_at(0)
	, m_test_resync_at(0)
	, m_selftest(selftest)
	, m_selftest_fail(0)
	, m_log(nullptr)
{
	std::memset(m_ring, 0, sizeof(m_ring));
	std::memset(m_out, 0, sizeof(m_out));
	m_missing[0] = 0;

	for (player_t p = 1; p <= m_players; p++)
		m_need = u8(m_need | slot_bit(p));

	// A stamp of ~0 cannot collide with a real frame number for the length of
	// any session, so a freshly zeroed ring holds no frame rather than frame 0.
	for (unsigned i = 0; i < RING; i++)
		m_ring[i].stamp = frame_t(~0);

	// Frames 0 .. delay-1 would have been published by frames -delay .. -1,
	// which never ran. Neutral is nothing pressed, which is what the machine
	// would see from a player who has not touched a control yet.
	for (frame_t f = 0; f < frame_t(m_delay); f++)
	{
		slot &s = at(f);
		s.stamp = f;
		s.have = m_need;
		for (player_t p = 0; p < MAX_PLAYERS; p++)
			s.words[p] = 0;
	}

	// Those primed frames are published, as far as the stream is
	// concerned: nothing may ever write them again. Starting the
	// high-water mark here is what lets one rule cover both the first
	// frame and every frame after a resync. m_delay is clamped to 1 or
	// more before we are constructed.
	m_published = frame_t(m_delay) - 1;

	// REDUNDANCY DEPTH, and the clamp is the whole point.
	//
	// A word for frame t is first sent at frame t - delay, and again in
	// each later datagram that still carries it: t - delay + batch,
	// t - delay + 2*batch, and so on. The receiver needs it by frame t, so
	// the last copy that is any use leaves at t - 1. That gives
	//
	//     depth <= (delay - 1) / batch + 1
	//
	// and a copy beyond it is posted after the letter was needed. Asking
	// for more is not refused, just clamped - see notes/protocol.txt 1.7.
	//
	// AT DELAY 1 THE CEILING IS 1: there is exactly one send opportunity,
	// so no amount of configuration protects a delay-1 session from a
	// single lost datagram. That is a reason to raise the delay on a real
	// link, not a defect here.
	unsigned want = env_uint("NETPLAY_REDUNDANCY", 0);
	if (want == 0)
		want = m_delay;             // unset: as deep as the deadline allows

	const unsigned ceiling = ((m_delay - 1) / m_batch) + 1;
	m_depth = want;
	if (m_depth > ceiling)
		m_depth = ceiling;
	if (m_depth > REDUNDANCY_MAX)
		m_depth = REDUNDANCY_MAX;
	if (m_depth < 1)
		m_depth = 1;

	osd_printf_info("netplay: redundancy depth %u (asked %u, ceiling %u), %u byte input packets\n",
			m_depth, want, ceiling,
			unsigned(HEADER_BYTES + (m_depth * m_batch * sizeof(input_word))));

	for (unsigned i = 0; i < CHAT_LINES; i++)
	{
		m_chat[i].text[0] = 0;
		m_chat[i].shown = 0;
	}
	m_chat_edit[0] = 0;
	m_chat_prompt[0] = 0;
	m_test_chat_at = frame_t(env_uint("NETPLAY_TEST_CHAT_AT", 0));

	// THE MASTER SWITCH. Default on, because most drivers do reload
	// their state; off is the deliberate choice for one that does not.
	const char *ss = std::getenv("NETPLAY_SAVESTATES");
	m_savestates = unset(ss)
			|| !((std::strcmp(ss, "0") == 0)
				|| (std::strcmp(ss, "off") == 0)
				|| (std::strcmp(ss, "no") == 0));

	// Sending configuration is on unless turned off.
	const char *sc = std::getenv("NETPLAY_SEND_CONFIG");
	m_send_config = unset(sc)
			|| !((std::strcmp(sc, "0") == 0)
				|| (std::strcmp(sc, "off") == 0)
				|| (std::strcmp(sc, "no") == 0));

	// SNAPSHOT PERIOD, IN SECONDS, HELD IN FRAMES. Seconds is what a
	// player means; frames is what the interval has to be measured in,
	// because what matters is how much PLAY is lost and a host that
	// stalls or fast-forwards drifts from the wall clock.
	//
	// 0 is off, and anything below 10 s is raised to it: snapshotting a
	// multi-megabyte state every couple of seconds costs more than the
	// play it would save.
	unsigned snap_secs = env_uint("NETPLAY_SNAPSHOT_SECS", 0);
	if ((snap_secs != 0) && (snap_secs < 10))
		snap_secs = 10;
	if (!m_savestates)
		snap_secs = 0;          // a state nothing can reload is not worth
	                        // writing, and writing one implies it is
	m_snap_period = frame_t(snap_secs * 60);
	m_snap_due = m_snap_period;

	m_snap_keep = env_uint("NETPLAY_SNAPSHOT_KEEP", 10);
	if (m_snap_keep < 1)
		m_snap_keep = 1;
	if (m_snap_keep > 99)
		m_snap_keep = 99;        // the name is two digits

	// PUSHING STATE TO A GUEST, and the default needs no configuration
	// at all: -state is itself the signal. A host told to load a snapshot
	// is a host whose machine does not match a cold boot, so its guests
	// have to be given that state rather than told to ask for it.
	//
	// Two values, not three. An "always" that pushed on a cold boot too
	// was meant to make a configuration difference harmless, and it
	// cannot: DIP, config and analog settings live outside the save set
	// entirely, so no state transfer repairs them. They are caught by the
	// handshake instead - protocol.txt 4.2.1 - which left that mode
	// paying for nothing.
	//
	// never is the Kaillera fallback, for a driver whose savestates do
	// not round-trip (determinism.txt 6.2): peers cold-boot, nobody
	// transfers anything, and lockstep alone carries the session.
	const char *push = std::getenv("NETPLAY_PUSH_STATE");
	const bool from_state = !unset(m_machine.options().state());
	const bool push_off = !unset(push)
			&& ((std::strcmp(push, "never") == 0)
				|| (std::strcmp(push, "0") == 0)
				|| (std::strcmp(push, "off") == 0));
	m_push_wanted = from_state && !push_off && m_savestates;

	// A CONTRADICTION WORTH SHOUTING ABOUT. -state means the host's
	// machine is not a cold boot, and savestates off means it will not
	// hand that state to anyone - so every guest cold-boots and is
	// desynced from frame 0 with no way back. The lobby will not offer
	// the combination; a command line can still ask for it.
	if (from_state && !m_savestates)
	{
		osd_printf_info("netplay: -state was given but savestates are off: "
				"guests will NOT receive it and will desync\n");
		m_machine.popmessage(
				"Started from a state with savestates disabled - guests will desync");
	}

	if (m_push_wanted && (m_local == 1))
		osd_printf_info("netplay: will push state to guests%s\n",
				from_state ? " (started from -state)" : "");
	m_test_chat_hold = frame_t(env_uint("NETPLAY_TEST_CHAT_HOLD", 0));
	m_test_resync_every =
			frame_t(env_uint("NETPLAY_TEST_RESYNC_EVERY", 0));

	machine.add_notifier(MACHINE_NOTIFY_EXIT,
			machine_notify_delegate(&manager::on_exit, this));
}


manager::~manager()
{
}


//-------------------------------------------------
//  setup - everything that needs the built map,
//  which the constructor cannot touch
//-------------------------------------------------

bool manager::setup(const char *logpath, std::unique_ptr<transport> link)
{
	if (!m_map.build(m_machine))
	{
		// notes/protocol.txt 4.4: a driver the table cannot represent is a
		// refused session, not a warning.
		osd_printf_info("netplay: %s refused - %s\n",
				m_machine.system().name,
				(m_map.unowned() != 0)
						? "digital fields with no owner in the table"
						: "the driver needs more digital bits than the word carries");
		return false;
	}

	m_link = std::move(link);
	m_local_mask = m_map.local_bit_mask();

	if (!unset(logpath))
	{
		m_log = std::fopen(logpath, "w");
		if (m_log != nullptr)
		{
			std::fprintf(m_log, "# machine=%s\n", m_machine.system().name);
			std::fprintf(m_log, "# config_digest=%016llx\n",
					static_cast<unsigned long long>(m_map.config_digest()));

			// Every DIP and config field with the value it is set to. A
			// REFUSE_INPUTCFG says the two peers disagree but not about
			// what; diffing two of these says which switch it was.
			for (auto &entry : m_machine.ioport().ports())
				for (const ioport_field &field : entry.second->fields())
				{
					if ((field.type() != IPT_DIPSWITCH)
							&& (field.type() != IPT_CONFIG))
						continue;
					std::fprintf(m_log,
							"# config %s:%s mask=%08x value=%08x default=%08x\n",
							entry.first.c_str(),
							(field.name() != nullptr) ? field.name() : "?",
							u32(field.mask()),
							u32(field.live().value & field.mask()),
							u32(field.defvalue() & field.mask()));
				}
			std::fprintf(m_log, "# inputmap_digest=%016llx\n",
					static_cast<unsigned long long>(m_map.digest()));
			std::fprintf(m_log, "# players=%d local=%d delay=%d batch=%d depth=%d\n",
					int(m_players), int(m_local), int(m_delay), int(m_batch),
					int(m_depth));
			std::fprintf(m_log, "# transport=%s local_bits=%08x\n",
					m_link->name(), m_local_mask);
			std::fprintf(m_log, "# budget_frames=%d\n",
					int(m_delay) - int(m_batch) + 1);
			std::fprintf(m_log, "frame,captured,injected,have\n");
			std::fflush(m_log);
		}
		else
		{
			osd_printf_info("netplay: unable to open log '%s'\n", logpath);
		}
	}

	osd_printf_info("netplay: savestates %s\n",
			m_savestates ? "enabled" : "DISABLED for this session");
	osd_printf_info("netplay: state is %d KB, resync limit %d KB, %s\n",
			int(state_blob::machine_state_size(m_machine) / 1024),
			int(RESYNC_MAX_BYTES / 1024),
			(state_blob::machine_state_size(m_machine) <= RESYNC_MAX_BYTES)
					? "resync possible" : "RESYNC WILL BE REFUSED");

	osd_printf_info("netplay: %s players=%d local=%d delay=%d batch=%d budget=%d bits=%d digest=%016llx config=%016llx\n",
			m_link->name(), int(m_players), int(m_local), int(m_delay),
			int(m_batch), int(m_delay) - int(m_batch) + 1,
			int(m_map.digital_used()),
			static_cast<unsigned long long>(m_map.digest()),
			static_cast<unsigned long long>(m_map.config_digest()));
	return true;
}


//-------------------------------------------------
//  open_session - lazy, once per machine
//-------------------------------------------------

void manager::open_session(running_machine &machine)
{
	s_instance.reset();
	s_owner = &machine;

	const char *const mode = std::getenv("NETPLAY_MODE");
	if (unset(mode))
		return;

	const bool is_loopback = (std::strcmp(mode, "loopback") == 0);
	const bool is_simlink = (std::strcmp(mode, "simlink") == 0);
	const bool is_udp = (std::strcmp(mode, "udp") == 0);
	if (!is_loopback && !is_simlink && !is_udp)
		return;

	player_t players = player_t(env_uint("NETPLAY_PLAYERS", is_simlink ? 2 : 1));
	player_t local = player_t(env_uint("NETPLAY_SLOT", inputmap::LOCAL_SLOT));
	unsigned delay = env_uint("NETPLAY_DELAY", 2);
	unsigned batch = env_uint("NETPLAY_BATCH", 1);

	// A delay of zero cannot work - see the header.
	if (delay < 1)
		delay = 1;
	if (delay > (RING - 2))
		delay = RING - 2;
	if (batch < 1)
		batch = 1;
	if (batch > MAX_BATCH)
		batch = MAX_BATCH;
	if (players < 1)
		players = 1;
	if (players > MAX_PLAYERS)
		players = MAX_PLAYERS;
	if ((local < 1) || (local > players))
		local = 1;

	// The echo slot is whichever slot is not ours. With one player there is
	// no remote peer, so the link echoes nothing and the session is pure
	// loopback through the real wire format.
	player_t echo_as = 0;
	if (is_simlink)
	{
		echo_as = (local == 1) ? 2 : 1;
		if (echo_as > players)
			echo_as = 0;
	}

	std::unique_ptr<transport> link;

	if (is_udp)
	{
		// The slot comes from configuration, not from the handshake. Waiting
		// for a WELCOME would mean blocking the main thread before the first
		// frame, and that wedges window creation - see netplay_transport.h.
		// The server still validates and can refuse; the refusal is reported
		// when it arrives.
		inputmap probe;
		if (!probe.build(machine))
		{
			osd_printf_info("netplay: %s cannot be represented; not joining\n",
					machine.system().name);
			return;
		}

		const char *host = std::getenv("NETPLAY_HOST");
		if (unset(host))
			host = "127.0.0.1";

		std::unique_ptr<udp_link> net(new udp_link(
				env_uint("NETPLAY_UDP_LOSS", 0),
				env_uint("NETPLAY_UDP_BURST", 1)));
		if (!net->open(host, env_uint("NETPLAY_PORT", 7000)))
			return;

		net->identify(machine.system().name, probe.digest(),
				savestate_layout_hash(machine), local, players);

		link = std::move(net);
	}
	else
	{
		link.reset(new simlink(
				env_uint("NETPLAY_SIM_DELAY", 0),
				env_uint("NETPLAY_SIM_JITTER", 0),
				env_uint("NETPLAY_SIM_LOSS", 0),
				echo_as,
				env_uint("NETPLAY_SIM_SEED", 1)));
	}

	// Overridable so a test can make starvation impossible and ask whether
	// lockstep holds when nothing is ever missing. [open] not a user option
	const unsigned sl = env_uint("NETPLAY_STALL_LIMIT", STALL_LIMIT);

	std::unique_ptr<manager> candidate(new manager(machine, local, players,
			delay, batch, env_flag("NETPLAY_SELFTEST")));

	candidate->m_stall_limit = (sl > 0) ? sl : STALL_LIMIT;
	const char *const status = std::getenv("NETPLAY_STATUS");
	if (!unset(status))
		candidate->m_status_path = status;
	candidate->m_test_pause_at = frame_t(env_uint("NETPLAY_TEST_PAUSE_AT", 0));
	candidate->m_test_corrupt_at = frame_t(env_uint("NETPLAY_TEST_CORRUPT_AT", 0));
	candidate->m_test_resync_at = frame_t(env_uint("NETPLAY_TEST_RESYNC_AT", 0));
	// DEFAULT 0, NOT A NUMBER. A non-zero default made every netplay pause
	// in a real session resume itself after a couple of seconds, because
	// the hold honours this whenever it is set. Observed as a pause that
	// 'unpaused itself'. The diagnostic only exists alongside
	// NETPLAY_TEST_PAUSE_AT, so it is ignored unless that is set too.
	candidate->m_test_pause_hold = (candidate->m_test_pause_at != 0)
			? env_uint("NETPLAY_TEST_PAUSE_HOLD", 120)
			: 0;
	if (sl > JOIN_STALL_LIMIT)
		candidate->m_join_limit = sl;

	if (!candidate->setup(std::getenv("NETPLAY_LOG"), std::move(link)))
		return;

	s_instance = std::move(candidate);
}


manager *manager::active()
{
	return s_instance.get();
}


//-------------------------------------------------
//  ring slots
//-------------------------------------------------

void manager::touch(frame_t frame)
{
	slot &s = at(frame);
	if (s.stamp == frame)
		return;

	// The ring wraps, so this slot still holds frame - RING. Resetting on
	// first touch is what stops a stale word being replayed RING frames on.
	s.stamp = frame;
	s.have = 0;
	for (player_t p = 0; p < MAX_PLAYERS; p++)
		s.words[p] = 0;
}


bool manager::complete(frame_t frame)
{
	const slot &s = at(frame);
	return (s.stamp == frame) && ((s.have & m_need) == m_need);
}


void manager::publish(frame_t frame, player_t player, input_word word)
{
	if ((player < 1) || (player > MAX_PLAYERS))
		return;

	// A frame already run cannot be changed, and a frame further ahead than
	// the ring can hold would alias onto a live slot.
	if (frame < m_frame)
		return;
	if ((frame - m_frame) >= RING)
		return;

	touch(frame);
	slot &s = at(frame);
	s.words[player - 1] = word;
	s.have = u8(s.have | slot_bit(player));
}


//-------------------------------------------------
//  outgoing batch
//-------------------------------------------------

void manager::batch_add(input_word word, frame_t frame)
{
	if (m_out_n == 0)
		m_out_base = frame;

	m_out[m_out_n++] = word;

	if (m_out_n >= m_batch)
		batch_flush();
}


void manager::publish_up_to(frame_t through, input_word word)
{
	if (m_published >= through)
		return;

	const frame_t first = m_published + 1;

	// The local word is known the moment it is captured and never travels
	// to reach us, so it goes straight into the ring. Only other peers'
	// words come off the link.
	for (frame_t f = first; f <= through; f++)
	{
		publish(f, m_local, word);
		batch_add(word, f);
	}

	if (first != through)
		osd_printf_info("netplay: filled stream gap %u..%u at frame %u\n",
				unsigned(first), unsigned(through), unsigned(m_frame));

	m_published = through;
}


unsigned manager::send_window(frame_t newest, unsigned span, unsigned fresh)
{
	if (!m_link || (span == 0))
		return 0;

	if (span > (REDUNDANCY_MAX * MAX_BATCH))
		span = REDUNDANCY_MAX * MAX_BATCH;
	if (span > MAX_PACKET_WORDS)
		span = MAX_PACKET_WORDS;

	// THE WORDS ARE ALREADY IN THE RING. Our own word for every frame we
	// published is sitting at at(f).words[m_local - 1], and the ring holds
	// RING frames, so a resend needs no separate history buffer - which is
	// also one fewer thing to keep in step with the ring.
	//
	// Walk back from newest while the ring still has our word. The header
	// carries ONE base_frame and a count (protocol.txt 3.2), so the run
	// must be contiguous and a gap ends it. Early in a session that
	// shortens the window by itself.
	unsigned n = 0;
	frame_t f = newest;
	for (;;)
	{
		const slot &sl = at(f);
		if ((sl.stamp != f) || ((sl.have & slot_bit(m_local)) == 0))
			break;

		n++;
		if ((n >= span) || (f == 0))
			break;
		f--;
	}

	if (n == 0)
		return 0;

	const frame_t first = newest - frame_t(n - 1);

	input_word words[REDUNDANCY_MAX * MAX_BATCH];
	for (unsigned i = 0; i < n; i++)
		words[i] = at(first + frame_t(i)).words[m_local - 1];

	packet_header head;
	head.type = MSG_INPUT;
	head.version = PROTOCOL_VERSION;
	head.session = 0;               // assigned by the server at join [open]
	head.base_frame = first;
	head.nframes = u8(n);
	head.player_mask = slot_bit(m_local);
	head.flags = (n > fresh) ? FLAG_REDUNDANT : 0;
	head.reserved = 0;

	u8 buf[MAX_DATAGRAM];
	const size_t len = build_input(buf, sizeof(buf), head, words, nullptr);
	if (len == 0)
		return 0;

	m_link->send(buf, len);
	return n;
}


void manager::batch_flush()
{
	if ((m_out_n == 0) || !m_link)
		return;

	// The batch defines only how many frames are NEW. How many go in the
	// datagram is depth x batch, because a frame's word has to appear in
	// m_depth separate datagrams for a burst of m_depth - 1 losses to be
	// survivable.
	const frame_t newest = m_out_base + frame_t(m_out_n) - 1;
	unsigned span = m_depth * m_batch;
	if (span < m_out_n)
		span = m_out_n;

	send_window(newest, span, m_out_n);

	m_out_n = 0;
}


//-------------------------------------------------
//  resend_last - keepalive that is also redundancy
//-------------------------------------------------

void manager::resend_last()
{
	// A held frame produces no new input, so there is nothing fresh to
	// send - but going quiet makes a liveness-tracking server drop the
	// session (measured: a pause longer than the server timeout ended the
	// game). Repeating the last batch costs one datagram, is idempotent on
	// the receiver because publish() ignores frames already run and
	// rewrites identical words, and doubles as loss repair.
	// The same window batch_flush sends, sent again. Nothing in it is
	// fresh, so every frame is flagged redundant.
	if (!m_link || (m_published == 0))
		return;

	send_window(m_published, m_depth * m_batch, 0);
}


//-------------------------------------------------
//  poll_link - drain whatever has arrived
//-------------------------------------------------

void manager::poll_link()
{
	if (!m_link)
		return;

	u8 buf[MAX_DATAGRAM];
	size_t len;

	while ((len = m_link->recv(buf, sizeof(buf))) != 0)
	{
		packet_header head;
		input_word words[MAX_PACKET_WORDS];   // a legal packet cannot exceed this
		u64 hash = 0;
		bool has_hash = false;

		resync_req_payload rq;
		if (parse_resync_req(buf, len, rq))
		{
			// Recorded, not acted on - see m_req_pending. A fresh ask
			// outranks a retransmission request: the retransmission
			// names chunks of a blob that is about to be replaced.
			if (!m_req_pending || (rq.id == 0))
			{
				m_req = rq;
				m_req_pending = true;
			}
			continue;
		}

		frame_t sf = 0;
		resync_payload rp;
		const u8 *rchunk = nullptr;
		size_t rlen = 0;
		if (parse_resync(buf, len, sf, rp, rchunk, rlen))
		{
			take_resync_chunk(sf, rp, rchunk, rlen);
			continue;
		}

		frame_t nak_frame = 0;
		player_t nak_from = 0;
		u8 nak_want = 0, nak_count = 0;
		if (parse_stall(buf, len, nak_frame, nak_from, nak_want, nak_count))
		{
			answer_nak(nak_frame, nak_from, nak_want, nak_count);
			continue;
		}

		player_t said_no = 0;
		u8 nk_about = 0, nk_reason = 0;
		if (parse_nak(buf, len, said_no, nk_about, nk_reason))
		{
			if (said_no != m_local)
				take_nak(nk_about, nk_reason);
			continue;
		}

		player_t cfg_from = 0;
		u64 cfg_digest = 0;
		u16 cfg_flags = 0;
		unsigned cfg_count = 0;
		config_rec cfg_recs[CONFIG_MAX_FIELDS];
		if (parse_config(buf, len, cfg_from, cfg_digest, cfg_flags,
				cfg_recs, CONFIG_MAX_FIELDS, cfg_count))
		{
			if (cfg_from != m_local)
				take_config(cfg_digest, cfg_flags, cfg_recs, cfg_count);
			continue;
		}

		player_t said = 0;
		char line[CHAT_MAX_TEXT + 1];
		if (parse_chat(buf, len, said, line))
		{
			// Our own line is shown when we send it, not when the relay
			// hands it back, so it does not appear twice.
			if (said != m_local)
				chat_push(said, line);
			continue;
		}

		frame_t eff = 0;
		u8 cmd = 0;
		if (parse_uicmd(buf, len, eff, cmd))
		{
			// A command the originator also applies, so it is not re-sent
			// here. First one wins: a second arriving before the first has
			// taken effect would otherwise silently replace it.
			if (!m_cmd_pending)
			{
				m_cmd_frame = eff;
				m_cmd = cmd;
				m_cmd_pending = true;
				osd_printf_info("netplay: %s scheduled for frame %u\n",
						uicmd_text(cmd), unsigned(eff));
			}
			continue;
		}

		if (!parse_input(buf, len, head, words,
				sizeof(words) / sizeof(words[0]), hash, has_hash))
		{
			m_malformed++;
			continue;
		}

		unsigned idx = 0;
		for (unsigned f = 0; f < head.nframes; f++)
		{
			for (player_t p = 1; p <= MAX_PLAYERS; p++)
			{
				if ((head.player_mask & slot_bit(p)) == 0)
					continue;
				if (p != m_local)
					m_got_remote = true;
				publish(head.base_frame + frame_t(f), p, words[idx++]);
			}
		}
	}
}


//-------------------------------------------------
//  apply_ui_command - at the stamped frame, on every
//  peer, including the one that asked
//-------------------------------------------------

void manager::apply_ui_command()
{
	if (!m_cmd_pending)
		return;

	// Nothing to do until the frame arrives. A stamp already behind us is
	// a bug in the margin ui_request() picks, not something to swallow:
	// pausing late is harmless but resetting late desyncs, so say so.
	if (m_frame < m_cmd_frame)
		return;
	if (m_frame > m_cmd_frame)
		osd_printf_warning("netplay: %s stamped for frame %u arrived at %u\n",
				uicmd_text(m_cmd), unsigned(m_cmd_frame), unsigned(m_frame));

	const u8 cmd = m_cmd;
	m_cmd_pending = false;

	switch (cmd)
	{
	case UICMD_PAUSE_TOGGLE:
		m_net_paused = !m_net_paused;
		osd_printf_info("netplay: %s at frame %u\n",
				m_net_paused ? "paused" : "resumed", unsigned(m_frame));
		break;

	case UICMD_SOFT_RESET:
		// Deterministic because every peer does it before the same frame.
		// The input ring is untouched: words already published for later
		// frames stay valid, since a reset does not change frame numbering.
		osd_printf_info("netplay: soft reset at frame %u\n", unsigned(m_frame));
		m_machine.schedule_soft_reset();
		break;

	default:
		break;
	}
}


//-------------------------------------------------
//  send_uicmd / ui_request
//-------------------------------------------------

void manager::send_uicmd(u8 cmd)
{
	// Normally far enough ahead that every peer still has the frame in
	// front of it: lockstep keeps peers within about m_delay frames of each
	// other, so twice the delay plus a margin covers the spread and the
	// flight time.
	//
	// A RESUME IS THE EXCEPTION AND MUST BE STAMPED FOR NOW. While the
	// session is held no frames pass, so a resume stamped for a future
	// frame can never come due and the pause is permanent. Stamping it for
	// the current frame is still deterministic: every peer is held on the
	// SAME frame, because that is the frame the pause was stamped for.
	const bool resuming = m_net_paused && (cmd == UICMD_PAUSE_TOGGLE);
	const frame_t eff = resuming
			? m_frame
			: (m_frame + frame_t((m_delay * 2) + 4));

	m_cmd_frame = eff;
	m_cmd = cmd;
	m_cmd_pending = true;

	if (!m_link)
		return;

	u8 buf[MAX_DATAGRAM];
	const size_t len = build_uicmd(buf, sizeof(buf), 0, eff, cmd);
	if (len != 0)
		m_link->send(buf, len);

	osd_printf_info("netplay: requested %s for frame %u\n",
			uicmd_text(cmd), unsigned(eff));
}


bool manager::stamp_frame(running_machine &machine, u64 &frame)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine))
		return false;

	frame = (m->m_frame == 0) ? 0 : u64(m->m_frame - 1);
	return true;
}


bool manager::session_active(running_machine &machine)
{
	return (active() != nullptr) && (s_owner == &machine);
}


bool manager::is_host(running_machine &machine)
{
	manager *const m = active();
	return (m != nullptr) && (s_owner == &machine) && (m->m_local == 1);
}


const char *manager::ui_locked(running_machine &machine, u8 what)
{
	if (!session_active(machine))
		return nullptr;

	switch (what)
	{
	// Frameskip is the only one of these that is a genuine desync: it
	// perturbs emulated state on several drivers (notes/determinism.txt 3).
	case LOCK_FRAMESKIP:
		return "Frameskip is locked during netplay";

	// These two are safe now that a missing word is waited for rather than
	// invented, but an unthrottled peer outruns its input every frame and
	// spends the session in the lockstep hold.
	case LOCK_THROTTLE:
		return "Throttle is locked during netplay";
	case LOCK_FASTFORWARD:
		return "Fast forward is locked during netplay";

	// Pauses the machine for as long as someone is browsing tiles, which
	// leaves every other peer waiting on input that is not coming.
	case LOCK_GFXVIEWER:
		return "The graphics viewer is locked during netplay";

	// The host's alone. A load replaces machine state under one peer, and
	// the intended use is the host saving, rehosting, and guests resyncing.
	case LOCK_SAVESTATE:
		return is_host(machine)
				? nullptr : "Only the host can save state during netplay";
	case LOCK_LOADSTATE:
		return is_host(machine)
				? nullptr : "Only the host can load state during netplay";

	default:
		return nullptr;
	}
}


void manager::idle_update(running_machine &machine)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine))
		return;

	// Only while the scheduler is stopped. When it is running the ioport
	// hooks already poll every frame, and doing it twice would just be
	// noise.
	if (!machine.paused())
		return;

	m->poll_link();

	// About twice a second. Deliberately NOT a new message type: repeating
	// the last input batch is idempotent on the receiver and doubles as
	// loss repair, which is the same reason the hold uses it.
	m->m_idle_ticks++;
	if ((m->m_idle_ticks % 30) == 0)
	{
		m->resend_last();
		m->write_status();
	}
}


bool manager::ui_request(running_machine &machine, u8 cmd)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine))
		return false;           // no session; let MAME act normally

	// One at a time. Queueing them would mean deciding what a pause
	// stacked behind a reset means, and there is no answer worth having.
	if (m->m_cmd_pending)
		return true;

	m->send_uicmd(cmd);
	return true;
}


//-------------------------------------------------
//  resync: ask, serve, assemble, apply
//-------------------------------------------------

bool manager::request_resync(running_machine &machine)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine))
		return false;

	if (m->m_local == 1)
	{
		machine.popmessage("The host has nobody to resync from");
		return true;
	}

	// Answered locally rather than by asking and being refused: this
	// peer already knows the session does not use savestates, and a
	// round trip would say nothing it does not.
	if (!m->m_savestates)
	{
		machine.popmessage("Savestates are disabled for this session");
		return true;
	}
	if (m->m_resync_wanted)
		return true;            // already asking

	m->m_blob.clear();
	m->m_resync_id = 0;         // 0 asks the host to start a fresh transfer
	m->m_resync_wanted = true;
	m->m_resync_nag = 0;
	m->send_resync_req();
	machine.popmessage("Resync requested from the host...");
	return true;
}


void manager::send_resync_req()
{
	if (!m_link)
		return;

	resync_req_payload req;
	req.id = m_resync_id;
	std::memset(req.bitmap, 0, sizeof(req.bitmap));

	// One message does both jobs: a fresh ask carries no bitmap at all, a
	// follow-up carries exactly the chunks still missing and no more.
	if (m_resync_id == 0)
	{
		req.want = 0;
		req.bitmap_bytes = 0;
	}
	else
	{
		req.want = u16(m_blob.missing(req.bitmap));
		req.bitmap_bytes = u16(resync_bitmap_bytes(m_blob.chunks()));
	}

	u8 buf[MAX_DATAGRAM];
	const size_t len = build_resync_req(buf, sizeof(buf), 0, req);
	if (len != 0)
		m_link->send(buf, len);
}


void manager::service_resync()
{
	if (m_req_pending)
	{
		m_req_pending = false;
		serve_resync(m_req);
	}
}


//-------------------------------------------------
//  safe_point - capture and restore, and only here
//-------------------------------------------------

void manager::safe_point(running_machine &machine)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine))
		return;
	if (!m->m_capture_pending && !m->m_resync_ready && !m->m_snap_pending)
		return;

	// Upstream's own gate, for upstream's own reason: an anonymous timer
	// still pending can fire and overwrite what was just written.
	// running_machine::handle_saveload refuses on the same condition.
	if (!machine.scheduler().can_save())
	{
		if (++m->m_unsafe < 600)
			return;

		m->m_unsafe = 0;
		m->m_capture_pending = false;
		m->m_resync_ready = false;
		m->m_resync_wanted = false;
		m->m_blob.clear();
		osd_printf_info("netplay: resync abandoned, anonymous timers never cleared\n");
		machine.popmessage("Resync failed: the machine never reached a safe point");
		return;
	}

	m->m_unsafe = 0;

	if (m->m_capture_pending)
	{
		m->m_capture_pending = false;
		m->do_capture();
	}

	if (m->m_snap_pending)
	{
		m->m_snap_pending = false;
		m->take_snapshot();
	}

	if (m->m_resync_ready)
	{
		m->m_resync_ready = false;
		m->apply_resync();
	}
}


void manager::serve_resync(const resync_req_payload &req)
{
	// The host alone answers these. A guest receiving one is seeing a relay
	// of somebody else's ask.
	if (m_local != 1)
		return;

	// A fresh ask cannot be answered from here: the snapshot has to be
	// taken at a scheduler-safe point, so it is only armed. The host does
	// NOT pause - it carries on playing while the blob goes out.
	if (req.id == 0)
	{
		// SAY NO RATHER THAN NOTHING. Silence is indistinguishable from a
		// lost packet, so the asker nags for ten seconds and then reports a
		// timeout - which sends whoever is playing after a network fault
		// that is not there.
		if (!m_savestates)
		{
			send_nak(NAK_ABOUT_RESYNC, NAK_DISABLED);
			return;
		}

		m_capture_pending = true;
		return;
	}

	if ((req.id != m_resync_id) || !m_resync_serving)
		return;                 // a stale attempt

	// A follow-up names the chunks still missing from a blob already held.
	m_send_queue.clear();
	for (unsigned i = 0; i < m_blob.chunks(); i++)
	{
		if ((req.bitmap[i / 8] & (1u << (i % 8))) != 0)
			m_send_queue.push_back(i);
	}
}


void manager::do_capture()
{
	if (!m_blob.capture(m_machine))
	{
		// The asker is told too, or it would wait out the nag for a
		// refusal already decided here.
		send_nak(NAK_ABOUT_RESYNC, NAK_TOO_LARGE);
		m_machine.popmessage("Resync refused: this driver state is too large");
		return;
	}

	m_resync_id++;
	if (m_resync_id == 0)
		m_resync_id = 1;

	// m_frame is the NEXT frame to be injected: the hook for the frame
	// now being emulated has already run and advanced it. The guest sets
	// its counter to exactly this and injects the same frame next, from a
	// machine sitting mid-frame in precisely the same place.
	m_resync_frame = m_frame;
	m_resync_serving = true;
	m_resyncs++;

	m_send_queue.clear();
	for (unsigned i = 0; i < m_blob.chunks(); i++)
		m_send_queue.push_back(i);

	osd_printf_info("netplay: serving resync %u at frame %u, %d KB raw -> %d KB in %d chunks\n",
			unsigned(m_resync_id), unsigned(m_resync_frame),
			int(m_blob.raw_size() / 1024), int(m_blob.size() / 1024),
			int(m_blob.chunks()));
}


void manager::pump_resync_send()
{
	// PACED, NOT BURSTED. Handing thousands of datagrams to the socket in
	// one call overruns its buffer and most are dropped before anyone reads
	// them - which then shows up as a transfer that never completes. A fixed
	// number per frame keeps it inside what the link can carry and lets the
	// game keep running while it goes out.
	if (m_send_queue.empty() || !m_link || !m_resync_serving)
		return;

	resync_payload p;
	p.id = m_resync_id;
	p.total = u32(m_blob.size());
	p.chunks = u16(m_blob.chunks());

	unsigned sent = 0;
	while (!m_send_queue.empty() && (sent < RESYNC_CHUNKS_PER_FRAME))
	{
		const unsigned i = m_send_queue.front();
		m_send_queue.pop_front();

		if (i >= m_blob.chunks())
			continue;

		const size_t off = size_t(i) * RESYNC_CHUNK;
		const size_t left = m_blob.size() - off;
		const size_t bytes = (left < RESYNC_CHUNK) ? left : RESYNC_CHUNK;

		p.index = u16(i);
		u8 buf[MAX_DATAGRAM];
		const size_t len = build_resync(buf, sizeof(buf), 0, m_resync_frame,
				p, m_blob.data() + off, bytes);
		if (len != 0)
			m_link->send(buf, len);
		sent++;
	}
}


void manager::take_resync_chunk(frame_t state_frame, const resync_payload &p,
		const u8 *chunk, size_t chunk_len)
{
	// A straggler from a transfer already applied. Without this it would
	// arm the push path below and start the whole thing again.
	if ((p.id != 0) && (p.id == m_resync_done_id))
		return;

	// UNSOLICITED CHUNKS ARE ACCEPTED, and that is the entire mechanism
	// for "the host started from a snapshot, take its state". No new
	// message type, no flag in the handshake, no server involvement: the
	// arrival of the state IS the notification.
	//
	// Arming m_resync_wanted is what makes that safe rather than
	// half-working. Every piece of the completion machinery keys off that
	// flag - the missing-chunk bitmap, the nag that chases them, the
	// give-up - so a push with one chunk lost would otherwise leave a
	// partial blob that nothing could ever finish and no one would
	// report, with the guest playing on desynced.
	if (!m_resync_wanted)
	{
		m_resync_wanted = true;
		m_resync_nag = 0;
		m_resync_id = 0;           // forces begin_receive below
		osd_printf_info("netplay: host is sending state unasked, taking it\n");
		m_machine.popmessage("Receiving state from the host...");
	}

	if (m_resync_id != p.id)
	{
		// First chunk of a new transfer, or the host restarted one.
		m_resync_id = p.id;
		m_blob.begin_receive(p.id, state_frame, p.total, p.chunks);
	}

	m_blob.add_chunk(p.index, chunk, chunk_len);

	if (m_blob.complete())
		m_resync_ready = true;
}


void manager::apply_resync()
{
	const frame_t target = m_frame;
	const frame_t from = m_blob.frame();

	// A STATE FROM THE HOST'S FUTURE IS NOT A FAILURE. The host leads by
	// up to m_delay frames - it is the faster peer and waits on this one -
	// so by the time its snapshot lands here it can be stamped AHEAD of
	// this peer's own frame. Measured on a real link: from was target + 3
	// every single time, which rejected every request made in the first
	// seconds of a session and none later, because a cold guest cannot
	// hold 60 Hz and the host sits at its maximum lead until it can.
	//
	// Refusing it was wrong. The blob is a complete machine state that
	// already contains this peer's own inputs for the frames in between,
	// because the host ran them, so the correct move is to jump forward
	// onto it. notes/protocol.txt 2.5
	const bool jump_forward = (target < from);

	// Genuinely stale is the other direction: the words needed to replay
	// from `from` up to here have long since left the ring.
	if (!jump_forward && ((target - from) >= (RING - 8)))
	{
		osd_printf_info("netplay: resync state for frame %u is %u frames "
				"behind frame %u, too stale to replay\n",
				unsigned(from), unsigned(target - from), unsigned(target));
		m_machine.popmessage("Resync failed: the state was too old to use");
		m_resync_wanted = false;
		m_blob.clear();
		return;
	}

	if (!m_blob.restore(m_machine))
	{
		m_machine.popmessage("Resync failed: the state did not fit this machine");
		m_resync_wanted = false;
		m_blob.clear();
		return;
	}

	// m_frame = from, where from is the frame the HOST had yet to inject
	// when it snapshotted. The blob carries the machine mid-frame, exactly
	// where the host was, so the next injection here is the same frame the
	// host injected next. Both sides agree on that only because capture
	// and restore both happen at a scheduler-safe point.
	m_resync_done_id = m_blob.id();

	const frame_t was = m_frame;
	m_frame = from;
	m_pending = 0;
	m_echo = 0;
	m_resync_wanted = false;
	m_blob.clear();

	if (jump_forward)
	{
		// Nothing to replay - the frames between are not re-run, they are
		// skipped, and the state already accounts for them.
		m_replaying = false;
		m_catchup = false;
		m_machine.video().set_fastforward(false);

		// TOP UP THE STREAM BEFORE RUNNING. frame_begin will want this
		// peer's own word for m_frame, and only publishing puts it in the
		// ring - so if the jump landed past m_published, the next
		// frame_begin blocks on its own input and never recovers. That is
		// the delay-1 deadlock class, reached a different way.
		//
		// The shortfall is at most one frame and the arithmetic says why:
		// the host has RUN every frame below `from`, which it could only do
		// with this peer's words for them, so m_published is at least
		// from - 1. Filling repeats the last word published.
		if (m_published < m_frame)
		{
			const slot &last = at(m_published);
			const input_word w = (last.stamp == m_published)
					? last.words[m_local - 1] : input_word(0);
			publish_up_to(m_frame, w);
			batch_flush();      // not inside frame_end, so send it now
		}

		osd_printf_info("netplay: resync applied, jumped %u -> %u" 
				" (host was %u ahead)\n",
				unsigned(was), unsigned(from), unsigned(from - was));
		m_machine.popmessage("Resynced - skipped forward %d frames",
				int(from - was));
		return;
	}

	// Behind the state: replay forward through input already in the ring.
	m_replaying = true;
	m_replay_until = was;
	m_catchup = true;

	// Unthrottled, or the gap never closes: running at 60 Hz while the host
	// also runs at 60 Hz leaves the guest permanently behind, and the host
	// then starves on guest input. Cleared when the stream is caught.
	m_machine.video().set_fastforward(true);

	osd_printf_info("netplay: resync applied, replaying %u..%u\n",
			unsigned(from), unsigned(was));
	m_machine.popmessage("Resynced - catching up %d frames",
			int(was - from));
}

//-------------------------------------------------
//  configuration: the host's settings
//-------------------------------------------------

void manager::send_config()
{
	if (!m_link)
		return;

	// THE DIGEST TRAVELS EVEN WHEN THE VALUES DO NOT. A host told not to
	// send configuration still has to say what its configuration IS, or
	// a guest set up differently has no way to find out and plays on
	// desynced - which is the failure this whole message exists to stop.
	config_rec recs[CONFIG_MAX_FIELDS];
	unsigned count = 0;
	u16 flags = 0;

	if (m_send_config)
	{
		count = inputmap::collect_config(m_machine, recs, CONFIG_MAX_FIELDS);
		flags = CONFIG_HAS_VALUES;

		// More configuration than one datagram holds. collect_config caps
		// what it writes, so a full buffer is the only signal available and
		// it is treated as truncated whether it was or not - a driver with
		// exactly the limit loses nothing but the values, and the digest
		// still protects it.
		//
		// Sending PART of the settings is the one thing not to do: the
		// guest would agree about some switches and not others, which is
		// worse than not trying. [open] chunk it like a resync
		if (count >= CONFIG_MAX_FIELDS)
		{
			osd_printf_info("netplay: %s has more than %u config fields, sending the digest only\n",
					m_machine.system().name, CONFIG_MAX_FIELDS);
			count = 0;
			flags = 0;
		}
	}

	u8 buf[MAX_DATAGRAM];
	const size_t len = build_config(buf, sizeof(buf), 0, m_frame, m_local,
			m_map.config_digest(), flags, recs, count);
	if (len == 0)
		return;

	m_link->send(buf, len);
	osd_printf_info("netplay: sent config digest %016llx%s\n",
			static_cast<unsigned long long>(m_map.config_digest()),
			(count != 0) ? " with values" : " only");
}


void manager::take_config(u64 digest, u16 flags, const config_rec *recs,
		unsigned count)
{
	// Once per session. A repeat would re-apply settings a player may
	// have deliberately changed since.
	if (m_config_checked)
		return;
	m_config_checked = true;

	if (digest == m_map.config_digest())
	{
		osd_printf_info("netplay: config matches the host\n");
		return;
	}

	if ((flags & CONFIG_HAS_VALUES) == 0)
	{
		// The host is not sharing settings and ours differ. Nothing can
		// reconcile that here, and playing on would desync every frame
		// with nothing in any log to explain it.
		osd_printf_info("netplay: config differs from the host and the host is not sharing it\n");
		osd_printf_info("netplay:   ours %016llx, host %016llx\n",
				static_cast<unsigned long long>(m_map.config_digest()),
				static_cast<unsigned long long>(digest));
		m_machine.popmessage(
				"DIP or analog settings differ from the host - cannot play");
		m_machine.schedule_exit();
		return;
	}

	// WHAT CHANGED, BY NAME. A digest says the two peers disagree and
	// nothing about what; both run the same driver, so the field names
	// are available locally and worth printing.
	config_rec mine[CONFIG_MAX_FIELDS];
	const unsigned n = inputmap::collect_config(m_machine, mine,
			CONFIG_MAX_FIELDS);
	for (unsigned i = 0; i < count; i++)
	{
		for (unsigned j = 0; j < n; j++)
		{
			if (mine[j].index != recs[i].index)
				continue;
			if ((mine[j].value == recs[i].value)
					&& (mine[j].flags == recs[i].flags)
					&& (mine[j].sensitivity == recs[i].sensitivity)
					&& (mine[j].delta == recs[i].delta)
					&& (mine[j].centerdelta == recs[i].centerdelta))
				break;

			std::string tag;
			const char *const nm = inputmap::config_field_name(m_machine,
					recs[i].index, tag);
			const char *const label = (nm != nullptr) ? nm : "?";

			// An analog field whose VALUE matches but whose sensitivity does
			// not would otherwise print "ours 0 -> host 0", which reads as no
			// difference at all. Say which number moved.
			if ((recs[i].flags & CONFIG_REC_ANALOG) != 0)
			{
				osd_printf_info("netplay:   %s:%s  sensitivity %d -> %d,"
						" keydelta %d -> %d, centerdelta %d -> %d,"
						" reverse %d -> %d\n",
						tag.c_str(), label,
						int(mine[j].sensitivity), int(recs[i].sensitivity),
						int(mine[j].delta), int(recs[i].delta),
						int(mine[j].centerdelta), int(recs[i].centerdelta),
						((mine[j].flags & CONFIG_REC_REVERSE) != 0) ? 1 : 0,
						((recs[i].flags & CONFIG_REC_REVERSE) != 0) ? 1 : 0);
			}
			else
			{
				osd_printf_info("netplay:   %s:%s  ours %u -> host %u\n",
						tag.c_str(), label,
						unsigned(mine[j].value), unsigned(recs[i].value));
			}
			break;
		}
	}

	const unsigned changed =
			inputmap::apply_config(m_machine, recs, count);

	// PROVE IT, DO NOT ASSUME IT. Recomputing and comparing is what makes
	// adoption safe: if anything did not take - a field the host does not
	// have, a setting that refused to move - the two peers would run on
	// quietly diverging, which is the outcome this exists to prevent.
	const u64 now = m_map.recompute_config_digest(m_machine);
	if (now != digest)
	{
		osd_printf_info("netplay: adopted %u setting(s) and still differ: %016llx vs %016llx\n",
				changed, static_cast<unsigned long long>(now),
				static_cast<unsigned long long>(digest));
		m_machine.popmessage(
				"Could not match the host's settings - cannot play");
		m_machine.schedule_exit();
		return;
	}

	osd_printf_info("netplay: adopted %u setting(s) from the host, config now matches\n", changed);
	if (changed != 0)
		m_machine.popmessage("Adopted %d setting(s) from the host",
				int(changed));
}


//-------------------------------------------------
//  refusing a request, and being refused
//-------------------------------------------------

void manager::send_nak(u8 about, u8 reason)
{
	if (!m_link)
		return;

	u8 buf[MAX_DATAGRAM];
	const size_t len = build_nak(buf, sizeof(buf), 0, m_frame, m_local,
			about, reason);
	if (len == 0)
		return;

	m_link->send(buf, len);
	osd_printf_info("netplay: refused a request - %s\n", nak_text(reason));
}


void manager::take_nak(u8 about, u8 reason)
{
	if (about != NAK_ABOUT_RESYNC)
		return;

	// END THE ATTEMPT, REMEMBER NOTHING. Clearing these is not a record
	// of the refusal, it is tearing down the request that was in flight:
	// without it the nag keeps re-asking for ten seconds and then
	// reports a timeout instead of the reason the host just gave.
	//
	// Deliberately no sticky flag. Asking again asks again, and is
	// answered again - a refusal is cheap and a remembered one is state
	// that can go stale the moment the host changes a setting.
	if (!m_resync_wanted)
		return;                 // not ours, or already over

	m_resync_wanted = false;
	m_resync_nag = 0;
	m_resync_id = 0;
	m_blob.clear();

	osd_printf_info("netplay: resync refused - %s\n", nak_text(reason));
	m_machine.popmessage("Resync unavailable: %s", nak_text(reason));
}


//-------------------------------------------------
//  take_snapshot - a rolling savestate the host can
//  resume a session from
//-------------------------------------------------

void manager::take_snapshot()
{
	// WRITTEN AS MAME WRITES ONE, WHERE MAME WRITES ONE. Same format,
	// same path convention, so -state <name> loads it with no new code
	// and the file is useful outside netplay. running_machine::
	// schedule_save would do the same thing, but handle_saveload pops
	// "State successfully saved" every time, which is noise every time
	// the period elapses.
	//
	// A FAILED SNAPSHOT IS NOT A FAILED SESSION. Nothing here touches
	// emulated state, so every error path just says so and returns.
	char name[32];
	std::snprintf(name, sizeof(name), "np%02u", m_snap_next + 1);
	m_snap_next = (m_snap_next + 1) % m_snap_keep;

	const char *searchpath = nullptr;
	const std::string fname =
			m_machine.compose_saveload_filename(std::string(name), &searchpath);

	emu_file file((searchpath != nullptr) ? searchpath : "",
			OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_PATHS);
	if (file.open(fname) != osd_file::error::NONE)
	{
		osd_printf_info("netplay: cannot write snapshot %s\n", fname.c_str());
		return;
	}

	const save_error err = m_machine.save().write_file(file);
	file.close();

	if (err != STATERR_NONE)
	{
		osd_printf_info("netplay: snapshot %s failed, error %d\n",
				name, int(err));
		return;
	}

	// A thumbnail beside it, so a lobby can show what it is rather than a
	// filename. save_snapshot renders through its own render target into
	// its own bitmap - presentation only, nothing in the save set, so it
	// cannot perturb the session. video.cpp create_snapshot_bitmap
	std::string png = fname;
	if (png.size() > 4)
		png.replace(png.size() - 4, 4, ".png");

	emu_file shot((searchpath != nullptr) ? searchpath : "",
			OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_PATHS);
	if (shot.open(png) == osd_file::error::NONE)
	{
		screen_device *const screen =
				screen_device_iterator(m_machine.root_device()).first();
		m_machine.video().save_snapshot(screen, shot);
		shot.close();
	}

	osd_printf_info("netplay: snapshot %s at frame %u\n",
			name, unsigned(m_frame));
}


//-------------------------------------------------
//  chat
//-------------------------------------------------

void manager::chat_push(player_t from, const char *text)
{
	// Oldest falls off the end. m_chat_n only ever counts up, so the
	// newest line is always at (m_chat_n - 1) % CHAT_LINES.
	chat_entry &e = m_chat[m_chat_n % CHAT_LINES];
	m_chat_n++;

	std::snprintf(e.text, sizeof(e.text), "P%d: %s", int(from), text);
	e.shown = osd_ticks();

	// Logged as well as shown. A line fades off the screen in seconds
	// and the log is the only record of what was said.
	osd_printf_info("netplay: chat P%d: %s\n", int(from), text);
}


void manager::chat_send()
{
	if ((m_chat_len == 0) || !m_link)
		return;

	u8 buf[MAX_DATAGRAM];
	const size_t len = build_chat(buf, sizeof(buf), 0, m_frame,
			m_local, m_chat_edit);
	if (len != 0)
	{
		m_link->send(buf, len);

		// Shown locally straight away rather than waiting for the relay to
		// echo it. Chat is not retransmitted, so if the datagram is lost
		// the sender still sees what they said - and nothing desyncs,
		// which is the whole reason this message needs no reliability.
		chat_push(m_local, m_chat_edit);
	}
}


bool manager::chat_composing(running_machine &machine)
{
	manager *const m = active();
	return (m != nullptr) && (s_owner == &machine) && m->m_chat_open;
}


bool manager::chat_begin(running_machine &machine)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine))
		return false;

	// CLEARING THE EDIT BUFFER IS NOT ENOUGH. Nothing consumes IME_CHAR
	// during arcade play - the only other reader is
	// process_natural_keyboard, gated on natkeyboard().in_use(), which no
	// arcade driver sets - so every key pressed while the box was shut is
	// still queued, up to EVENT_QUEUE_SIZE of them. Without this the first
	// frame of composing pops the lot, and the player is greeted by a line
	// of fire-button mashing they then have to delete.
	//
	// reset() rather than a pop loop: it also clears the pending UI key
	// state, which is why menu.cpp does the same on entry. It does not
	// touch input_manager, so the Enter and Escape edges chat_keys watches
	// for are unaffected.
	machine.ui_input().reset();

	m->m_chat_open = true;
	m->m_chat_len = 0;
	m->m_chat_edit[0] = 0;
	return true;
}


//-------------------------------------------------
//  sample_ui_keys / enter_edge
//-------------------------------------------------

// WHY NOT input_manager::code_pressed_once. That function detects the edge
// with a memory it updates only when it is called, and the chat test lives in
// handler_ingame, which does not run while a menu owns the UI. Menus close on
// IPT_UI_SELECT through the ui_input queue, which never touches that memory -
// so the Enter that dismissed a menu still looked like a brand new press to
// the next handler_ingame, and opened the chat box.
//
// Sampled from update_and_render instead, which runs for every rendered frame
// no matter which handler is active and no matter whether the machine is
// paused. The press that closes a menu therefore consumes its own edge on the
// frame it happens.
void manager::sample_ui_keys(running_machine &machine)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine))
		return;

	const bool down = machine.input().code_pressed(KEYCODE_ENTER)
			|| machine.input().code_pressed(KEYCODE_ENTER_PAD);

	m->m_enter_edge = down && !m->m_enter_prev;
	m->m_enter_prev = down;
}


bool manager::enter_edge(running_machine &machine)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine))
		return false;
	return m->m_enter_edge;
}


void manager::chat_keys(running_machine &machine)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine) || !m->m_chat_open)
		return;

	// Enter sends, Escape abandons. Tested before the characters so a
	// Return that also arrives as a character cannot be inserted.
	if (machine.input().code_pressed_once(KEYCODE_ESC))
	{
		m->m_chat_open = false;
		m->m_chat_len = 0;
		m->m_chat_edit[0] = 0;
		return;
	}

	// The same edge the box was opened with, so one held press cannot
	// open the box and immediately send an empty line.
	if (enter_edge(machine))
	{
		m->chat_send();
		m->m_chat_open = false;
		m->m_chat_len = 0;
		m->m_chat_edit[0] = 0;
		return;
	}

	if (machine.input().code_pressed_once(KEYCODE_BACKSPACE)
			&& (m->m_chat_len != 0))
	{
		m->m_chat_len--;
		m->m_chat_edit[m->m_chat_len] = 0;
	}

	// IME_CHAR is how MAME delivers typed text (menu.cpp does the same
	// for incremental search). Nothing else consumes these during arcade
	// play: the only other reader is process_natural_keyboard, gated on
	// natkeyboard().in_use(), which no arcade driver sets.
	ui_event ev;
	while (machine.ui_input().pop_event(&ev))
	{
		if (ev.event_type != ui_event::IME_CHAR)
			continue;

		// ASCII only, which is also all the wire format carries.
		const char32_t ch = ev.ch;
		if ((ch < 0x20) || (ch >= 0x7f))
			continue;
		if (m->m_chat_len >= CHAT_MAX_TEXT)
			continue;

		m->m_chat_edit[m->m_chat_len++] = char(ch);
		m->m_chat_edit[m->m_chat_len] = 0;
	}
}


unsigned manager::chat_view(running_machine &machine, chat_line *out,
		unsigned max)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine) || (out == nullptr))
		return 0;

	const osd_ticks_t now = osd_ticks();
	const osd_ticks_t per = osd_ticks_per_second();
	const osd_ticks_t life = per * CHAT_SECONDS;
	const osd_ticks_t fade = per * CHAT_FADE_SECONDS;

	const unsigned held = (m->m_chat_n < CHAT_LINES)
			? m->m_chat_n : CHAT_LINES;
	unsigned n = 0;

	for (unsigned i = 0; (i < held) && (n < max); i++)
	{
		// Oldest first, so the newest ends up at the bottom.
		const unsigned idx = (m->m_chat_n - held + i) % CHAT_LINES;
		const chat_entry &e = m->m_chat[idx];
		if (e.text[0] == 0)
			continue;

		const osd_ticks_t age = (now > e.shown) ? (now - e.shown) : 0;
		if (age >= life)
			continue;

		u8 alpha = 255;
		if ((fade != 0) && (age > (life - fade)))
			alpha = u8((255 * (life - age)) / fade);

		out[n].text = e.text;
		out[n].alpha = alpha;
		n++;
	}

	return n;
}


const char *manager::chat_prompt(running_machine &machine)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine) || !m->m_chat_open)
		return nullptr;

	std::snprintf(m->m_chat_prompt, sizeof(m->m_chat_prompt), "Say: %s_",
			m->m_chat_edit);
	return m->m_chat_prompt;
}


//-------------------------------------------------
//  retransmission requests
//-------------------------------------------------

void manager::send_nak()
{
	if (!m_link)
		return;

	// Only what is actually missing. at(m_frame).have carries the words
	// in hand; anything in m_need that is not there is what to ask for.
	// A stale slot counts as having nothing.
	const slot &sl = at(m_frame);
	const u8 held = (sl.stamp == m_frame) ? sl.have : 0;
	const u8 want = u8(m_need & ~held);
	if (want == 0)
		return;

	// NEVER ASK FOR OUR OWN WORD. Nothing on the link can supply it -
	// only publishing puts it in the ring - so a request for it would
	// never be answered and would hide the real fault, which is a hole in
	// our own stream. wait_for_input reports that case separately.
	if ((want & ~slot_bit(m_local)) == 0)
		return;

	// Ask for a run rather than one frame: if several are missing, one
	// request and one reply cover them all, and the reply is the same
	// size as an ordinary input packet either way.
	unsigned count = m_depth * m_batch;
	if (count < 1)
		count = 1;
	if (count > 255)
		count = 255;

	u8 buf[MAX_DATAGRAM];
	const size_t len = build_stall(buf, sizeof(buf), 0, m_frame, m_local,
			u8(want & ~slot_bit(m_local)), u8(count));
	if (len == 0)
		return;

	m_link->send(buf, len);
	m_naks_sent++;
}


void manager::answer_nak(frame_t frame, player_t from, u8 want, u8 count)
{
	// Relayed to every peer, so most of these are not ours to answer.
	if ((from == m_local) || ((want & slot_bit(m_local)) == 0))
		return;

	m_naks_answered++;

	// CLAMP TO WHAT HAS BEEN PUBLISHED. The asker names a run that may
	// reach past what this peer has produced - it is behind, which is why
	// it is asking - and send_window walks BACK from its newest frame, so
	// an unpublished newest would break the walk immediately and send
	// nothing at all.
	frame_t newest = frame + frame_t(count) - 1;
	if (newest > m_published)
		newest = m_published;
	if (newest < frame)
		return;

	const unsigned span = unsigned(newest - frame) + 1;
	if (send_window(newest, span, 0) != 0)
		m_naks_served++;
}


//-------------------------------------------------
//  wait_for_input - the lockstep stall
//-------------------------------------------------

void manager::wait_for_input()
{
	poll_link();
	apply_ui_command();

	// Nothing may run before the server releases the session. A word
	// published while a peer has not joined is relayed to nobody and is
	// gone for good, which costs exactly m_delay frames of input and
	// desyncs both peers permanently. notes/protocol.txt 4.1
	// REFUSED MEANS THERE IS NO SESSION TO WAIT FOR. Nothing read this
	// before, so a peer the server turned away sat in the join gate for
	// good - and if the gate ever gave up it would run on alone and
	// desynced, which is worse than stopping.
	if (m_link && m_link->refused() && !m_refused_reported)
	{
		m_refused_reported = true;
		osd_printf_info("netplay: cannot join - %s\n",
				m_link->refuse_reason());
		m_machine.popmessage("Cannot join: %s", m_link->refuse_reason());
		m_machine.schedule_exit();
	}

	const bool gated = (m_link && !m_link->ready());

	if (!gated && !m_net_paused && complete(m_frame))
	{
		m_held_polls = 0;
		m_stall_warned = false;
		return;
	}

	// HAVING TO WAIT IS THE DEFINITION OF CAUGHT UP. While behind, the ring
	// always has the next frame ready and the guest burns through it
	// unthrottled; the first frame it has to wait for means it has reached
	// the live stream and normal pacing should resume.
	if (m_catchup && !m_net_paused && !m_replaying)
	{
		m_catchup = false;
		m_machine.video().set_fastforward(false);
		osd_printf_info("netplay: caught up at frame %u\n", unsigned(m_frame));
		m_machine.popmessage("Back in sync");
	}

	if (!m_net_paused)
		m_stalls++;

	// WAITING ON OUR OWN WORD IS NEVER RECOVERABLE. Nothing on the link
	// can supply it - only publishing puts it in the ring - so this is a
	// hole in our own stream and the session is over. It cost one silent
	// hang already; say so rather than sit there looking like packet loss.
	if (!gated && !m_net_paused && !m_own_hole)
	{
		const slot &s = at(m_frame);
		if ((s.stamp != m_frame) || ((s.have & slot_bit(m_local)) == 0))
		{
			m_own_hole = true;
			osd_printf_info("netplay: BUG - waiting on our own word for frame %u, published through %u\n",
					unsigned(m_frame), unsigned(m_published));
			m_machine.popmessage("Netplay bug: no local input for frame %d - this will not recover", int(m_frame));
		}
	}

	// PAUSE AROUND THE WAIT, and the reason is determinism, not tidiness.
	//
	// A held frame still has to service the window: the window lives on its
	// own thread and reaches the main thread by SendMessage, so a main
	// thread asleep in a loop stops the display responding. video_manager::
	// frame_update is how MAME pumps it, and the debugger uses exactly this
	// to stay alive while the emulation is stopped.
	//
	// LANDMINE: video_manager::frame_update CANNOT be used for that, at
	// any pause state. It perturbs two save-set entries; the pump below
	// calls only the two pieces that draw. See the comment there.
	// PAUSE LAZILY. Pausing dims the screen and puts [paused] in the FPS
	// overlay (video.cpp), so pausing for every few-millisecond jitter
	// stall makes the picture flicker - badly, if the peer is unthrottled
	// and therefore stalls every single frame. Observed on a peer with F10
	// pressed. Below PUMP_AFTER we just sleep and poll: nothing needs the
	// window serviced for 100 ms, and nothing needs dimming either.
	//
	// A deliberate netplay pause is the exception and dims at once, because
	// there the dimming is the feedback that tells the player it worked.
	bool we_paused = false;

	// Whether this hold put a warning on screen, so the clear at the end
	// only fires when there is something to clear. Most stalls are a few
	// milliseconds and say nothing; clearing after those would wipe a
	// message the driver or the UI had just posted.
	bool warned_here = false;

	u64 since_keepalive = 0;

	while (m_net_paused || gated_now() || !complete(m_frame))
	{
		// THE USER MUST ALWAYS BE ABLE TO LEAVE. Escape and the window close
		// button set this flag through the UI, which keeps running because
		// the pump below calls draw_user_interface. Without this check a
		// held session is unkillable except from Task Manager - observed.
		if (m_machine.exit_pending() || m_machine.scheduled_event_pending())
			break;

		if (!m_link || !m_link->wait())
			break;

		const bool want_pause = m_net_paused || (m_held_polls >= PUMP_AFTER);
		if (want_pause && !we_paused && !m_machine.paused())
		{
			m_machine.pause();
			we_paused = true;
		}

		// ~60 Hz, not on every poll: wait() returns about every millisecond,
		// and pumping that often renders a thousand frames a second.
		if (want_pause && ((m_held_polls % 16) == 0))
		{
			// DRAW, BUT DO NOT CALL video_manager::frame_update.
			//
			// It ends with
			//
			//     if (screen && (paused() || from_debugger || ...))
			//         screen->reset_partial_updates();
			//
			// and reset_partial_updates writes m_last_partial_scan and
			// re-adjusts m_scanline0_timer. BOTH ARE IN THE SAVE SET, so a
			// peer that pumps while held ends up holding different machine
			// state from one that did not - measured as exactly two entries
			// of 931 diverging, m_last_partial_scan and
			// timer/:screen/2/0/m_start.attoseconds.
			//
			// PAUSING DOES NOT AVOID IT. Pausing skips
			// finish_screen_updates, but from_debugger=true satisfies that
			// condition on its own, so the reset fires either way. These two
			// calls are the subset that draws - the UI into the container,
			// then the blit - and neither touches emulated state.
			emulator_info::draw_user_interface(m_machine);
			m_machine.osd().update(false);

			// LATCH THE UI KEYS BY HAND. ui_input_manager::frame_update is
			// registered on MACHINE_NOTIFY_FRAME, and
			// video_manager::frame_update only fires those notifiers when
			// from_debugger is false. So the pump above draws the UI but
			// never updates which UI keys are down, and pressed() stays false
			// forever: the player can neither resume nor quit, which wedges
			// the session beyond Task Manager. Observed.
			//
			// Called directly rather than switching to from_debugger=false,
			// because that would also re-enable throttle and frameskip
			// updates, and frameskip perturbs emulated state
			// (notes/determinism.txt 3).
			m_machine.ui_input().frame_update();
		}

		poll_link();
		apply_ui_command();

		// ASK FOR WHAT IS MISSING. Redundancy covers a burst shorter than
		// its depth and nothing longer, so this is the only way a session
		// survives a longer one. A deliberate pause is not a loss, and a
		// session that has not started yet has nothing to ask for.
		if (!m_net_paused && !gated_now()
				&& (m_held_polls >= NAK_FIRST)
				&& (((m_held_polls - NAK_FIRST) % NAK_REPEAT) == 0))
		{
			send_nak();
		}

		// SAY SO ON SCREEN. A wait this long is no longer jitter or a lost
		// packet being re-asked for - the link is down or the peer is gone.
		// The session still waits rather than inventing input, so without
		// this the player sees a frozen picture and no reason for it.
		if (!m_net_paused && !gated_now() && !m_stall_warned
				&& (m_held_polls >= m_stall_limit))
		{
			m_stall_warned = true;
			player_t who = 0;
			const slot &sl = at(m_frame);
			const u8 held = (sl.stamp == m_frame) ? sl.have : 0;
			for (player_t pl = 1; pl <= MAX_PLAYERS; pl++)
			{
				if (((m_need & slot_bit(pl)) != 0)
						&& ((held & slot_bit(pl)) == 0))
				{
					who = pl;
					break;
				}
			}
			m_machine.popmessage(
					"Waiting for player %d - the connection may be down",
					int(who));
			warned_here = true;
		}

		m_held_polls++;
		since_keepalive++;

		// wait() is about a millisecond, so this is roughly twice a second.
		if (since_keepalive >= 500)
		{
			since_keepalive = 0;
			resend_last();
			write_status();        // a held session must keep reporting
		}

		// NEVER SYNTHESIZE INPUT. Substituting a neutral word for a player
		// whose packet has not arrived desyncs the session silently and
		// permanently: this peer runs the frame with nothing pressed while
		// the owner runs it with whatever they actually pressed. Waiting is
		// the only honest option, so say what is happening and keep waiting.
		if (!m_net_paused && ((m_held_polls % 3000) == 0))
		{
			m_waits++;
			m_machine.popmessage("Waiting for %s (frame %u, %llu s)\n\nEscape to quit.",
					missing_text(), unsigned(m_frame),
					static_cast<unsigned long long>(m_held_polls / 1000));
			warned_here = true;
		}

		if (m_test_pause_hold != 0)
			test_pause_tick();
	}

	if (we_paused)
		m_machine.resume();

	if (warned_here && !m_net_paused && complete(m_frame))
		m_machine.popmessage();  // clear the warning

	m_held_polls = 0;
}


//-------------------------------------------------
//  gated_now / missing_text / test_pause_tick
//-------------------------------------------------

bool manager::gated_now() const
{
	return (m_link != nullptr) && !m_link->ready();
}


const char *manager::missing_text()
{
	// Names the slots whose word has not arrived, so the message says who
	// to go and look at rather than just that something is wrong.
	const slot &sl = at(m_frame);
	const bool stamped = (sl.stamp == m_frame);
	size_t used = 0;

	m_missing[0] = 0;
	for (player_t p = 1; p <= m_players; p++)
	{
		if (stamped && ((sl.have & slot_bit(p)) != 0))
			continue;
		const int n = snprintf(m_missing + used, sizeof(m_missing) - used,
				"%splayer %d", (used != 0) ? ", " : "", int(p));
		if (n <= 0)
			break;
		used += size_t(n);
		if (used >= (sizeof(m_missing) - 1))
			break;
	}

	if (m_missing[0] == 0)
		snprintf(m_missing, sizeof(m_missing), "the session to start");
	return m_missing;
}


void manager::test_corrupt()
{
	// Scribble on the largest registered entry, which on any of these
	// drivers is work RAM. Enough bytes that the state hash cannot miss it
	// and the game visibly misbehaves.
	save_manager &save = m_machine.save();
	const int count = save.registration_count();

	void *biggest = nullptr;
	size_t biggest_bytes = 0;
	for (int i = 0; i < count; i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0;
		if (save.indexed_item(i, base, valsize, valcount) == nullptr)
			continue;
		if (base == nullptr)
			continue;
		const size_t bytes = size_t(valsize) * size_t(valcount);
		if (bytes > biggest_bytes)
		{
			biggest = base;
			biggest_bytes = bytes;
		}
	}

	if ((biggest == nullptr) || (biggest_bytes < 256))
		return;

	std::memset(biggest, 0x5a, 256);
	osd_printf_info("netplay: TEST corrupted 256 bytes at frame %u\n",
			unsigned(m_frame));
}


void manager::test_pause_tick()
{
	// DIAGNOSTIC: a real session is resumed by a keypress, which the UI can
	// still deliver because the pump runs draw_user_interface. A headless
	// test has no keypress and cannot count frames either - they do not
	// advance while held - so it counts poll iterations.
	if (!m_net_paused)
		return;
	m_test_held++;
	if (m_test_held <= (m_test_pause_hold * 17))
		return;
	m_test_held = 0;
	m_test_pause_hold = 0;              // once only
	ui_request(m_machine, UICMD_PAUSE_TOGGLE);
}

//-------------------------------------------------
//  check_selftest - is injection the inverse of
//  capture?
//-------------------------------------------------

void manager::check_selftest()
{
	// Only the bits LOCAL_SLOT owns: capture_port reads that slot's fields
	// and no other, so nothing else is expected to echo back.
	const u32 want = word_digital(at(m_frame).words[inputmap::LOCAL_SLOT - 1]) & m_local_mask;
	const u32 got = word_digital(m_echo) & m_local_mask;

	if (want != got)
	{
		if (m_selftest_fail == 0)
			osd_printf_info("netplay: SELFTEST frame %u want %08x got %08x\n",
					unsigned(m_frame), want, got);
		m_selftest_fail++;
	}
}


//-------------------------------------------------
//  hooks
//-------------------------------------------------

void manager::frame_begin(running_machine &machine)
{
	if (s_owner != &machine)
		open_session(machine);

	manager *const m = active();
	if (m == nullptr)
		return;

	// DIAGNOSTIC: stand in for a keypress. See the header.
	if ((m->m_test_pause_at != 0) && (m->m_frame == m->m_test_pause_at))
		ui_request(machine, UICMD_PAUSE_TOGGLE);
	if ((m->m_test_corrupt_at != 0) && (m->m_frame == m->m_test_corrupt_at))
		m->test_corrupt();
	if ((m->m_test_resync_at != 0) && (m->m_frame == m->m_test_resync_at))
		request_resync(machine);

	// DIAGNOSTIC: stand in for a player leaning on the key. A refusal
	// must be answered every single time, so this asks repeatedly.
	if ((m->m_test_resync_every != 0)
			&& ((m->m_frame % m->m_test_resync_every) == 0)
			&& (m->m_frame != 0))
		request_resync(machine);
	if ((m->m_test_chat_at != 0) && (m->m_frame == m->m_test_chat_at))
	{
		// DIAGNOSTIC: the backlog a player leaves behind by mashing fire
		// buttons before opening the box. Nothing consumes IME_CHAR during
		// arcade play, so these sit in the queue until chat pops them.
		for (int j = 0; j < 5; j++)
		{
			ui_event junk;
			std::memset(&junk, 0, sizeof(junk));
			junk.event_type = ui_event::IME_CHAR;
			junk.ch = char32_t('Z');
			machine.ui_input().push_event(junk);
		}

		chat_begin(machine);
	}

	// DIAGNOSTIC: type the message the frame AFTER opening, through the
	// same IME_CHAR path a keyboard uses. Typing it rather than assigning
	// m_chat_edit is the point: it proves characters still arrive after
	// chat_begin resets the queue.
	if ((m->m_test_chat_at != 0) && (m->m_frame == (m->m_test_chat_at + 1))
			&& m->m_chat_open)
	{
		char line[CHAT_MAX_TEXT];
		std::snprintf(line, sizeof(line), "hello from player %d",
				int(m->m_local));
		for (const char *q = line; *q != 0; q++)
		{
			ui_event key;
			std::memset(&key, 0, sizeof(key));
			key.event_type = ui_event::IME_CHAR;
			key.ch = char32_t((unsigned char)(*q));
			machine.ui_input().push_event(key);
		}
	}
	if (m->m_chat_open && (m->m_test_chat_at != 0)
			&& (m->m_frame == (m->m_test_chat_at + m->m_test_chat_hold))
			&& (m->m_test_chat_hold > 2))
	{
		m->chat_send();
		m->m_chat_open = false;
		m->m_chat_len = 0;
		m->m_chat_edit[0] = 0;
	}

	// Before wait_for_input, because applying a resync moves m_frame and
	// the wait has to be for the frame we end up on.
	m->poll_link();
	m->service_resync();

	m->wait_for_input();
}


void manager::port_capture(ioport_port &port)
{
	manager *const m = active();
	if (m == nullptr)
		return;

	// Not while replaying. Those frames were captured and published once
	// already; reading the live controls now would invent input for a frame
	// the other peer has long since run with something else.
	if (m->m_replaying)
		return;

	// NOT WHILE TYPING. The keyboard belongs to the message, so nothing
	// is captured and a neutral word goes out instead - no firing, no
	// walking, no coin inserted by the letter C.
	//
	// This is not instant and cannot be: the words for the next m_delay
	// frames are already published and on their way. At delay 1 that is
	// one frame. Suppressing what has already been sent would mean
	// rewriting a frame the other peer may have run - a desync to avoid
	// one stray shot.
	if (m->m_chat_open)
		return;

	m->m_map.capture_port(port, m->m_pending);
}


void manager::port_inject(ioport_port &port)
{
	manager *const m = active();
	if (m == nullptr)
		return;

	// frame_begin guarantees the slot is marked complete by here, either
	// because the input arrived or because the stall gave up.
	m->m_map.inject_port(port, m->at(m->m_frame).words, m->m_players);

	if (m->m_selftest)
		m->m_map.capture_port(port, m->m_echo);
}


void manager::frame_end(running_machine &machine)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine))
		return;

	if (m->m_selftest)
		m->check_selftest();

	if (m->m_log != nullptr)
		std::fprintf(m->m_log, "%u,%016llx,%016llx,%02x\n",
				unsigned(m->m_frame),
				static_cast<unsigned long long>(m->m_pending),
				static_cast<unsigned long long>(
						m->at(m->m_frame).words[m->m_local - 1]),
				m->at(m->m_frame).have);

	if (m->m_replaying && (m->m_frame >= m->m_replay_until))
	{
		m->m_replaying = false;
		osd_printf_info("netplay: replay complete at frame %u\n",
				unsigned(m->m_frame));
	}

	// ONE RULE DECIDES WHAT GOES ON THE WIRE: every frame after
	// m_published, up to m_frame + delay, and nothing else. It has to hold
	// both ways at once, and the two failures are not symmetric.
	//
	// Re-publishing a frame overwrites a word the host has already
	// consumed, so a replay must stay silent - and it does, with no test
	// for it: while replaying, target is still behind m_published and the
	// loop does not run.
	//
	// A HOLE IN THE STREAM STALLS EVERY PEER FOREVER. One frame with no
	// word from one peer cannot be skipped and nothing will ever produce
	// it. Worse, a peer can end up waiting on its OWN word, which only
	// publishing puts in the ring - so the wait can never end. The frame
	// a replay finishes on is exactly where that hole opens: the counter
	// has moved while the stream has not.
	//
	// AT DELAY 1 THERE IS NO SLACK TO ABSORB IT. With a larger delay the
	// frames already published cover the gap and it closes a frame later;
	// at delay 1 the next frame_begin blocks before any later frame_end
	// can fill anything. So this cannot be a separate repair step that
	// runs afterwards - it has to be the same rule that does the normal
	// publish. Filling repeats an input, a visible artifact and nothing
	// worse.
	m->publish_up_to(m->m_frame + frame_t(m->m_delay), m->m_pending);

	if (m->m_link)
		m->m_link->tick();
	m->poll_link();

	m->m_pending = 0;
	m->m_echo = 0;
	m->m_frame++;

	// ~2 Hz while running. Often enough to watch, cheap enough to ignore.
	// ARMED HERE, TAKEN AT THE SAFE POINT. A savestate may only be
	// written where MAME writes one - determinism.txt 6.1 - and this is
	// inside the input hook, which is not that place.
	if ((m->m_snap_period != 0) && (m->m_local == 1)
			&& (m->m_frame >= m->m_snap_due))
	{
		m->m_snap_due = m->m_frame + m->m_snap_period;
		m->m_snap_pending = true;
	}

	// ANNOUNCED ONCE THE SESSION IS LIVE, by the host only. Before that
	// there is nobody to tell; the server relays it like anything else.
	if (!m->m_config_done && (m->m_local == 1)
			&& m->m_link && m->m_link->ready())
	{
		m->m_config_done = true;
		m->send_config();
	}

	// PUSH ONCE, AND ONLY AFTER THE SESSION IS LIVE. While gated the
	// host is blocked inside this hook and never reaches a safe point, so
	// a capture armed then would sit pending forever. do_capture already
	// queues every chunk for sending, so arming it IS the push.
	//
	// AND NOT UNTIL A PENDING LOAD HAS LANDED. -state does not load
	// before the first frame: running_machine::run calls handle_saveload
	// once before its loop, but that returns without loading while
	// m_scheduler.can_save() is false, and retries after each timeslice -
	// so the state arrives a frame or two INTO the session. Measured on
	// dino: frame 0 was still a cold boot and frame 1 was the snapshot.
	// Pushing first would hand the guest a cold-booted machine and then
	// leave the host jumping away from it, desynced for good.
	if (m->m_push_wanted && !m->m_push_done && (m->m_local == 1)
			&& m->m_link && m->m_link->ready()
			&& !m->m_machine.save_or_load_pending())
	{
		m->m_push_done = true;
		m->m_capture_pending = true;
		osd_printf_info("netplay: pushing state to guests\n");
	}

	m->pump_resync_send();

	if ((m->m_frame % 30) == 0)
		m->write_status();

	// Chase missing chunks about twice a second. A datagram link drops
	// things, and without this a single lost chunk means a resync that never
	// completes and never says why.
	if (m->m_resync_wanted && ((m->m_frame % 30) == 15))
	{
		m->m_resync_nag++;
		if (m->m_resync_nag > 20)
		{
			m->m_resync_wanted = false;
			m->m_blob.clear();
			m->m_machine.popmessage("Resync gave up - no answer from the host");
		}
		else if (m->m_resync_id != 0)
		{
			// A transfer is under way: ask only for the chunks still missing.
			m->send_resync_req();
		}
		else if ((m->m_resync_nag % 4) == 0)
		{
			// No chunk has arrived yet, so m_resync_id is still 0 and this
			// would be another FRESH ask - which makes the host snapshot
			// again, at a later frame, under a new id. Two blobs in flight
			// then reset each other in take_resync_chunk every time the id
			// flips and neither completes. Rate-limited to once every two
			// seconds so a slow round trip cannot start a pile-up.
			m->send_resync_req();
		}
	}
}


//-------------------------------------------------
//  write_status - a readout for a session nobody
//  can interrogate any other way
//-------------------------------------------------

void manager::write_status()
{
	if (m_status_path.empty())
		return;

	// Rewritten whole each time rather than appended, so the file is a
	// snapshot and not a log to seek through. A reader can catch a partial
	// write, which is why the last line is a marker: no "# end", no trust.
	FILE *const f = std::fopen(m_status_path.c_str(), "w");
	if (f == nullptr)
		return;

	const slot &sl = at(m_frame);
	const bool stamped = (sl.stamp == m_frame);

	std::fprintf(f, "# netplay status\n");
	std::fprintf(f, "time=%lld\n", static_cast<long long>(time(nullptr)));
	std::fprintf(f, "driver=%s\n", m_machine.system().name);
	std::fprintf(f, "inputmap=%016llx\n",
			static_cast<unsigned long long>(m_map.digest()));
	std::fprintf(f, "config=%016llx\n",
			static_cast<unsigned long long>(m_map.config_digest()));
	std::fprintf(f, "transport=%s\n", m_link ? m_link->name() : "none");
	std::fprintf(f, "slot=%d\n", int(m_local));
	std::fprintf(f, "players=%d\n", int(m_players));
	std::fprintf(f, "delay=%d\n", int(m_delay));
	std::fprintf(f, "batch=%d\n", int(m_batch));
	std::fprintf(f, "depth=%d\n", int(m_depth));
	std::fprintf(f, "savestates=%d\n", m_savestates ? 1 : 0);
	std::fprintf(f, "send_config=%d\n", m_send_config ? 1 : 0);
	std::fprintf(f, "naks_sent=%llu\n",
			static_cast<unsigned long long>(m_naks_sent));
	std::fprintf(f, "naks_answered=%llu\n",
			static_cast<unsigned long long>(m_naks_answered));
	std::fprintf(f, "naks_served=%llu\n",
			static_cast<unsigned long long>(m_naks_served));
	std::fprintf(f, "budget=%d\n", int(m_delay) - int(m_batch) + 1);

	std::fprintf(f, "frame=%u\n", unsigned(m_frame));
	std::fprintf(f, "net_paused=%d\n", m_net_paused ? 1 : 0);
	std::fprintf(f, "gated=%d\n", gated_now() ? 1 : 0);
	std::fprintf(f, "frame_complete=%d\n", complete(m_frame) ? 1 : 0);
	std::fprintf(f, "have=%02x\n", stamped ? sl.have : 0);
	std::fprintf(f, "need=%02x\n", m_need);
	std::fprintf(f, "missing=%s\n", complete(m_frame) ? "" : missing_text());

	std::fprintf(f, "held_polls=%llu\n",
			static_cast<unsigned long long>(m_held_polls));
	std::fprintf(f, "stalls=%llu\n", static_cast<unsigned long long>(m_stalls));
	std::fprintf(f, "stall_frames=%llu\n",
			static_cast<unsigned long long>(m_stall_frames));
	std::fprintf(f, "waits=%llu\n", static_cast<unsigned long long>(m_waits));
	std::fprintf(f, "malformed=%llu\n",
			static_cast<unsigned long long>(m_malformed));

	std::fprintf(f, "link_sent=%llu\n",
			static_cast<unsigned long long>(m_link ? m_link->sent_count() : 0));
	std::fprintf(f, "link_recv=%llu\n",
			static_cast<unsigned long long>(m_link ? m_link->recv_count() : 0));
	std::fprintf(f, "link_dropped=%llu\n",
			static_cast<unsigned long long>(m_link ? m_link->drop_count() : 0));

	std::fprintf(f, "cmd_pending=%d\n", m_cmd_pending ? 1 : 0);
	std::fprintf(f, "cmd=%s\n", m_cmd_pending ? uicmd_text(m_cmd) : "");
	std::fprintf(f, "cmd_frame=%u\n", unsigned(m_cmd_frame));

	// The ring from here forward: C where every player's word has arrived,
	// dot where it has not. Where the Cs stop is where the session is stuck,
	// and that is the one thing a wedged peer cannot otherwise tell you.
	std::fprintf(f, "ring=");
	for (unsigned i = 0; i < 48; i++)
	{
		const frame_t fr = m_frame + frame_t(i);
		std::fputc(complete(fr) ? 0x43 : 0x2e, f);
	}
	std::fprintf(f, "\n");

	std::fprintf(f, "# end\n");
	std::fclose(f);
}

//-------------------------------------------------
//  on_exit
//-------------------------------------------------

void manager::on_exit()
{
	batch_flush();

	if (m_log != nullptr)
	{
		std::fflush(m_log);
		std::fclose(m_log);
		m_log = nullptr;
	}

	if (m_selftest)
	{
		if (m_selftest_fail == 0)
			osd_printf_info("netplay: selftest PASS over %u frame(s)\n",
					unsigned(m_frame));
		else
			osd_printf_info("netplay: selftest FAIL on %llu of %u frame(s)\n",
					static_cast<unsigned long long>(m_selftest_fail),
					unsigned(m_frame));
	}

	osd_printf_info("netplay: %u frames, stalls=%llu stall_frames=%llu "
			"waits=%llu naks=%llu/%llu served=%llu "
			"malformed=%llu sent=%llu recv=%llu dropped=%llu\n",
			unsigned(m_frame),
			static_cast<unsigned long long>(m_stalls),
			static_cast<unsigned long long>(m_stall_frames),
			static_cast<unsigned long long>(m_waits),
			static_cast<unsigned long long>(m_naks_sent),
			static_cast<unsigned long long>(m_naks_answered),
			static_cast<unsigned long long>(m_naks_served),
			static_cast<unsigned long long>(m_malformed),
			static_cast<unsigned long long>(m_link ? m_link->sent_count() : 0),
			static_cast<unsigned long long>(m_link ? m_link->recv_count() : 0),
			static_cast<unsigned long long>(m_link ? m_link->drop_count() : 0));

	// Release ownership but DO NOT reset s_instance: this runs as a member of
	// the object s_instance owns, so resetting would destroy `this` and then
	// return into freed memory. open_session() replaces the instance when the
	// next machine starts, which destroys this one at a safe point.
	s_owner = nullptr;
}

} // namespace netplay
