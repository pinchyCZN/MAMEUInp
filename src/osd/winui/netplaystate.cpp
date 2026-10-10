// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
//
// See netplaystate.h.
//
// The thumbnail beside each state is one WE wrote: netplay_manager
// take_snapshot puts np01.png next to np01.sta. A state saved by hand with
// the save-state key has no image at all, so the preview has to say so
// rather than show the last one.

// standard windows headers
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

// standard C headers
#include <tchar.h>

// MAME/MAMEUI headers
#include "winui.h"
#include "drivenum.h"
#include "resource.h"
#include "mui_opts.h"
#include "screenshot.h"
#include "netplaystate.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>


namespace {

struct state_entry
{
	std::string name;       // bare, no extension
	std::string when;       // for the eye
	ULONGLONG   stamp;      // for the sort
};

std::vector<state_entry>    s_states;
int                         s_game = -1;
HBITMAP                     s_preview = nullptr;
int                         s_preview_w = 0;
int                         s_preview_h = 0;
std::string                 s_dir;
std::string                 s_chosen;


// MAME composes <state_directory>/<statename>/<name>.sta, where statename is
// the state_name option with %g standing for the system - machine.cpp
// get_statename. Only %g is expanded here; the other code is %d_ for image
// devices, which no arcade driver produces.
std::string state_folder(int drv)
{
	std::string tpl = MameUIGlobal().state_name();
	if (tpl.empty())
		tpl = "%g";

	const size_t dot = tpl.find_last_of('.');
	if (dot != std::string::npos)
		tpl.erase(dot);

	const size_t g = tpl.find("%g");
	if (g != std::string::npos)
		tpl.replace(g, 2, driver_list::driver(drv).name);

	// Anything still unexpanded needs a running machine to resolve. The
	// default is the system name, and that is where a state of ours lives.
	if (tpl.find('%') != std::string::npos)
		tpl = driver_list::driver(drv).name;

	return tpl;
}


void free_preview()
{
	if (s_preview != nullptr)
	{
		DeleteObject(s_preview);
		s_preview = nullptr;
	}
	s_preview_w = 0;
	s_preview_h = 0;
}


// Absolute, because the path is shown to the user to copy into an explorer
// window and the option is usually relative to the working directory.
std::string absolute(const std::string &rel)
{
	char full[MAX_PATH];
	const DWORD n = GetFullPathNameA(rel.c_str(), MAX_PATH, full, nullptr);
	return ((n != 0) && (n < MAX_PATH)) ? std::string(full) : rel;
}


void scan(int drv)
{
	s_states.clear();
	if (drv < 0)
		return;

	const std::string pattern = s_dir + "\\*.sta";

	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return;

	do
	{
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			continue;

		state_entry e;
		e.name = fd.cFileName;
		const size_t dot = e.name.find_last_of('.');
		if (dot != std::string::npos)
			e.name.erase(dot);

		ULARGE_INTEGER u;
		u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
		u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
		e.stamp = u.QuadPart;

		FILETIME local;
		SYSTEMTIME st;
		char buf[64] = "";
		if (FileTimeToLocalFileTime(&fd.ftLastWriteTime, &local)
				&& FileTimeToSystemTime(&local, &st))
		{
			std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
					st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
					st.wSecond);
		}
		e.when = buf;

		s_states.push_back(e);
	}
	while (FindNextFileA(h, &fd));
	FindClose(h);

	// Newest first: the rotation overwrites np01 again once it wraps, so
	// the highest number is not the most recent one.
	std::sort(s_states.begin(), s_states.end(),
			[] (const state_entry &a, const state_entry &b)
			{ return a.stamp > b.stamp; });
}


