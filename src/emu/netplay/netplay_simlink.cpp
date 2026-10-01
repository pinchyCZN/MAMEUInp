// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_simlink.cpp

    See netplay_simlink.h.

***************************************************************************/

#include "emu.h"
#include "netplay/netplay_simlink.h"

#include <cstring>


namespace netplay {

//-------------------------------------------------
//  construction
//-------------------------------------------------

simlink::simlink(unsigned delay, unsigned jitter, unsigned loss_pct,
		player_t echo_as, u64 seed)
	: m_flight()
	, m_now(0)
	, m_delay(delay)
	, m_jitter(jitter)
	, m_loss_pct((loss_pct > 100) ? 100 : loss_pct)
	, m_echo_as(echo_as)
	, m_rng(seed != 0 ? seed : 0x9E3779B97F4A7C15ULL)
	, m_sent(0)
	, m_recv(0)
	, m_dropped(0)
	, m_wait_ticks(0)
{
}


//-------------------------------------------------
//  next_random - splitmix64
//
//  Seeded and self-contained on purpose: machine().rand() would couple a
//  test harness to emulated state, and a host RNG would make a stall count
//  irreproducible. Jitter cannot perturb emulated state anyway - a late
//  packet changes WHEN a frame runs, never what input it runs with - so
//  this only has to be repeatable.
//-------------------------------------------------

u64 simlink::next_random()
{
	m_rng += 0x9E3779B97F4A7C15ULL;
	u64 z = m_rng;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}


//-------------------------------------------------
//  send
//-------------------------------------------------

void simlink::send(const u8 *data, size_t len)
{
	if ((data == nullptr) || (len == 0) || (len > MAX_DATAGRAM))
		return;

	m_sent++;

	if ((m_loss_pct != 0) && ((next_random() % 100) < m_loss_pct))
	{
		m_dropped++;
		return;
	}

	datagram d;
	d.len = len;
	std::memcpy(d.data, data, len);

	// Jitter can reorder, because a real link does. recv() therefore scans
	// for any datagram whose release has passed rather than taking the
	// front of the queue.
	const unsigned extra = (m_jitter == 0) ? 0 : unsigned(next_random() % (m_jitter + 1));
	d.release = m_now + m_delay + extra;

	// Rewrite the owning slot so the echo looks like a remote peer. Only
	// valid for a single-player packet, which is what a peer sends.
	if ((m_echo_as >= 1) && (m_echo_as <= MAX_PLAYERS) && (d.len > HEADER_BYTES))
	{
		packet_header head;
		if (parse_header(d.data, d.len, head) && (head.type == MSG_INPUT)
				&& (popcount8(head.player_mask) == 1))
		{
			put_u8(d.data + 9, u8(1u << (m_echo_as - 1)));
		}
	}

	m_flight.push_back(d);
}


//-------------------------------------------------
//  recv
//-------------------------------------------------

size_t simlink::recv(u8 *buf, size_t cap)
{
	for (auto it = m_flight.begin(); it != m_flight.end(); ++it)
	{
		if (it->release > m_now)
			continue;
		if (it->len > cap)
		{
			// cannot deliver into the caller's buffer; account for it rather
			// than spinning on the same datagram forever
			m_dropped++;
			m_flight.erase(it);
			return 0;
		}
		const size_t len = it->len;
		std::memcpy(buf, it->data, len);
		m_flight.erase(it);
		m_recv++;
		return len;
	}
	return 0;
}


//-------------------------------------------------
//  tick / wait
//-------------------------------------------------

void simlink::tick()
{
	m_now++;
}


bool simlink::wait()
{
	// Nothing in flight means no amount of waiting will produce input, so
	// the manager must stop waiting. Without this a dropped packet is an
	// infinite stall rather than a stall that ends.
	if (m_flight.empty())
		return false;

	m_now++;
	m_wait_ticks++;
	return true;
}

} // namespace netplay
