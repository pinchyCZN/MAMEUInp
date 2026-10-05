// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_server.cpp

    The relay server. One UDP socket, one session, N players.

    It compiles the SAME wire format the emulator does - the sources under
    src/emu/netplay are included directly rather than reimplemented,
    because two copies of a packet layout is how two programs come to
    disagree about one. NETPLAY_STANDALONE supplies the MAME integer types
    that emu.h would otherwise provide; see netplay_types.h.

    Build (MSVC, from the repo root):

        cl /nologo /EHsc /O2 /std:c++14 /DNETPLAY_STANDALONE ^
           /I src/emu /Fe:build/netplay_server.exe ^
           server/netplay_server.cpp src/emu/netplay/netplay_packet.cpp ^
           ws2_32.lib

    Build (gcc/clang):

        g++ -O2 -std=c++14 -DNETPLAY_STANDALONE -Isrc/emu \
            -o build/netplay_server server/netplay_server.cpp \
            src/emu/netplay/netplay_packet.cpp

    Run:

        netplay_server -port 7000 -players 2

    WHAT IT DOES AND DOES NOT DO. It is a relay: an INPUT packet from one
    peer is forwarded verbatim to every other peer, because the packet
    already declares which slot it belongs to (notes/protocol.txt 3.2) and
    the server has no reason to rewrite it. It assigns slots, checks the
    two handshake digests it is given, and releases START once the session
    is full.

    It does NOT synthesize input for a late player. That was considered and
    deferred - notes/protocol.txt 5 - so a peer whose input is late stalls
    its own peers rather than having a word invented for it.

***************************************************************************/

#include "netplay/netplay_packet.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define NP_INVALID_SOCKET   INVALID_SOCKET
#define NP_CLOSESOCKET(s)   closesocket(s)
typedef SOCKET np_socket;
typedef int np_socklen;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define NP_INVALID_SOCKET   (-1)
#define NP_CLOSESOCKET(s)   ::close(s)
typedef int np_socket;
typedef socklen_t np_socklen;
#endif

using namespace netplay;


namespace {

constexpr unsigned DEFAULT_PORT = 7000;

// A peer that has said nothing for this long is gone. Input flows every
// frame in a live session, so silence this long is not jitter.
constexpr time_t CLIENT_TIMEOUT = 15;

struct client
{
	bool                used;
	struct sockaddr_in  addr;
	u64                 inputmap;
	u64                 layout;
	std::string         driver;
	u64                 packets;
	time_t              last_seen;

	client() : used(false), inputmap(0), layout(0), packets(0), last_seen(0)
	{
		std::memset(&addr, 0, sizeof(addr));
	}
};


bool same_addr(const struct sockaddr_in &a, const struct sockaddr_in &b)
{
	return (a.sin_addr.s_addr == b.sin_addr.s_addr) && (a.sin_port == b.sin_port);
}


std::string addr_text(const struct sockaddr_in &a)
{
	char buf[64];
	const char *const s = inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
	char out[96];
	std::snprintf(out, sizeof(out), "%s:%u",
			(s != nullptr) ? s : "?", unsigned(ntohs(a.sin_port)));
	return std::string(out);
}


class relay
{
public:
	relay(unsigned port, player_t players, u16 session)
		: m_sock(NP_INVALID_SOCKET)
		, m_port(port)
		, m_players(players)
		, m_session(session)
		, m_joined(0)
		, m_started(false)
		, m_relayed(0)
		, m_malformed(0)
	{
	}

	~relay()
	{
		if (m_sock != NP_INVALID_SOCKET)
			NP_CLOSESOCKET(m_sock);
#if defined(_WIN32)
		WSACleanup();
#endif
	}

