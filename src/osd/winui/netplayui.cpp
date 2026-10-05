// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
//
// See netplayui.h.

// standard windows headers
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>

// standard C headers
#include <tchar.h>

// MAME/MAMEUI headers
#include "winui.h"
#include "audit.h"
#include "drivenum.h"
#include "resource.h"
#include "mui_opts.h"
#include "netplayui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

// Kept in our own file rather than MAMEUI.ini, so persistence costs no
// upstream edit. notes/netplay-ui.txt 7
#define NETPLAY_UI_INI  "netplay.ini"


static netplay_launch s_cfg;
static int s_game = -1;
static int s_selected = -1;    // the main window's selection

// The hosted game as the server last described it. One game at a time,
// so one copy of it.
static bool         s_g_active = false;
static std::string  s_g_driver;
static std::string  s_g_host;
static unsigned     s_g_count = 0;
static unsigned     s_g_players = 2;
static unsigned     s_g_delay = 6;
static unsigned     s_g_batch = 1;
static bool         s_g_savestates = true;
static bool         s_g_config = true;
static bool         s_g_mine = false;     // in the roster
static bool         s_g_amhost = false;


static void defaults(netplay_launch &c)
{
	std::strcpy(c.host, "127.0.0.1");
	c.port = 7000;
	c.players = 2;
	c.slot = 1;
	c.delay = 6;
	c.batch = 1;
	c.savestates = true;
	c.send_config = true;
	c.game = -1;
}


static void load_cfg(netplay_launch &c)
{
	defaults(c);

	FILE *f = std::fopen(NETPLAY_UI_INI, "r");
	if (f == nullptr)
		return;

	char line[256];
	while (std::fgets(line, sizeof(line), f) != nullptr)
	{
		char key[64], val[128];
		if (std::sscanf(line, "%63s %127s", key, val) != 2)
			continue;

		if (!std::strcmp(key, "host"))
			std::strncpy(c.host, val, sizeof(c.host) - 1);
		else if (!std::strcmp(key, "port"))
			c.port = unsigned(std::atoi(val));
		else if (!std::strcmp(key, "players"))
			c.players = unsigned(std::atoi(val));
		else if (!std::strcmp(key, "slot"))
			c.slot = unsigned(std::atoi(val));
		else if (!std::strcmp(key, "delay"))
			c.delay = unsigned(std::atoi(val));
		else if (!std::strcmp(key, "batch"))
			c.batch = unsigned(std::atoi(val));
		else if (!std::strcmp(key, "savestates"))
			c.savestates = (std::atoi(val) != 0);
		else if (!std::strcmp(key, "sendconfig"))
			c.send_config = (std::atoi(val) != 0);
	}
	std::fclose(f);
}


static void save_cfg(const netplay_launch &c)
{
	FILE *f = std::fopen(NETPLAY_UI_INI, "w");
	if (f == nullptr)
		return;

	std::fprintf(f, "host %s\n", c.host);
	std::fprintf(f, "port %u\n", c.port);
	std::fprintf(f, "players %u\n", c.players);
	std::fprintf(f, "slot %u\n", c.slot);
	std::fprintf(f, "delay %u\n", c.delay);
	std::fprintf(f, "batch %u\n", c.batch);
	std::fprintf(f, "savestates %d\n", c.savestates ? 1 : 0);
	std::fprintf(f, "sendconfig %d\n", c.send_config ? 1 : 0);
	std::fclose(f);
}


//-------------------------------------------------
//  the ROM check
//-------------------------------------------------

// Our own auditor rather than MameUIVerifyRomSet, which sends its detail
// to DetailsPrintf - the audit dialog's text box - where this cannot read
// it. The per-file list is the whole point: one wrong ROM in an otherwise
// working set should say which. notes/netplay-ui.txt 4
static media_auditor::summary audit_driver(int nGame, std::string &text)
{
	driver_enumerator e(MameUIGlobal(), driver_list::driver(nGame));
	e.next();
	media_auditor auditor(e);
	const media_auditor::summary summary = auditor.audit_media(AUDIT_VALIDATE_FAST);

	if (summary == media_auditor::NOTFOUND)
	{
		text = "ROM set not found.";
	}
	else
	{
		std::ostringstream out;
		auditor.summarize(driver_list::driver(nGame).name, &out);
		text = out.str();
		if (text.empty())
			text = "ROM set is good.";
	}

	SetRomAuditResults(nGame, summary);
	return summary;
}


// Good enough to launch. BEST_AVAILABLE is a set missing only what MAME will
// run without. NOTFOUND and INCORRECT would fail at startup, and failing
// there strands the other players in a session that never fills.
static bool audit_ok(media_auditor::summary s)
{
	return (s == media_auditor::CORRECT)
			|| (s == media_auditor::BEST_AVAILABLE)
			|| (s == media_auditor::NONE_NEEDED);
}


