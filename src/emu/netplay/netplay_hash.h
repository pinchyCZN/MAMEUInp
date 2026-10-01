// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_hash.h

    Per-frame fingerprint of emulated machine state, for desync detection.

    Two different hashes, answering two different questions:

      layout hash   computed once, over the SHAPE of the save state -
                    the entry count and every entry's name, element size
                    and element count. Two peers with different layout
                    hashes are not running the same machine, and no
                    amount of input synchronisation will help them. This
                    belongs in the session handshake.

      state hash    computed per frame, over the BYTES of every
                    registered entry. Two peers whose state hashes differ
                    at the same frame have diverged.

    Desync detection has to exist before resync or rollback, because it
    is the trigger for both. See notes/protocol.txt and
    notes/netplay-hooks.txt.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_NETPLAY_HASH_H
#define MAME_EMU_NETPLAY_NETPLAY_HASH_H

#pragma once

#include <cstdio>
#include <string>


// ======================> netplay_state_hash

class netplay_state_hash
{
public:
	// called once per frame from ioport_manager::frame_update, after input
	// for the frame has been applied; creates and tears down the per-machine
	// instance as needed
	static void frame_update(running_machine &machine);

	// fingerprint of the save-state SHAPE; stable for a given machine and
	// build, and intended for the session handshake
	u64 layout_hash() const { return m_layout_hash; }

	// fingerprint of the save-state CONTENTS right now
	u64 state_hash() const;

private:
	netplay_state_hash(running_machine &machine, const char *logpath, int interval);

	void on_frame();
	void on_exit();

	u64 compute_layout_hash() const;

	running_machine &   m_machine;
	u64                 m_layout_hash;
	u64                 m_frame;
	int                 m_interval;
	FILE *              m_log;
};

#endif // MAME_EMU_NETPLAY_NETPLAY_HASH_H
