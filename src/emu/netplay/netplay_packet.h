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
	MSG_BYE     = 0x0b,
	MSG_UICMD   = 0x0c,
	MSG_RESYNC_REQ = 0x0d,
	MSG_NAK     = 0x0e
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

// ---- control packets ----
//
// Everything that is not INPUT. nframes and player_mask are 0, so these
// cannot be confused with an input packet, and the payload layout is per
// type. notes/protocol.txt 3.3, 4.1

// notes/protocol.txt 4.2 lists nine digests the handshake should carry.
// Two are implemented: the translation table, because peers that route
// (player, logical) differently misroute each other's input while agreeing
// on everything else, and the save-state layout, because it catches a
// different binary, driver, device configuration or MAME version in one
// comparison. The other seven - bios, dipsw, nvram, perf - are [open].
struct join_payload
{
	u64     inputmap;
	u64     layout;
	u8      want_players;
	char    driver[14];        // NUL-padded, not NUL-terminated
};

struct welcome_payload
{
	u8      slot;              // 1-based
	u8      players;
	frame_t start_frame;
};

constexpr size_t JOIN_BYTES = HEADER_BYTES + 32;
constexpr size_t WELCOME_BYTES = HEADER_BYTES + 8;
constexpr size_t REFUSE_BYTES = HEADER_BYTES + 4;
constexpr size_t START_BYTES = HEADER_BYTES + 4;

size_t build_join(u8 *buf, size_t cap, const join_payload &p);
bool parse_join(const u8 *buf, size_t len, join_payload &p);

size_t build_welcome(u8 *buf, size_t cap, u16 session,
		const welcome_payload &p);
bool parse_welcome(const u8 *buf, size_t len, u16 &session,
		welcome_payload &p);

size_t build_refuse(u8 *buf, size_t cap, u8 reason);
size_t build_start(u8 *buf, size_t cap, u16 session, frame_t start_frame);
bool parse_start(const u8 *buf, size_t len, frame_t &start_frame);

// 4.4. Naming the field that mismatched is the point: a generic refusal
// makes the user compare an entire configuration by hand.
enum refuse_reason : u8
{
	REFUSE_VERSION  = 0x01,
	REFUSE_BUILD    = 0x02,
	REFUSE_DRIVER   = 0x03,
	REFUSE_BIOS     = 0x04,
	REFUSE_DIPSW    = 0x05,
	REFUSE_INPUTCFG = 0x06,
	REFUSE_NVRAM    = 0x07,
	REFUSE_LAYOUT   = 0x08,
	REFUSE_PERF     = 0x09,
	REFUSE_FULL     = 0x0a,
	REFUSE_RUNNING  = 0x0b,
	REFUSE_TOOWIDE  = 0x0c
};

const char *refuse_text(u8 reason);

// ---- UI commands ----
//
// Pause and soft reset, FRAME STAMPED. The stamp is the point: a bare
// 'pause' would take effect on each peer whenever its packet landed, and
// the peers would then be running different frames. base_frame carries the
// frame the command takes effect before, so every peer acts at the same
// place and a soft reset is as deterministic as any other input.
//
// NOT spare bits in the input word. notes/protocol.txt 1.5 retired that
// Kaillera trick because it couples the UI protocol to each driver's port
// layout to save a byte.
enum ui_command : u8
{
	UICMD_PAUSE_TOGGLE = 0x01,
	UICMD_SOFT_RESET   = 0x02
};

constexpr size_t UICMD_BYTES = HEADER_BYTES + 4;

size_t build_uicmd(u8 *buf, size_t cap, u16 session, frame_t effective,
		u8 cmd);
bool parse_uicmd(const u8 *buf, size_t len, frame_t &effective, u8 &cmd);

// CHAT. The sender travels in the header's player_mask, so there is no
// sender field. base_frame is informational: chat changes no emulated
// state, so unlike UICMD it is not frame-gated and a lost line is lost.
//
// parse_chat GUARANTEES out is NUL-terminated and holds only printable
// ASCII. It is the one message whose content comes from a person, so it
// is the one that must not be trusted: a control character here would
// reach a text renderer, and a missing terminator would read past the
// buffer. Sanitising on receive rather than trusting the sender is the
// whole job. out must have room for CHAT_MAX_TEXT + 1.
size_t build_chat(u8 *buf, size_t cap, u16 session, frame_t frame,
		player_t from, const char *text);
bool parse_chat(const u8 *buf, size_t len, player_t &from, char *out);

// STALL - a retransmission request, and the ONLY recovery for input
// that redundancy depth did not cover (notes/protocol.txt 1.7.4).
//
//     base_frame   the first frame the sender lacks
//     player_mask  the SENDER, as in CHAT
//     want         players whose words are missing, one bit each
//     count        how many consecutive frames from base_frame
//
// ONLY THE RECEIVER KNOWS WHAT IS MISSING. A loss happens on the path
// and the sender sees nothing go wrong, so this cannot be replaced by a
// peer resending its recent window unprompted - measured, the stalled
// peer was ten frames BEHIND the other and needed a frame that was no
// longer in the other's recent window at all.
size_t build_stall(u8 *buf, size_t cap, u16 session, frame_t frame,
		player_t from, u8 want, u8 count);
