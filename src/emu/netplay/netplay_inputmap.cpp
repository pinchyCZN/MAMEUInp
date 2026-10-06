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

unsigned popcount32(u32 v)
{
	unsigned n = 0;
	for ( ; v != 0; v >>= 1)
		n += (v & 1u);
	return n;
}


unsigned count_trailing_zeros(u32 v)
{
	if (v == 0)
		return 0;

	unsigned n = 0;
	while ((v & 1u) == 0)
	{
		v >>= 1;
		n++;
	}
	return n;
}


// A mask with a hole in it cannot be carried as one right-justified
// field. Every analog mask measured over 2675 axes is a contiguous run,
// including the shifted ones - 0xff00, 0x0018, 0x00e0.
// [verified, tools/netplay_analog.py]
bool contiguous_mask(u32 v)
{
	if (v == 0)
		return false;

	const u32 r = v >> count_trailing_zeros(v);
	return (r & (r + 1)) == 0;
}

} // anonymous namespace


//-------------------------------------------------
//  construction
//-------------------------------------------------

inputmap::inputmap()
	: m_digest(0)
	, m_config_digest(0)
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
	std::memset(&m_layout, 0, sizeof(m_layout));
	m_digest = 0;
	m_config_digest = 0;
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

	// pass 2: place every non-toggle digital field, and every axis.
	//
	// A PLAYER'S AXES ARE NUMBERED IN THIS WALK'S ORDER - port tag, then
	// field declaration - which is the same on every peer because
	// ioport_list is an ordered map. The widths collected here are what
	// the layout is built from below, so there is one walk rather than
	// two that have to agree.
	u8 pbits[MAX_PLAYERS][MAX_AXES];
	u8 pcount[MAX_PLAYERS];
	std::memset(pbits, 0, sizeof(pbits));
	std::memset(pcount, 0, sizeof(pcount));

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
			if (field.is_analog())
			{
				player_t ap = 0;
				classify(field, ap);

				const ioport_value m = field.mask();
				const unsigned wid = popcount32(u32(m));

				// AN ANALOG FIELD NOBODY OWNS IS A DESYNC, NOT A WARNING.
				// Its value reaches the driver through ioport_port::read()
				// whatever we do, so a field we cannot route stays local
				// and the two peers compute different machine state from
				// frame one, with nothing in any log to say why. Landing in
				// m_unowned makes it a refused session instead.
				if ((ap < 1) || (ap > MAX_PLAYERS) || (m == 0)
						|| (wid > AXIS_MAX_BITS) || !contiguous_mask(u32(m))
						|| (pcount[ap - 1] >= MAX_AXES)
						|| (field.live().analog == nullptr))
				{
					m_unowned.push_back(util::string_format(
							"%s:%s (analog, mask %08x, player %d)",
							pe.tag,
							(field.name() != nullptr) ? field.name() : "?",
							u32(m), int(ap)));
					continue;
				}

				axis a;
				a.an = field.live().analog;
				a.mask = m;
				a.shift = u8(count_trailing_zeros(u32(m)));
				a.bits = u8(wid);
				a.player = ap;
				a.index = pcount[ap - 1];
				pe.axes.push_back(a);

				pbits[ap - 1][pcount[ap - 1]] = u8(wid);
				pcount[ap - 1]++;
				m_players = std::max(m_players, ap);
				continue;
			}

			if (field.toggle())
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

		// OR AN AXIS. A port of nothing but analog fields - arkanoid's
		// two spinners, area51's four FAKE axes - was skipped while the
		// format was digital only. Skipping it now means capture and
		// inject never see the axis at all.
		if (any_digital || !pe.axes.empty())
		{
			m_index[&port] = u32(m_ports.size());
			m_ports.push_back(std::move(pe));
		}
	}

	// THE LAYOUT IS THE UNION OF EVERY PLAYER'S AXES, sized by the
	// widest. protocol.txt 3.5 puts frame N player P at a fixed offset, so
	// one shape has to serve every slot and a player short of the maximum
	// leaves its tail zero. aceattac is the driver that needs this: its
	// four players hold two, three, one and two axes.
	{
		unsigned axes = 0;
		for (unsigned i = 0; i < MAX_PLAYERS; i++)
			axes = std::max(axes, unsigned(pcount[i]));

		u8 axis_bits[MAX_AXES];
		std::memset(axis_bits, 0, sizeof(axis_bits));
		for (unsigned k = 0; k < axes; k++)
			for (unsigned i = 0; i < MAX_PLAYERS; i++)
				if (pbits[i][k] > axis_bits[k])
					axis_bits[k] = pbits[i][k];

		if (!layout_build(m_layout, unsigned(m_logical.size()),
				axis_bits, axes))
		{
			osd_printf_error("netplay: %s does not fit the word: %d digital bits, %d axes\n",
					machine.system().name, int(m_logical.size()), int(axes));
			m_built = true;
			return false;
		}
	}

	// CONFIG DIGEST: the per-field live values that DIP switches, config
	// settings and toggles are stored in.
	//
	// ioport_port::read does `result ^= m_live->defvalue`, and that port
	// defvalue is where all of it ends up. inject_port writes
	// live().digital and never touches defvalue, so none of it travels
	// and none of it is overwritten: two peers with one DIP set
	// differently read different values every frame and diverge
	// immediately and permanently, with nothing in any log to say why.
	// Free play on one side against coins on the other is the easy case
	// to picture.
	//
	// HASHED PER FIELD, NOT PER PORT, AND THE REASON IS ORDERING. The
	// port's defvalue is recomputed from these fields by
	// ioport_port::update_defvalue, whose only callers sit in
	// ioport_manager::frame_update AFTER the netplay hook this runs from -
	// and nothing calls it when the config file is read. So at this point
	// the port defvalue has never absorbed the loaded settings, and
	// hashing it would give two differently configured peers the same
	// digest: a check that looks present and detects nothing.
	// ioport_manager::load_config writes field.live().value directly,
	// which is why that is the field to read.
	//
	// Over the WHOLE port list, not m_ports: a port only enters m_ports
	// when it has a digital field that is neither analog nor a toggle, so
	// a port of nothing but analog or nothing but toggles is absent from
	// it - and its settings matter just the same. Disabled fields are
	// included too, because whether a field is enabled depends on other
	// fields' values through PORT_CONDITION, and a digest should not
	// depend on evaluating that.
	u64 c = MIX_SEED;
	for (auto &entry : portlist)
	{
		c = mix_string(c, entry.first.c_str());
		for (const ioport_field &field : entry.second->fields())
		{
			c = mix(c, (u64(field.mask()) << 32)
					| u64(field.live().value & field.mask()));
			c = mix(c, field.live().toggle ? 1u : 0u);

			// ANALOG SETTINGS ARE THE SAME CLASS OF HAZARD AS A DIP.
			// keydelta, centerdelta, sensitivity and reverse are per-field,
			// are written to the cfg file (ioport.cpp save_game_inputs), and
			// are in no savestate - so two peers who set a dial's
			// sensitivity differently diverge every frame exactly as a DIP
			// mismatch does. A driver with no analog fields cannot show it,
			// which is why dino never did.
			const analog_field *const an = field.live().analog;
			if (an != nullptr)
			{
				c = mix(c, u64(u32(an->sensitivity())));
				c = mix(c, u64(u32(an->delta())));
				c = mix(c, u64(u32(an->centerdelta())));
				c = mix(c, an->reverse() ? 1u : 0u);
			}
		}
	}
	m_config_digest = finalise(c);

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

		// THE AXIS TABLE IS PART OF THE MAPPING. Two peers that number a
		// player's axes differently, or disagree on a width, read each
		// other's word at the wrong offset while agreeing on every other
		// digest. protocol.txt 4.2
		for (const axis &a : pe.axes)
			h = mix(h, (u64(a.mask) << 32) | (u64(a.player) << 24)
					| (u64(a.index) << 16) | (u64(a.bits) << 8) | u64(a.shift));
	}
	h = mix(h, (u64(m_layout.bytes) << 8) | u64(m_layout.axes));
	h = mix(h, u64(m_layout.digital_bytes));
	// THE CONFIG DIGEST IS DELIBERATELY NOT FOLDED IN HERE.
	//
	// It was, and that made a DIP mismatch a clean refusal at join - but
	// it also made adoption impossible, because the server compares one
	// opaque number and cannot tell "different driver build" (never
	// adoptable) from "different DIP" (adoptable). Nor could a guest
	// adopt before computing the digest it joins with: there is no peer
	// channel before the session exists.
	//
	// So the config check moved to the peers (protocol.txt 4.2.1): this
	// digest stays structural and the server keeps enforcing it, while
	// the host announces its config digest over MSG_CONFIG and the guest
	// adopts, verifies, or refuses itself.

	m_digest = finalise(h);

	m_fits = m_unowned.empty();
	m_built = true;
	return m_fits;
}


