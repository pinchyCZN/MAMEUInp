// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_hash.cpp

    See netplay_hash.h.

***************************************************************************/

#include "emu.h"
#include "emuopts.h"                  // emu_options is not complete via emu.h
#include "netplay/netplay_hash.h"

#include <cstdlib>
#include <cstring>
#include <memory>


namespace {

//-------------------------------------------------
//  a 64-bit non-cryptographic hash
//
//  NOT util::crc32_creator, for two reasons: it is 32 bits, and it is
//  table-driven a byte at a time. We hash the whole save state every frame -
//  445 KB on CPS1, 24.8 MB on crystal.cpp (notes/protocol.txt 2.4) - so
//  throughput is the binding constraint, and 64 bits puts an accidental
//  collision at ~2^-64 per comparison instead of ~2^-32.
//
//  Mixing is xor-multiply-rotate per 8-byte word with a splitmix64-style
//  finaliser, so a single flipped bit anywhere changes the result.
//-------------------------------------------------

constexpr u64 HASH_SEED  = 0x9E3779B97F4A7C15ULL;
constexpr u64 HASH_PRIME = 0xFF51AFD7ED558CCDULL;

inline u64 rotl64(u64 x, int r) { return (x << r) | (x >> (64 - r)); }

inline u64 hash_finalise(u64 h)
{
	h ^= h >> 33;
	h *= 0xFF51AFD7ED558CCDULL;
	h ^= h >> 33;
	h *= 0xC4CEB9FE1A85EC53ULL;
	h ^= h >> 33;
	return h;
}

u64 hash_bytes(u64 h, const void *data, size_t length)
{
	const u8 *p = reinterpret_cast<const u8 *>(data);

	// whole 8-byte words; memcpy rather than a cast because the entries are
	// device members and nothing guarantees 8-byte alignment
	while (length >= 8)
	{
		u64 word;
		std::memcpy(&word, p, sizeof(word));
		h = rotl64((h ^ word) * HASH_PRIME, 29);
		p += 8;
		length -= 8;
	}

	// tail
	if (length != 0)
	{
		u64 word = 0;
		std::memcpy(&word, p, length);
		h = rotl64((h ^ word) * HASH_PRIME, 29);
	}

	return h;
}

// one instance per machine; MAMEUI runs machines sequentially in-process, so
// this is reset by the exit notifier rather than assumed to outlive one run
std::unique_ptr<netplay_state_hash> s_instance;
running_machine *s_owner = nullptr;

} // anonymous namespace


//-------------------------------------------------
//  frame_update - entry point, called once per
//  frame from ioport_manager::frame_update
//-------------------------------------------------

void netplay_state_hash::frame_update(running_machine &machine)
{
	if (&machine != s_owner)
	{
		// first frame of a new machine
		const char *logpath = machine.options().netplay_hashlog();
		if ((logpath == nullptr) || (*logpath == 0))
			return;                         // disabled; do nothing at all

		s_instance.reset(new netplay_state_hash(
				machine, logpath, machine.options().netplay_hashinterval()));
		s_owner = &machine;
	}

	if (s_instance)
		s_instance->on_frame();
}


//-------------------------------------------------
//  construction
//-------------------------------------------------

netplay_state_hash::netplay_state_hash(running_machine &machine, const char *logpath, int interval)
	: m_machine(machine)
	, m_layout_hash(0)
	, m_frame(0)
	, m_interval(interval > 0 ? interval : 1)
	, m_log(nullptr)
	, m_entry_log(nullptr)
	, m_entry_frames()
{
	open_entry_log();

	m_layout_hash = compute_layout_hash();

	m_log = std::fopen(logpath, "w");
	if (m_log != nullptr)
	{
		// the header is the handshake-relevant part: anything that differs
		// here means the two sides are not the same machine
		std::fprintf(m_log, "# machine=%s\n", machine.system().name);
		std::fprintf(m_log, "# entries=%d\n", machine.save().registration_count());
		std::fprintf(m_log, "# layout=%016llx\n", static_cast<unsigned long long>(m_layout_hash));
		std::fprintf(m_log, "# interval=%d\n", m_interval);
		std::fprintf(m_log, "frame,state\n");
		std::fflush(m_log);
	}
	else
	{
		osd_printf_error("netplay: unable to open hash log '%s'\n", logpath);
	}

	machine.add_notifier(MACHINE_NOTIFY_EXIT, machine_notify_delegate(&netplay_state_hash::on_exit, this));

	osd_printf_info("netplay: state hash enabled, %d entries, layout %016llx, every %d frame(s)\n",
			machine.save().registration_count(),
			static_cast<unsigned long long>(m_layout_hash),
			m_interval);
}


//-------------------------------------------------
//  compute_layout_hash - fingerprint of the save
//  state's SHAPE, not its contents
//-------------------------------------------------

u64 netplay_state_hash::compute_layout_hash() const
{
	save_manager &save = m_machine.save();
	const int count = save.registration_count();

	u64 h = HASH_SEED;
	h = hash_bytes(h, &count, sizeof(count));

	for (int i = 0; i < count; i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0;
		const char *name = save.indexed_item(i, base, valsize, valcount);
		if (name == nullptr)
			continue;

		h = hash_bytes(h, name, std::strlen(name));
		h = hash_bytes(h, &valsize, sizeof(valsize));
		h = hash_bytes(h, &valcount, sizeof(valcount));
	}

	return hash_finalise(h);
}


//-------------------------------------------------
//  state_hash - fingerprint of the save state's
//  CONTENTS
//-------------------------------------------------

