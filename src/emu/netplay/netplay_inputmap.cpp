// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_inputmap.cpp

    See netplay_inputmap.h.

***************************************************************************/

#include "emu.h"
#include "netplay/netplay_inputmap.h"
#include "netplay/netplay_mix.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>


namespace netplay {

namespace {

// Synthetic logical ids for start and coin, above every ioport_type so the
// sort that assigns bits stays stable as MAME's enum grows.
constexpr u32 LOGICAL_START = 0x00010000;
constexpr u32 LOGICAL_COIN  = 0x00010001;

constexpr u32 LOGICAL_NONE  = 0xFFFFFFFF;

//-------------------------------------------------
//  classify - the two routing rules
//-------------------------------------------------

u32 classify(const ioport_field &field, player_t &player)
{
	const ioport_type type = field.type();

	if ((type >= IPT_START1) && (type <= IPT_START10))
	{
		player = player_t(type - IPT_START1 + 1);
		return LOGICAL_START;
	}

	if ((type >= IPT_COIN1) && (type <= IPT_COIN12))
	{
		player = player_t(type - IPT_COIN1 + 1);
		return LOGICAL_COIN;
	}

	if (field.type_class() == INPUT_CLASS_CONTROLLER)
	{
		player = player_t(field.player() + 1);

		// A THIRD RULE, for one type. IPT_START is the per-player start,
		// declared PLAYER-grouped - inpttype.h has
		// INPUT_PORT_DIGITAL_TYPE(3, PLAYER3, START, "P3 Start") - so unlike
		// IPT_START1..10 its player comes from field.player(). A driver may
		// use either mechanism, and rsgun uses BOTH: START1/START2 for
		// players 1 and 2, IPT_START for players 3 and 4. Folding it onto the
		// same logical input keeps one bit for start however the driver spelt
		// it. Routing is correct without this; the word is one bit wider.
		if (type == IPT_START)
			return LOGICAL_START;

		return u32(type);
	}

	player = 0;
	return LOGICAL_NONE;
}

} // anonymous namespace


//-------------------------------------------------
//  construction
//-------------------------------------------------

inputmap::inputmap()
	: m_digest(0)
	, m_players(0)
	, m_analog_seen(0)
	, m_fits(false)
	, m_built(false)
{
}


//-------------------------------------------------
//  build
//-------------------------------------------------

bool inputmap::build(running_machine &machine)
{
	m_ports.clear();
	m_index.clear();
	m_logical.clear();
	m_unowned.clear();
	m_digest = 0;
	m_players = 0;
	m_analog_seen = 0;
	m_fits = false;
	m_built = false;

	// DETERMINISM CONSTRAINT: ioport_list derives from
	// std::map<std::string, ...>, so iteration is ordered by port tag and is
	// identical on every peer running the same driver. [verified ioport.h]
	// Both the bit assignment below and the digest depend on that, which is
	// what lets each peer build the same table without transmitting it.
	const ioport_list &portlist = machine.ioport().ports();

	// pass 1: which logical inputs does this driver use at all
	std::set<u32> used;
	for (auto &entry : portlist)
	{
		for (const ioport_field &field : entry.second->fields())
		{
			if (field.is_analog())
			{
				m_analog_seen++;
				continue;
			}

			// Toggles never reach a port's digital value - frame_update folds
			// them into the port default and returns false - so they are not
			// ours to route. They are pinned by the handshake instead.
			// netplay-hooks.txt 1.3
			if (field.toggle())
				continue;

			player_t player = 0;
			const u32 logical = classify(field, player);
			if (logical == LOGICAL_NONE)
				continue;
			if ((player < 1) || (player > MAX_PLAYERS))
				continue;

			used.insert(logical);
			m_players = std::max(m_players, player);
		}
	}

	m_logical.assign(used.begin(), used.end());      // std::set: ascending
	if (m_logical.size() > DIGITAL_BITS)
	{
		// A driver using all of the MAHJONG and GAMBLING blocks would land
		// here. Measured maximum is 30 of 32. notes/protocol.txt 1.6.1
		osd_printf_error("netplay: %s needs %d digital bits, the word carries %d\n",
				machine.system().name, int(m_logical.size()), int(DIGITAL_BITS));
		m_built = true;
		return false;
	}

	// pass 2: place every non-toggle digital field
	for (auto &entry : portlist)
	{
		ioport_port &port = *entry.second.get();

		port_entry pe;
		pe.port = &port;
		pe.tag = entry.first;
		pe.shared_mask = 0;

		bool any_digital = false;

		for (const ioport_field &field : port.fields())
		{
			if (field.is_analog() || field.toggle())
				continue;

			any_digital = true;

			player_t player = 0;
			const u32 logical = classify(field, player);

			if ((logical == LOGICAL_NONE) || (player < 1) || (player > MAX_PLAYERS))
			{
				// Service, tilt and anything else with no player. Every
				// digital field must land in one of the three sets; a field
				// in none of them is a bug, not a default.
				// netplay-hooks.txt 1.5
				pe.shared_mask |= field.mask();
				continue;
			}

			const auto it = std::lower_bound(m_logical.begin(), m_logical.end(), logical);
			if ((it == m_logical.end()) || (*it != logical))
			{
				m_unowned.push_back(util::string_format("%s:%s", pe.tag,
						(field.name() != nullptr) ? field.name() : "?"));
				continue;
			}

			owned o;
			o.mask = field.mask();
			o.player = player;
			o.bit = u8(it - m_logical.begin());
			pe.fields.push_back(o);
		}

		if (any_digital)
		{
			m_index[&port] = u32(m_ports.size());
			m_ports.push_back(std::move(pe));
		}
	}

	// the digest covers the whole mapping, in the order built above
	u64 h = MIX_SEED;
	h = mix_string(h, machine.system().name);
	h = mix(h, u64(m_logical.size()));
	h = mix(h, u64(m_players));
	for (u32 logical : m_logical)
		h = mix(h, u64(logical));
	for (const port_entry &pe : m_ports)
	{
		h = mix_string(h, pe.tag.c_str());
		h = mix(h, u64(pe.shared_mask));
		for (const owned &o : pe.fields)
			h = mix(h, (u64(o.mask) << 16) | (u64(o.player) << 8) | u64(o.bit));
	}
	m_digest = finalise(h);

	m_fits = m_unowned.empty();
	m_built = true;
	return m_fits;
}


//-------------------------------------------------
//  find
//-------------------------------------------------

const inputmap::port_entry *inputmap::find(const ioport_port &port) const
{
	const auto it = m_index.find(&port);
	return (it == m_index.end()) ? nullptr : &m_ports[it->second];
}


//-------------------------------------------------
//  capture_port - local input for one port into
//  the pending word
//-------------------------------------------------

void inputmap::capture_port(ioport_port &port, input_word &word) const
{
	const port_entry *const pe = find(port);
	if (pe == nullptr)
		return;

	const ioport_value digital = port.live().digital;
	for (const owned &o : pe->fields)
		if ((o.player == LOCAL_SLOT) && ((digital & o.mask) != 0))
			word_set_digital(word, o.bit, true);
}


//-------------------------------------------------
//  inject_port - overwrite one port's digital
//  value from every player's word
//-------------------------------------------------

void inputmap::inject_port(ioport_port &port, const input_word *words, player_t count) const
{
	const port_entry *const pe = find(port);
	if (pe == nullptr)
		return;

	ioport_value value = 0;
	for (const owned &o : pe->fields)
	{
		if ((o.player < 1) || (o.player > count))
			continue;
		if ((word_digital(words[o.player - 1]) & (u32(1) << o.bit)) != 0)
			value |= o.mask;
	}

	// A PLAIN WRITE, not a masked read-modify-write, and shared fields come
	// out ZERO. Both are deliberate.
	//
	// Plain write is safe because PORT_CUSTOM and analog values are never
	// stored in digital - ioport_port::read() recomputes them on every read -
	// so this cannot clobber either. netplay-hooks.txt 1.3
	//
	// Zeroing the shared mask is what makes service and tilt deterministic:
	// left local they would differ between peers and desync. The cost is that
	// nobody can press service during a session. notes/protocol.txt 1.2
	//
	// Writing here also discards everything ioport_field::frame_update did
	// locally - autofire, coin impulse, coin lockout, the 4-way restriction,
	// the menu-active early-out - which is the point: all of it is local
	// state and local options sitting between a keypress and the port value.
	// netplay-hooks.txt 1.5
	port.live().digital = value;
}


//-------------------------------------------------
//  describe
//-------------------------------------------------

std::string inputmap::describe() const
{
	std::string out;

	out += util::string_format("players=%d digital_bits=%d/%d analog_seen=%d unowned=%d fits=%s\n",
			int(m_players), int(m_logical.size()), int(DIGITAL_BITS),
			int(m_analog_seen), int(m_unowned.size()), m_fits ? "yes" : "NO");
	out += util::string_format("digest=%016llx\n",
			static_cast<unsigned long long>(m_digest));

	for (u32 bit = 0; bit < m_logical.size(); bit++)
	{
		const u32 logical = m_logical[bit];
		const char *name =
				(logical == LOGICAL_START) ? "<START>" :
				(logical == LOGICAL_COIN) ? "<COIN>" : nullptr;
		if (name != nullptr)
			out += util::string_format("  bit %2d  %s\n", int(bit), name);
		else
			out += util::string_format("  bit %2d  ioport_type %d\n", int(bit), int(logical));
	}

	for (const port_entry &pe : m_ports)
	{
		out += util::string_format("  port %-14s shared=%08x\n",
				pe.tag, u32(pe.shared_mask));
		for (const owned &o : pe.fields)
			out += util::string_format("      p%d bit %2d mask %08x\n",
					int(o.player), int(o.bit), u32(o.mask));
	}

	for (const std::string &u : m_unowned)
		out += util::string_format("  UNOWNED %s\n", u);

	return out;
}

//-------------------------------------------------
//  dump_once - diagnostic, see the header
//-------------------------------------------------

void inputmap::dump_once(running_machine &machine)
{
	static running_machine *s_done = nullptr;
	if (s_done == &machine)
		return;

	const char *const path = std::getenv("NETPLAY_INPUTMAP");
	if ((path == nullptr) || (*path == 0))
		return;
	s_done = &machine;

	inputmap map;
	const bool ok = map.build(machine);

	FILE *const out = std::fopen(path, "w");
	if (out == nullptr)
	{
		osd_printf_error("netplay: unable to open inputmap dump '%s'\n", path);
		return;
	}

	const std::string text = map.describe();
	std::fprintf(out, "# driver=%s\n", machine.system().name);
	std::fprintf(out, "# build_ok=%s\n", ok ? "yes" : "no");
	std::fputs(text.c_str(), out);
	std::fclose(out);

	osd_printf_info("netplay: inputmap players=%d bits=%d digest=%016llx -> %s\n",
			int(map.players()), int(map.digital_used()),
			static_cast<unsigned long long>(map.digest()), path);
}

} // namespace netplay