	bool open();
	void run();

private:
	void on_join(const struct sockaddr_in &from, const u8 *buf, size_t len);
	void on_input(const struct sockaddr_in &from, const u8 *buf, size_t len);
	void send_to(const struct sockaddr_in &to, const u8 *buf, size_t len);
	void refuse(const struct sockaddr_in &to, u8 reason);
	void broadcast_start();
	void sweep();
	int find(const struct sockaddr_in &from) const;

	np_socket   m_sock;
	unsigned    m_port;
	player_t    m_players;
	u16         m_session;
	player_t    m_joined;
	bool        m_started;
	u64         m_relayed;
	u64         m_malformed;
	client      m_client[MAX_PLAYERS];
};


bool relay::open()
{
#if defined(_WIN32)
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		std::fprintf(stderr, "WSAStartup failed\n");
		return false;
	}
#endif

	m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (m_sock == NP_INVALID_SOCKET)
	{
		std::fprintf(stderr, "cannot create socket\n");
		return false;
	}

	struct sockaddr_in local;
	std::memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = htons(u16(m_port));

	if (bind(m_sock, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) != 0)
	{
		std::fprintf(stderr, "cannot bind port %u\n", m_port);
		return false;
	}

	std::printf("netplay server: port %u, %d players, session %u\n",
			m_port, int(m_players), unsigned(m_session));
	return true;
}


int relay::find(const struct sockaddr_in &from) const
{
	for (unsigned i = 0; i < MAX_PLAYERS; i++)
		if (m_client[i].used && same_addr(m_client[i].addr, from))
			return int(i);
	return -1;
}


void relay::send_to(const struct sockaddr_in &to, const u8 *buf, size_t len)
{
	sendto(m_sock, reinterpret_cast<const char *>(buf), int(len), 0,
			reinterpret_cast<const struct sockaddr *>(&to), sizeof(to));
}


void relay::refuse(const struct sockaddr_in &to, u8 reason)
{
	u8 out[MAX_DATAGRAM];
	const size_t len = build_refuse(out, sizeof(out), reason);
	if (len != 0)
		send_to(to, out, len);
	std::printf("  refused %s: %s\n", addr_text(to).c_str(), refuse_text(reason));
}


void relay::on_join(const struct sockaddr_in &from, const u8 *buf, size_t len)
{
	join_payload jp;
	if (!parse_join(buf, len, jp))
	{
		m_malformed++;
		return;
	}

	std::string driver(jp.driver, sizeof(jp.driver));
	const size_t nul = driver.find('\0');
	if (nul != std::string::npos)
		driver.resize(nul);

	// A repeat JOIN is the normal case, not an error: the client
	// retransmits until it is welcomed, because the first datagram is the
	// one most likely to be lost. Re-welcome the same slot.
	const int existing = find(from);
	if (existing >= 0)
	{
		m_client[existing].last_seen = time(nullptr);
		welcome_payload wp;
		wp.slot = u8(existing + 1);
		wp.players = m_players;
		wp.start_frame = 0;
		u8 out[MAX_DATAGRAM];
		const size_t n = build_welcome(out, sizeof(out), m_session, wp);
		if (n != 0)
			send_to(from, out, n);
		if (m_started)
			broadcast_start();
		return;
	}

	if (m_started)
	{
		// protocol.txt 4.4: a late joiner is a resync with M equal to the
		// whole session, and resync does not exist yet.
		refuse(from, REFUSE_RUNNING);
		return;
	}

	if (m_joined >= m_players)
	{
		refuse(from, REFUSE_FULL);
		return;
	}

	// The first peer to join sets what the rest are checked against. The
	// server has no independent idea of the right driver or table, and
	// inventing one would mean shipping a copy of every driver's input map.
	if (m_joined > 0)
	{
		const client *ref = nullptr;
		for (unsigned i = 0; i < MAX_PLAYERS; i++)
			if (m_client[i].used)
			{
				ref = &m_client[i];
				break;
			}

		if (ref != nullptr)
		{
			if (driver != ref->driver)
			{
				refuse(from, REFUSE_DRIVER);
				return;
			}
			if (jp.inputmap != ref->inputmap)
			{
				refuse(from, REFUSE_INPUTCFG);
				return;
			}
			if (jp.layout != ref->layout)
			{
				refuse(from, REFUSE_LAYOUT);
				return;
			}
		}
	}

	unsigned slot = MAX_PLAYERS;
	for (unsigned i = 0; i < m_players; i++)
		if (!m_client[i].used)
		{
			slot = i;
			break;
		}
	if (slot >= MAX_PLAYERS)
	{
		refuse(from, REFUSE_FULL);
		return;
	}

	m_client[slot].used = true;
	m_client[slot].addr = from;
	m_client[slot].inputmap = jp.inputmap;
	m_client[slot].layout = jp.layout;
	m_client[slot].driver = driver;
	m_client[slot].last_seen = time(nullptr);
	m_joined++;

	std::printf("  joined %s as player %u of %d (%s, map %016llx)\n",
			addr_text(from).c_str(), slot + 1, int(m_players), driver.c_str(),
			static_cast<unsigned long long>(jp.inputmap));

	welcome_payload wp;
	wp.slot = u8(slot + 1);
	wp.players = m_players;
	wp.start_frame = 0;
	u8 out[MAX_DATAGRAM];
	const size_t n = build_welcome(out, sizeof(out), m_session, wp);
	if (n != 0)
		send_to(from, out, n);

	if (m_joined >= m_players)
	{
		m_started = true;
		std::printf("  session full, starting at frame 0\n");
		broadcast_start();
	}
}