// The edit controls want CRLF.
static std::string crlf_of(const std::string &text)
{
	std::string out;
	for (char ch : text)
	{
		if (ch == '\n')
			out += '\r';
		out += ch;
	}
	return out;
}


static void verify_roms(HWND hDlg, int nGame)
{
	if (nGame < 0)
		return;

	SetDlgItemTextA(hDlg, IDC_NP_RESULT, "Checking...");

	std::string text;
	audit_driver(nGame, text);
	SetDlgItemTextA(hDlg, IDC_NP_RESULT, crlf_of(text).c_str());
}


//-------------------------------------------------
//  dialog
//-------------------------------------------------

static void to_dialog(HWND hDlg, const netplay_launch &c)
{
	SetDlgItemTextA(hDlg, IDC_NP_HOST, c.host);
	SetDlgItemInt(hDlg, IDC_NP_PORT, c.port, false);
	SetDlgItemInt(hDlg, IDC_NP_PLAYERS, c.players, false);
	SetDlgItemInt(hDlg, IDC_NP_SLOT, c.slot, false);
	SetDlgItemInt(hDlg, IDC_NP_DELAY, c.delay, false);
	SetDlgItemInt(hDlg, IDC_NP_BATCH, c.batch, false);
	CheckDlgButton(hDlg, IDC_NP_SAVESTATES, c.savestates ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hDlg, IDC_NP_SENDCONFIG, c.send_config ? BST_CHECKED : BST_UNCHECKED);
}


// Returns an error string, or nullptr if the values are usable.
static const char *from_dialog(HWND hDlg, netplay_launch &c)
{
	GetDlgItemTextA(hDlg, IDC_NP_HOST, c.host, sizeof(c.host));
	c.port = GetDlgItemInt(hDlg, IDC_NP_PORT, nullptr, false);
	c.players = GetDlgItemInt(hDlg, IDC_NP_PLAYERS, nullptr, false);
	c.slot = GetDlgItemInt(hDlg, IDC_NP_SLOT, nullptr, false);
	c.delay = GetDlgItemInt(hDlg, IDC_NP_DELAY, nullptr, false);
	c.batch = GetDlgItemInt(hDlg, IDC_NP_BATCH, nullptr, false);
	c.savestates = (IsDlgButtonChecked(hDlg, IDC_NP_SAVESTATES) == BST_CHECKED);
	c.send_config = (IsDlgButtonChecked(hDlg, IDC_NP_SENDCONFIG) == BST_CHECKED);

	if (c.host[0] == 0)
		return "Enter the server address.";
	if ((c.port < 1) || (c.port > 65535))
		return "Port must be 1 to 65535.";
	if ((c.players < 2) || (c.players > 10))
		return "Players must be 2 to 10.";
	if ((c.slot < 1) || (c.slot > c.players))
		return "Your player number must be within the player count.";
	if ((c.delay < 1) || (c.delay > 60))
		return "Delay must be 1 to 60 frames.";
	if ((c.batch < 1) || (c.batch > 8))
		return "Frames per packet must be 1 to 8.";

	// Delay 1 leaves no room for redundancy, so one lost packet ends the
	// session. protocol.txt 1.7.1
	if ((c.delay < 2) && std::strcmp(c.host, "127.0.0.1"))
		return "Delay 1 has no loss tolerance. Use 2 or more off this machine.";

	return nullptr;
}


static INT_PTR CALLBACK NetplayDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
		{
			to_dialog(hDlg, s_cfg);

			std::string cap("Netplay");
			if (s_game >= 0)
			{
				cap += " - ";
				cap += driver_list::driver(s_game).type.fullname();
				cap += " [";
				cap += driver_list::driver(s_game).name;
				cap += "]";
			}
			SetWindowTextA(hDlg, cap.c_str());
			SetDlgItemTextA(hDlg, IDC_NP_RESULT, "");
			return true;
		}

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_NP_VERIFY:
			verify_roms(hDlg, s_game);
			return true;

		case IDOK:
			{
				netplay_launch c;
				const char *const err = from_dialog(hDlg, c);
				if (err != nullptr)
				{
					MessageBoxA(hDlg, err, "Netplay", MB_OK | MB_ICONWARNING);
					return true;
				}
				c.game = s_game;
				s_cfg = c;
				save_cfg(s_cfg);
				EndDialog(hDlg, 1);
				return true;
			}

		case IDCANCEL:
			EndDialog(hDlg, 0);
			return true;
		}
		break;
	}
	return false;
}


