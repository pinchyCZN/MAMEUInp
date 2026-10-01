// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_mix.h

    The 64-bit mixing primitive, shared by everything in this directory
    that needs a digest: the per-frame state hash, the savestate layout
    hash, and the translation-table digest in the handshake.

    NOT util::crc32_creator. That is 32 bits and table-driven a byte at a
    time; the state hash runs over the whole save set every frame, so
    throughput binds and 64 bits puts an accidental collision at ~2^-64
    per comparison instead of ~2^-32. See notes/protocol.txt 4.2.

    xor-multiply-rotate per 8-byte word with a splitmix64-style finaliser,
    so one flipped bit anywhere changes the result.

    WIRE-FORMAT INVARIANT: these values appear in the handshake, so both
    peers must compute them identically. Do not change the constants, the
    rotation, the word order or the tail handling without a protocol
    version bump - notes/protocol.txt 3.2.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_MIX_H
#define MAME_EMU_NETPLAY_NETPLAY_MIX_H

#pragma once

#include <cstring>


namespace netplay {

constexpr u64 MIX_SEED  = 0x9E3779B97F4A7C15ULL;
constexpr u64 MIX_PRIME = 0xFF51AFD7ED558CCDULL;

inline u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }

inline u64 mix(u64 h, u64 word)
{
	return rotl64((h ^ word) * MIX_PRIME, 29);
}

inline u64 finalise(u64 h)
{
	h ^= h >> 33;
	h *= 0xFF51AFD7ED558CCDULL;
	h ^= h >> 33;
	h *= 0xC4CEB9FE1A85EC53ULL;
	h ^= h >> 33;
	return h;
}

inline u64 mix_bytes(u64 h, const void *data, size_t length)
{
	const u8 *p = reinterpret_cast<const u8 *>(data);

	// memcpy rather than a cast: the save-state entries this runs over are
	// device members and nothing guarantees 8-byte alignment
	while (length >= 8)
	{
		u64 word;
		std::memcpy(&word, p, sizeof(word));
		h = mix(h, word);
		p += 8;
		length -= 8;
	}

	if (length != 0)
	{
		u64 word = 0;
		std::memcpy(&word, p, length);
		h = mix(h, word);
	}

	return h;
}

inline u64 mix_string(u64 h, const char *s)
{
	return (s == nullptr) ? mix(h, 0) : mix_bytes(h, s, std::strlen(s));
}

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_MIX_H