//-------------------------------------------------
//  configuration: collect, apply, name
//-------------------------------------------------

namespace {

// THE ONE CANONICAL ORDER, used by every function below and by the
// config digest. Ports come from an ordered map so their sequence is
// the same on every peer, and a port's fields keep declaration order.
// Only configuration is selected: a DIP or config field, or anything
// analog. A play field's value is not a setting and is never sent.
bool config_interesting(const ioport_field &field)
{
	return (field.type() == IPT_DIPSWITCH)
			|| (field.type() == IPT_CONFIG)
			|| field.is_analog();
}

} // anonymous namespace


unsigned inputmap::collect_config(running_machine &machine,
		config_rec *out, unsigned max)
{
	if (out == nullptr)
		return 0;

	unsigned idx = 0, n = 0;
	for (auto &entry : machine.ioport().ports())
	{
		for (const ioport_field &field : entry.second->fields())
		{
			if (!config_interesting(field))
				continue;

			if (n < max)
			{
				config_rec &r = out[n];
				r.index = u16(idx);
				r.flags = field.live().toggle ? CONFIG_REC_TOGGLE : 0;
				r.value = u32(field.live().value & field.mask());
				r.sensitivity = 0;
				r.delta = 0;
				r.centerdelta = 0;

				const analog_field *const an = field.live().analog;
				if (an != nullptr)
				{
					r.flags = u16(r.flags | CONFIG_REC_ANALOG);
					if (an->reverse())
						r.flags = u16(r.flags | CONFIG_REC_REVERSE);
					r.sensitivity = an->sensitivity();
					r.delta = an->delta();
					r.centerdelta = an->centerdelta();
				}
				n++;
			}
			idx++;
		}
	}

	return n;
}