void relay::broadcast_start()
{
	u8 out[MAX_DATAGRAM];
	const size_t len = build_start(out, sizeof(out), m_session, 0);
	if (len == 0)
		return;
	for (unsigned i = 0; i < MAX_PLAYERS; i++)
		if (m_client[i].used)
			send_to(m_client[i].addr, out, len);
}


void relay::on_input(const struct sockaddr_in &from, const u8 *buf, size_t len)
{
	const int who = find(from);
	if (who < 0)
		return;         // not a member of this session

	m_client[who].packets++;
	m_client[who].last_seen = time(nullptr);

	// Forwarded VERBATIM. The packet already says which slot it belongs to,
	// so there is nothing to rewrite, and not rewriting means the server
	// never has to understand the input word.
	for (unsigned i = 0; i < MAX_PLAYERS; i++)
	{
		if (!m_client[i].used || (int(i) == who))
			continue;
		send_to(m_client[i].addr, buf, len);
		m_relayed++;
	}
}


//-------------------------------------------------
//  sweep - drop the departed, and recycle the
//  session so the server outlives it
//-------------------------------------------------

void relay::sweep()
{
	const time_t now = time(nullptr);
	unsigned live = 0;

	for (unsigned i = 0; i < MAX_PLAYERS; i++)
	{
		if (!m_client[i].used)
			continue;
		if ((now - m_client[i].last_seen) > CLIENT_TIMEOUT)
		{
			std::printf("  player %u timed out after %llu packet(s)\n",
					i + 1,
					static_cast<unsigned long long>(m_client[i].packets));
			m_client[i] = client();
			if (m_joined > 0)
				m_joined--;
		}
		else
		{
			live++;
		}
	}

	// THE SESSION IS RECYCLED, NOT THE PROCESS. Once started, a session
	// refuses new joins (4.4 REFUSE_RUNNING), so without this the server
	// would be useless after one game and need restarting. A fresh session
	// id makes stray packets from the old one identifiable.
	if (m_started && (live == 0))
	{
		m_session++;
		m_started = false;
		m_joined = 0;
		std::printf("\nsession over - relayed %llu packet(s). Waiting for %d player(s),\n",
				static_cast<unsigned long long>(m_relayed), int(m_players));
		std::printf("session %u.\n", unsigned(m_session));
		m_relayed = 0;
	}
}