bool NetplayDialog(HWND hParent, int nGame, netplay_launch *out)
{
	if (out == nullptr)
		return false;

	load_cfg(s_cfg);
	s_game = nGame;

	const INT_PTR r = DialogBox(GetModuleHandle(nullptr),
			MAKEINTRESOURCE(IDD_NETPLAY), hParent, NetplayDialogProc);
	if (r != 1)
		return false;

	*out = s_cfg;
	return true;
}


//-------------------------------------------------
//  handing the settings to the emulator
//-------------------------------------------------

// _putenv_s, not SetEnvironmentVariable: the emulator reads these with
// std::getenv, which sees the CRT's copy of the environment and not the
// Win32 block.
static void put(const char *name, const char *value)
{
	_putenv_s(name, value);
}


void NetplayApply(const netplay_launch &cfg)
{
	char buf[64];

	put("NETPLAY_MODE", "udp");
	put("NETPLAY_HOST", cfg.host);

	std::snprintf(buf, sizeof(buf), "%u", cfg.port);
	put("NETPLAY_PORT", buf);
	std::snprintf(buf, sizeof(buf), "%u", cfg.players);
	put("NETPLAY_PLAYERS", buf);
	std::snprintf(buf, sizeof(buf), "%u", cfg.slot);
	put("NETPLAY_SLOT", buf);
	std::snprintf(buf, sizeof(buf), "%u", cfg.delay);
	put("NETPLAY_DELAY", buf);
	std::snprintf(buf, sizeof(buf), "%u", cfg.batch);
	put("NETPLAY_BATCH", buf);

	put("NETPLAY_SAVESTATES", cfg.savestates ? "1" : "0");
	put("NETPLAY_SEND_CONFIG", cfg.send_config ? "1" : "0");

	// Full depth; the ceiling is derived from delay and batch.
	put("NETPLAY_REDUNDANCY", "99");

	std::snprintf(buf, sizeof(buf), "player %u", cfg.slot);
	put("NETPLAY_TAG", buf);
}


void NetplayClear(void)
{
	static const char *const vars[] =
	{
		"NETPLAY_MODE", "NETPLAY_HOST", "NETPLAY_PORT", "NETPLAY_PLAYERS",
		"NETPLAY_SLOT", "NETPLAY_DELAY", "NETPLAY_BATCH",
		"NETPLAY_SAVESTATES", "NETPLAY_SEND_CONFIG", "NETPLAY_REDUNDANCY",
		"NETPLAY_TAG"
	};

	for (unsigned i = 0; i < ARRAY_LENGTH(vars); i++)
		put(vars[i], "");
}


//-------------------------------------------------
//  the lobby: a TCP client on a dialog timer
//-------------------------------------------------

// No thread. A non-blocking socket polled from WM_TIMER keeps every socket
// touch on the UI thread, which is the whole reason there is nothing to
// synchronise here.

#define LOBBY_TIMER     1
#define LOBBY_TICK_MS   100

enum lobby_state { LOBBY_IDLE, LOBBY_CONNECTING, LOBBY_UP };

static SOCKET       s_sock = INVALID_SOCKET;
static lobby_state  s_state = LOBBY_IDLE;
static std::string  s_rx;
static std::string  s_name;

// Defined with the game panel below; lobby_close has to reach it.
static void game_enable(HWND hDlg);


static void lobby_status(HWND hDlg, const char *text)
{
	SetDlgItemTextA(hDlg, IDC_NPL_STATUS, text);
}


static void lobby_say(HWND hDlg, const std::string &line)
{
	const std::string text = line + "\r\n";
	const int end = GetWindowTextLengthA(GetDlgItem(hDlg, IDC_NPL_CHAT));
	SendDlgItemMessageA(hDlg, IDC_NPL_CHAT, EM_SETSEL, end, end);
	SendDlgItemMessageA(hDlg, IDC_NPL_CHAT, EM_REPLACESEL, false,
			reinterpret_cast<LPARAM>(text.c_str()));
}


static void lobby_close(HWND hDlg, const char *why)
{
	if (s_sock != INVALID_SOCKET)
	{
		closesocket(s_sock);
		s_sock = INVALID_SOCKET;
	}
	s_state = LOBBY_IDLE;
	s_rx.clear();

	SendDlgItemMessage(hDlg, IDC_NPL_PLAYERS, LB_RESETCONTENT, 0, 0);
	SendDlgItemMessage(hDlg, IDC_NPG_ROSTER, LVM_DELETEALLITEMS, 0, 0);
	SetDlgItemTextA(hDlg, IDC_NPL_COUNT, "");
	SetDlgItemTextA(hDlg, IDC_NPL_CONNECT, "&Connect");

	s_g_active = false;
	s_g_mine = false;
	s_g_amhost = false;
	s_g_count = 0;
	game_enable(hDlg);

	if (why != nullptr)
		lobby_status(hDlg, why);
}


