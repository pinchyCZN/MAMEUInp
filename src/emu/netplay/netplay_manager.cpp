// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_manager.cpp

    See netplay_manager.h.

***************************************************************************/

#include "emu.h"
#include "netplay/netplay_manager.h"

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
// blanks rather than empty, and testing only the first byte treats it as
// real. The same applies to an environment variable set to nothing.
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

} // anonymous namespace


//-------------------------------------------------
//  construction
//-------------------------------------------------

manager::manager(running_machine &machine, player_t local, player_t players,
		unsigned delay, bool selftest)
	: m_machine(machine)
	, m_map()
	, m_frame(0)
	, m_local(local)
	, m_players(players)
	, m_delay(delay)
	, m_pending(0)
	, m_echo(0)
	, m_local_mask(0)
	, m_selftest(selftest)
	, m_selftest_fail(0)
	, m_log(nullptr)
{
	std::memset(m_ring, 0, sizeof(m_ring));

	// Frames 0 .. delay-1 would have been published by frames -delay .. -1,
	// which never ran. Neutral is nothing pressed, which is what the machine
	// would see from a player who has not touched a control yet.
	for (frame_t f = 0; f < frame_t(m_delay); f++)
	{
		slot &s = at(f);
		s.complete = true;
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

bool manager::setup(const char *logpath)
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

	m_local_mask = m_map.local_bit_mask();

	if (!unset(logpath))
	{
		m_log = std::fopen(logpath, "w");
		if (m_log != nullptr)
		{
			std::fprintf(m_log, "# machine=%s\n", m_machine.system().name);
			std::fprintf(m_log, "# inputmap_digest=%016llx\n",
					static_cast<unsigned long long>(m_map.digest()));
			std::fprintf(m_log, "# players=%d local=%d delay=%d\n",
					int(m_players), int(m_local), int(m_delay));
			std::fprintf(m_log, "# local_bits=%08x\n", m_local_mask);
			std::fprintf(m_log, "frame,captured,injected\n");
			std::fflush(m_log);
		}
		else
		{
			osd_printf_error("netplay: unable to open log '%s'\n", logpath);
		}
	}

	osd_printf_info("netplay: session players=%d local=%d delay=%d bits=%d digest=%016llx\n",
			int(m_players), int(m_local), int(m_delay), int(m_map.digital_used()),
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
	if (unset(mode) || (std::strcmp(mode, "loopback") != 0))
		return;

	player_t players = player_t(env_uint("NETPLAY_PLAYERS", 1));
	player_t local = player_t(env_uint("NETPLAY_SLOT", inputmap::LOCAL_SLOT));
	unsigned delay = env_uint("NETPLAY_DELAY", 2);

	// A delay of zero cannot work - see the header.
	if (delay < 1)
		delay = 1;
	if (delay > (RING - 2))
		delay = RING - 2;
	if (players < 1)
		players = 1;
	if (players > MAX_PLAYERS)
		players = MAX_PLAYERS;
	if ((local < 1) || (local > players))
		local = 1;

	std::unique_ptr<manager> candidate(new manager(machine, local, players,
			delay, !unset(std::getenv("NETPLAY_SELFTEST"))));

	if (!candidate->setup(std::getenv("NETPLAY_LOG")))
		return;

	s_instance = std::move(candidate);
}


//-------------------------------------------------
//  active
//-------------------------------------------------

manager *manager::active()
{
	return s_instance.get();
}


//-------------------------------------------------
//  port_capture - local input, one port
//-------------------------------------------------

void manager::port_capture(ioport_port &port)
{
	if (s_owner != &port.machine())
		open_session(port.machine());

	manager *const m = active();
	if (m == nullptr)
		return;

	m->m_map.capture_port(port, m->m_pending);
}


//-------------------------------------------------
//  port_inject - the frame's words, one port
//-------------------------------------------------

void manager::port_inject(ioport_port &port)
{
	manager *const m = active();
	if (m == nullptr)
		return;

	slot &s = m->at(m->m_frame);
	if (!s.complete)
	{
		// A STALL. In lockstep the machine must not advance past a frame
		// whose input has not arrived, and returning here lets it advance
		// with the local values still in place - which desyncs. Loopback
		// never reaches this because it publishes its own input; a real
		// transport does, and the frame has to be held before it starts
		// rather than patched here. [open]
		return;
	}

	m->m_map.inject_port(port, s.words, m->m_players);

	if (m->m_selftest)
		m->m_map.capture_port(port, m->m_echo);
}


//-------------------------------------------------
//  publish
//-------------------------------------------------

void manager::publish(frame_t frame, player_t player, input_word word)
{
	if ((player < 1) || (player > MAX_PLAYERS))
		return;

	slot &s = at(frame);

	// The ring wraps, so a slot being published for frame F still holds
	// frame F-RING. Clearing on the first publish for a frame is what stops
	// a stale word being replayed RING frames later.
	if (!s.complete)
		for (player_t p = 0; p < MAX_PLAYERS; p++)
			s.words[p] = 0;

	s.words[player - 1] = word;

	// Loopback has one source, so one publish completes the frame. With a
	// transport this becomes "every player's word has arrived". [open]
	s.complete = true;
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
//  frame_end
//-------------------------------------------------

void manager::frame_end(running_machine &machine)
{
	manager *const m = active();
	if ((m == nullptr) || (s_owner != &machine))
		return;

	if (m->m_selftest && m->at(m->m_frame).complete)
		m->check_selftest();

	if (m->m_log != nullptr)
		std::fprintf(m->m_log, "%u,%016llx,%016llx\n",
				unsigned(m->m_frame),
				static_cast<unsigned long long>(m->m_pending),
				static_cast<unsigned long long>(
						m->at(m->m_frame).words[m->m_local - 1]));

	// LOOPBACK: the local word is its own remote input, delay frames later.
	// A transport sends m_pending here and publishes whatever has arrived.
	m->publish(m->m_frame + frame_t(m->m_delay), m->m_local, m->m_pending);

	m->m_pending = 0;
	m->m_echo = 0;
	m->m_frame++;
}


//-------------------------------------------------
//  on_exit
//-------------------------------------------------

void manager::on_exit()
{
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

	osd_printf_info("netplay: session closed after %u frame(s)\n", unsigned(m_frame));

	// Release ownership but DO NOT reset s_instance: this runs as a member of
	// the object s_instance owns, so resetting would destroy `this` and then
	// return into freed memory. open_session() replaces the instance when the
	// next machine starts, which destroys this one at a safe point.
	s_owner = nullptr;
}

} // namespace netplay
