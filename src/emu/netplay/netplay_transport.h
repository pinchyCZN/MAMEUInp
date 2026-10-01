// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_transport.h

    How datagrams get from one peer to another. Datagram in, datagram out -
    the transport knows nothing about the wire format, and the manager knows
    nothing about sockets.

    POLLED, NOT CALLBACK-DRIVEN, and never blocking on its own. Everything
    here runs on the emulation thread inside
    ioport_manager::frame_update, so a transport that blocked would stall
    the machine at a point MAME cannot interrupt. The one place waiting is
    legitimate is a lockstep stall, and that is what wait() exists for:
    the manager decides to wait, the transport decides how.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_TRANSPORT_H
#define MAME_EMU_NETPLAY_NETPLAY_TRANSPORT_H

#pragma once

#include "netplay/netplay_packet.h"


namespace netplay {

class transport
{
public:
	virtual ~transport() { }

	// queue one datagram; silently drops what cannot be sent, because a
	// lost input packet is a transport condition the protocol already has
	// to survive (notes/protocol.txt 1.6.2, redundancy)
	virtual void send(const u8 *data, size_t len) = 0;

	// next received datagram, or 0 if none is ready. Never blocks.
	virtual size_t recv(u8 *buf, size_t cap) = 0;

	// one frame of emulated time has passed
	virtual void tick() { }

	// Called repeatedly while the manager is stalled waiting for input.
	// Returns false when waiting cannot help, which ends the stall. A
	// simulated link advances its own clock here; a socket sleeps briefly.
	virtual bool wait() = 0;

	virtual const char *name() const = 0;

	// diagnostics, for the stall and loss accounting the sizing work needs
	virtual u64 sent_count() const { return 0; }
	virtual u64 recv_count() const { return 0; }
	virtual u64 drop_count() const { return 0; }
};

} // namespace netplay

#endif // MAME_EMU_NETPLAY_NETPLAY_TRANSPORT_H
