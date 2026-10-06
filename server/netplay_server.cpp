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

// Lobby. TCP on the same port number as the relay's UDP - different
// protocol, so they do not collide, and one port to remember.
constexpr unsigned MAX_LOBBY = 16;
constexpr size_t LOBBY_LINE = 512;

struct lobby_client
{
	// Initialised here, not in relay's constructor: an uninitialised
	// `used` puts garbage handles in the fd_set and select then fails
	// every iteration, which looks like a server that accepts nothing.
	bool        used = false;
	np_socket   sock = NP_INVALID_SOCKET;
	std::string name;
	std::string in;        // partial line
};

// The hosted game. One at a time: the relay serves one session, so a
// second game would have nowhere to run.
struct lobby_game
{
	bool        active = false;
	std::string driver;
	unsigned    players = 2;
	unsigned    delay = 6;
	unsigned    batch = 1;
	bool        savestates = true;
	bool        send_config = true;

	// Lobby indices in slot order: roster[0] is the host and player 1.
	// Slot order is the point - the server seats each emulator in the
	// slot its LAUNCH named.
	int         roster[MAX_PLAYERS];
	unsigned    count = 0;

	lobby_game()
	{
		for (unsigned i = 0; i < MAX_PLAYERS; i++)
			roster[i] = -1;
	}
};


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
	bool open_lobby(unsigned port);
	void lobby_accept();
	void lobby_read(unsigned i);
	void lobby_drop(unsigned i, const char *why);
	void lobby_line(unsigned i, const std::string &line);
	void lobby_send(unsigned i, const std::string &line);
	void lobby_broadcast(const std::string &line);
	void lobby_players();
	void lobby_game_state(int only = -1);
	void lobby_game_drop(unsigned i);
	void lobby_create(unsigned i, const std::string &arg);
	void lobby_join(unsigned i);
	void lobby_leave(unsigned i);
	void lobby_start(unsigned i);
	int  lobby_in_game(unsigned i) const;

	np_socket       m_tcp = NP_INVALID_SOCKET;
	lobby_client    m_lobby[MAX_LOBBY];
	lobby_game      m_game;