unsigned inputmap::apply_config(running_machine &machine,
		const config_rec *recs, unsigned count)
{
	if ((recs == nullptr) || (count == 0))
		return 0;

	unsigned idx = 0, changed = 0;
	for (auto &entry : machine.ioport().ports())
	{
		for (ioport_field &field : entry.second->fields())
		{
			if (!config_interesting(field))
				continue;

			const unsigned here = idx++;

			// Matched by the index in the record, not by position in the
			// array, so a sender that sends a subset still lands correctly.
			const config_rec *r = nullptr;
			for (unsigned i = 0; i < count; i++)
				if (recs[i].index == here)
				{
					r = &recs[i];
					break;
				}
			if (r == nullptr)
				continue;

			// THROUGH set_user_settings, THE SAME ENTRY POINT MAME'S OWN UI
			// USES. analog_field keeps its live members private and names
			// ioport_field::set_user_settings as its friend, so that is the
			// sanctioned way in rather than reaching past it.
			//
			// IN MEMORY ONLY. These are somebody else's settings, borrowed
			// for one session; nothing here writes the cfg file, so joining
			// a game cannot permanently change this machine's DIP switches.
			ioport_field::user_settings us;
			field.get_user_settings(us);

			bool moved = false;
			const ioport_value want = (r->value & field.mask())
					| (us.value & ~field.mask());
			if (us.value != want)
			{
				us.value = want;
				moved = true;
			}

			const bool tog = ((r->flags & CONFIG_REC_TOGGLE) != 0);
			if (us.toggle != tog)
			{
				us.toggle = tog;
				moved = true;
			}

			if (field.is_analog() && ((r->flags & CONFIG_REC_ANALOG) != 0))
			{
				const bool rev = ((r->flags & CONFIG_REC_REVERSE) != 0);
				if ((us.sensitivity != r->sensitivity)
						|| (us.delta != r->delta)
						|| (us.centerdelta != r->centerdelta)
						|| (us.reverse != rev))
				{
					us.sensitivity = r->sensitivity;
					us.delta = r->delta;
					us.centerdelta = r->centerdelta;
					us.reverse = rev;
					moved = true;
				}
			}

			if (moved)
			{
				field.set_user_settings(us);
				changed++;
			}
		}
	}

	// The port defvalue is rebuilt from these fields by
	// ioport_port::update_defvalue, which runs later in
	// ioport_manager::frame_update - so the next frame sees them without
	// anything here having to force it.
	return changed;
}


