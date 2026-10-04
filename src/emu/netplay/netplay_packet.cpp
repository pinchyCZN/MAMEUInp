// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_packet.cpp

    See netplay_packet.h.

***************************************************************************/

#ifndef NETPLAY_STANDALONE
#include "emu.h"
#endif
#include "netplay/netplay_packet.h"

#include <cstring>


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
	if ((head.type == 0) || (head.type > MSG_RESYNC_REQ))
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

//-------------------------------------------------
//  control packets
//
//  nframes and player_mask are left 0, which is what distinguishes a
//  control packet from an input one and is why build_input rejects those
//  values.
//-------------------------------------------------

namespace {

size_t control_head(u8 *buf, size_t cap, u8 type, u16 session,
		frame_t base_frame, size_t payload)
{
	if ((buf == nullptr) || ((HEADER_BYTES + payload) > cap))
		return 0;
	put_u8(buf + 0, type);
	put_u8(buf + 1, PROTOCOL_VERSION);
	put_u16(buf + 2, session);
	put_u32(buf + 4, base_frame);
	put_u8(buf + 8, 0);
	put_u8(buf + 9, 0);
	put_u8(buf + 10, 0);
	put_u8(buf + 11, 0);
	return HEADER_BYTES + payload;
}

} // anonymous namespace


size_t build_join(u8 *buf, size_t cap, const join_payload &p)
{
	const size_t len = control_head(buf, cap, MSG_JOIN, 0, 0, 32);
	if (len == 0)
		return 0;

	u8 *const q = buf + HEADER_BYTES;
	put_u64(q + 0, p.inputmap);
	put_u64(q + 8, p.layout);
	put_u8(q + 16, p.want_players);
	put_u8(q + 17, 0);
	for (unsigned i = 0; i < sizeof(p.driver); i++)
		q[18 + i] = u8(p.driver[i]);
	return len;
}


bool parse_join(const u8 *buf, size_t len, join_payload &p)
{
	packet_header head;
	if (!parse_header(buf, len, head) || (head.type != MSG_JOIN))
		return false;
	if (len != JOIN_BYTES)
		return false;

	const u8 *const q = buf + HEADER_BYTES;
	p.inputmap = get_u64(q + 0);
	p.layout = get_u64(q + 8);
	p.want_players = get_u8(q + 16);
	for (unsigned i = 0; i < sizeof(p.driver); i++)
		p.driver[i] = char(q[18 + i]);
	return true;
}


size_t build_welcome(u8 *buf, size_t cap, u16 session, const welcome_payload &p)
{
	const size_t len = control_head(buf, cap, MSG_WELCOME, session, 0, 8);
	if (len == 0)
		return 0;

	u8 *const q = buf + HEADER_BYTES;
	put_u8(q + 0, p.slot);
	put_u8(q + 1, p.players);
	put_u16(q + 2, 0);
	put_u32(q + 4, p.start_frame);
	return len;
}


bool parse_welcome(const u8 *buf, size_t len, u16 &session, welcome_payload &p)
{
	packet_header head;
	if (!parse_header(buf, len, head) || (head.type != MSG_WELCOME))
		return false;
	if (len != WELCOME_BYTES)
		return false;

	session = head.session;
	const u8 *const q = buf + HEADER_BYTES;
	p.slot = get_u8(q + 0);
	p.players = get_u8(q + 1);
	p.start_frame = get_u32(q + 4);
	return true;
}


size_t build_refuse(u8 *buf, size_t cap, u8 reason)
{
	const size_t len = control_head(buf, cap, MSG_REFUSE, 0, 0, 4);
	if (len == 0)
		return 0;
	put_u8(buf + HEADER_BYTES + 0, reason);
	put_u8(buf + HEADER_BYTES + 1, 0);
	put_u16(buf + HEADER_BYTES + 2, 0);
	return len;
}


size_t build_start(u8 *buf, size_t cap, u16 session, frame_t start_frame)
{
	const size_t len = control_head(buf, cap, MSG_START, session, start_frame, 4);
	if (len == 0)
		return 0;
	put_u32(buf + HEADER_BYTES, start_frame);
	return len;
}


bool parse_start(const u8 *buf, size_t len, frame_t &start_frame)
{
	packet_header head;
	if (!parse_header(buf, len, head) || (head.type != MSG_START))
		return false;
	if (len != START_BYTES)
		return false;
	start_frame = get_u32(buf + HEADER_BYTES);
	return true;
}


const char *refuse_text(u8 reason)
{
	switch (reason)
	{
	case REFUSE_VERSION:  return "protocol version mismatch";
	case REFUSE_BUILD:    return "different binary";
	case REFUSE_DRIVER:   return "different system or ROM set";
	case REFUSE_BIOS:     return "different -bios";
	case REFUSE_DIPSW:    return "DIP or config settings differ";
	case REFUSE_INPUTCFG: return "translation tables differ";
	case REFUSE_NVRAM:    return "NVRAM or EEPROM differs";
	case REFUSE_LAYOUT:   return "savestate layout differs";
	case REFUSE_PERF:     return "frameskip, cheat or drc differs";
	case REFUSE_FULL:     return "no free player slot";
	case REFUSE_RUNNING:  return "session already started";
	case REFUSE_TOOWIDE:  return "driver needs more than 64 bits per player";
	default:              return "unknown reason";
	}
}


