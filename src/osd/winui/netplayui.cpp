// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
//
// See netplayui.h.

// standard windows headers
#include <windows.h>
#include <windowsx.h>

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
static void verify_roms(HWND hDlg, int nGame)
{
	if (nGame < 0)
		return;

	SetDlgItemTextA(hDlg, IDC_NP_RESULT, "Checking...");

	driver_enumerator e(MameUIGlobal(), driver_list::driver(nGame));
	e.next();
	media_auditor auditor(e);
	const media_auditor::summary summary = auditor.audit_media(AUDIT_VALIDATE_FAST);

	std::string text;
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

	// The control is single-line-ish; CRLF so it reads in an edit box.
	std::string crlf;
	for (char ch : text)
	{
		if (ch == '\n')
			crlf += '\r';
		crlf += ch;
	}

	SetDlgItemTextA(hDlg, IDC_NP_RESULT, crlf.c_str());
	SetRomAuditResults(nGame, summary);
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
