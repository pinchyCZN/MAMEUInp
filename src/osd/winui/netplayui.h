// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
//
// Direct connect dialog. Phase 1 of notes/netplay-ui.txt: no lobby, no
// server traffic from the frontend, just the settings and a launch.

#ifndef MAME_OSD_WINUI_NETPLAYUI_H
#define MAME_OSD_WINUI_NETPLAYUI_H

struct netplay_launch
{
	char        host[64];
	unsigned    port;
	unsigned    players;
	unsigned    slot;
	unsigned    delay;
	unsigned    batch;
	bool        savestates;
	bool        send_config;
};

// Returns false if cancelled. nGame is the driver index, used for the
// caption and the ROM check.
bool NetplayDialog(HWND hParent, int nGame, netplay_launch *out);

// The emulator reads these from the environment, and MAMEUI runs it
// in-process - so they must be cleared afterwards or the next ordinary
// game starts trying to netplay.
void NetplayApply(const netplay_launch &cfg);
void NetplayClear(void);

#endif // MAME_OSD_WINUI_NETPLAYUI_H
