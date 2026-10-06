// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_inputmap.h

    The translation table: (player, logical input) <-> (port, mask).

    This is the format. The wire payload is a per-player LOGICAL word, so
    it is only meaningful alongside this table, and the table is built once
    at session start because the metadata it needs - type() and player() -
    lives on the fields and is gone by the time a port's digital value is
    assembled. notes/protocol.txt 1.2, notes/netplay-hooks.txt 1.5.

    Two rules, not one. Joysticks and buttons take the player from
    field.player(); start and coin take it from the TYPE, because MAME
    declares IPT_START1 and IPT_COIN1 in IPG_OTHER with player 0, so
    routing them by player() sends every player's start to player 1.
    netplay-hooks.txt 1.4.

    DIGITAL AND ANALOG. A digital field is one bit of the word's bitmap; an
    analog field is a byte-aligned slot carrying its POST-MASK value, the
    number analog_field::read() inserts into the port. Injection goes
    through analog_field::netplay_force rather than m_accum, so no peer has
    to agree about sensitivity, reverse or scaling, and a wrapping axis -
    whose m_accum has no bound at all - is carried in a fixed width.
    notes/protocol.txt 1.6.1.

    ONE LAYOUT FOR EVERY PLAYER, sized by the widest. Players do not have
    to be symmetric: aceattac gives player 1 a positional and a trackball
    and player 2 a stick, a positional and a pedal. protocol.txt 3.5 wants
    frame N player P at a fixed offset, so the word is the union of every
    player's axes and a player short of the maximum leaves its tail zero.

    [open] Analog sensitivity, delta, centerdelta and reverse are still
    collected and pinned by the config layer below. That is now
    conservative rather than required: the wire carries the post-mask
    value, so two peers set differently no longer diverge, they only feel
    different. Dropping them from the digest would remove a refusal reason
    at the cost of a session where one player's gun is twitchier.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_INPUTMAP_H
#define MAME_EMU_NETPLAY_NETPLAY_INPUTMAP_H

#pragma once

#include "netplay/netplay_types.h"
#include "netplay/netplay_packet.h"

#include <string>
#include <unordered_map>
#include <vector>


namespace netplay {

class inputmap
{
public:
	// The local human drives THIS slot's fields whatever session slot they
	// were given, because MAME's default sequences are disjoint per player
	// and nobody should have to learn a different keyboard because they
	// joined second. notes/protocol.txt 1.1
	static constexpr player_t LOCAL_SLOT = 1;

	inputmap();

	// walks every port and field; returns false if the driver cannot be
	// represented, which is a refused session, not a warning
	bool build(running_machine &machine);

	bool built() const { return m_built; }
	bool fits() const { return m_fits; }

	// handshake identity: two peers resolving (player, logical) to different
	// (port, mask) pairs misroute each other's input while agreeing on every
	// other digest. notes/protocol.txt 4.2
	u64 digest() const { return m_digest; }

	// Every port's defvalue, hashed. DIP switches, config settings,
	// toggle state and active-low polarity all live there, none of it is
	// on the wire, and injection never touches it - so two peers set
	// differently diverge every frame, forever. Folded into digest() so
	// the handshake refuses that, and exposed separately so a mismatch
	// can be told apart from a structural one by comparing two logs.
	u64 config_digest() const { return m_config_digest; }

	// Recomputes it against the machine as it stands now. Needed after
	// applying somebody else's settings, to prove the two peers agree
	// rather than hoping they do.
	u64 recompute_config_digest(running_machine &machine);

	// The settings that are configuration rather than play: DIP
	// switches, config settings and anything analog. Collected and
	// applied in one canonical order - ports in tag order, fields in
	// declaration order - so a position identifies a field and no tag
	// strings need to travel. The structural digest the handshake
	// enforces is what makes that ordering agree between peers.
	static unsigned collect_config(running_machine &machine,
			config_rec *out, unsigned max);

	// Writes them into the live machine. Reports how many it changed, so
	// a caller can say nothing happened rather than claim it did.
	static unsigned apply_config(running_machine &machine,
			const config_rec *recs, unsigned count);

	// Names the field at a canonical index, for a diff a human can read.
	// nullptr past the end.
	static const char *config_field_name(running_machine &machine,
			unsigned index, std::string &tag);

	player_t players() const { return m_players; }
	unsigned digital_used() const { return unsigned(m_logical.size()); }
	unsigned analog_seen() const { return m_analog_seen; }
	unsigned unowned() const { return unsigned(m_unowned.size()); }

	// The word's shape for this session. Both peers derive it from the
	// driver and the handshake digest proves they agree, so nothing on the
	// wire carries it. Meaningless until build() has returned true.
	const word_layout &layout() const { return m_layout; }

	// digital bits LOCAL_SLOT owns; the self-check compares only these
	// because capture_port reads that slot's fields and no other
	u32 local_bit_mask() const;

	// Called per port from inside ioport_manager::frame_update's loop.
	// CAPTURE BEFORE INJECT in the same iteration, or the capture reads back
	// the remote values just written.
	void capture_port(ioport_port &port, input_word &word) const;
	void inject_port(ioport_port &port, const input_word *words, player_t count) const;

	std::string describe() const;

	// DIAGNOSTIC one-shot: builds the table and writes describe() to the
	// path in NETPLAY_INPUTMAP. Env-driven rather than an emu_option so
	// this module stays a one-TU rebuild - emuopts.h reaches 179 TUs.
	// The call site moves to netplay_manager when that lands. [open]
	static void dump_once(running_machine &machine);

private:
	// a digital field one player owns
	struct owned
	{
		ioport_value    mask;
		player_t        player;         // 1-based
		u8              bit;            // digital bit in the word
	};

	// One analog axis. Located in the word by its owner's slot and its
	// position in that owner's canonical axis order - POSITIONAL rather
	// than keyed by ioport_type, because a player can hold two fields of
	// the same type under opposing PORT_CONDITIONs (coco3's two lightgun
	// X axes) and a type key would collide. The walk order is the port
	// tag order of an ordered map, so both peers assign the same indices.
	struct axis
	{
		analog_field *  an;         // ioport_field::live() hands this over
		                            // non-const from a const field, and it
		                            // lives as long as the machine does
		ioport_value    mask;
		u8              shift;      // trailing zeros of mask
		u8              bits;       // set bits of mask: the wire width
		player_t        player;     // 1-based
		u8              index;      // 0-based, within this player
	};

	struct port_entry
	{
		ioport_port *       port;
		std::string         tag;
		std::vector<owned>  fields;
		std::vector<axis>   axes;
		ioport_value        shared_mask;    // non-toggle digital, no player
	};

	const port_entry *find(const ioport_port &port) const;

	std::vector<port_entry>                         m_ports;
	std::unordered_map<const ioport_port *, u32>    m_index;
	std::vector<u32>                                m_logical;  // bit -> logical id
	std::vector<std::string>                        m_unowned;
	word_layout                                     m_layout;
	u64                                             m_digest;
	u64                                             m_config_digest;
	player_t                                        m_players;
	unsigned                                        m_analog_seen;
	bool                                            m_fits;
	bool                                            m_built;
};

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_INPUTMAP_H