//-------------------------------------------------
//  UI commands
//-------------------------------------------------

size_t build_uicmd(u8 *buf, size_t cap, u16 session, frame_t effective, u8 cmd)
{
	const size_t len = control_head(buf, cap, MSG_UICMD, session, effective, 4);
	if (len == 0)
		return 0;
	put_u8(buf + HEADER_BYTES + 0, cmd);
	put_u8(buf + HEADER_BYTES + 1, 0);
	put_u16(buf + HEADER_BYTES + 2, 0);
	return len;
}


bool parse_uicmd(const u8 *buf, size_t len, frame_t &effective, u8 &cmd)
{
	packet_header head;
	if (!parse_header(buf, len, head) || (head.type != MSG_UICMD))
		return false;
	if (len != UICMD_BYTES)
		return false;
	effective = head.base_frame;
	cmd = get_u8(buf + HEADER_BYTES);
	return true;
}


const char *uicmd_text(u8 cmd)
{
	switch (cmd)
	{
	case UICMD_PAUSE_TOGGLE: return "pause toggle";
	case UICMD_SOFT_RESET:   return "soft reset";
	default:                 return "unknown";
	}
}


//-------------------------------------------------
//  resync
//-------------------------------------------------

size_t build_resync_req(u8 *buf, size_t cap, u16 session,
		const resync_req_payload &p)
{
	if (p.bitmap_bytes > sizeof(p.bitmap))
		return 0;

	const size_t len = control_head(buf, cap, MSG_RESYNC_REQ, session, 0,
			RESYNC_REQ_FIXED + p.bitmap_bytes);
	if (len == 0)
		return 0;

	u8 *const q = buf + HEADER_BYTES;
	put_u32(q + 0, p.id);
	put_u16(q + 4, p.want);
	put_u16(q + 6, p.bitmap_bytes);
	for (unsigned i = 0; i < p.bitmap_bytes; i++)
		q[8 + i] = p.bitmap[i];
	return len;
}


bool parse_resync_req(const u8 *buf, size_t len, resync_req_payload &p)
{
	packet_header head;
	if (!parse_header(buf, len, head) || (head.type != MSG_RESYNC_REQ))
		return false;
	if (len < (HEADER_BYTES + RESYNC_REQ_FIXED))
		return false;

	const u8 *const q = buf + HEADER_BYTES;
	p.id = get_u32(q + 0);
	p.want = get_u16(q + 4);
	p.bitmap_bytes = get_u16(q + 6);

	// The declared bitmap length has to match the datagram exactly, or a
	// short packet would be read past its end.
	if (p.bitmap_bytes > sizeof(p.bitmap))
		return false;
	if (len != (HEADER_BYTES + RESYNC_REQ_FIXED + p.bitmap_bytes))
		return false;

	std::memset(p.bitmap, 0, sizeof(p.bitmap));
	for (unsigned i = 0; i < p.bitmap_bytes; i++)
		p.bitmap[i] = q[8 + i];
	return true;
}


size_t build_resync(u8 *buf, size_t cap, u16 session, frame_t state_frame,
		const resync_payload &p, const u8 *chunk, size_t chunk_len)
{
	if ((chunk == nullptr) || (chunk_len == 0))
		return 0;
	if ((RESYNC_HEAD_BYTES + chunk_len) > cap)
		return 0;
	if ((RESYNC_HEAD_BYTES + chunk_len) > MAX_DATAGRAM)
		return 0;

	const size_t len = control_head(buf, cap, MSG_RESYNC, session, state_frame,
			12 + chunk_len);
	if (len == 0)
		return 0;

	u8 *const q = buf + HEADER_BYTES;
	put_u32(q + 0, p.id);
	put_u32(q + 4, p.total);
	put_u16(q + 8, p.chunks);
	put_u16(q + 10, p.index);
	for (size_t i = 0; i < chunk_len; i++)
		q[12 + i] = chunk[i];
	return len;
}


bool parse_resync(const u8 *buf, size_t len, frame_t &state_frame,
		resync_payload &p, const u8 *&chunk, size_t &chunk_len)
{
	chunk = nullptr;
	chunk_len = 0;

	packet_header head;
	if (!parse_header(buf, len, head) || (head.type != MSG_RESYNC))
		return false;
	if (len <= RESYNC_HEAD_BYTES)
		return false;

	state_frame = head.base_frame;
	const u8 *const q = buf + HEADER_BYTES;
	p.id = get_u32(q + 0);
	p.total = get_u32(q + 4);
	p.chunks = get_u16(q + 8);
	p.index = get_u16(q + 10);

	chunk = q + 12;
	chunk_len = len - RESYNC_HEAD_BYTES;
	return true;
}

} // namespace netplay