static void lobby_send_line(const std::string &line)
{
	if (s_state != LOBBY_UP)
		return;

	const std::string out = line + "\n";
	send(s_sock, out.c_str(), int(out.size()), 0);
}


static void lobby_open(HWND hDlg)
{
	char host[64];
	GetDlgItemTextA(hDlg, IDC_NPL_HOST, host, sizeof(host));
	const unsigned port = GetDlgItemInt(hDlg, IDC_NPL_PORT, nullptr, false);

	char name[32];
	GetDlgItemTextA(hDlg, IDC_NPL_NAME, name, sizeof(name));
	if (name[0] == 0)
		std::strcpy(name, "player");
	s_name = name;

	if ((host[0] == 0) || (port < 1) || (port > 65535))
	{
		lobby_status(hDlg, "Enter a server address and port.");
		return;
	}

	// Started, never cleaned up: Winsock is reference counted and the
	// emulator shares it, so a WSACleanup here would pull it out from
	// under a running game.
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s_sock == INVALID_SOCKET)
	{
		lobby_status(hDlg, "Could not create a socket.");
		return;
	}

	u_long nb = 1;
	ioctlsocket(s_sock, FIONBIO, &nb);

	struct sockaddr_in a;
	std::memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(u16(port));
	// getaddrinfo takes a literal address as well as a name, so there is
	// no separate parse path.
	struct addrinfo hints, *res = nullptr;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if ((getaddrinfo(host, nullptr, &hints, &res) != 0) || (res == nullptr))
	{
		lobby_close(hDlg, "Could not resolve that address.");
		return;
	}
	a.sin_addr = reinterpret_cast<struct sockaddr_in *>(res->ai_addr)->sin_addr;
	freeaddrinfo(res);

	connect(s_sock, reinterpret_cast<struct sockaddr *>(&a), sizeof(a));
	s_state = LOBBY_CONNECTING;
	SetDlgItemTextA(hDlg, IDC_NPL_CONNECT, "&Disconnect");
	lobby_status(hDlg, "Connecting...");
}


//-------------------------------------------------
//  the hosted game panel
//-------------------------------------------------

static void game_result(HWND hDlg, const std::string &text)
{
	SetDlgItemTextA(hDlg, IDC_NPG_RESULT, crlf_of(text).c_str());
}


// "delay=6" -> 6. Same reader as the server's; the lines are symmetric.
static unsigned kv(const std::string &s, const char *key, unsigned fallback)
{
	const std::string want = std::string(key) + "=";
	size_t at = s.find(want);
	while (at != std::string::npos)
	{
		if ((at == 0) || (s[at - 1] == ' '))
			return unsigned(std::atoi(s.c_str() + at + want.size()));
		at = s.find(want, at + 1);
	}
	return fallback;
}


// Exactly LOBBY_ROWS rows. Measured rather than set in the template: the item
// height follows the font and the DPI, so a height in dialog units that shows
// four rows here shows three or six somewhere else.
#define LOBBY_ROWS 4

static void size_player_list(HWND hDlg)
{
	HWND const lb = GetDlgItem(hDlg, IDC_NPL_PLAYERS);
	HWND const count = GetDlgItem(hDlg, IDC_NPL_COUNT);
	if ((lb == nullptr) || (count == nullptr))
		return;

	const int ih = int(SendMessage(lb, LB_GETITEMHEIGHT, 0, 0));
	if (ih <= 0)
		return;

	RECT r;
	GetWindowRect(lb, &r);
	POINT tl = { r.left, r.top };
	ScreenToClient(hDlg, &tl);

	const int h = (ih * LOBBY_ROWS) + (GetSystemMetrics(SM_CYEDGE) * 2);
	SetWindowPos(lb, nullptr, 0, 0, r.right - r.left, h,
			SWP_NOMOVE | SWP_NOZORDER);

	RECT cr;
	GetWindowRect(count, &cr);
	POINT cp = { cr.left, cr.top };
	ScreenToClient(hDlg, &cp);
	SetWindowPos(count, nullptr, cp.x, tl.y + h + 4, 0, 0,
			SWP_NOSIZE | SWP_NOZORDER);
}


static void roster_init(HWND hDlg)
{
	HWND const lv = GetDlgItem(hDlg, IDC_NPG_ROSTER);

	// W messages: the build is UNICODE, so the control is in Unicode
	// format and the ANSI variants hand it text it reads as wide.
	LVCOLUMNW col;
	std::memset(&col, 0, sizeof(col));
	col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

	col.iSubItem = 0;
	col.cx = 36;
	col.pszText = const_cast<wchar_t *>(L"Slot");
	SendMessageW(lv, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));

	col.iSubItem = 1;
	col.cx = 180;
	col.pszText = const_cast<wchar_t *>(L"Player");
	SendMessageW(lv, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));

	SendMessage(lv, LVM_SETEXTENDEDLISTVIEWSTYLE,
			LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
}