void load_preview(HWND hDlg, int index)
{
	free_preview();

	if ((index >= 0) && (index < int(s_states.size())))
	{
		const std::string png = s_dir + "\\" + s_states[index].name + ".png";

		util::core_file::ptr file;
		if (util::core_file::open(png.c_str(), OPEN_FLAG_READ, file)
				== osd_file::error::NONE)
		{
			HGLOBAL dib = nullptr;
			HPALETTE pal = nullptr;
			if (png_read_bitmap_gui(*file, &dib, &pal))
			{
				MYBITMAPINFO desc;
				std::memset(&desc, 0, sizeof(desc));

				HDC hdc = GetDC(hDlg);
				s_preview = DIBToDDB(hdc, dib, &desc);
				ReleaseDC(hDlg, hdc);

				s_preview_w = desc.bmWidth;
				s_preview_h = desc.bmHeight;
				GlobalFree(dib);
			}
			if (pal != nullptr)
				DeleteObject(pal);
			file.reset();
		}
	}

	InvalidateRect(GetDlgItem(hDlg, IDC_NPS_PREVIEW), nullptr, TRUE);
}


void draw_preview(LPDRAWITEMSTRUCT di)
{
	RECT r = di->rcItem;
	FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));

	const int w = r.right - r.left;
	const int h = r.bottom - r.top;

	if ((s_preview == nullptr) || (s_preview_w <= 0) || (s_preview_h <= 0))
	{
		SetBkMode(di->hDC, TRANSPARENT);
		DrawTextA(di->hDC, "No image", -1, &r,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		return;
	}

	// Letterboxed rather than stretched: a squashed screenshot is harder to
	// recognise than a smaller one, and recognising it is the whole point.
	int dw = w;
	int dh = (w * s_preview_h) / s_preview_w;
	if (dh > h)
	{
		dh = h;
		dw = (h * s_preview_w) / s_preview_h;
	}

	HDC mem = CreateCompatibleDC(di->hDC);
	HBITMAP old = (HBITMAP)SelectObject(mem, s_preview);
	SetStretchBltMode(di->hDC, HALFTONE);
	StretchBlt(di->hDC, r.left + ((w - dw) / 2), r.top + ((h - dh) / 2),
			dw, dh, mem, 0, 0, s_preview_w, s_preview_h, SRCCOPY);
	SelectObject(mem, old);
	DeleteDC(mem);
}


void show_path(HWND hDlg, int index)
{
	std::string text = s_dir;
	if ((index >= 0) && (index < int(s_states.size())))
	{
		text += "\\";
		text += s_states[index].name;
		text += ".sta";
	}
	SetDlgItemTextA(hDlg, IDC_NPS_PATH, text.c_str());
}


int selected(HWND hDlg)
{
	HWND const lv = GetDlgItem(hDlg, IDC_NPS_LIST);
	return int(SendMessage(lv, LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED));
}


void take(HWND hDlg)
{
	const int i = selected(hDlg);
	if ((i < 0) || (i >= int(s_states.size())))
		return;

	s_chosen = s_states[i].name;
	EndDialog(hDlg, 1);
}


void fill(HWND hDlg)
{
	HWND const lv = GetDlgItem(hDlg, IDC_NPS_LIST);

	// W messages throughout: the build is UNICODE, so the control is in
	// Unicode format and the ANSI variants hand it text it reads as wide.
	LVCOLUMNW col;
	std::memset(&col, 0, sizeof(col));
	col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

	col.iSubItem = 0;
	col.cx = 90;
	col.pszText = const_cast<wchar_t *>(L"State");
	SendMessageW(lv, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));

	col.iSubItem = 1;
	col.cx = 150;
	col.pszText = const_cast<wchar_t *>(L"Saved");
	SendMessageW(lv, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));

	SendMessage(lv, LVM_SETEXTENDEDLISTVIEWSTYLE,
			LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);

	for (size_t i = 0; i < s_states.size(); i++)
	{
		const std::wstring name(s_states[i].name.begin(), s_states[i].name.end());
		const std::wstring when(s_states[i].when.begin(), s_states[i].when.end());

		LVITEMW it;
		std::memset(&it, 0, sizeof(it));
		it.mask = LVIF_TEXT;
		it.iItem = int(i);
		it.pszText = const_cast<wchar_t *>(name.c_str());
		SendMessageW(lv, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));

		it.iSubItem = 1;
		it.pszText = const_cast<wchar_t *>(when.c_str());
		SendMessageW(lv, LVM_SETITEMTEXTW, i, reinterpret_cast<LPARAM>(&it));
	}
}


