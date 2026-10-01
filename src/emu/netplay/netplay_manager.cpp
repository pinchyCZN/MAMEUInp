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
	, m_starved(0)
	, m_malformed(0)
	, m_selftest(selftest)
	, m_selftest_fail(0)
	, m_log(nullptr)
{
	std::memset(m_ring, 0, sizeof(m_ring));
	std::memset(m_out, 0, sizeof(m_out));
	std::memset(m_prev, 0, sizeof(m_prev));

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
				savestate_layout_hash(machine), players);

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

	std::unique_ptr<manager> candidate(new manager(machine, local, players,
			delay, batch, env_flag("NETPLAY_REDUNDANCY"),
			env_flag("NETPLAY_SELFTEST")));

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
				publish(head.base_frame + frame_t(f), p, words[idx++]);
			}
		}
	}
}


//-------------------------------------------------
//  wait_for_input - the lockstep stall
//-------------------------------------------------

void manager::wait_for_input()
{
	poll_link();
	if (complete(m_frame))
		return;

	// This is the stutter. Every frame counted here is a frame the player
	// saw the picture stop, and driving this to zero is what the delay is
	// chosen for. notes/protocol.txt 1.6.3
	m_stalls++;

	// Frame 0 is a different problem from every other frame: the peers are
	// still launching, so the first wait is seconds rather than milliseconds.
	// One limit cannot serve both, so the budget is generous until the first
	// remote packet has ever arrived and tight afterwards.
	const unsigned limit = (m_link && (m_link->recv_count() == 0))
			? JOIN_STALL_LIMIT : STALL_LIMIT;

	unsigned spins = 0;
	while (!complete(m_frame))
	{
		if (!m_link || !m_link->wait())
			break;
		poll_link();
		m_stall_frames++;
		if (++spins >= limit)
			break;
	}

	if (!complete(m_frame))
	{
		// Running a frame on incomplete input is a desync: this peer applies
		// a neutral word where the owner applied something else. Counted,
		// not hidden - resync is the answer and does not exist yet. [open]
		m_starved++;
		touch(m_frame);
		at(m_frame).have = m_need;
	}
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
			"starved=%llu malformed=%llu sent=%llu recv=%llu dropped=%llu\n",
			unsigned(m_frame),
			static_cast<unsigned long long>(m_stalls),
			static_cast<unsigned long long>(m_stall_frames),
			static_cast<unsigned long long>(m_starved),
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