void relay::run()
{
	u8 buf[MAX_DATAGRAM];

	// NEVER RETURNS. select() rather than a blocking recvfrom so the sweep
	// above runs even when nothing is arriving.
	for ( ; ; )
	{
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(m_sock, &rd);

		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		const int ready = select(int(m_sock) + 1, &rd, nullptr, nullptr, &tv);
		sweep();
		if (ready <= 0)
			continue;

		struct sockaddr_in from;
		np_socklen fromlen = sizeof(from);
		const int n = recvfrom(m_sock, reinterpret_cast<char *>(buf), int(sizeof(buf)), 0,
				reinterpret_cast<struct sockaddr *>(&from), &fromlen);

		// On Windows a datagram sent to a closed port makes the NEXT
		// recvfrom fail with WSAECONNRESET. Ignoring the error is correct;
		// treating it as fatal would kill the server when a peer exits.
		if (n <= 0)
			continue;

		packet_header head;
		if (!parse_header(buf, size_t(n), head))
		{
			m_malformed++;
			continue;
		}

		switch (head.type)
		{
		case MSG_JOIN:
			on_join(from, buf, size_t(n));
			break;

		case MSG_INPUT:
		case MSG_UICMD:
		case MSG_RESYNC_REQ:
		case MSG_RESYNC:
		case MSG_CHAT:
		case MSG_STALL:
		case MSG_NAK:
			// All forwarded verbatim. A UI command carries the frame it takes
			// effect on and a resync chunk carries its own index and id, so the
			// server has no more reason to understand any of them than it does
			// an input word.
			//
			// Sent to every OTHER peer rather than to the one that wants it:
			// a guest ignores a request it did not make and a chunk it is not
			// waiting for, so addressing costs a peer table lookup to save
			// bandwidth that only matters above two players. [open]
			on_input(from, buf, size_t(n));
			break;

		case MSG_BYE:
			{
				const int who = find(from);
				if (who >= 0)
				{
					std::printf("  player %d left after %llu packet(s)\n", who + 1,
							static_cast<unsigned long long>(m_client[who].packets));
					m_client[who] = client();
					if (m_joined > 0)
						m_joined--;
				}
			}
			break;

		default:
			// HASH and DESYNC are relayed like input once they mean
			// something; for now they are counted. [open]
			m_malformed++;
			break;
		}
	}
}


void usage()
{
	std::printf(
			"netplay_server - MAMEUI netplay relay\n"
			"\n"
			"  -port N       UDP port to listen on (default %u)\n"
			"  -players N    players required before the session starts (default 2)\n"
			"  -session N    session id reported to clients (default 1)\n",
			DEFAULT_PORT);
}

} // anonymous namespace


int main(int argc, char *argv[])
{
	unsigned port = DEFAULT_PORT;
	unsigned players = 2;
	unsigned session = 1;

	for (int i = 1; i < argc; i++)
	{
		const bool more = ((i + 1) < argc);
		if ((std::strcmp(argv[i], "-port") == 0) && more)
			port = unsigned(std::strtoul(argv[++i], nullptr, 10));
		else if ((std::strcmp(argv[i], "-players") == 0) && more)
			players = unsigned(std::strtoul(argv[++i], nullptr, 10));
		else if ((std::strcmp(argv[i], "-session") == 0) && more)
			session = unsigned(std::strtoul(argv[++i], nullptr, 10));
		else
		{
			usage();
			return 1;
		}
	}

	if ((players < 1) || (players > MAX_PLAYERS))
	{
		std::fprintf(stderr, "players must be 1 to %d\n", int(MAX_PLAYERS));
		return 1;
	}

	// Unbuffered: this is a server you watch in a terminal, and a join or a
	// refusal that only appears when the process exits is no use.
	setvbuf(stdout, nullptr, _IONBF, 0);

	relay r(port, player_t(players), u16(session));
	if (!r.open())
		return 1;

	r.run();
	return 0;
}