INT_PTR CALLBACK StateDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
		{
			std::string cap("Select Savestate");
			if (s_game >= 0)
			{
				cap += " - ";
				cap += driver_list::driver(s_game).name;
			}
			SetWindowTextA(hDlg, cap.c_str());

			fill(hDlg);
			show_path(hDlg, -1);        // the directory, until something is picked

			if (!s_states.empty())
			{
				LVITEMW it;
				std::memset(&it, 0, sizeof(it));
				it.mask = LVIF_STATE;
				it.state = LVIS_SELECTED | LVIS_FOCUSED;
				it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
				SendMessageW(GetDlgItem(hDlg, IDC_NPS_LIST), LVM_SETITEMSTATE,
						0, reinterpret_cast<LPARAM>(&it));
			}
			else
			{
				EnableWindow(GetDlgItem(hDlg, IDOK), false);
			}
			return true;
		}

	case WM_DRAWITEM:
		if (wParam == IDC_NPS_PREVIEW)
		{
			draw_preview(reinterpret_cast<LPDRAWITEMSTRUCT>(lParam));
			return true;
		}
		break;

	case WM_NOTIFY:
		{
			LPNMHDR const nm = reinterpret_cast<LPNMHDR>(lParam);
			if (nm->idFrom != IDC_NPS_LIST)
				break;

			if (nm->code == LVN_ITEMCHANGED)
			{
				LPNMLISTVIEW const lv = reinterpret_cast<LPNMLISTVIEW>(lParam);
				if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED))
				{
					load_preview(hDlg, lv->iItem);
					show_path(hDlg, lv->iItem);
				}
				return true;
			}

			if (nm->code == NM_DBLCLK)
			{
				take(hDlg);
				return true;
			}
		}
		break;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
			take(hDlg);
			return true;

		case IDCANCEL:
			EndDialog(hDlg, 0);
			return true;
		}
		break;

	case WM_DESTROY:
		free_preview();
		break;
	}
	return false;
}

} // anonymous namespace


bool NetplayStateDir(int nGame, char *out, size_t cap)
{
	if ((nGame < 0) || (out == nullptr) || (cap == 0))
		return false;

	std::string dir = MameUIGlobal().state_directory();
	dir += "\\";
	dir += state_folder(nGame);
	dir = absolute(dir);

	if (dir.size() >= cap)
		return false;

	std::strcpy(out, dir.c_str());
	return true;
}


bool NetplayStateExists(int nGame, const char *name)
{
	if ((nGame < 0) || (name == nullptr) || (name[0] == 0))
		return false;

	char dir[MAX_PATH];
	if (!NetplayStateDir(nGame, dir, sizeof(dir)))
		return false;

	std::string path(dir);
	path += "\\";
	path += name;
	path += ".sta";

	const DWORD attr = GetFileAttributesA(path.c_str());
	return (attr != INVALID_FILE_ATTRIBUTES)
			&& ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
}


bool NetplayPickState(HWND hParent, int nGame, char *name, size_t cap)
{
	if ((nGame < 0) || (name == nullptr) || (cap == 0))
		return false;

	char dir[MAX_PATH];
	if (!NetplayStateDir(nGame, dir, sizeof(dir)))
		return false;

	s_game = nGame;
	s_dir = dir;
	s_chosen.clear();
	scan(nGame);

	const INT_PTR r = DialogBox(GetModuleHandle(nullptr),
			MAKEINTRESOURCE(IDD_NETPLAY_STATES), hParent, StateDialogProc);

	free_preview();
	s_states.clear();

	if ((r != 1) || s_chosen.empty() || (s_chosen.size() >= cap))
		return false;

	std::strcpy(name, s_chosen.c_str());
	return true;
}