bool parse_stall(const u8 *buf, size_t len, frame_t &frame, player_t &from,
		u8 &want, u8 &count);

// NAK - a peer answering "no" to a request it cannot serve.
//
//     base_frame   the frame the refusal was decided on, informational
//     player_mask  the SENDER, as in CHAT and STALL
//     about        which request is refused, NAK_ABOUT_*
//     reason       why, NAK_* - only so the message on screen is true
//
// WHY A TYPE AND NOT A ZERO-LENGTH RESYNC. A resync carrying nothing
// would have to mean "no" in three places that each currently read it
// as corruption: parse_resync rejects a header-only datagram,
// state_blob::begin_receive treats total or chunks of 0 as invalid, and
// take_resync_chunk arms the guest's own want flag on any unsolicited
// chunk - so a refusal would switch the puller ON and leave it chasing
// a transfer that will never arrive. One branch in poll_link is less
// code and cannot be misread.
//
// NO STATE IS KEPT ACROSS REQUESTS. Every request gets its own answer
// and its own message; a peer that asks ten times is told ten times.
size_t build_nak(u8 *buf, size_t cap, u16 session, frame_t frame,
		player_t from, u8 about, u8 reason);
bool parse_nak(const u8 *buf, size_t len, player_t &from, u8 &about,
		u8 &reason);

// what a NAK is about
enum nak_about : u8
{
	NAK_ABOUT_RESYNC = 0x01
};

// why. Reported to the player, so each one has to be true of the case
// it names - "not supported" for a state that was merely too big would
// send somebody hunting the wrong problem.
enum nak_reason : u8
{
	NAK_DISABLED  = 0x01,      // savestates off for this session
	NAK_TOO_LARGE = 0x02,      // over the resync size limit
	NAK_FAILED    = 0x03       // the capture itself failed
};

const char *nak_text(u8 reason);
const char *uicmd_text(u8 cmd);

// ---- resync ----
//
// RESYNC_REQ doubles as the retransmission request: the bitmap says which
// chunks are still wanted, so one message covers both 'send me a state'
// and 'I am missing these'. id is 0 on a first ask and the host's assigned
// id thereafter, which stops a late chunk from an abandoned attempt being
// mistaken for part of the current one.
struct resync_req_payload
{
	u32     id;                 // 0 = start a new transfer
	u16     want;               // how many chunks the bitmap asks for
	u16     bitmap_bytes;       // length actually carried
	u8      bitmap[1024];       // RESYNC_BITMAP_MAX
};

// A RESYNC packet's header base_frame is THE FRAME THE STATE IS AS OF,
// which is what the receiver rewinds its frame counter to.
struct resync_payload
{
	u32     id;
	u32     total;              // bytes in the whole blob
	u16     chunks;             // chunks in the whole blob
	u16     index;              // which chunk this is
};

// Variable length: 8 fixed bytes plus however many bitmap bytes the
// chunk count needs. A 13-chunk transfer carries 2, not 1024.
constexpr size_t RESYNC_REQ_FIXED = 8;

// Chat text, bytes. Nothing needs it this short - the datagram has room
// for nine times as much - but a cap has to exist somewhere and a line
// nobody can read is not worth carrying.
constexpr size_t CHAT_MAX_TEXT = 128;

constexpr size_t STALL_BYTES = HEADER_BYTES + 4;
constexpr size_t NAK_BYTES = HEADER_BYTES + 4;

// The most words any one datagram can carry, derived rather than picked:
// a receiver sized by this cannot be overrun by a legal packet, and a
// sender that respects it cannot build an illegal one.
constexpr unsigned MAX_PACKET_WORDS =
		unsigned((MAX_DATAGRAM - HEADER_BYTES) / sizeof(input_word));
constexpr size_t RESYNC_HEAD_BYTES = HEADER_BYTES + 12;

size_t build_resync_req(u8 *buf, size_t cap, u16 session,
		const resync_req_payload &p);
bool parse_resync_req(const u8 *buf, size_t len, resync_req_payload &p);

size_t build_resync(u8 *buf, size_t cap, u16 session, frame_t state_frame,
		const resync_payload &p, const u8 *chunk, size_t chunk_len);
bool parse_resync(const u8 *buf, size_t len, frame_t &state_frame,
		resync_payload &p, const u8 *&chunk, size_t &chunk_len);

// Bytes an INPUT packet of this shape occupies.
inline size_t input_bytes(unsigned nframes, u8 player_mask, bool hash)
{
	return HEADER_BYTES
			+ (size_t(nframes) * popcount8(player_mask) * sizeof(input_word))
			+ (hash ? sizeof(u64) : 0);
}

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_PACKET_H
