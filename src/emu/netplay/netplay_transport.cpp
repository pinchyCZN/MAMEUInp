// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
/***************************************************************************

    netplay_transport.cpp

    See netplay_transport.h.

***************************************************************************/

#include "emu.h"
#include "netplay/netplay_transport.h"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t_compat;
#define NP_INVALID_SOCKET   INVALID_SOCKET
#define NP_CLOSESOCKET(s)   closesocket(s)
#define NP_WOULDBLOCK()     (WSAGetLastError() == WSAEWOULDBLOCK)
typedef SOCKET np_socket;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef socklen_t socklen_t_compat;
#define NP_INVALID_SOCKET   (-1)
#define NP_CLOSESOCKET(s)   ::close(s)
#define NP_WOULDBLOCK()     ((errno == EAGAIN) || (errno == EWOULDBLOCK))
typedef int np_socket;
#endif


namespace netplay {

//**************************************************************************
//  SIMLINK
//**************************************************************************

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
{
}


//-------------------------------------------------
//  next_random - splitmix64
//
//  Seeded and self-contained on purpose. machine().rand() would couple a
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
	// for any datagram whose release has passed rather than taking the front
	// of the queue.
	const unsigned extra = (m_jitter == 0) ? 0 : unsigned(next_random() % (m_jitter + 1));
	d.release = m_now + m_delay + extra;

	// Rewrite the owning slot so the echo looks like a remote peer. Only
	// meaningful for a single-player input packet, which is what a peer sends.
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


void simlink::tick()
{
	m_now++;
}


bool simlink::wait()
{
	// Nothing in flight means no amount of waiting produces input, so the
	// manager must stop. Without this a dropped packet is an infinite stall
	// rather than a stall that ends.
	if (m_flight.empty())
		return false;

	m_now++;
	return true;
}



//**************************************************************************
//  UDP
//**************************************************************************

namespace {

struct udp_state
{
	np_socket           sock;
	struct sockaddr_in  server;
	bool                started;
};

bool set_nonblocking(np_socket s)
{
#if defined(_WIN32)
	u_long nb = 1;
	return ioctlsocket(s, FIONBIO, &nb) == 0;
#else
	const int fl = fcntl(s, F_GETFL, 0);
	return (fl != -1) && (fcntl(s, F_SETFL, fl | O_NONBLOCK) != -1);
#endif
}

void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
	Sleep(ms);
#else
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = long(ms % 1000) * 1000000L;
	nanosleep(&ts, nullptr);
#endif
}

} // anonymous namespace


udp_link::udp_link()
	: m_impl(nullptr)
	, m_slot(0)
	, m_players(0)
	, m_session(0)
	, m_welcomed(false)
	, m_started(false)
	, m_refused(false)
	, m_ticks(0)
	, m_join_len(0)
	, m_sent(0)
	, m_recv(0)
	, m_dropped(0)
{
	std::memset(m_join, 0, sizeof(m_join));
}


udp_link::~udp_link()
{
	udp_state *const st = reinterpret_cast<udp_state *>(m_impl);
	if (st != nullptr)
	{
		if (st->sock != NP_INVALID_SOCKET)
			NP_CLOSESOCKET(st->sock);
		delete st;
		m_impl = nullptr;
	}

	// NO WSACleanup. Winsock's initialisation is refcounted per process and
	// MAME calls WSAStartup itself per socket it opens, so a cleanup from
	// here can decrement a count another subsystem owns. Leaking one
	// initialisation in a process that is exiting is the cheaper mistake.
}


//-------------------------------------------------
//  open
//-------------------------------------------------

bool udp_link::open(const char *host, unsigned port)
{
#if defined(_WIN32)
	// Refcounted, and MAME's own winsocket.cpp does the same per use, so
	// calling it again here is correct rather than merely tolerated.
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		osd_printf_error("netplay: WSAStartup failed\n");
		return false;
	}
#endif

	udp_state *const st = new udp_state;
	st->sock = NP_INVALID_SOCKET;
	st->started = false;
	std::memset(&st->server, 0, sizeof(st->server));

	st->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (st->sock == NP_INVALID_SOCKET)
	{
		osd_printf_error("netplay: cannot create socket\n");
		delete st;
		return false;
	}

	// Ephemeral local port: a peer only ever talks to the server, which
	// learns the address from the JOIN, so nothing has to be reachable on a
	// fixed port and two peers can share one host.
	struct sockaddr_in local;
	std::memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = 0;
	if (bind(st->sock, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)) != 0)
	{
		osd_printf_error("netplay: cannot bind local socket\n");
		NP_CLOSESOCKET(st->sock);
		delete st;
		return false;
	}

	if (!set_nonblocking(st->sock))
	{
		osd_printf_error("netplay: cannot set the socket non-blocking\n");
		NP_CLOSESOCKET(st->sock);
		delete st;
		return false;
	}

	st->server.sin_family = AF_INET;
	st->server.sin_port = htons(u16(port));

	if (inet_pton(AF_INET, host, &st->server.sin_addr) != 1)
	{
		// not a literal address, so resolve it
		struct addrinfo hints;
		struct addrinfo *res = nullptr;
		std::memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		if ((getaddrinfo(host, nullptr, &hints, &res) != 0) || (res == nullptr))
		{
			osd_printf_error("netplay: cannot resolve '%s'\n", host);
			NP_CLOSESOCKET(st->sock);
			delete st;
			return false;
		}
		st->server.sin_addr =
				reinterpret_cast<struct sockaddr_in *>(res->ai_addr)->sin_addr;
		freeaddrinfo(res);
	}

	m_impl = st;
	return true;
}