const char *inputmap::config_field_name(running_machine &machine,
		unsigned index, std::string &tag)
{
	unsigned idx = 0;
	for (auto &entry : machine.ioport().ports())
	{
		for (const ioport_field &field : entry.second->fields())
		{
			if (!config_interesting(field))
				continue;
			if (idx++ != index)
				continue;

			tag = entry.first;
			return (field.name() != nullptr) ? field.name() : "?";
		}
	}
	return nullptr;
}


u64 inputmap::recompute_config_digest(running_machine &machine)
{
	u64 c = MIX_SEED;
	for (auto &entry : machine.ioport().ports())
	{
		c = mix_string(c, entry.first.c_str());
		for (const ioport_field &field : entry.second->fields())
		{
			c = mix(c, (u64(field.mask()) << 32)
					| u64(field.live().value & field.mask()));
			c = mix(c, field.live().toggle ? 1u : 0u);

			const analog_field *const an = field.live().analog;
			if (an != nullptr)
			{
				c = mix(c, u64(u32(an->sensitivity())));
				c = mix(c, u64(u32(an->delta())));
				c = mix(c, u64(u32(an->centerdelta())));
				c = mix(c, an->reverse() ? 1u : 0u);
			}
		}
	}

	m_config_digest = finalise(c);
	return m_config_digest;
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
//  local_bit_mask
//-------------------------------------------------

u32 inputmap::local_bit_mask() const
{
	u32 mask = 0;
	for (const port_entry &pe : m_ports)
		for (const owned &o : pe.fields)
			if (o.player == LOCAL_SLOT)
				mask |= u32(1) << o.bit;
	return mask;
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

	// THE AXES OF LOCAL_SLOT, the same rule the digital half uses: the
	// human at this keyboard drives player 1's bindings whatever slot
	// the session gave them, so nobody has to learn a different gun
	// because they joined second.
	//
	// netplay_value() is the POST-MASK value - apply_settings(m_accum)
	// right-justified - so what goes on the wire is the number the
	// driver would have read, not the device-space accumulator behind
	// it. notes/protocol.txt 1.6.1
	for (const axis &a : pe->axes)
		if (a.player == LOCAL_SLOT)
			word_set_analog(word, m_layout, a.index,
					u32(a.an->netplay_value()));
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
		if ((word_digital(words[o.player - 1], m_layout)
				& (u32(1) << o.bit)) != 0)
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

	// THE AXES, which digital cannot carry: ioport_port::read() applies
	// analog AFTER digital and overwrites its own mask from m_accum, so a
	// value written into live().digital for an analog field is discarded.
	// netplay_force replaces the result of apply_settings inside
	// analog_field::read instead, which is also what crosshair_read uses -
	// so the drawn crosshair follows the networked axis rather than this
	// machine's device. See the NETPLAY block in ioport.cpp.
	for (const axis &a : pe->axes)
	{
		if ((a.player < 1) || (a.player > count))
			continue;

		a.an->netplay_force(s32(word_analog(words[a.player - 1],
				m_layout, a.index)));
	}
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

	out += util::string_format("word=%d bytes (digital %d, axes %d)\n",
			int(m_layout.bytes), int(m_layout.digital_bytes),
			int(m_layout.axes));
	for (unsigned k = 0; k < m_layout.axes; k++)
		out += util::string_format("  axis %d at byte %d, %d byte(s)\n",
				int(k), int(m_layout.axis_off[k]),
				int(m_layout.axis_bytes[k]));
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
		for (const axis &a : pe.axes)
			out += util::string_format(
					"      p%d axis %d mask %08x shift %d bits %d\n",
					int(a.player), int(a.index), u32(a.mask), int(a.shift),
					int(a.bits));
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
