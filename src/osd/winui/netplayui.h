// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
//
// Direct connect dialog. Phase 1 of notes/netplay-ui.txt: no lobby, no
// server traffic from the frontend, just the settings and a launch.

#ifndef MAME_OSD_WINUI_NETPLAYUI_H
#define MAME_OSD_WINUI_NETPLAYUI_H

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
};

// Returns false if cancelled. nGame is the driver index, used for the
// caption and the ROM check, and comes back in out->game.
bool NetplayDialog(HWND hParent, int nGame, netplay_launch *out);

// The emulator reads these from the environment, and MAMEUI runs it
// in-process - so they must be cleared afterwards or the next ordinary
// game starts trying to netplay.
void NetplayApply(const netplay_launch &cfg);
void NetplayClear(void);

// Phase 2: the lobby. Connect, see who else is here, chat. Starting a
// game is not its job yet; Direct connect is. Returns true when the
// user chose to launch, with the settings in out - the launch itself
// belongs to the caller, which is where MamePlayGameWithOptions lives.
// nSelected is the driver index selected in the main window, or -1. It is
// passed in rather than read here: Picker_GetSelectedItem needs the
// listview HWND, which only winui.cpp has.
bool NetplayLobbyDialog(HWND hParent, int nSelected, netplay_launch *out);

#endif // MAME_OSD_WINUI_NETPLAYUI_H
