// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_transport.h

    How datagrams get from one peer to another, and the two ways of doing
    it. Datagram in, datagram out - a transport knows nothing about the
    wire format, and the manager knows nothing about sockets.

    POLLED, NOT CALLBACK-DRIVEN, and never blocking on its own. Everything
    here runs on the emulation thread inside
    ioport_manager::frame_update, so a transport that blocked would stall
    the machine at a point MAME cannot interrupt. The one place waiting is
    legitimate is a lockstep stall, and that is what wait() exists for:
    the manager decides to wait, the transport decides how.

    TWO IMPLEMENTATIONS, ONE FILE:

      simlink    a simulated network. Delay, jitter and loss are set by
                 the caller and counted in FRAMES, so a timing property
                 of the protocol can be measured without two machines and
                 without a wall clock to make it irreproducible. This is
                 how the budget arithmetic in notes/protocol.txt 1.6.3
                 was verified.

      udp_link   a real UDP socket talking to the relay server. Joining
                 lives here rather than in the manager, because the
                 manager needs its slot and player count before it can
                 size anything, and both come out of the handshake.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_TRANSPORT_H
#define MAME_EMU_NETPLAY_NETPLAY_TRANSPORT_H

#pragma once

#include "netplay/netplay_packet.h"

#include <deque>
#include <string>


namespace netplay {

class transport
{
public:
	virtual ~transport() { }

	// queue one datagram; silently drops what cannot be sent, because a
	// lost input packet is a condition the protocol already has to survive
	// (notes/protocol.txt 1.6.2, redundancy)
	virtual void send(const u8 *data, size_t len) = 0;

	// next received datagram, or 0 if none is ready. Never blocks.
	virtual size_t recv(u8 *buf, size_t cap) = 0;

	// one frame of emulated time has passed
	virtual void tick() { }

	// Called repeatedly while the manager is stalled waiting for input.
	// Returns false when waiting cannot help, which ends the stall.
	virtual bool wait() = 0;

	virtual const char *name() const = 0;

	// diagnostics; the sizing work in protocol.txt 1.6.3 is these numbers
	virtual u64 sent_count() const { return 0; }
	virtual u64 recv_count() const { return 0; }
	virtual u64 drop_count() const { return 0; }
};


// ======================> simlink

class simlink : public transport
{
public:
	// delay and jitter in FRAMES, loss in percent. echo_as is the player
	// slot outgoing packets come back as, which makes a one-process
	// two-player session against a peer that mirrors the local one; 0
	// echoes them unchanged.
	simlink(unsigned delay, unsigned jitter, unsigned loss_pct,
			player_t echo_as, u64 seed);

	void send(const u8 *data, size_t len) override;
	size_t recv(u8 *buf, size_t cap) override;
	void tick() override;
	bool wait() override;
	const char *name() const override { return "simlink"; }

	u64 sent_count() const override { return m_sent; }
	u64 recv_count() const override { return m_recv; }
	u64 drop_count() const override { return m_dropped; }

private:
	struct datagram
	{
		u64     release;            // tick at which it becomes visible
		size_t  len;
		u8      data[MAX_DATAGRAM];
	};

	u64 next_random();

	std::deque<datagram>    m_flight;
	u64                     m_now;
	unsigned                m_delay;
	unsigned                m_jitter;
	unsigned                m_loss_pct;
	player_t                m_echo_as;
	u64                     m_rng;
	u64                     m_sent;
	u64                     m_recv;
	u64                     m_dropped;
};


// ======================> udp_link

class udp_link : public transport
{
public:
	udp_link();
	~udp_link();

	// host:port of the relay server. Returns false on any socket failure,
	// which fails the session rather than running without a peer.
	bool open(const char *host, unsigned port);

	// NEVER BLOCKS, and that is a hard constraint rather than a
	// preference. Blocking MAME's main thread inside frame_update wedges
	// window creation: the window runs on its own thread and reaches the
	// main thread by SendMessage, so a main thread asleep in a socket call
	// leaves a half-initialised display device behind. Measured: one peer
	// blocking for 300 ms was enough to make a SECOND instance on the same
	// machine fail video init outright.
	//
	// So the slot and player count come from configuration, JOIN is
	// announced in the background, and WELCOME or REFUSE is handled
	// whenever it turns up. Frame 0 alignment is the lockstep stall's job,
	// which is what it already does for every other frame.
	void identify(const char *driver, u64 inputmap_digest, u64 layout_digest,
			player_t want_players);

	bool welcomed() const { return m_welcomed; }
	bool refused() const { return m_refused; }
	player_t slot() const { return m_slot; }
	player_t players() const { return m_players; }

	void send(const u8 *data, size_t len) override;
	size_t recv(u8 *buf, size_t cap) override;
	void tick() override;
	bool wait() override;
	const char *name() const override { return "udp"; }

	u64 sent_count() const override { return m_sent; }
	u64 recv_count() const override { return m_recv; }
	u64 drop_count() const override { return m_dropped; }

private:
	bool recv_raw(u8 *buf, size_t cap, size_t &got);

	// Deliberately not a sockaddr_in: including winsock2.h here would put it
	// in front of every file that includes the manager, and MAME's own
	// headers are particular about that. The real type lives in the .cpp.
	void *      m_impl;
	player_t    m_slot;
	player_t    m_players;
	u16         m_session;
	bool        m_welcomed;
	bool        m_refused;
	u64         m_ticks;
	u8          m_join[MAX_DATAGRAM];
	size_t      m_join_len;
	u64         m_sent;
	u64         m_recv;
	u64         m_dropped;
};

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_TRANSPORT_H