static void roster_set(HWND hDlg, const std::string &csv)
{
	HWND const lv = GetDlgItem(hDlg, IDC_NPG_ROSTER);
	SendMessage(lv, LVM_DELETEALLITEMS, 0, 0);

	s_g_mine = false;
	s_g_amhost = false;
	s_g_count = 0;

	size_t at = 0;
	while (at <= csv.size())
	{
		const size_t comma = csv.find(',', at);
		const std::string one = csv.substr(at,
				(comma == std::string::npos) ? std::string::npos : comma - at);
		if (!one.empty())
		{
			wchar_t slot[16];
			_snwprintf(slot, 16, L"%u", s_g_count + 1);

			std::wstring wide(one.begin(), one.end());

			LVITEMW it;
			std::memset(&it, 0, sizeof(it));
			it.mask = LVIF_TEXT;
			it.iItem = int(s_g_count);
			it.pszText = slot;
			SendMessageW(lv, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));

			it.iSubItem = 1;
			it.pszText = const_cast<wchar_t *>(wide.c_str());
			SendMessageW(lv, LVM_SETITEMTEXTW, s_g_count,
					reinterpret_cast<LPARAM>(&it));

			if (one == s_name)
			{
				s_g_mine = true;
				s_g_amhost = (s_g_count == 0);
			}
			s_g_count++;
		}
		if (comma == std::string::npos)
			break;
		at = comma + 1;
	}
}


// Which buttons make sense right now. Driven by server state only, so a stale
// local idea of the game cannot enable a button the server will refuse.
static void game_enable(HWND hDlg)
{
	const bool up = (s_state == LOBBY_UP);
	const bool mine = (up && s_g_active && s_g_mine);

	EnableWindow(GetDlgItem(hDlg, IDC_NPG_HOST), up && !s_g_active);
	EnableWindow(GetDlgItem(hDlg, IDC_NPG_JOIN),
			up && s_g_active && !s_g_mine && (s_g_count < s_g_players));
	EnableWindow(GetDlgItem(hDlg, IDC_NPG_LEAVE), mine);
	EnableWindow(GetDlgItem(hDlg, IDC_NPG_START),
			mine && s_g_amhost && (s_g_count >= s_g_players));

	// A guest editing the session settings would be editing a display: the
	// host chose them and the server already has them.
	const bool editable = (!s_g_active || s_g_amhost);
	// Left enabled on purpose: a disabled edit draws its text in grey,
	// which is unreadable on a dark theme, and Host game being disabled
	// already makes the box inert while a game is up.
	EnableWindow(GetDlgItem(hDlg, IDC_NPG_USESEL), !s_g_active);
	EnableWindow(GetDlgItem(hDlg, IDC_NPG_PLAYERS), editable);
	EnableWindow(GetDlgItem(hDlg, IDC_NPG_DELAY), editable);
	EnableWindow(GetDlgItem(hDlg, IDC_NPG_BATCH), editable);
	EnableWindow(GetDlgItem(hDlg, IDC_NPG_SAVESTATES), editable);
	EnableWindow(GetDlgItem(hDlg, IDC_NPG_SENDCONFIG), editable);
}


static void show_fullname(HWND hDlg, const std::string &driver)
{
	const int idx = driver.empty() ? -1 : driver_list::find(driver.c_str());
	if (idx < 0)
	{
		SetDlgItemTextA(hDlg, IDC_NPG_FULLNAME,
				driver.empty() ? "" : "unknown system");
		return;
	}
	SetDlgItemTextA(hDlg, IDC_NPG_FULLNAME,
			driver_list::driver(idx).type.fullname());
}


static void game_show(HWND hDlg)
{
	if (!s_g_active)
	{
		SendDlgItemMessage(hDlg, IDC_NPG_ROSTER, LVM_DELETEALLITEMS, 0, 0);
		game_enable(hDlg);
		return;
	}

	SetDlgItemTextA(hDlg, IDC_NPG_DRIVER, s_g_driver.c_str());
	show_fullname(hDlg, s_g_driver);
	SetDlgItemInt(hDlg, IDC_NPG_PLAYERS, s_g_players, false);
	SetDlgItemInt(hDlg, IDC_NPG_DELAY, s_g_delay, false);
	SetDlgItemInt(hDlg, IDC_NPG_BATCH, s_g_batch, false);
	CheckDlgButton(hDlg, IDC_NPG_SAVESTATES,
			s_g_savestates ? BST_CHECKED : BST_UNCHECKED);
	CheckDlgButton(hDlg, IDC_NPG_SENDCONFIG,
			s_g_config ? BST_CHECKED : BST_UNCHECKED);
	game_enable(hDlg);
}


