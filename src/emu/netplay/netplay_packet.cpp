// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_packet.cpp

    See netplay_packet.h.

***************************************************************************/

#include "emu.h"
#include "netplay/netplay_packet.h"


namespace netplay {

//-------------------------------------------------
//  build_input
//-------------------------------------------------

size_t build_input(u8 *buf, size_t cap, const packet_header &head,
		const input_word *words, const u64 *hash)
{
	const bool with_hash = (hash != nullptr);
	const size_t need = input_bytes(head.nframes, head.player_mask, with_hash);

	if ((buf == nullptr) || (need > cap) || (need > MAX_DATAGRAM))
		return 0;
	if ((head.nframes == 0) || (head.player_mask == 0))
		return 0;

	put_u8(buf + 0, head.type);
	put_u8(buf + 1, PROTOCOL_VERSION);
	put_u16(buf + 2, head.session);
	put_u32(buf + 4, head.base_frame);
	put_u8(buf + 8, head.nframes);
	put_u8(buf + 9, head.player_mask);
	put_u8(buf + 10, u8(with_hash ? (head.flags | FLAG_HASH_TRAILER)
			: (head.flags & ~FLAG_HASH_TRAILER)));
	put_u8(buf + 11, 0);

	u8 *p = buf + HEADER_BYTES;
	const unsigned n = head.nframes * popcount8(head.player_mask);
	for (unsigned i = 0; i < n; i++)
	{
		put_u64(p, words[i]);
		p += sizeof(input_word);
	}

	if (with_hash)
		put_u64(p, *hash);

	return need;
}


//-------------------------------------------------
//  parse_header
//-------------------------------------------------

bool parse_header(const u8 *buf, size_t len, packet_header &head)
{
	if ((buf == nullptr) || (len < HEADER_BYTES) || (len > MAX_DATAGRAM))
		return false;

	head.type = get_u8(buf + 0);
	head.version = get_u8(buf + 1);
	head.session = get_u16(buf + 2);
	head.base_frame = get_u32(buf + 4);
	head.nframes = get_u8(buf + 8);
	head.player_mask = get_u8(buf + 9);
	head.flags = get_u8(buf + 10);
	head.reserved = get_u8(buf + 11);

	// A zero type is always malformed - see the numbering note in the header.
	if ((head.type == 0) || (head.type > MSG_BYE))
		return false;
	if (head.version != PROTOCOL_VERSION)
		return false;

	// reserved is NOT checked: a field receivers validate cannot later be
	// used without a version bump; one they ignore can. protocol.txt 3.2
	return true;
}


//-------------------------------------------------
//  parse_input
//-------------------------------------------------

bool parse_input(const u8 *buf, size_t len, packet_header &head,
		input_word *words, unsigned max_words, u64 &hash, bool &has_hash)
{
	hash = 0;
	has_hash = false;

	if (!parse_header(buf, len, head))
		return false;
	if (head.type != MSG_INPUT)
		return false;
	if ((head.nframes == 0) || (head.player_mask == 0))
		return false;

	has_hash = ((head.flags & FLAG_HASH_TRAILER) != 0);

	// The declared shape must match the datagram exactly. A short packet
	// would read past the end and a long one means the sender and receiver
	// disagree about the format, which is not something to paper over.
	if (len != input_bytes(head.nframes, head.player_mask, has_hash))
		return false;

	const unsigned n = head.nframes * popcount8(head.player_mask);
	if (n > max_words)
		return false;

	const u8 *p = buf + HEADER_BYTES;
	for (unsigned i = 0; i < n; i++)
	{
		words[i] = get_u64(p);
		p += sizeof(input_word);
	}

	if (has_hash)
		hash = get_u64(p);

	return true;
}

} // namespace netplay
