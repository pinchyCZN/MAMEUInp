// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
//
// Does the word carry every value at every width and offset?
//
// A two-peer session only ever exercises the ONE layout its driver needs,
// and the axis byte of a driver nobody is touching is whatever the field
// defaults to. So a live session cannot tell "the format carries analog"
// from "both ends agree the value is zero". This does: it builds every
// layout our limits allow, writes boundary and pseudo-random values into
// every field, and round-trips the result through build_input/parse_input.
//
// Links netplay_packet.cpp with NETPLAY_STANDALONE, so it needs no MAME
// and no ROM.
//
//     cl /nologo /EHsc /O2 /DNETPLAY_STANDALONE /Isrc/emu /Isrc/emu/netplay ^
//        /Fe:wordtest.exe tools/netplay_wordtest.cpp ^
//        src/emu/netplay/netplay_packet.cpp
//     g++ -O2 -std=c++14 -DNETPLAY_STANDALONE -Isrc/emu -Isrc/emu/netplay \
//        -o wordtest tools/netplay_wordtest.cpp src/emu/netplay/netplay_packet.cpp
//
// Exit status is the failure count, so it is usable from a script.

#include "netplay/netplay_packet.h"

#include <cstdio>

using namespace netplay;

namespace {

unsigned g_checks = 0;
unsigned g_fails = 0;

void fail(const char *what, unsigned a, unsigned b)
{
	g_fails++;
	if (g_fails <= 20)
		std::printf("  FAIL %-34s got %u want %u\n", what, a, b);
}

void check(const char *what, unsigned got, unsigned want)
{
	g_checks++;
	if (got != want)
		fail(what, got, want);
}

// A cheap deterministic sequence. Math.random would make a failure
// unreproducible, which is the one thing a format test must not be.
u32 next(u32 &state)
{
	state = (state * 1103515245u) + 12345u;
	return (state >> 16) & 0xffff;
}


// ---- 1. layout_build agrees with its own arithmetic ----

void test_layouts()
{
	std::printf("layout_build\n");

	// Refusals. Each of these is a refused session rather than a word that
	// quietly drops bits.
	word_layout l;
	u8 bits[MAX_AXES];
	for (unsigned k = 0; k < MAX_AXES; k++)
		bits[k] = 8;

	check("digital over cap refused",
			layout_build(l, DIGITAL_BITS + 1, bits, 1) ? 1 : 0, 0);
	check("too many axes refused",
			layout_build(l, 8, bits, MAX_AXES + 1) ? 1 : 0, 0);

	u8 wide[1] = { u8(AXIS_MAX_BITS + 1) };
	check("over-wide axis refused",
			layout_build(l, 8, wide, 1) ? 1 : 0, 0);

	u8 zero[1] = { 0 };
	check("zero-width axis refused",
			layout_build(l, 8, zero, 1) ? 1 : 0, 0);

	check("no inputs at all refused",
			layout_build(l, 0, bits, 0) ? 1 : 0, 0);

	// Every legal shape: digital 1..32 bits against 0..MAX_AXES axes of
	// each legal width.
	for (unsigned d = 1; d <= DIGITAL_BITS; d++)
		for (unsigned axes = 0; axes <= MAX_AXES; axes++)
			for (unsigned w = 1; w <= AXIS_MAX_BITS; w++)
			{
				for (unsigned k = 0; k < axes; k++)
					bits[k] = u8(w);

				if (!layout_build(l, d, bits, axes))
				{
					fail("legal shape refused", 0, 1);
					continue;
				}

				const unsigned want_dig = (d + 7) / 8;
				const unsigned per = (w <= 8) ? 1 : 2;
				g_checks++;
				if (l.digital_bytes != want_dig)
					fail("digital_bytes", l.digital_bytes, want_dig);
				g_checks++;
				if (l.bytes != (want_dig + (axes * per)))
					fail("bytes", l.bytes, want_dig + (axes * per));

				// Fields must not overlap and must not run past the word.
				for (unsigned k = 0; k < axes; k++)
				{
					g_checks++;
					if (l.axis_off[k] != (want_dig + (k * per)))
						fail("axis_off", l.axis_off[k], want_dig + (k * per));
					g_checks++;
					if ((l.axis_off[k] + l.axis_bytes[k]) > l.bytes)
						fail("axis runs past the word", 1, 0);
				}
			}
}


// ---- 2. a field written is the field read, and nothing else moves ----

void test_fields()
{
	std::printf("word_set_digital / word_set_analog\n");

	u32 rng = 1;

	for (unsigned d = 1; d <= DIGITAL_BITS; d++)
		for (unsigned axes = 1; axes <= MAX_AXES; axes++)
		{
			u8 bits[MAX_AXES];
			for (unsigned k = 0; k < axes; k++)
				bits[k] = u8(1 + (next(rng) % AXIS_MAX_BITS));

			word_layout l;
			if (!layout_build(l, d, bits, axes))
			{
				fail("layout refused", 0, 1);
				continue;
			}

			// Each axis gets a value that fits its own width, including
			// both boundaries.
			u32 want[MAX_AXES];
			for (unsigned k = 0; k < axes; k++)
			{
				const u32 top = (bits[k] >= 32)
						? 0xffffffffu : ((1u << bits[k]) - 1u);
				switch (next(rng) % 3)
				{
				case 0:  want[k] = 0; break;
				case 1:  want[k] = top; break;
				default: want[k] = next(rng) & top; break;
				}
			}

			input_word w;
			word_clear(w);

			for (unsigned b = 0; b < d; b++)
				if ((next(rng) & 1) != 0)
					word_set_digital(w, b, true);
			const u32 dig = word_digital(w, l);

			for (unsigned k = 0; k < axes; k++)
				word_set_analog(w, l, k, want[k]);

			// THE AXES MUST NOT HAVE DISTURBED THE DIGITAL BITMAP. An axis
			// whose offset overlapped the bitmap would show up here and
			// nowhere else - the game would just read a stuck button.
			check("digital survives the axes", word_digital(w, l), dig);

			for (unsigned k = 0; k < axes; k++)
				check("axis reads back", word_analog(w, l, k), want[k]);

			// And writing one axis must not move its neighbours.
			if (axes >= 2)
			{
				word_set_analog(w, l, 0, want[0]);
				for (unsigned k = 1; k < axes; k++)
					check("neighbour axis undisturbed",
							word_analog(w, l, k), want[k]);
			}
		}
}


// ---- 3. the packet round-trips ----

void test_packets()
{
	std::printf("build_input / parse_input\n");

	u32 rng = 7;

	static const unsigned WIDTHS[] = { 1, 2, 5, 8, 15, 28 };

	for (unsigned wi = 0; wi < (sizeof(WIDTHS) / sizeof(WIDTHS[0])); wi++)
	{
		const unsigned wb = WIDTHS[wi];

		for (unsigned nframes = 1; nframes <= 8; nframes++)
			for (unsigned pm = 1; pm <= 7; pm++)
			{
				const unsigned n = nframes * popcount8(u8(pm));
				if (n > MAX_PACKET_WORDS)
					continue;
				if (input_bytes(nframes, u8(pm), true, wb) > MAX_DATAGRAM)
					continue;

				input_word out[MAX_PACKET_WORDS];
				for (unsigned i = 0; i < n; i++)
				{
					word_clear(out[i]);
					for (unsigned b = 0; b < wb; b++)
						out[i].b[b] = u8(next(rng) & 0xff);
				}

				packet_header head;
				head.type = MSG_INPUT;
				head.version = PROTOCOL_VERSION;
				head.session = u16(next(rng));
				head.base_frame = (frame_t(next(rng)) << 16) | next(rng);
				head.nframes = u8(nframes);
				head.player_mask = u8(pm);
				head.flags = 0;
				head.reserved = 0;

				const u64 hash = (u64(next(rng)) << 32) | next(rng);

				u8 buf[MAX_DATAGRAM];
				const size_t len = build_input(buf, sizeof(buf), head, out,
						&hash, wb);

				check("build_input length",
						unsigned(len),
						unsigned(input_bytes(nframes, u8(pm), true, wb)));
				if (len == 0)
					continue;

				packet_header got;
				input_word back[MAX_PACKET_WORDS];
				u64 gothash = 0;
				bool hashed = false;

				// Pre-dirty the destination: parse must zero above
				// word_bytes, or two peers comparing whole words would
				// differ on padding alone.
				for (unsigned i = 0; i < MAX_PACKET_WORDS; i++)
					for (unsigned b = 0; b < WORD_MAX_BYTES; b++)
						back[i].b[b] = 0xa5;

				if (!parse_input(buf, len, got, back, MAX_PACKET_WORDS,
						gothash, hashed, wb))
				{
					fail("parse_input rejected a packet it built", 0, 1);
					continue;
				}

				check("nframes", got.nframes, nframes);
				check("player_mask", got.player_mask, pm);
				check("base_frame low", unsigned(got.base_frame & 0xffff),
						unsigned(head.base_frame & 0xffff));
				check("has_hash", hashed ? 1 : 0, 1);
				check("hash low", unsigned(gothash & 0xffff),
						unsigned(hash & 0xffff));

				for (unsigned i = 0; i < n; i++)
				{
					for (unsigned b = 0; b < wb; b++)
						check("payload byte", back[i].b[b], out[i].b[b]);
					for (unsigned b = wb; b < WORD_MAX_BYTES; b++)
						check("padding zeroed", back[i].b[b], 0);
				}

				// A length that does not match the declared shape is
				// rejected rather than papered over.
				packet_header junk;
				u64 h2 = 0;
				bool f2 = false;
				check("short packet rejected",
						parse_input(buf, len - 1, junk, back,
								MAX_PACKET_WORDS, h2, f2, wb) ? 1 : 0, 0);
				check("wrong word size rejected",
						parse_input(buf, len, junk, back, MAX_PACKET_WORDS,
								h2, f2, (wb == 1) ? 2 : (wb - 1)) ? 1 : 0, 0);
				check("zero word size rejected",
						parse_input(buf, len, junk, back, MAX_PACKET_WORDS,
								h2, f2, 0) ? 1 : 0, 0);
			}
	}
}


// ---- 4. the datagram clamp ----

void test_clamp()
{
	std::printf("max_words_for\n");

	check("zero width", max_words_for(0), 0);

	for (unsigned wb = 1; wb <= WORD_MAX_BYTES; wb++)
	{
		const unsigned fit = max_words_for(wb);
		g_checks++;
		if (input_bytes(fit, 0x01, false, wb) > MAX_DATAGRAM)
			fail("clamp allows an oversized datagram", wb, 0);
		g_checks++;
		if (input_bytes(fit + 1, 0x01, false, wb) <= MAX_DATAGRAM)
			fail("clamp is stricter than it needs to be", wb, 0);
	}

	// The sender's deepest window against the widest word, which is the
	// case that made this clamp reachable at all.
	std::printf("  widest word %u bytes: %u words per datagram"
			" (a window is at most %u)\n",
			WORD_MAX_BYTES, max_words_for(WORD_MAX_BYTES), MAX_PACKET_WORDS);
}

} // anonymous namespace


int main()
{
	std::printf("netplay word format test - WORD_MAX_BYTES=%u MAX_AXES=%u"
			" AXIS_MAX_BITS=%u DIGITAL_BITS=%u\n\n",
			WORD_MAX_BYTES, MAX_AXES, AXIS_MAX_BITS, DIGITAL_BITS);

	test_layouts();
	test_fields();
	test_packets();
	test_clamp();

	std::printf("\n%u check(s), %u failure(s)\n", g_checks, g_fails);
	return int(g_fails);
}