// The audit IS the join button: nothing reaches the server until the ROMs are
// known good, so nobody takes a slot in a game they cannot run.
// notes/netplay-ui.txt 4
static bool rom_gate(HWND hDlg, const std::string &driver, int &idx)
{
	idx = driver.empty() ? -1 : driver_list::find(driver.c_str());
	if (idx < 0)
	{
		game_result(hDlg, "No such system: " + driver);
		return false;
	}

	game_result(hDlg, "Checking ROMs...");
	std::string text;
	const media_auditor::summary sum = audit_driver(idx, text);
	game_result(hDlg, text);
	return audit_ok(sum);
}


static void game_host(HWND hDlg)
{
	char driver[32];
	GetDlgItemTextA(hDlg, IDC_NPG_DRIVER, driver, sizeof(driver));

	int idx = -1;
	if (!rom_gate(hDlg, driver, idx))
		return;

	char line[256];
	std::snprintf(line, sizeof(line),
			"CREATE %s players=%u delay=%u batch=%u savestates=%u config=%u",
			driver,
			GetDlgItemInt(hDlg, IDC_NPG_PLAYERS, nullptr, false),
			GetDlgItemInt(hDlg, IDC_NPG_DELAY, nullptr, false),
			GetDlgItemInt(hDlg, IDC_NPG_BATCH, nullptr, false),
			(IsDlgButtonChecked(hDlg, IDC_NPG_SAVESTATES) == BST_CHECKED) ? 1u : 0u,
			(IsDlgButtonChecked(hDlg, IDC_NPG_SENDCONFIG) == BST_CHECKED) ? 1u : 0u);
	lobby_send_line(line);
}


static void game_join(HWND hDlg)
{
	int idx = -1;
	if (!rom_gate(hDlg, s_g_driver, idx))
		return;
	lobby_send_line("JOIN");
}


// Built from the server's LAUNCH, not from this dialog's controls: the host's
// settings win, and a guest is never told what to type.
static bool game_launch(HWND hDlg, const std::string &arg)
{
	char driver[64] = { 0 };
	const size_t at = arg.find("game=");
	if (at != std::string::npos)
	{
		size_t n = 0;
		for (size_t j = at + 5; (j < arg.size()) && (arg[j] != ' ')
				&& (n < (sizeof(driver) - 1)); j++)
			driver[n++] = arg[j];
	}

	int idx = -1;
	if (!rom_gate(hDlg, driver, idx))
	{
		// Say so rather than leaving a slot held by somebody who cannot run
		// the game; the host would wait on a peer who is never coming.
		lobby_send_line("LEAVE");
		return false;
	}

	netplay_launch c = s_cfg;
	c.game = idx;
	c.players = kv(arg, "players", 2);
	c.slot = kv(arg, "slot", 1);
	c.delay = kv(arg, "delay", 6);
	c.batch = kv(arg, "batch", 1);
	c.savestates = (kv(arg, "savestates", 1) != 0);
	c.send_config = (kv(arg, "config", 1) != 0);

	// The lobby and the relay are the same machine and port.
	GetDlgItemTextA(hDlg, IDC_NPL_HOST, c.host, sizeof(c.host));
	c.port = GetDlgItemInt(hDlg, IDC_NPL_PORT, nullptr, false);

	s_cfg = c;
	save_cfg(s_cfg);
	return true;
}