//-------------------------------------------------
//  recv_raw
//-------------------------------------------------

bool udp_link::recv_raw(u8 *buf, size_t cap, size_t &got)
{
	got = 0;
	udp_state *const st = reinterpret_cast<udp_state *>(m_impl);
	if (st == nullptr)
		return false;

	struct sockaddr_in from;
	socklen_t_compat fromlen = sizeof(from);
	const int n = recvfrom(st->sock, reinterpret_cast<char *>(buf), int(cap), 0,
			reinterpret_cast<struct sockaddr *>(&from), &fromlen);

	if (n <= 0)
		return false;

	// Only the server is ever a legitimate source. An unsolicited datagram
	// from anywhere else is counted and discarded rather than parsed.
	if ((from.sin_addr.s_addr != st->server.sin_addr.s_addr)
			|| (from.sin_port != st->server.sin_port))
	{
		m_dropped++;
		return false;
	}

	got = size_t(n);
	return true;
}


//-------------------------------------------------
//  identify - build the JOIN once; tick() sends it
//-------------------------------------------------

void udp_link::identify(const char *driver, u64 inputmap_digest,
		u64 layout_digest, player_t slot, player_t want_players)
{
	m_slot = slot;
	m_players = want_players;

	join_payload jp;
	jp.inputmap = inputmap_digest;
	jp.layout = layout_digest;
	jp.want_players = want_players;
	std::memset(jp.driver, 0, sizeof(jp.driver));
	for (unsigned i = 0; (i < sizeof(jp.driver)) && (driver[i] != 0); i++)
		jp.driver[i] = driver[i];

	m_join_len = build_join(m_join, sizeof(m_join), jp);

	// Send the first one immediately rather than waiting a frame: it costs
	// nothing and it is the packet the peer is waiting on.
	if (m_join_len != 0)
	{
		udp_state *const st = reinterpret_cast<udp_state *>(m_impl);
		if (st != nullptr)
		{
			sendto(st->sock, reinterpret_cast<const char *>(m_join),
					int(m_join_len), 0,
					reinterpret_cast<struct sockaddr *>(&st->server),
					sizeof(st->server));
			m_sent++;
		}
	}
}


//-------------------------------------------------
//  tick - re-announce until welcomed
//-------------------------------------------------

void udp_link::tick()
{
	m_ticks++;

	// JOIN carries no acknowledgement of its own and the first one is the
	// datagram most likely to be lost - the server may not be listening
	// yet. Re-announce about twice a second until welcomed.
	if (m_welcomed || m_refused || (m_join_len == 0))
		return;
	if ((m_ticks % 30) != 0)
		return;

	udp_state *const st = reinterpret_cast<udp_state *>(m_impl);
	if (st == nullptr)
		return;

	sendto(st->sock, reinterpret_cast<const char *>(m_join), int(m_join_len), 0,
			reinterpret_cast<struct sockaddr *>(&st->server), sizeof(st->server));
	m_sent++;
}

void udp_link::send(const u8 *data, size_t len)
{
	udp_state *const st = reinterpret_cast<udp_state *>(m_impl);
	if ((st == nullptr) || (data == nullptr) || (len == 0) || (len > MAX_DATAGRAM))
		return;

	const int n = sendto(st->sock, reinterpret_cast<const char *>(data), int(len), 0,
			reinterpret_cast<struct sockaddr *>(&st->server), sizeof(st->server));
	if (n == int(len))
		m_sent++;
	else
		m_dropped++;
}


size_t udp_link::recv(u8 *buf, size_t cap)
{
	for ( ; ; )
	{
		size_t got = 0;
		if (!recv_raw(buf, cap, got))
			return 0;
		m_recv++;

		packet_header head;
		if (!parse_header(buf, got, head))
			continue;

		// Control traffic is this layer's business and never reaches the
		// manager, which only knows about input.
		if (head.type == MSG_WELCOME)
		{
			welcome_payload wp;
			u16 session = 0;
			if (parse_welcome(buf, got, session, wp) && !m_welcomed)
			{
				m_session = session;
				m_welcomed = true;
				if (wp.slot != m_slot)
					osd_printf_warning("netplay: server put us in slot %d, we configured %d\n",
							int(wp.slot), int(m_slot));
				osd_printf_info("netplay: session %u acknowledged, slot %d of %d\n",
						unsigned(m_session), int(wp.slot), int(wp.players));
			}
			continue;
		}

		if (head.type == MSG_REFUSE)
		{
			if (!m_refused && (got >= REFUSE_BYTES))
			{
				m_refused = true;
				osd_printf_error("netplay: server refused the session - %s\n",
						refuse_text(get_u8(buf + HEADER_BYTES)));
			}
			continue;
		}

		if (head.type == MSG_START)
		{
			if (!m_started)
			{
				m_started = true;
				osd_printf_info("netplay: session starting\n");
			}
			continue;
		}

		return got;
	}
}


bool udp_link::wait()
{
	// A millisecond is a sixteenth of a frame, so a stall resolves at
	// roughly the granularity the budget is measured in without spinning a
	// core. The manager's STALL_LIMIT bounds how many of these can happen.
	sleep_ms(1);
	return true;
}

} // namespace netplay