public:

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

	// The lobby is optional: a relay with no lobby still runs games.
	if (open_lobby(m_port))
		std::printf("lobby: TCP %u, up to %u players\n", m_port, MAX_LOBBY);
	else
		std::printf("lobby: TCP %u unavailable, relay only\n", m_port);

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

	// A client that names a slot is placed there or refused. Picking the
	// lowest free slot instead would silently seat two peers that were
	// both configured as player 1, and the client keeps its configured
	// slot regardless of what WELCOME says - so both would send as
	// player 1 and nobody would see player 2.
	unsigned slot = MAX_PLAYERS;
	if ((jp.slot >= 1) && (jp.slot <= m_players))
	{
		if (m_client[jp.slot - 1].used)
		{
			refuse(from, REFUSE_SLOT);
			return;
		}
		slot = jp.slot - 1;
	}
	else
	{
		// Slot 0 means no preference. An out-of-range slot is treated the
		// same way rather than refused, so a peer asking for player 3 of a
		// 2-player session still gets a game.
		for (unsigned i = 0; i < m_players; i++)
			if (!m_client[i].used)
			{
				slot = i;
				break;
			}
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


bool relay::open_lobby(unsigned port)
{
	m_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (m_tcp == NP_INVALID_SOCKET)
		return false;

	int on = 1;
	setsockopt(m_tcp, SOL_SOCKET, SO_REUSEADDR,
			reinterpret_cast<const char *>(&on), sizeof(on));

	struct sockaddr_in a;
	std::memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(u16(port));

	if (bind(m_tcp, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) != 0)
	{
		NP_CLOSESOCKET(m_tcp);
		m_tcp = NP_INVALID_SOCKET;
		return false;
	}
	if (listen(m_tcp, 8) != 0)
	{
		NP_CLOSESOCKET(m_tcp);
		m_tcp = NP_INVALID_SOCKET;
		return false;
	}
	return true;
}


void relay::lobby_send(unsigned i, const std::string &line)
{
	if (!m_lobby[i].used)
		return;

	const std::string out = line + "\n";
	send(m_lobby[i].sock, out.c_str(), int(out.size()), 0);
}


void relay::lobby_broadcast(const std::string &line)
{
	for (unsigned i = 0; i < MAX_LOBBY; i++)
		if (m_lobby[i].used && !m_lobby[i].name.empty())
			lobby_send(i, line);
}


void relay::lobby_players()
{
	std::string list = "PLAYERS ";
	bool first = true;
	for (unsigned i = 0; i < MAX_LOBBY; i++)
	{
		if (!m_lobby[i].used || m_lobby[i].name.empty())
			continue;
		if (!first)
			list += ",";
		list += m_lobby[i].name;
		first = false;
	}
	lobby_broadcast(list);
}


void relay::lobby_drop(unsigned i, const char *why)
{
	if (!m_lobby[i].used)
		return;

	const std::string who = m_lobby[i].name;

	// Before the entry is cleared: the roster holds lobby indices and
	// resolves names through them.
	lobby_game_drop(i);

	NP_CLOSESOCKET(m_lobby[i].sock);
	m_lobby[i] = lobby_client();

	if (!who.empty())
	{
		std::printf("  lobby: %s left (%s)\n", who.c_str(), why);
		lobby_players();
	}
}


void relay::lobby_accept()
{
	struct sockaddr_in from;
	np_socklen len = sizeof(from);
	const np_socket c = accept(m_tcp,
			reinterpret_cast<struct sockaddr *>(&from), &len);
	if (c == NP_INVALID_SOCKET)
		return;

	for (unsigned i = 0; i < MAX_LOBBY; i++)
	{
		if (m_lobby[i].used)
			continue;
		m_lobby[i].used = true;
		m_lobby[i].sock = c;
		m_lobby[i].name.clear();
		m_lobby[i].in.clear();
		return;
	}

	// Full. Say so rather than dropping silently.
	const char *const full = "ERROR lobby full\n";
	send(c, full, int(std::strlen(full)), 0);
	NP_CLOSESOCKET(c);
}


//-------------------------------------------------
//  the hosted game
//-------------------------------------------------

// "delay=6" -> 6, or fallback if the key is absent or unparsable.
static unsigned kv(const std::string &s, const char *key, unsigned fallback)
{
	const std::string want = std::string(key) + "=";
	size_t at = s.find(want);
	while (at != std::string::npos)
	{
		// Must start the string or follow a space, so "config=" does not
		// match inside "sendconfig=".
		if ((at == 0) || (s[at - 1] == ' '))
			return unsigned(std::atoi(s.c_str() + at + want.size()));
		at = s.find(want, at + 1);
	}
	return fallback;
}


int relay::lobby_in_game(unsigned i) const
{
	if (!m_game.active)
		return -1;
	for (unsigned k = 0; k < m_game.count; k++)
		if (m_game.roster[k] == int(i))
			return int(k);
	return -1;
}


// only >= 0 sends to that one client instead of broadcasting, for a client
// that has just connected and needs the current state.
void relay::lobby_game_state(int only)
{
	if (!m_game.active)
	{
		if (only >= 0)
			lobby_send(unsigned(only), "NOGAME");
		else
			lobby_broadcast("NOGAME");
		return;
	}

	char buf[256];
	std::snprintf(buf, sizeof(buf),
			// players= rather than the count as "2/4": the count is in
			// ROSTER, and a field carried twice is a field that disagrees
			// with itself.
			"GAME %s host=%s players=%u delay=%u batch=%u savestates=%u config=%u",
			m_game.driver.c_str(),
			m_lobby[m_game.roster[0]].name.c_str(),
			m_game.players,
			m_game.delay, m_game.batch,
			m_game.savestates ? 1u : 0u,
			m_game.send_config ? 1u : 0u);

	std::string roster("ROSTER ");
	for (unsigned k = 0; k < m_game.count; k++)
	{
		if (k != 0)
			roster += ",";
		roster += m_lobby[m_game.roster[k]].name;
	}

	if (only >= 0)
	{
		lobby_send(unsigned(only), buf);
		lobby_send(unsigned(only), roster);
	}
	else
	{
		lobby_broadcast(buf);
		lobby_broadcast(roster);
	}
}


// Called when a client leaves for any reason, including disconnecting.
void relay::lobby_game_drop(unsigned i)
{
	const int k = lobby_in_game(i);
	if (k < 0)
		return;

	if (k == 0)
	{
		// The host leaving ends the game. Reseating somebody else as host
		// would hand them settings they did not choose.
		std::printf("  lobby: game %s cancelled (host left)\n",
				m_game.driver.c_str());
		m_game = lobby_game();
		lobby_game_state();
		return;
	}

	for (unsigned j = unsigned(k); (j + 1) < m_game.count; j++)
		m_game.roster[j] = m_game.roster[j + 1];
	m_game.roster[m_game.count - 1] = -1;
	m_game.count--;
	lobby_game_state();
}


void relay::lobby_create(unsigned i, const std::string &arg)
{
	if (m_game.active)
	{
		lobby_send(i, "ERROR a game is already hosted");
		return;
	}

	// The relay serves one session. Changing the player count under a live
	// one would leave it waiting for a number of peers nobody agreed to.
	if (m_started || (m_joined > 0))
	{
		lobby_send(i, "ERROR a session is already running");
		return;
	}

	const size_t sp = arg.find(' ');
	const std::string driver = arg.substr(0, sp);
	if (driver.empty() || (driver.size() > 14))
	{
		lobby_send(i, "ERROR bad system name");
		return;
	}

	const unsigned players = kv(arg, "players", 2);
	const unsigned delay = kv(arg, "delay", 6);
	const unsigned batch = kv(arg, "batch", 1);
	if ((players < 2) || (players > MAX_PLAYERS))
	{
		lobby_send(i, "ERROR players must be 2 to "
				+ std::to_string(unsigned(MAX_PLAYERS)));
		return;
	}
	if ((delay < 1) || (delay > 60))
	{
		lobby_send(i, "ERROR delay must be 1 to 60");
		return;
	}
	if ((batch < 1) || (batch > 8))
	{
		lobby_send(i, "ERROR batch must be 1 to 8");
		return;
	}

	m_game = lobby_game();
	m_game.active = true;
	m_game.driver = driver;
	m_game.players = players;
	m_game.delay = delay;
	m_game.batch = batch;
	m_game.savestates = (kv(arg, "savestates", 1) != 0);
	m_game.send_config = (kv(arg, "config", 1) != 0);
	m_game.roster[0] = int(i);
	m_game.count = 1;

	m_players = player_t(players);

	std::printf("  lobby: %s hosts %s for %u player(s), delay %u batch %u\n",
			m_lobby[i].name.c_str(), driver.c_str(), players, delay, batch);
	lobby_game_state();
}


void relay::lobby_join(unsigned i)
{
	if (!m_game.active)
	{
		lobby_send(i, "ERROR no game is hosted");
		return;
	}
	if (lobby_in_game(i) >= 0)
	{
		lobby_send(i, "ERROR you are already in the game");
		return;
	}
	if (m_game.count >= m_game.players)
	{
		lobby_send(i, "ERROR the game is full");
		return;
	}

	m_game.roster[m_game.count] = int(i);
	m_game.count++;
	std::printf("  lobby: %s joined %s as player %u\n",
			m_lobby[i].name.c_str(), m_game.driver.c_str(), m_game.count);
	lobby_game_state();
}


void relay::lobby_leave(unsigned i)
{
	if (lobby_in_game(i) < 0)
	{
		lobby_send(i, "ERROR you are not in the game");
		return;
	}
	std::printf("  lobby: %s left the game\n", m_lobby[i].name.c_str());
	lobby_game_drop(i);
}


void relay::lobby_start(unsigned i)
{
	if (!m_game.active)
	{
		lobby_send(i, "ERROR no game is hosted");
		return;
	}
	if (lobby_in_game(i) != 0)
	{
		lobby_send(i, "ERROR only the host can start");
		return;
	}

	// The relay starts the UDP session when m_players have joined, so
	// starting short would leave everyone waiting on a peer who was never
	// coming.
	if (m_game.count < m_game.players)
	{
		lobby_send(i, "ERROR waiting for "
				+ std::to_string(m_game.players - m_game.count)
				+ " more player(s)");
		return;
	}

	std::printf("  lobby: starting %s\n", m_game.driver.c_str());
	for (unsigned k = 0; k < m_game.count; k++)
	{
		char buf[256];
		std::snprintf(buf, sizeof(buf),
				"LAUNCH game=%s slot=%u players=%u delay=%u batch=%u"
				" savestates=%u config=%u",
				m_game.driver.c_str(), k + 1, m_game.players,
				m_game.delay, m_game.batch,
				m_game.savestates ? 1u : 0u,
				m_game.send_config ? 1u : 0u);
		lobby_send(unsigned(m_game.roster[k]), buf);
	}

	// The game record has done its job; the UDP relay runs the session from
	// here. Keeping it would block the next game, because every member is
	// about to disconnect from the lobby to launch.
	m_game = lobby_game();
	lobby_game_state();
}


void relay::lobby_line(unsigned i, const std::string &line)
{
	const size_t sp = line.find(' ');
	const std::string cmd = line.substr(0, sp);
	const std::string arg = (sp == std::string::npos)
			? std::string() : line.substr(sp + 1);

	if (cmd == "HELLO")
	{
		std::string want = arg.empty() ? std::string("player") : arg;
		if (want.size() > 24)
			want.resize(24);

		// De-duplicate rather than refuse: a name clash mid-dialog is a
		// worse experience than being renamed.
		std::string name = want;
		for (unsigned n = 2; n < 100; n++)
		{
			bool taken = false;
			for (unsigned j = 0; j < MAX_LOBBY; j++)
				if ((j != i) && m_lobby[j].used && (m_lobby[j].name == name))
					taken = true;
			if (!taken)
				break;
			name = want + "_" + std::to_string(n);
		}

		m_lobby[i].name = name;
		std::printf("  lobby: %s joined\n", name.c_str());
		lobby_send(i, "YOUARE " + name);
		lobby_players();
		lobby_game_state(int(i));
		return;
	}

	if (m_lobby[i].name.empty())
	{
		lobby_send(i, "ERROR say HELLO first");
		return;
	}

	if (cmd == "CHAT")
	{
		if (!arg.empty())
			lobby_broadcast("CHAT " + m_lobby[i].name + " " + arg);
		return;
	}

	if (cmd == "CREATE")
	{
		lobby_create(i, arg);
		return;
	}

	if (cmd == "JOIN")
	{
		lobby_join(i);
		return;
	}

	if (cmd == "LEAVE")
	{
		lobby_leave(i);
		return;
	}

	if (cmd == "START")
	{
		lobby_start(i);
		return;
	}

	if (cmd == "BYE")
	{
		lobby_drop(i, "said bye");
		return;
	}

	lobby_send(i, "ERROR unknown command " + cmd);
}


void relay::lobby_read(unsigned i)
{
	char buf[LOBBY_LINE];
	const int n = recv(m_lobby[i].sock, buf, int(sizeof(buf)), 0);
	if (n <= 0)
	{
		lobby_drop(i, "disconnected");
		return;
	}

	m_lobby[i].in.append(buf, size_t(n));

	// A client that never sends a newline must not grow this forever.
	if (m_lobby[i].in.size() > (LOBBY_LINE * 4))
	{
		lobby_drop(i, "line too long");
		return;
	}

	for ( ; ; )
	{
		const size_t nl = m_lobby[i].in.find('\n');
		if (nl == std::string::npos)
			break;

		std::string line = m_lobby[i].in.substr(0, nl);
		m_lobby[i].in.erase(0, nl + 1);
		if (!line.empty() && (line[line.size() - 1] == '\r'))
			line.resize(line.size() - 1);

		if (!line.empty())
		{
			lobby_line(i, line);
			if (!m_lobby[i].used)
				return;         // dropped itself
		}
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

		np_socket maxfd = m_sock;
		if (m_tcp != NP_INVALID_SOCKET)
		{
			FD_SET(m_tcp, &rd);
			if (m_tcp > maxfd)
				maxfd = m_tcp;
		}
		for (unsigned i = 0; i < MAX_LOBBY; i++)
		{
			if (!m_lobby[i].used)
				continue;
			FD_SET(m_lobby[i].sock, &rd);
			if (m_lobby[i].sock > maxfd)
				maxfd = m_lobby[i].sock;
		}

		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		const int ready = select(int(maxfd) + 1, &rd, nullptr, nullptr, &tv);
		sweep();
		if (ready <= 0)
			continue;

		if ((m_tcp != NP_INVALID_SOCKET) && FD_ISSET(m_tcp, &rd))
			lobby_accept();

		for (unsigned i = 0; i < MAX_LOBBY; i++)
			if (m_lobby[i].used && FD_ISSET(m_lobby[i].sock, &rd))
				lobby_read(i);

		if (!FD_ISSET(m_sock, &rd))
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
		case MSG_CONFIG:
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
				on_input(from, buf, size_t(n));

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
