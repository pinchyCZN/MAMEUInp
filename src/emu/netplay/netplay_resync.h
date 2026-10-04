// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_resync.h

    Machine state as a transmissible blob, and the chunking that gets it
    across a datagram link.

    WHY NOT ram_state. src/emu/save.h already has ram_state, which is what
    the rewinder uses, and it does exactly the right capture - but its
    buffer is private, so the bytes cannot be read out to put on a wire.
    Capturing the same way here needs no upstream edit: save_manager's
    indexed_item(), registration_count(), dispatch_presave() and
    dispatch_postload() are all public, and walking them is already how
    netplay_hash fingerprints the same state.

    THE ENTRY SET MUST MATCH BETWEEN PEERS, because the blob is just the
    entries' bytes end to end with no names or offsets. That is exactly
    what the savestate layout hash in the handshake establishes
    (notes/protocol.txt 4.2), so a peer that would mis-apply a blob has
    already been refused the session.

    A DRIVER THAT CANNOT RESTORE ITS OWN STATE IS NOT DETECTABLE HERE.
    Measured: dino round-trips bit-exactly, bublbob2o does not, and
    bublbob2o reports no error while doing it - it simply comes back in an
    unrelated state. MACHINE_SUPPORTS_SAVE is the driver's own claim and
    only a warning in MAME. tools/netplay_roundtrip.lua is the test that
    settles it per driver; nothing in this file can.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_RESYNC_H
#define MAME_EMU_NETPLAY_NETPLAY_RESYNC_H

#pragma once

#include "netplay/netplay_types.h"

#include <vector>


namespace netplay {

// 1024 so the arithmetic is obvious, and comfortably inside MAX_DATAGRAM
// once the header and the per-chunk fields are counted.
constexpr unsigned RESYNC_CHUNK = 1024;

// THE BLOB IS COMPRESSED, and that is the difference between working and
// not. Uncompressed, the SMALLEST driver measured is already 444 KB -
// every entry in notes/protocol.txt 2.4's table is 0.41 MB or more - so a
// limit sized against that table's COMPRESSED column while sending raw
// bytes refuses everything. Measured on dino: 444 KB raw, about 13 KB
// deflated.
//
// 8192 chunks is 8 MB compressed, which covers every driver in that table
// including crysking's 5589 KB. NOT a judgement that 8 MB transfers well -
// it is deliberately high so the big ones can be TRIED. A limit exists
// only because the chunk index and the missing-bitmap need a bound; the
// question of which sizes are usable is one for measurement, not for a
// constant chosen in advance.
constexpr unsigned RESYNC_MAX_CHUNKS = 8192;
constexpr size_t RESYNC_MAX_BYTES = size_t(RESYNC_CHUNK) * RESYNC_MAX_CHUNKS;

// Chunks sent per frame while serving. Unpaced, a multi-megabyte blob is
// thousands of datagrams handed to the socket in one call, and most are
// dropped before anyone reads them. 32 KB a frame is ~1.9 MB/s, so dino
// goes out in one frame and crysking takes about three seconds.
constexpr unsigned RESYNC_CHUNKS_PER_FRAME = 32;

// Bitmap bytes for a given chunk count. Sent at exactly this length rather
// than padded to the maximum, so a 13-chunk transfer does not carry a
// kilobyte of zeroes in every retransmission request.
inline unsigned resync_bitmap_bytes(unsigned chunks)
{
	return (chunks + 7) / 8;
}

constexpr unsigned RESYNC_BITMAP_MAX = RESYNC_MAX_CHUNKS / 8;


class state_blob
{
public:
	state_blob();

	// ---- sending side ----

	// Captures every registered entry. Returns false if the machine's state
	// exceeds RESYNC_MAX_BYTES, which is a refusal, not an error.
	bool capture(running_machine &machine);

	// raw bytes before compression, for reporting
	size_t raw_size() const { return m_raw; }

	// ---- receiving side ----

	void begin_receive(u32 id, frame_t frame, size_t total, unsigned chunks);
	bool add_chunk(unsigned index, const u8 *src, size_t len);
	bool complete() const;

	// Fills the first resync_bitmap_bytes(chunks()) bytes of bitmap with the
	// chunks still wanted and returns how many that is.
	unsigned missing(u8 *bitmap) const;

	// Writes the bytes back over the live machine state and dispatches
	// postload. Refuses if the size does not match this machine's entry set,
	// which would mean applying one driver's state to another.
	bool restore(running_machine &machine) const;

	void clear();

	bool empty() const { return m_data.empty(); }
	size_t size() const { return m_data.size(); }
	const u8 *data() const { return m_data.data(); }
	unsigned chunks() const { return m_chunks; }
	u32 id() const { return m_id; }
	frame_t frame() const { return m_frame; }

	// bytes the live machine's entry set occupies; what a blob must match
	static size_t machine_state_size(running_machine &machine);

private:
	std::vector<u8>     m_data;
	std::vector<u8>     m_have;      // one byte per chunk, not vector<bool>
	unsigned            m_chunks;
	size_t              m_raw;
	u32                 m_id;
	frame_t             m_frame;
};

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_RESYNC_H
