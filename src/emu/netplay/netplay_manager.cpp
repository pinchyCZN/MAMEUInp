// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_manager.cpp

    See netplay_manager.h.

***************************************************************************/

#include "emu.h"
#include "netplay/netplay_manager.h"
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

u8 slot_bit(player_t p)
{
	return u8(1u << (p - 1));
}

} // anonymous namespace


//-------------------------------------------------
//  construction
//-------------------------------------------------

manager::manager(running_machine &machine, player_t local, player_t players,
		unsigned delay, unsigned batch, bool redundant, bool selftest)
	: m_machine(machine)
	, m_map()
	, m_link()
	, m_frame(0)
	, m_local(local)
	, m_players(players)
	, m_delay(delay)
	, m_batch(batch)
	, m_redundant(redundant)
	, m_need(0)
	, m_pending(0)
	, m_echo(0)
	, m_local_mask(0)
	, m_out_base(0)
	, m_out_n(0)
	, m_prev_base(0)
	, m_prev_n(0)
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
	, m_selftest(selftest)
	, m_selftest_fail(0)
	, m_log(nullptr)
{
	std::memset(m_ring, 0, sizeof(m_ring));
	std::memset(m_out, 0, sizeof(m_out));
	std::memset(m_prev, 0, sizeof(m_prev));
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
		osd_printf_error("netplay: %s refused - %s\n",
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
			std::fprintf(m_log, "# inputmap_digest=%016llx\n",
					static_cast<unsigned long long>(m_map.digest()));
			std::fprintf(m_log, "# players=%d local=%d delay=%d batch=%d redundant=%d\n",
					int(m_players), int(m_local), int(m_delay), int(m_batch),
					m_redundant ? 1 : 0);
			std::fprintf(m_log, "# transport=%s local_bits=%08x\n",
					m_link->name(), m_local_mask);
			std::fprintf(m_log, "# budget_frames=%d\n",
					int(m_delay) - int(m_batch) + 1);
			std::fprintf(m_log, "frame,captured,injected,have\n");
			std::fflush(m_log);
		}
		else
		{
			osd_printf_error("netplay: unable to open log '%s'\n", logpath);
		}
	}

	osd_printf_info("netplay: %s players=%d local=%d delay=%d batch=%d budget=%d bits=%d digest=%016llx\n",
			m_link->name(), int(m_players), int(m_local), int(m_delay),
			int(m_batch), int(m_delay) - int(m_batch) + 1,
			int(m_map.digital_used()),
			static_cast<unsigned long long>(m_map.digest()));
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
			osd_printf_error("netplay: %s cannot be represented; not joining\n",
					machine.system().name);
			return;
		}

		const char *host = std::getenv("NETPLAY_HOST");
		if (unset(host))
			host = "127.0.0.1";

		std::unique_ptr<udp_link> net(new udp_link);
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
			delay, batch, env_flag("NETPLAY_REDUNDANCY"),
			env_flag("NETPLAY_SELFTEST")));

	candidate->m_stall_limit = (sl > 0) ? sl : STALL_LIMIT;
	candidate->m_test_pause_at = frame_t(env_uint("NETPLAY_TEST_PAUSE_AT", 0));
	candidate->m_test_pause_hold = env_uint("NETPLAY_TEST_PAUSE_HOLD", 120);
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


