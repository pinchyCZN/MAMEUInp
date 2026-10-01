// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_types.h

    The input word and the types that travel with it.

    The word is the hot path: one per player per frame, and the only
    thing sent 60 times a second. Its layout and width are fixed and
    measured - notes/protocol.txt 1.6.1.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_TYPES_H
#define MAME_EMU_NETPLAY_NETPLAY_TYPES_H

#pragma once

// MAME supplies u8..u64 through emu.h, which every file under src/emu
// includes first. The relay server in server/ compiles this same header
// and netplay_packet.cpp WITHOUT MAME, so the types are defined here when
// NETPLAY_STANDALONE is set. One definition of the wire format shared by
// two programs - a second copy is how two builds come to disagree.
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


// ======================> input_word
//
// WIRE FORMAT. 64 bits, little-endian on the wire (notes/protocol.txt 3.1):
//
//     bits  0-31   digital, one bit per logical input, assigned by
//                  netplay_inputmap
//     bits 32-39   analog axis 0
//     bits 40-47   analog axis 1
//     bits 48-55   analog axis 2
//     bits 56-63   analog axis 3
//
// 32 digital bits covers mahjong's 30, the widest measured. Four axes
// covers the measured maximum of three (daytona: pedal, brake, steer).
// Every analog field measured is 8 bits wide or narrower, and that is the
// GAME-VISIBLE width - the field mask decides how many bits reach the
// driver through ioport_port::read(). notes/protocol.txt 1.6.1
//
// This is a measured bound, not a structural one: a driver using all of
// both the MAHJONG and GAMBLING type blocks would need 57 digital bits.
// netplay_inputmap::fits() is what makes that a refused session rather
// than silently dropped high bits.

typedef u64 input_word;

constexpr unsigned DIGITAL_BITS = 32;
constexpr unsigned ANALOG_AXES  = 4;
constexpr unsigned ANALOG_BITS  = 8;

constexpr unsigned ANALOG_SHIFT = DIGITAL_BITS;

constexpr input_word DIGITAL_FIELD_MASK = 0x00000000FFFFFFFFULL;

inline u32 word_digital(input_word w)
{
	return u32(w & DIGITAL_FIELD_MASK);
}

inline u8 word_analog(input_word w, unsigned axis)
{
	return u8((w >> (ANALOG_SHIFT + (axis * ANALOG_BITS))) & 0xff);
}

inline void word_set_digital(input_word &w, unsigned bit, bool down)
{
	const input_word m = input_word(1) << bit;
	if (down)
		w |= m;
	else
		w &= ~m;
}

inline void word_set_analog(input_word &w, unsigned axis, u8 value)
{
	const unsigned shift = ANALOG_SHIFT + (axis * ANALOG_BITS);
	w &= ~(input_word(0xff) << shift);
	w |= input_word(value) << shift;
}

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_TYPES_H
