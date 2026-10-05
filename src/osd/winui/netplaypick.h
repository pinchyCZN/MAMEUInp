// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
//
// The game picker the lobby's Select Game button opens. See
// notes/netplay-ui.txt 5.4.

#ifndef MAME_OSD_WINUI_NETPLAYPICK_H
#define MAME_OSD_WINUI_NETPLAYPICK_H

// Returns the chosen driver index, or -1 if cancelled. nCurrent is selected
// when the dialog opens, or -1 for none.
int NetplayPickGame(HWND hParent, int nCurrent);

#endif // MAME_OSD_WINUI_NETPLAYPICK_H
