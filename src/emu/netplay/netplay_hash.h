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
#include <utility>
#include <vector>


// ======================> netplay_state_hash

namespace netplay {

// Fingerprint of the save state's SHAPE - entry count, and every
// entry's name, element size and element count. Two peers with
// different values are not running the same machine, which is why the
// handshake carries it (notes/protocol.txt 4.2). A free function
// because both the per-frame hash and the JOIN packet need it and a
// second implementation is how two peers come to disagree.
u64 savestate_layout_hash(running_machine &machine);

} // namespace netplay


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

	// DIAGNOSTIC, env-driven rather than an emu_option: NETPLAY_ENTRYLOG
	// names a file, NETPLAY_ENTRYFRAMES a frame spec like
	// "601-606,1201-1206", and every registered entry is hashed
	// individually on those frames. Answers "WHICH entries differ", which
	// one hash per frame cannot. Env vars because adding an emu_option
	// means editing emuopts.h, which 179 translation units include; this
	// way the module stays a one-TU rebuild. See notes/determinism.txt.

private:
	netplay_state_hash(running_machine &machine, const char *logpath, int interval);

	void on_frame();
	void on_exit();
	void open_entry_log();
	void dump_entries(u64 frame);
	bool want_entries(u64 frame) const;

	u64 compute_layout_hash() const;
	void find_presave_mirrors();

	running_machine &   m_machine;
	u64                 m_layout_hash;
	u64                 m_frame;
	int                 m_interval;
	FILE *              m_log;
	FILE *              m_entry_log;
	std::vector<std::pair<u64, u64> > m_entry_frames;

	// Hashed NAMES of the entries dispatch_presave writes - mirrors
	// carried into a savestate and never read back during execution,
	// excluded from the hash. See find_presave_mirrors.
	//
	// BY NAME AND NOT BY INDEX. indexed_item's ordering is not stable
	// across a machine's life: save_manager::allow_registration(false)
	// sorts the list by name when registration closes, so an index taken
	// before that names a different entry afterwards. Measured: index 53
	// was maincpu m_save_sr early and m_mmu_urp_aptr later.
	std::vector<u64>    m_mirror_names;
};

#endif // MAME_EMU_NETPLAY_NETPLAY_HASH_H
