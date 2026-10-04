// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_resync.cpp

    See netplay_resync.h.

***************************************************************************/

#include "emu.h"
#include "netplay/netplay_resync.h"

#include <cstring>

// zlib is already an include dir for the emu project (scripts/src/emu.lua)
// and src/emu/render.cpp uses it, so this needs no build change.
#include <zlib.h>


namespace netplay {

state_blob::state_blob()
	: m_data()
	, m_have()
	, m_chunks(0)
	, m_raw(0)
	, m_id(0)
	, m_frame(0)
{
}


void state_blob::clear()
{
	m_data.clear();
	m_have.clear();
	m_chunks = 0;
	m_raw = 0;
	m_id = 0;
	m_frame = 0;
}


//-------------------------------------------------
//  machine_state_size
//-------------------------------------------------

size_t state_blob::machine_state_size(running_machine &machine)
{
	save_manager &save = machine.save();
	const int count = save.registration_count();
	size_t total = 0;

	for (int i = 0; i < count; i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0;
		if (save.indexed_item(i, base, valsize, valcount) == nullptr)
			continue;
		if (base == nullptr)
			continue;
		total += size_t(valsize) * size_t(valcount);
	}

	return total;
}


//-------------------------------------------------
//  capture
//-------------------------------------------------

bool state_blob::capture(running_machine &machine)
{
	clear();

	save_manager &save = machine.save();
	m_raw = machine_state_size(machine);

	// Exactly what ram_state::save() does first. The handlers write derived
	// state back into saved members, so skipping it captures members that
	// are stale with respect to the machine.
	save.dispatch_presave();

	std::vector<u8> raw(m_raw);

	const int count = save.registration_count();
	size_t off = 0;
	for (int i = 0; i < count; i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0;
		if (save.indexed_item(i, base, valsize, valcount) == nullptr)
			continue;
		if (base == nullptr)
			continue;

		const size_t bytes = size_t(valsize) * size_t(valcount);
		if ((off + bytes) > raw.size())
			break;
		std::memcpy(&raw[off], base, bytes);
		off += bytes;
	}

	// Deflate. notes/protocol.txt 2.4 measured 33x on dino and 4.5x on
	// crysking - the big states compress worst because they are dominated
	// by video memory holding real image data - so this helps most exactly
	// where the state is smallest. Level 6 rather than 9: the extra second
	// of CPU buys a few per cent and this runs on the emulation thread.
	uLongf bound = compressBound(uLong(raw.size()));
	m_data.resize(bound);
	const int zerr = compress2(m_data.data(), &bound,
			raw.data(), uLong(raw.size()), 6);

	if (zerr != Z_OK)
	{
		osd_printf_info("netplay: deflate failed (%d)\n", zerr);
		clear();
		return false;
	}

	m_data.resize(size_t(bound));

	if (m_data.size() > RESYNC_MAX_BYTES)
	{
		osd_printf_info("netplay: %s state is %d KB raw, %d KB packed, over the %d KB limit\n",
				machine.system().name, int(m_raw / 1024),
				int(m_data.size() / 1024), int(RESYNC_MAX_BYTES / 1024));
		clear();
		return false;
	}

	m_chunks = unsigned((m_data.size() + RESYNC_CHUNK - 1) / RESYNC_CHUNK);
	m_have.assign(m_chunks, 1);
	return true;
}


//-------------------------------------------------
//  restore
//-------------------------------------------------

bool state_blob::restore(running_machine &machine) const
{
	save_manager &save = machine.save();
	const size_t expect = machine_state_size(machine);

	// Inflate into exactly the space this machine's entry set occupies. If
	// the blob does not expand to precisely that, the two peers do not have
	// the same entry set and applying it would scatter one driver's bytes
	// across another's members. The handshake layout digest should have
	// caught that already; refusing here costs nothing.
	std::vector<u8> raw(expect);
	uLongf got = uLongf(expect);
	const int zerr = uncompress(raw.data(), &got,
			m_data.data(), uLong(m_data.size()));

	if ((zerr != Z_OK) || (size_t(got) != expect))
	{
		osd_printf_info("netplay: resync inflate failed (%d), got %d of %d\n",
				zerr, int(got), int(expect));
		return false;
	}

	const int count = save.registration_count();
	size_t off = 0;
	for (int i = 0; i < count; i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0;
		if (save.indexed_item(i, base, valsize, valcount) == nullptr)
			continue;
		if (base == nullptr)
			continue;

		const size_t bytes = size_t(valsize) * size_t(valcount);
		if ((off + bytes) > raw.size())
			break;
		std::memcpy(base, &raw[off], bytes);
		off += bytes;
	}

	// As ram_state::load() does. Handlers re-derive whatever they own from
	// the members just written.
	save.dispatch_postload();
	return true;
}


//-------------------------------------------------
//  receiving
//-------------------------------------------------

void state_blob::begin_receive(u32 id, frame_t frame, size_t total, unsigned chunks)
{
	clear();

	if ((total == 0) || (total > RESYNC_MAX_BYTES))
		return;
	if ((chunks == 0) || (chunks > RESYNC_MAX_CHUNKS))
		return;

	m_id = id;
	m_frame = frame;
	m_chunks = chunks;
	m_data.assign(total, 0);
	m_have.assign(chunks, 0);
}


bool state_blob::add_chunk(unsigned index, const u8 *src, size_t len)
{
	if ((m_chunks == 0) || (index >= m_chunks) || (src == nullptr))
		return false;

	const size_t off = size_t(index) * RESYNC_CHUNK;
	if (off >= m_data.size())
		return false;

	// The last chunk is short; every other one must be full, or the sender
	// and receiver disagree about the chunk size.
	const size_t want = ((m_data.size() - off) < RESYNC_CHUNK)
			? (m_data.size() - off) : RESYNC_CHUNK;
	if (len != want)
		return false;

	std::memcpy(&m_data[off], src, len);
	m_have[index] = 1;
	return true;
}


bool state_blob::complete() const
{
	if (m_chunks == 0)
		return false;
	for (unsigned i = 0; i < m_chunks; i++)
		if (m_have[i] == 0)
			return false;
	return true;
}


unsigned state_blob::missing(u8 *bitmap) const
{
	std::memset(bitmap, 0, resync_bitmap_bytes(m_chunks));

	unsigned n = 0;
	for (unsigned i = 0; (i < m_chunks) && (i < RESYNC_MAX_CHUNKS); i++)
	{
		if (m_have[i] != 0)
			continue;
		bitmap[i / 8] = u8(bitmap[i / 8] | (1u << (i % 8)));
		n++;
	}
	return n;
}

} // namespace netplay
