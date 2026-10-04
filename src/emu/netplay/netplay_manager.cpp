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
	, m_capture_pending(false)
	, m_unsafe(0)
	, m_published(0)
	, m_own_hole(false)
	, m_chat_n(0)
	, m_chat_open(false)
	, m_chat_len(0)
	, m_test_chat_at(0)
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

	// Those primed frames are published, as far as the stream is
	// concerned: nothing may ever write them again. Starting the
	// high-water mark here is what lets one rule cover both the first
	// frame and every frame after a resync. m_delay is clamped to 1 or
	// more before we are constructed.
	m_published = frame_t(m_delay) - 1;

	for (unsigned i = 0; i < CHAT_LINES; i++)
	{
		m_chat[i].text[0] = 0;
		m_chat[i].shown = 0;
	}
	m_chat_edit[0] = 0;
	m_chat_prompt[0] = 0;
	m_test_chat_at = frame_t(env_uint("NETPLAY_TEST_CHAT_AT", 0));
	m_test_chat_hold = frame_t(env_uint("NETPLAY_TEST_CHAT_HOLD", 0));

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
			osd_printf_info("netplay: unable to open log '%s'\n", logpath);
		}
	}

	osd_printf_info("netplay: state is %d KB, resync limit %d KB, %s\n",
			int(state_blob::machine_state_size(m_machine) / 1024),
			int(RESYNC_MAX_BYTES / 1024),
			(state_blob::machine_state_size(m_machine) <= RESYNC_MAX_BYTES)
					? "resync possible" : "RESYNC WILL BE REFUSED");

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
			osd_printf_info("netplay: %s cannot be represented; not joining\n",
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
	if (!m->m_capture_pending && !m->m_resync_ready)
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
		osd_printf_info("netplay: resync refused, state too large\n");
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
	if (!m_resync_wanted)
		return;

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

	// The state is older than the ring can still account for, so the words
	// needed to replay forward are gone. Asking again is the only option.
	if ((target < from) || ((target - from) >= (RING - 8)))
	{
		osd_printf_info("netplay: resync state for frame %u is unusable at frame %u\n",
				unsigned(from), unsigned(target));
		m_machine.popmessage("Resync failed: state arrived too late");
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

	// Rewind the frame counter to the state's frame and replay forward
	// through input that is already in the ring. The pending local word is
	// dropped: it belongs to a frame that is about to be re-run from
	// already-published input.
	//
	// m_frame = from, where from is the frame the HOST had yet to inject
	// when it snapshotted. The blob carries the machine mid-frame, exactly
	// where the host was, so the next injection here is the same frame the
	// host injected next. Both sides agree on that only because capture
	// and restore both happen at a scheduler-safe point.
	m_frame = from;
	m_pending = 0;
	m_echo = 0;
	m_replaying = true;
	m_replay_until = target;
	m_catchup = true;
	m_resync_wanted = false;
	m_blob.clear();

	// Unthrottled, or the gap never closes: running at 60 Hz while the host
	// also runs at 60 Hz leaves the guest permanently behind, and the host
	// then starves on guest input. Cleared when the stream is caught.
	m_machine.video().set_fastforward(true);

	osd_printf_info("netplay: resync applied, replaying %u..%u\n",
			unsigned(from), unsigned(target));
	m_machine.popmessage("Resynced - catching up %d frames",
			int(target - from));
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

	m->m_chat_open = true;
	m->m_chat_len = 0;
	m->m_chat_edit[0] = 0;
	return true;
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

	if (machine.input().code_pressed_once(KEYCODE_ENTER)
			|| machine.input().code_pressed_once(KEYCODE_ENTER_PAD))
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
	// LANDMINE: from_debugger=true does NOT make that call side-effect
	// free. finish_screen_updates() runs whenever the machine is RUNNING
	// and not paused, and it writes screen partial-update state that IS
	// part of the save set. Measured on two peers: pumping unpaused left
	// Video Screen/:screen/0/m_last_partial_scan and
	// timer/:screen/2/0/m_start.attoseconds different between them - 2 of
	// 931 entries - purely because one peer stalled more often than the
	// other. Pausing first is what makes the pump safe.
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
			m_machine.video().frame_update(true);

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
		}

		if (m_test_pause_hold != 0)
			test_pause_tick();
	}

	if (we_paused)
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
	if ((m->m_test_chat_at != 0) && (m->m_frame == m->m_test_chat_at))
	{
		chat_begin(machine);
		std::snprintf(m->m_chat_edit, sizeof(m->m_chat_edit),
				"hello from player %d", int(m->m_local));
		m->m_chat_len = unsigned(std::strlen(m->m_chat_edit));
	}
	if (m->m_chat_open && (m->m_test_chat_at != 0)
			&& (m->m_frame == (m->m_test_chat_at + m->m_test_chat_hold)))
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
	const frame_t target = m->m_frame + frame_t(m->m_delay);
	if (m->m_published < target)
	{
		const frame_t first = m->m_published + 1;

		// The local word is known the moment it is captured and never
		// travels to reach us, so it goes straight into the ring. Only
		// other peers' words come off the link.
		for (frame_t f = first; f <= target; f++)
		{
			m->publish(f, m->m_local, m->m_pending);
			m->batch_add(m->m_pending, f);
		}

		if (first != target)
			osd_printf_info("netplay: filled stream gap %u..%u at frame %u\n",
					unsigned(first), unsigned(target), unsigned(m->m_frame));

		m->m_published = target;
	}

	if (m->m_link)
		m->m_link->tick();
	m->poll_link();

	m->m_pending = 0;
	m->m_echo = 0;
	m->m_frame++;

	// ~2 Hz while running. Often enough to watch, cheap enough to ignore.
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
		else
		{
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
	std::fprintf(f, "transport=%s\n", m_link ? m_link->name() : "none");
	std::fprintf(f, "slot=%d\n", int(m_local));
	std::fprintf(f, "players=%d\n", int(m_players));
	std::fprintf(f, "delay=%d\n", int(m_delay));
	std::fprintf(f, "batch=%d\n", int(m_batch));
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