u64 netplay_state_hash::state_hash() const
{
	save_manager &save = m_machine.save();
	const int count = save.registration_count();

	// NOTE: dispatch_presave() is deliberately NOT called here, although
	// ram_state::save() does call it. Two reasons. It has side effects - the
	// handlers write derived state back into saved members - and invoking
	// them on frames that would not otherwise have saved is a behavioural
	// change we do not want in a determinism tool. And it is unnecessary:
	// detection only needs a value that is a deterministic function of
	// machine state, not one that matches what a savestate would contain.
	//
	// The cost is that state living only in unsaved derived variables is
	// invisible here, so two genuinely divergent machines can agree for a
	// frame or two. A persistent desync still shows up almost immediately.
	u64 h = HASH_SEED;

	for (int i = 0; i < count; i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0;
		if (save.indexed_item(i, base, valsize, valcount) == nullptr)
			continue;
		if (base == nullptr)
			continue;

		h = hash_bytes(h, base, size_t(valsize) * size_t(valcount));
	}

	return hash_finalise(h);
}


//-------------------------------------------------
//  open_entry_log - DIAGNOSTIC per-entry dump, see
//  the header for why this is env-driven
//-------------------------------------------------

void netplay_state_hash::open_entry_log()
{
	const char *const path = std::getenv("NETPLAY_ENTRYLOG");
	const char *const spec = std::getenv("NETPLAY_ENTRYFRAMES");
	if ((path == nullptr) || (*path == 0) || (spec == nullptr) || (*spec == 0))
		return;

	// spec is "A-B,C-D,..." or bare frame numbers; anything unparseable is
	// skipped rather than guessed at
	const char *p = spec;
	while (*p != 0)
	{
		char *end = nullptr;
		const unsigned long long lo = std::strtoull(p, &end, 10);
		if (end == p)
			break;
		unsigned long long hi = lo;
		p = end;
		if (*p == '-')
		{
			p++;
			hi = std::strtoull(p, &end, 10);
			if (end == p)
				hi = lo;
			else
				p = end;
		}
		m_entry_frames.push_back(std::make_pair(u64(lo), u64(hi)));
		while ((*p != 0) && (*p != ','))
			p++;
		if (*p == ',')
			p++;
	}

	if (m_entry_frames.empty())
		return;

	m_entry_log = std::fopen(path, "w");
	if (m_entry_log == nullptr)
	{
		osd_printf_error("netplay: unable to open entry log '%s'\n", path);
		return;
	}

	std::fprintf(m_entry_log, "# machine=%s\n", m_machine.system().name);
	std::fprintf(m_entry_log, "# entries=%d\n", m_machine.save().registration_count());
	std::fprintf(m_entry_log, "# frames=%s\n", spec);
	std::fprintf(m_entry_log, "frame,index,name,typesize,typecount,hash\n");
	std::fflush(m_entry_log);

	osd_printf_info("netplay: per-entry dump to '%s' for frames %s\n", path, spec);
}


//-------------------------------------------------
//  want_entries
//-------------------------------------------------

bool netplay_state_hash::want_entries(u64 frame) const
{
	for (const auto &range : m_entry_frames)
		if ((frame >= range.first) && (frame <= range.second))
			return true;
	return false;
}


//-------------------------------------------------
//  dump_entries - one row per registered entry
//-------------------------------------------------

void netplay_state_hash::dump_entries(u64 frame)
{
	save_manager &save = m_machine.save();
	const int count = save.registration_count();

	for (int i = 0; i < count; i++)
	{
		void *base = nullptr;
		u32 valsize = 0, valcount = 0;
		const char *const name = save.indexed_item(i, base, valsize, valcount);
		if ((name == nullptr) || (base == nullptr))
			continue;

		const u64 h = hash_finalise(hash_bytes(HASH_SEED, base,
				size_t(valsize) * size_t(valcount)));

		// names carry '/' and ':' but not ',', so plain CSV is safe
		std::fprintf(m_entry_log, "%llu,%d,%s,%u,%u,%016llx\n",
				static_cast<unsigned long long>(frame), i, name,
				valsize, valcount, static_cast<unsigned long long>(h));
	}
	std::fflush(m_entry_log);
}


//-------------------------------------------------
//  on_frame
//-------------------------------------------------

void netplay_state_hash::on_frame()
{
	const u64 frame = m_frame++;

	// before the interval early-out: the dump must not depend on it
	if ((m_entry_log != nullptr) && want_entries(frame))
		dump_entries(frame);

	if ((frame % u64(m_interval)) != 0)
		return;

	if (m_log != nullptr)
	{
		std::fprintf(m_log, "%llu,%016llx\n",
				static_cast<unsigned long long>(frame),
				static_cast<unsigned long long>(state_hash()));
	}
}


//-------------------------------------------------
//  on_exit
//-------------------------------------------------

void netplay_state_hash::on_exit()
{
	if (m_log != nullptr)
	{
		std::fflush(m_log);
		std::fclose(m_log);
		m_log = nullptr;
	}

	if (m_entry_log != nullptr)
	{
		std::fflush(m_entry_log);
		std::fclose(m_entry_log);
		m_entry_log = nullptr;
	}

	osd_printf_info("netplay: state hash log closed after %llu frame(s)\n",
			static_cast<unsigned long long>(m_frame));

	// Release ownership but DO NOT reset s_instance here: this function runs
	// as a member of the object s_instance owns, and resetting would destroy
	// `this` and then return into freed memory. Clearing s_owner is enough -
	// frame_update() replaces the instance when the next machine starts,
	// which destroys this one at a safe point.
	s_owner = nullptr;
}
