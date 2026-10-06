// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
//
// Savestates: where they live, whether one exists, and the picker the lobby
// opens to choose one. See notes/netplay-ui.txt 5.4.

#ifndef MAME_OSD_WINUI_NETPLAYSTATE_H
#define MAME_OSD_WINUI_NETPLAYSTATE_H

// The absolute directory holding nGame's savestates. False if it cannot be
// composed, in which case out is untouched.
bool NetplayStateDir(int nGame, char *out, size_t cap);

// Is there a <dir>/<name>.sta? name is the bare one -state takes, so no
// extension and no path.
bool NetplayStateExists(int nGame, const char *name);

// Returns true and fills name with the bare state name the user chose.
bool NetplayPickState(HWND hParent, int nGame, char *name, size_t cap);

#endif // MAME_OSD_WINUI_NETPLAYSTATE_H
