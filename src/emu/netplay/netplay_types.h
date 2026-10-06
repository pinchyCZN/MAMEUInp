// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_types.h

    The input word and the types that travel with it.

    The word is the hot path: one per player per frame, and the only
    thing sent 60 times a second. Its layout is DRIVER-DERIVED and fixed
    for the length of a session - notes/protocol.txt 1.6.1.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_TYPES_H
#define MAME_EMU_NETPLAY_NETPLAY_TYPES_H

#pragma once

// MAME supplies u8..u64 through emu.h, which every file under src/emu
// includes first. The relay server in server/ compiles this same header
// and netplay_packet.cpp WITHOUT MAME, so the types are defined here when
// NETPLAY_STANDALONE is set. One definition of the wire format shared by
// two programs - a second copy is how two builds come to disagree.
// word_clear and friends below are inline and use memset/memcmp, so this
// is needed in both builds rather than only the standalone one.
#include <cstring>

#ifdef NETPLAY_STANDALONE
#include <cstddef>
#include <cstdint>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
#endif


namespace netplay {

// 32 bits, not 16 or 24: at 60 Hz a 16-bit counter wraps every 18 minutes
// and a 24-bit one every 77 hours, and a wrap bug surfaces in a session and
// never in a test. notes/protocol.txt 1.5
typedef u32 frame_t;

typedef u8 player_t;

// MAME declares IPT_START1..START10 and IPT_COIN1..COIN12, so ten is the
// most players a driver can attribute start buttons to. [verified ioport.h]
constexpr player_t MAX_PLAYERS = 10;

// Eight bits per player, one bit per player, so both fit a u16 mask.
constexpr unsigned PLAYER_MASK_BITS = 16;

// WIRE-FORMAT INVARIANT: player N is bit N-1 of a player mask. Players
// are 1-based because that is how MAME numbers them, so there is no bit
// 0 player and a zero mask names nobody.
inline u8 slot_bit(player_t p)
{
	return u8(1u << (p - 1));
}


// ======================> the word
//
// WIRE FORMAT, little-endian throughout (notes/protocol.txt 3.1). The word
// is a run of byte-aligned fields whose number and width come from the
// driver:
//
//     offset 0                digital bitmap, ceil(D/8) bytes.
//                             Logical input i is bit (i % 8) of byte (i / 8).
//     then, per axis k        1 byte if that axis's mask is 8 bits or
//                             fewer, otherwise 2 bytes little-endian.
//
// D is the count of distinct logical digital inputs one player can own and
// the axis list is that player's analog fields, both in the canonical order
// netplay_inputmap assigns. NOTHING ON THE WIRE CARRIES THE LAYOUT: both
// peers derive it from the driver, and the handshake digest proves they
// agree before frame 0 (protocol.txt 4.2). A disagreement is a refused
// session, and if one ever slipped past the digest it would show up as a
// length mismatch in parse_input rather than as misread input.
//
// BYTE-ALIGNED FIELDS RATHER THAN A PACKED BIT RUN, and the reason is that
// packing buys almost nothing: measured over 928 analog drivers the worst
// case loses three bytes to alignment and the median loses none
// [verified, tools/netplay_analog.py]. What alignment buys is a word you
// can read in a hex dump and an axis accessor that cannot straddle a byte.
//
// THE AXIS VALUE IS THE POST-MASK, GAME-VISIBLE VALUE - what
// analog_field::read() inserts into the port, which is
// apply_settings(m_accum) & (mask >> shift). NOT m_accum. m_accum is an
// accumulator in device space whose width depends on m_sensitivity: the
// lowest default sensitivity in the driver set is 1, which puts it at
// +/- 6553600, and for a wrapping axis it has no bound at all because
// frame_update skips apply_min_max when m_wraps. Sending the post-mask
// value instead is narrower, is bounded by construction, and needs no
// peer to agree about sensitivity, reverse or scaling.
// notes/protocol.txt 1.6.1

// Enforced ceiling on digital inputs one player can own. Measured maximum
// is mahjong's 30. inputmap::fits() refuses above it rather than dropping
// high bits. notes/protocol.txt 1.6.1
constexpr unsigned DIGITAL_BITS = 32;

// Analog axes one player can own. Measured maximum is 7 (harddriv,
// racedriv). [verified, tools/netplay_analog.py]
constexpr unsigned MAX_AXES = 12;

// Measured maximum game-visible axis width, from field.mask popcount over
// 2675 axes. [verified, tools/netplay_analog.py]
constexpr unsigned AXIS_MAX_BITS = 16;

// The in-memory word is fixed at the worst case our own limits allow -
// 4 bytes of digital plus MAX_AXES two-byte axes - so the ring and every
// array of words can be a plain array. ONLY layout.bytes of it is ever
// written to the wire.
constexpr unsigned WORD_MAX_BYTES = 4 + (MAX_AXES * 2);

struct input_word
{
	u8  b[WORD_MAX_BYTES];
};


// Where each field sits. Built once at session start by
// netplay_inputmap::build and then read-only.
struct word_layout
{
	u8  digital_bytes;
	u8  axes;
	u8  axis_off[MAX_AXES];     // byte offset within the word
	u8  axis_bytes[MAX_AXES];   // 1 or 2
	u8  bytes;                  // the whole word, what goes on the wire
};


inline void word_clear(input_word &w)
{
	std::memset(w.b, 0, sizeof(w.b));
}


// Fills layout from a digital bit count and a list of axis widths in bits.
// False if the driver does not fit, which is a refused session rather than
// a truncated word.
inline bool layout_build(word_layout &layout, unsigned digital_bits,
		const u8 *axis_bits, unsigned axes)
{
	std::memset(&layout, 0, sizeof(layout));

	if ((digital_bits > DIGITAL_BITS) || (axes > MAX_AXES))
		return false;

	layout.digital_bytes = u8((digital_bits + 7) / 8);
	layout.axes = u8(axes);

	unsigned at = layout.digital_bytes;
	for (unsigned k = 0; k < axes; k++)
	{
		if ((axis_bits[k] == 0) || (axis_bits[k] > AXIS_MAX_BITS))
			return false;

		layout.axis_off[k] = u8(at);
		layout.axis_bytes[k] = u8((axis_bits[k] <= 8) ? 1 : 2);
		at += layout.axis_bytes[k];
	}

	// A driver with no inputs at all would give a zero-length word, which
	// build_input would then emit as a payload of nothing.
	if ((at == 0) || (at > WORD_MAX_BYTES))
		return false;

	layout.bytes = u8(at);
	return true;
}


// Hex, most significant byte first, so a log line reads the way the
// layout comment above does. Needs 2 * layout.bytes + 1.
inline void word_hex(const input_word &w, const word_layout &layout,
		char *out, unsigned cap)
{
	static const char D[] = "0123456789abcdef";

	if ((out == nullptr) || (cap == 0))
		return;

	unsigned at = 0;
	for (unsigned i = layout.bytes; (i > 0) && ((at + 3) <= cap); i--)
	{
		out[at++] = D[(w.b[i - 1] >> 4) & 0x0f];
		out[at++] = D[w.b[i - 1] & 0x0f];
	}
	out[at] = 0;
}


// Twice WORD_MAX_BYTES plus a terminator, for a word_hex buffer.
constexpr unsigned WORD_HEX_CHARS = (WORD_MAX_BYTES * 2) + 1;


inline u32 word_digital(const input_word &w, const word_layout &layout)
{
	u32 v = 0;
	for (unsigned i = 0; i < layout.digital_bytes; i++)
		v |= u32(w.b[i]) << (i * 8);
	return v;
}


inline void word_set_digital(input_word &w, unsigned bit, bool down)
{
	const unsigned byte = bit / 8;
	const u8 m = u8(1u << (bit % 8));

	if (down)
		w.b[byte] = u8(w.b[byte] | m);
	else
		w.b[byte] = u8(w.b[byte] & ~m);
}


inline u32 word_analog(const input_word &w, const word_layout &layout,
		unsigned axis)
{
	const unsigned off = layout.axis_off[axis];
	if (layout.axis_bytes[axis] == 1)
		return u32(w.b[off]);
	return u32(w.b[off]) | (u32(w.b[off + 1]) << 8);
}


inline void word_set_analog(input_word &w, const word_layout &layout,
		unsigned axis, u32 value)
{
	const unsigned off = layout.axis_off[axis];
	w.b[off] = u8(value & 0xff);
	if (layout.axis_bytes[axis] == 2)
		w.b[off + 1] = u8((value >> 8) & 0xff);
}

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_TYPES_H