static void lobby_handle(HWND hDlg, const std::string &line)
{
	const size_t sp = line.find(' ');
	const std::string cmd = line.substr(0, sp);
	const std::string arg = (sp == std::string::npos)
			? std::string() : line.substr(sp + 1);

	if (cmd == "YOUARE")
	{
		s_name = arg;
		SetDlgItemTextA(hDlg, IDC_NPL_NAME, arg.c_str());
		lobby_status(hDlg, ("Connected as " + arg).c_str());
		return;
	}

	if (cmd == "PLAYERS")
	{
		SendDlgItemMessage(hDlg, IDC_NPL_PLAYERS, LB_RESETCONTENT, 0, 0);
		unsigned seen = 0;
		size_t at = 0;
		while (at <= arg.size())
		{
			const size_t comma = arg.find(',', at);
			const std::string one = arg.substr(at,
					(comma == std::string::npos) ? std::string::npos : comma - at);
			if (!one.empty())
			{
				SendDlgItemMessageA(hDlg, IDC_NPL_PLAYERS, LB_ADDSTRING, 0,
						reinterpret_cast<LPARAM>(one.c_str()));
				seen++;
			}
			if (comma == std::string::npos)
				break;
			at = comma + 1;
		}

		char note[64];
		std::snprintf(note, sizeof(note), "%u here", seen);
		SetDlgItemTextA(hDlg, IDC_NPL_COUNT, note);
		return;
	}

	if (cmd == "CHAT")
	{
		const size_t sp2 = arg.find(' ');
		if (sp2 == std::string::npos)
			return;
		lobby_say(hDlg, "<" + arg.substr(0, sp2) + "> " + arg.substr(sp2 + 1));
		return;
	}

	if (cmd == "GAME")
	{
		const size_t sp2 = arg.find(' ');
		s_g_active = true;
		s_g_driver = arg.substr(0, sp2);
		s_g_players = kv(arg, "players", 2);
		s_g_delay = kv(arg, "delay", 6);
		s_g_batch = kv(arg, "batch", 1);
		s_g_savestates = (kv(arg, "savestates", 1) != 0);
		s_g_config = (kv(arg, "config", 1) != 0);

		// "host=name" and "2/4" are for the eye; the roster is the truth
		// about who is in and in what slot.
		const size_t h = arg.find("host=");
		if (h != std::string::npos)
		{
			const size_t e = arg.find(' ', h);
			s_g_host = arg.substr(h + 5,
					(e == std::string::npos) ? std::string::npos : e - h - 5);
		}

		game_show(hDlg);
		return;
	}

	if (cmd == "ROSTER")
	{
		roster_set(hDlg, arg);
		game_enable(hDlg);
		return;
	}

	if (cmd == "NOGAME")
	{
		s_g_active = false;
		s_g_mine = false;
		s_g_amhost = false;
		s_g_count = 0;
		s_g_driver.clear();
		s_g_host.clear();

		// The system edit is left as it is: with no game hosted it holds
		// what this player is about to host, not a stale server value.
		game_show(hDlg);
		return;
	}

	if (cmd == "LAUNCH")
	{
		if (!game_launch(hDlg, arg))
			return;

		// Leaving the lobby to play. The relay runs the session from here,
		// and the server has already forgotten the game.
		lobby_send_line("BYE");
		lobby_close(hDlg, nullptr);
		KillTimer(hDlg, LOBBY_TIMER);
		EndDialog(hDlg, 2);
		return;
	}

	if (cmd == "ERROR")
	{
		lobby_say(hDlg, "* " + arg);
		game_result(hDlg, arg);
		return;
	}
}


static void lobby_poll(HWND hDlg)
{
	if (s_state == LOBBY_IDLE)
		return;

	fd_set rd, wr, ex;
	FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
	FD_SET(s_sock, &rd);
	FD_SET(s_sock, &wr);
	FD_SET(s_sock, &ex);

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	if (select(0, &rd, &wr, &ex, &tv) <= 0)
		return;

	if (FD_ISSET(s_sock, &ex))
	{
		lobby_close(hDlg, "Could not connect.");
		return;
	}

	if (s_state == LOBBY_CONNECTING)
	{
		if (!FD_ISSET(s_sock, &wr))
			return;
		s_state = LOBBY_UP;
		lobby_status(hDlg, "Connected.");
		lobby_send_line("HELLO " + s_name);
		return;
	}

	if (!FD_ISSET(s_sock, &rd))
		return;

	char buf[1024];
	const int n = recv(s_sock, buf, int(sizeof(buf)), 0);
	if (n == 0)
	{
		lobby_close(hDlg, "The server closed the connection.");
		return;
	}
	if (n < 0)
	{
		if (WSAGetLastError() != WSAEWOULDBLOCK)
			lobby_close(hDlg, "Connection lost.");
		return;
	}

	s_rx.append(buf, size_t(n));
	for ( ; ; )
	{
		const size_t nl = s_rx.find('\n');
		if (nl == std::string::npos)
			break;

		std::string line = s_rx.substr(0, nl);
		s_rx.erase(0, nl + 1);
		if (!line.empty() && (line[line.size() - 1] == '\r'))
			line.resize(line.size() - 1);
		if (!line.empty())
			lobby_handle(hDlg, line);
	}
}


static void lobby_send_chat(HWND hDlg)
{
	char text[256];
	GetDlgItemTextA(hDlg, IDC_NPL_SAY, text, sizeof(text));
	if (text[0] == 0)
		return;

	if (s_state != LOBBY_UP)
	{
		lobby_status(hDlg, "Not connected.");
		return;
	}

	lobby_send_line(std::string("CHAT ") + text);
	SetDlgItemTextA(hDlg, IDC_NPL_SAY, "");
}