void manager::batch_flush()
{
	if ((m_out_n == 0) || !m_link)
		return;

	u8 buf[MAX_DATAGRAM];
	input_word words[MAX_BATCH * 2];

	packet_header head;
	head.type = MSG_INPUT;
	head.version = PROTOCOL_VERSION;
	head.session = 0;               // assigned by the server at join [open]
	head.player_mask = slot_bit(m_local);
	head.flags = 0;
	head.reserved = 0;

	unsigned n = 0;

	// Redundancy: repeat the previous batch ahead of this one, but only when
	// the two are contiguous. Non-contiguous would need two frame ranges in
	// one packet, which the header cannot express (protocol.txt 3.2 carries
	// one base_frame and a count).
	const bool repeat = m_redundant && (m_prev_n != 0)
			&& ((m_prev_base + frame_t(m_prev_n)) == m_out_base);

	if (repeat)
	{
		head.base_frame = m_prev_base;
		head.flags = u8(head.flags | FLAG_REDUNDANT);
		for (unsigned i = 0; i < m_prev_n; i++)
			words[n++] = m_prev[i];
	}
	else
	{
		head.base_frame = m_out_base;
	}

	for (unsigned i = 0; i < m_out_n; i++)
		words[n++] = m_out[i];

	head.nframes = u8(n);

	const size_t len = build_input(buf, sizeof(buf), head, words, nullptr);
	if (len != 0)
		m_link->send(buf, len);

	std::memcpy(m_prev, m_out, sizeof(m_prev));
	m_prev_base = m_out_base;
	m_prev_n = m_out_n;
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
	if (!m_link || (m_prev_n == 0))
		return;

	u8 buf[MAX_DATAGRAM];
	packet_header head;
	head.type = MSG_INPUT;
	head.version = PROTOCOL_VERSION;
	head.session = 0;
	head.base_frame = m_prev_base;
	head.nframes = u8(m_prev_n);
	head.player_mask = slot_bit(m_local);
	head.flags = FLAG_REDUNDANT;
	head.reserved = 0;

	const size_t len = build_input(buf, sizeof(buf), head, m_prev, nullptr);
	if (len != 0)
		m_link->send(buf, len);
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
		input_word words[MAX_BATCH * 2 * MAX_PLAYERS];
		u64 hash = 0;
		bool has_hash = false;

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
	const bool gated = (m_link && !m_link->ready());

	if (!gated && !m_net_paused && complete(m_frame))
	{
		m_held_polls = 0;
		return;
	}

	if (!m_net_paused)
		m_stalls++;

	// PAUSE AROUND THE WAIT, and the reason is determinism, not tidiness.
	//
	// A held frame still has to service the window: the window lives on its
	// own thread and reaches the main thread by SendMessage, so a main
	// thread asleep in a loop stops the display responding. video_manager::
	// frame_update is how MAME pumps it, and the debugger uses exactly this
	// to stay alive while the emulation is stopped.
	//
	// LANDMINE: from_debugger=true does NOT make that call side-effect
	// free. finish_screen_updates() runs whenever the machine is RUNNING
	// and not paused, and it writes screen partial-update state that IS
	// part of the save set. Measured on two peers: pumping unpaused left
	// Video Screen/:screen/0/m_last_partial_scan and
	// timer/:screen/2/0/m_start.attoseconds different between them - 2 of
	// 931 entries - purely because one peer stalled more often than the
	// other. Pausing first is what makes the pump safe.
	const bool was_paused = m_machine.paused();
	if (!was_paused)
		m_machine.pause();

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

		// ~60 Hz, not on every poll: wait() returns about every millisecond,
		// and pumping that often renders a thousand frames a second.
		if ((m_held_polls % 16) == 0)
			m_machine.video().frame_update(true);

		poll_link();
		apply_ui_command();

		m_held_polls++;
		since_keepalive++;

		// wait() is about a millisecond, so this is roughly twice a second.
		if (since_keepalive >= 500)
		{
			since_keepalive = 0;
			resend_last();
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
		}

		if (m_test_pause_hold != 0)
			test_pause_tick();
	}

	if (!was_paused)
		m_machine.resume();

	if (!m_net_paused && complete(m_frame))
		m_machine.popmessage("");      // clear the warning

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
			osd_printf_error("netplay: SELFTEST frame %u want %08x got %08x\n",
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

	m->wait_for_input();
}


void manager::port_capture(ioport_port &port)
{
	manager *const m = active();
	if (m == nullptr)
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

	const frame_t target = m->m_frame + frame_t(m->m_delay);

	// The local word is known the moment it is captured and never travels to
	// reach us, so it goes straight into the ring. Only other peers' words
	// come off the link.
	m->publish(target, m->m_local, m->m_pending);
	m->batch_add(m->m_pending, target);

	if (m->m_link)
		m->m_link->tick();
	m->poll_link();

	m->m_pending = 0;
	m->m_echo = 0;
	m->m_frame++;
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
			osd_printf_error("netplay: selftest FAIL on %llu of %u frame(s)\n",
					static_cast<unsigned long long>(m_selftest_fail),
					unsigned(m_frame));
	}

	osd_printf_info("netplay: %u frames, stalls=%llu stall_frames=%llu "
			"waits=%llu malformed=%llu sent=%llu recv=%llu dropped=%llu\n",
			unsigned(m_frame),
			static_cast<unsigned long long>(m_stalls),
			static_cast<unsigned long long>(m_stall_frames),
			static_cast<unsigned long long>(m_waits),
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
