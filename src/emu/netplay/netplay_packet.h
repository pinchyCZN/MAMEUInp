// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_packet.h

    The wire format. notes/protocol.txt 3.

    Pure serialisation - no I/O, no state, no machine. Everything here is
    a function of its arguments so the format can be tested without a
    socket or a running driver.

    WIRE-FORMAT INVARIANTS. Changing any of these without bumping
    PROTOCOL_VERSION makes two builds silently disagree:

      o little-endian, every field (protocol.txt 3.1)
      o the 12-byte header layout (3.2)
      o message type numbering (3.3)
      o flag bit assignments (3.4)
      o words ordered frame-major then by ascending player slot (3.5)

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_PACKET_H
#define MAME_EMU_NETPLAY_NETPLAY_PACKET_H

#pragma once

#include "netplay/netplay_types.h"


namespace netplay {

constexpr u8 PROTOCOL_VERSION = 1;

// 3.2
constexpr unsigned HEADER_BYTES = 12;

// 12 + 6 frames x 10 players x 8 bytes x 2 (redundancy) + 8 hash = 968.
// Rounded up and still far inside any MTU, so the format never fragments.
constexpr unsigned MAX_DATAGRAM = 1200;

// 3.3. Numbering starts at 1 so a zero first byte is always malformed.
enum msg_type : u8
{
	MSG_INPUT   = 0x01,
	MSG_JOIN    = 0x02,
	MSG_WELCOME = 0x03,
	MSG_REFUSE  = 0x04,
	MSG_START   = 0x05,
	MSG_HASH    = 0x06,
	MSG_DESYNC  = 0x07,
	MSG_RESYNC  = 0x08,
	MSG_STALL   = 0x09,
	MSG_CHAT    = 0x0a,
	MSG_BYE     = 0x0b
};

// 3.4
enum msg_flag : u8
{
	FLAG_HASH_TRAILER = 0x01,
	FLAG_REDUNDANT    = 0x02,
	FLAG_FINAL        = 0x04
};


// ======================> packet_header

struct packet_header
{
	u8      type;
	u8      version;
	u16     session;
	frame_t base_frame;
	u8      nframes;
	u8      player_mask;
	u8      flags;
	u8      reserved;
};


// ---- little-endian primitives ----
//
// Byte-wise rather than a struct cast. The cast would work on x86 and would
// also make the wire format depend on the compiler's padding, which is not a
// thing to leave to chance in a format two builds have to agree on.

inline void put_u8(u8 *p, u8 v) { p[0] = v; }

inline void put_u16(u8 *p, u16 v)
{
	p[0] = u8(v & 0xff);
	p[1] = u8((v >> 8) & 0xff);
}

inline void put_u32(u8 *p, u32 v)
{
	p[0] = u8(v & 0xff);
	p[1] = u8((v >> 8) & 0xff);
	p[2] = u8((v >> 16) & 0xff);
	p[3] = u8((v >> 24) & 0xff);
}

inline void put_u64(u8 *p, u64 v)
{
	put_u32(p, u32(v & 0xffffffffULL));
	put_u32(p + 4, u32((v >> 32) & 0xffffffffULL));
}

inline u8 get_u8(const u8 *p) { return p[0]; }

inline u16 get_u16(const u8 *p)
{
	return u16(u16(p[0]) | (u16(p[1]) << 8));
}

inline u32 get_u32(const u8 *p)
{
	return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

inline u64 get_u64(const u8 *p)
{
	return u64(get_u32(p)) | (u64(get_u32(p + 4)) << 32);
}

inline unsigned popcount8(u8 v)
{
	unsigned n = 0;
	for ( ; v != 0; v >>= 1)
		n += (v & 1);
	return n;
}


// ---- build and parse ----

// Returns bytes written, or 0 if the arguments do not fit. `words` is
// frame-major: nframes blocks, each holding one word per set bit of
// player_mask in ascending slot order.
size_t build_input(u8 *buf, size_t cap, const packet_header &head,
		const input_word *words, const u64 *hash);

// Validates length, version and type against the declared shape. Returns
// false rather than reading past the end on anything malformed.
bool parse_header(const u8 *buf, size_t len, packet_header &head);

// Fills `words` with nframes x popcount(player_mask) entries. `max_words` is
// the capacity of that array.
bool parse_input(const u8 *buf, size_t len, packet_header &head,
		input_word *words, unsigned max_words, u64 &hash, bool &has_hash);

// Bytes an INPUT packet of this shape occupies.
inline size_t input_bytes(unsigned nframes, u8 player_mask, bool hash)
{
	return HEADER_BYTES
			+ (size_t(nframes) * popcount8(player_mask) * sizeof(input_word))
			+ (hash ? sizeof(u64) : 0);
}

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_PACKET_H
