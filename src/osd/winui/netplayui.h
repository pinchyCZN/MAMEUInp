// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
//
// Direct connect dialog. Phase 1 of notes/netplay-ui.txt: no lobby, no
// server traffic from the frontend, just the settings and a launch.

#ifndef MAME_OSD_WINUI_NETPLAYUI_H
#define MAME_OSD_WINUI_NETPLAYUI_H

#include <string>

struct netplay_launch
{
	int         game;          // driver index, -1 if none
	char        host[64];
	unsigned    port;
	unsigned    players;
	unsigned    slot;
	unsigned    delay;
	unsigned    batch;
	bool        savestates;
	bool        send_config;
	unsigned    autosave;

	// The bare name -state takes, "np01" - not a filename and not a
	// path. Empty is a cold boot. Not saved, like autosave.
	char        state[64];
	bool        netstats;
	char        cart[128];
};

// Returns false if cancelled. nGame is the driver index, used for the
// caption and the ROM check, and comes back in out->game.
bool NetplayDialog(HWND hParent, int nGame, netplay_launch *out);

// The emulator reads these from the environment, and MAMEUI runs it
// in-process - so they must be cleared afterwards or the next ordinary
// game starts trying to netplay.
void NetplayApply(const netplay_launch &cfg);
void NetplayClear(void);

// The image device instance name this system loads media into -
// "cart" on a cartridge console, "cdrm" on a CD one - or empty if
// it takes none. The launch needs it because an image option is
// named after its slot rather than being a fixed OPTION_ constant.
std::string NetplayMediaSlot(int drv);

// The bare media name resolved to a full path against this machine's ROM
// directories, or empty if it is not in any of them. MAME's image loader
// opens the path as given and does NOT search, so the launch needs this.
std::string NetplayMediaPath(const char *name);

// Phase 2: the lobby. Connect, see who else is here, chat. Starting a
// game is not its job yet; Direct connect is. Returns true when the
// user chose to launch, with the settings in out - the launch itself
// belongs to the caller, which is where MamePlayGameWithOptions lives.
// nSelected is the driver index selected in the main window, or -1. It is
// passed in rather than read here: Picker_GetSelectedItem needs the
// listview HWND, which only winui.cpp has.
bool NetplayLobbyDialog(HWND hParent, int nSelected, netplay_launch *out);

#endif // MAME_OSD_WINUI_NETPLAYUI_H