static INT_PTR CALLBACK LobbyDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
		load_cfg(s_cfg);
		SetDlgItemTextA(hDlg, IDC_NPL_HOST, s_cfg.host);
		SetDlgItemInt(hDlg, IDC_NPL_PORT, s_cfg.port, false);
		SetDlgItemTextA(hDlg, IDC_NPL_NAME, s_name.empty() ? "player" : s_name.c_str());
		SendDlgItemMessage(hDlg, IDC_NPL_SAY, EM_SETLIMITTEXT, 200, 0);

		roster_init(hDlg);
		size_player_list(hDlg);
		SetDlgItemInt(hDlg, IDC_NPG_PLAYERS, s_cfg.players, false);
		SetDlgItemInt(hDlg, IDC_NPG_DELAY, s_cfg.delay, false);
		SetDlgItemInt(hDlg, IDC_NPG_BATCH, s_cfg.batch, false);
		CheckDlgButton(hDlg, IDC_NPG_SAVESTATES,
				s_cfg.savestates ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hDlg, IDC_NPG_SENDCONFIG,
				s_cfg.send_config ? BST_CHECKED : BST_UNCHECKED);

		// Seed the system from the main window selection, so the common
		// case is Connect then Host game.
		if (s_selected >= 0)
		{
			SetDlgItemTextA(hDlg, IDC_NPG_DRIVER, driver_list::driver(s_selected).name);
			show_fullname(hDlg, driver_list::driver(s_selected).name);
		}

		game_enable(hDlg);
		SetTimer(hDlg, LOBBY_TIMER, LOBBY_TICK_MS, nullptr);
		return true;

	case WM_TIMER:
		if (wParam == LOBBY_TIMER)
			lobby_poll(hDlg);
		return true;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_NPL_CONNECT:
			if (s_state == LOBBY_IDLE)
			{
				// Remember the address even if the connect fails.
				GetDlgItemTextA(hDlg, IDC_NPL_HOST, s_cfg.host, sizeof(s_cfg.host));
				s_cfg.port = GetDlgItemInt(hDlg, IDC_NPL_PORT, nullptr, false);
				save_cfg(s_cfg);
				lobby_open(hDlg);
			}
			else
			{
				lobby_send_line("BYE");
				lobby_close(hDlg, "Disconnected.");
			}
			return true;

		case IDC_NPL_DIRECT:
			{
				// Whatever is in the System box, so Direct connect follows the
				// same pick as Host game.
				char pick[32];
				GetDlgItemTextA(hDlg, IDC_NPG_DRIVER, pick, sizeof(pick));
				const int nGame = (pick[0] != 0)
						? driver_list::find(pick) : s_selected;
				if (nGame < 0)
				{
					lobby_status(hDlg, "Pick a system first.");
					return true;
				}
				netplay_launch cfg;
				if (NetplayDialog(hDlg, nGame, &cfg))
				{
					if (s_state != LOBBY_IDLE)
						lobby_send_line("BYE");
					lobby_close(hDlg, nullptr);
					KillTimer(hDlg, LOBBY_TIMER);
					EndDialog(hDlg, 2);     // the caller launches
				}
				return true;
			}

		case IDC_NPG_USESEL:
			if (s_selected < 0)
			{
				game_result(hDlg, "Select a system in the main window first.");
				return true;
			}
			SetDlgItemTextA(hDlg, IDC_NPG_DRIVER, driver_list::driver(s_selected).name);
			show_fullname(hDlg, driver_list::driver(s_selected).name);
			return true;

		case IDC_NPG_HOST:
			game_host(hDlg);
			return true;

		case IDC_NPG_JOIN:
			game_join(hDlg);
			return true;

		case IDC_NPG_LEAVE:
			lobby_send_line("LEAVE");
			return true;

		case IDC_NPG_START:
			lobby_send_line("START");
			return true;

		case IDOK:
			lobby_send_chat(hDlg);
			return true;

		case IDCANCEL:
			if (s_state != LOBBY_IDLE)
				lobby_send_line("BYE");
			lobby_close(hDlg, nullptr);
			KillTimer(hDlg, LOBBY_TIMER);
			EndDialog(hDlg, 0);
			return true;
		}
		break;

	case WM_DESTROY:
		KillTimer(hDlg, LOBBY_TIMER);
		break;
	}
	return false;
}


bool NetplayLobbyDialog(HWND hParent, int nSelected, netplay_launch *out)
{
	s_selected = nSelected;

	const INT_PTR r = DialogBox(GetModuleHandle(nullptr),
			MAKEINTRESOURCE(IDD_NETPLAY_LOBBY), hParent, LobbyDialogProc);
	if ((r != 2) || (out == nullptr))
		return false;

	*out = s_cfg;
	return true;
}
