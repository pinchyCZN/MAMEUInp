// license:BSD-3-Clause
// copyright-holders:MAMEUI Netplay
//
// See netplaypick.h.
//
// The list is MAMEUI's own picker control (picker.cpp) attached to a listview
// of ours. SetupPicker subclasses any listview and routes every decision back
// through the callback table below, so we get header sorting, the three view
// modes and the clone grouping without inheriting the main window's folder
// tree, filter flags, icon cache or column-customise menu.
//
// GROUPED VIEW IS A SORT ORDER AND A DRAWING INDENT, NOT A TREE. Rows stay a
// flat list; pick_compare puts clones next to their parent and the draw path
// indents them. That is why filtering needs no special handling: a clone whose
// parent the filter removed is simply a row with nothing above it.

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
#include "mui_util.h"
#include "picker.h"
#include "netplaypick.h"

#include <cstdio>
#include <cstring>

// Our own file. netplay.ini belongs to the lobby and is rewritten wholesale
// when it saves, so a second writer there would lose these on every launch.
#define NETPLAY_PICK_INI  "netplaypick.ini"


enum
{
	PICK_MACHINE = 0,
	PICK_DIRECTORY,
	PICK_MANUFACTURER,
	PICK_YEAR,
	PICK_COLUMNS
};

static const LPCTSTR s_column_names[PICK_COLUMNS] =
{
	TEXT("Machine"),
	TEXT("Directory"),
	TEXT("Manufacturer"),
	TEXT("Year")
};

static const int s_default_widths[PICK_COLUMNS] = { 230, 90, 170, 50 };

static int  s_sort_column = PICK_MACHINE;
static BOOL s_sort_reverse = false;
static int  s_view = VIEW_GROUPED;

static HWND s_hDlg = nullptr;
static int  s_chosen = -1;


//-------------------------------------------------
//  the three remembered settings
//-------------------------------------------------

static void load_prefs(void)
{
	FILE *f = std::fopen(NETPLAY_PICK_INI, "r");
	if (f == nullptr)
		return;

	char line[128];
	while (std::fgets(line, sizeof(line), f) != nullptr)
	{
		char key[64];
		int val = 0;
		if (std::sscanf(line, "%63s %d", key, &val) != 2)
			continue;

		if (!std::strcmp(key, "view"))
			s_view = val;
		else if (!std::strcmp(key, "sort"))
			s_sort_column = val;
		else if (!std::strcmp(key, "reverse"))
			s_sort_reverse = (val != 0);
	}
	std::fclose(f);

	if ((s_view != VIEW_INLIST) && (s_view != VIEW_GROUPED) && (s_view != VIEW_REPORT))
		s_view = VIEW_GROUPED;
	if ((s_sort_column < 0) || (s_sort_column >= PICK_COLUMNS))
		s_sort_column = PICK_MACHINE;
}


static void save_prefs(void)
{
	FILE *f = std::fopen(NETPLAY_PICK_INI, "w");
	if (f == nullptr)
		return;

	std::fprintf(f, "view %d\n", s_view);
	std::fprintf(f, "sort %d\n", s_sort_column);
	std::fprintf(f, "reverse %d\n", s_sort_reverse ? 1 : 0);
	std::fclose(f);
}


//-------------------------------------------------
//  picker callbacks
//-------------------------------------------------

static void pick_set_sort_column(int column) { s_sort_column = column; }
static int  pick_get_sort_column(void) { return s_sort_column; }
static void pick_set_sort_reverse(BOOL reverse) { s_sort_reverse = reverse; }
static BOOL pick_get_sort_reverse(void) { return s_sort_reverse; }
static void pick_set_view_mode(int val) { s_view = val; }
static int  pick_get_view_mode(void) { return s_view; }


// Widths are not remembered. The columns are fixed and so are their defaults;
// nothing here is worth a settings file growing for.
static void pick_set_column_widths(int widths[]) { (void)widths; }

static void pick_get_column_widths(int widths[])
{
	for (int i = 0; i < PICK_COLUMNS; i++)
		widths[i] = s_default_widths[i];
}


// Fixed order, all shown. The header context menu is not wired up, so there is
// no way for either to change.
static void pick_set_column_order(int order[]) { (void)order; }

static void pick_get_column_order(int order[])
{
	for (int i = 0; i < PICK_COLUMNS; i++)
		order[i] = i;
}

static void pick_set_column_shown(int shown[]) { (void)shown; }

static void pick_get_column_shown(int shown[])
{
	for (int i = 0; i < PICK_COLUMNS; i++)
		shown[i] = 1;
}


// False keeps us out of GetParentFound, which answers from the MAIN window's
// current folder and filters - a question about a list that is not this one.
// The cost is that a clone whose parent the filter removed is still drawn
// indented, which is cosmetic.
static BOOL pick_get_offset_children(void) { return false; }


static int pick_compare(HWND hwndPicker, int index1, int index2, int sort_subitem)
{
	(void)hwndPicker;

	switch (sort_subitem)
	{
	case PICK_DIRECTORY:
		return core_stricmp(driver_list::driver(index1).name,
				driver_list::driver(index2).name);

	case PICK_MANUFACTURER:
		return core_stricmp(driver_list::driver(index1).manufacturer,
				driver_list::driver(index2).manufacturer);

	case PICK_YEAR:
		return core_stricmp(driver_list::driver(index1).year,
				driver_list::driver(index2).year);

	default:
		// ModifyThe rotates four buffers, so two calls in one expression
		// are safe.
		return core_stricmp(ModifyThe(driver_list::driver(index1).type.fullname()),
				ModifyThe(driver_list::driver(index2).type.fullname()));
	}
}


static const TCHAR *pick_get_item_string(HWND hwndPicker, int nItem, int nColumn,
		TCHAR *pszBuffer, UINT nBufferLength)
{
	(void)hwndPicker;

	const char *utf8 = "";
	switch (nColumn)
	{
	case PICK_DIRECTORY:    utf8 = driver_list::driver(nItem).name; break;
	case PICK_MANUFACTURER: utf8 = driver_list::driver(nItem).manufacturer; break;
	case PICK_YEAR:         utf8 = driver_list::driver(nItem).year; break;
	default:                utf8 = ModifyThe(driver_list::driver(nItem).type.fullname()); break;
	}

	TCHAR *const t = ui_wstring_from_utf8(utf8);
	if (t == nullptr)
		return TEXT("");

	_sntprintf(pszBuffer, nBufferLength, TEXT("%s"), t);
	free(t);
	return pszBuffer;
}


// No icons: nothing sets an image list, so there is no image to name.
static int pick_get_item_image(HWND hwndPicker, int nItem)
{
	(void)hwndPicker;
	(void)nItem;
	return -1;
}


static int pick_find_item_parent(HWND hwndPicker, int nItem)
{
	(void)hwndPicker;
	return GetParentRomSetIndex(&driver_list::driver(nItem));
}


static void pick_double_click(void);


static const struct PickerCallbacks s_callbacks =
{
	pick_set_sort_column,
	pick_get_sort_column,
	pick_set_sort_reverse,
	pick_get_sort_reverse,
	pick_set_view_mode,
	pick_get_view_mode,
	pick_set_column_widths,
	pick_get_column_widths,
	pick_set_column_order,
	pick_get_column_order,
	pick_set_column_shown,
	pick_get_column_shown,
	pick_get_offset_children,

	pick_compare,
	pick_double_click,
	pick_get_item_string,
	pick_get_item_image,
	nullptr,                // pfnLeavingItem
	nullptr,                // pfnEnteringItem
	nullptr,                // pfnBeginListViewDrag
	pick_find_item_parent,
	nullptr,                // pfnOnIdle - the main window audits here, we do not
	nullptr,                // pfnOnHeaderContextMenu - no column customising
	nullptr                 // pfnOnBodyContextMenu
};


//-------------------------------------------------
//  filtering
//-------------------------------------------------

// OR across the checked boxes. With none checked nothing is filtered, whatever
// has been typed.
static bool pick_match(HWND hDlg, int index, const char *text)
{
	if (text[0] == 0)
		return true;

	bool any = false;

	if (IsDlgButtonChecked(hDlg, IDC_NPP_F_MACHINE) == BST_CHECKED)
	{
		any = true;
		if (MyStrStrI(driver_list::driver(index).type.fullname(), text) != nullptr)
			return true;
	}

	if (IsDlgButtonChecked(hDlg, IDC_NPP_F_MANUF) == BST_CHECKED)
	{
		any = true;
		if (MyStrStrI(driver_list::driver(index).manufacturer, text) != nullptr)
			return true;
	}

	if (IsDlgButtonChecked(hDlg, IDC_NPP_F_DIR) == BST_CHECKED)
	{
		any = true;
		if (MyStrStrI(driver_list::driver(index).name, text) != nullptr)
			return true;
	}

	return !any;
}


// Rebuilt on every keystroke, as the main window does - no timer. The text is
// fetched through pfnGetItemString only for rows actually drawn, so the work
// here is one substring test per driver and an insert per match.
static void pick_populate(HWND hDlg, int select_driver)
{
	HWND const lv = GetDlgItem(hDlg, IDC_NPP_LISTVIEW);

	char text[128];
	GetDlgItemTextA(hDlg, IDC_NPP_FILTER, text, sizeof(text));

	SetWindowRedraw(lv, false);
	ListView_DeleteAllItems(lv);

	LVITEM lvi;
	std::memset(&lvi, 0, sizeof(lvi));
	lvi.mask = LVIF_TEXT | LVIF_PARAM;
	lvi.pszText = LPSTR_TEXTCALLBACK;

	const int total = driver_list::total();
	int shown = 0;
	for (int i = 0; i < total; i++)
	{
		if (!pick_match(hDlg, i, text))
			continue;

		lvi.iItem = shown++;
		lvi.lParam = i;
		ListView_InsertItem(lv, &lvi);
	}

	Picker_Sort(lv);

	if (select_driver >= 0)
		Picker_SetSelectedItem(lv, select_driver);
	else if (shown > 0)
		Picker_SetSelectedPick(lv, 0);

	SetWindowRedraw(lv, true);

	char note[64];
	std::snprintf(note, sizeof(note), "%d of %d", shown, total);
	SetDlgItemTextA(hDlg, IDC_NPP_COUNT, note);
}


//-------------------------------------------------
//  dialog
//-------------------------------------------------

static void pick_take(HWND hDlg)
{
	const int sel = Picker_GetSelectedItem(GetDlgItem(hDlg, IDC_NPP_LISTVIEW));
	if (sel < 0)
		return;

	s_chosen = sel;
	save_prefs();
	EndDialog(hDlg, 1);
}


// The picker hands this no window, so it works from the dialog we stashed.
static void pick_double_click(void)
{
	if (s_hDlg != nullptr)
		pick_take(s_hDlg);
}


static void pick_set_view(HWND hDlg, int view)
{
	Picker_SetViewID(GetDlgItem(hDlg, IDC_NPP_LISTVIEW), view);
	CheckRadioButton(hDlg, IDC_NPP_VIEW_LIST, IDC_NPP_VIEW_DETAILS,
			(view == VIEW_INLIST) ? IDC_NPP_VIEW_LIST :
			(view == VIEW_GROUPED) ? IDC_NPP_VIEW_GROUPED : IDC_NPP_VIEW_DETAILS);
}


static INT_PTR CALLBACK PickDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_INITDIALOG:
		{
			s_hDlg = hDlg;
			load_prefs();

			struct PickerOptions opts;
			std::memset(&opts, 0, sizeof(opts));
			opts.pCallbacks = &s_callbacks;
			opts.nColumnCount = PICK_COLUMNS;
			opts.ppszColumnNames = s_column_names;
			SetupPicker(GetDlgItem(hDlg, IDC_NPP_LISTVIEW), &opts);

			CheckDlgButton(hDlg, IDC_NPP_F_MACHINE, BST_CHECKED);
			CheckDlgButton(hDlg, IDC_NPP_F_MANUF, BST_UNCHECKED);
			CheckDlgButton(hDlg, IDC_NPP_F_DIR, BST_UNCHECKED);
			SendDlgItemMessage(hDlg, IDC_NPP_FILTER, EM_SETLIMITTEXT, 64, 0);

			pick_set_view(hDlg, s_view);
			pick_populate(hDlg, int(lParam));
			SetFocus(GetDlgItem(hDlg, IDC_NPP_FILTER));
			return false;       // we set the focus ourselves
		}

	case WM_NOTIFY:
		return Picker_HandleNotify(reinterpret_cast<LPNMHDR>(lParam));

	case WM_DRAWITEM:
		// Required, not optional: list, details and grouped all carry
		// LVS_OWNERDRAWFIXED when the XP control is in use, and without this
		// the rows draw empty.
		Picker_HandleDrawItem(GetDlgItem(hDlg, IDC_NPP_LISTVIEW),
				reinterpret_cast<LPDRAWITEMSTRUCT>(lParam));
		return true;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_NPP_VIEW_LIST:
			pick_set_view(hDlg, VIEW_INLIST);
			return true;

		case IDC_NPP_VIEW_GROUPED:
			pick_set_view(hDlg, VIEW_GROUPED);
			Picker_Sort(GetDlgItem(hDlg, IDC_NPP_LISTVIEW));
			return true;

		case IDC_NPP_VIEW_DETAILS:
			pick_set_view(hDlg, VIEW_REPORT);
			Picker_Sort(GetDlgItem(hDlg, IDC_NPP_LISTVIEW));
			return true;

		case IDC_NPP_FILTER:
			if (HIWORD(wParam) == EN_CHANGE)
				pick_populate(hDlg, -1);
			return true;

		case IDC_NPP_F_MACHINE:
		case IDC_NPP_F_MANUF:
		case IDC_NPP_F_DIR:
			pick_populate(hDlg, -1);
			return true;

		case IDOK:
			pick_take(hDlg);
			return true;

		case IDCANCEL:
			save_prefs();
			EndDialog(hDlg, 0);
			return true;
		}
		break;

	case WM_DESTROY:
		s_hDlg = nullptr;
		break;
	}
	return false;
}


int NetplayPickGame(HWND hParent, int nCurrent)
{
	s_chosen = -1;

	const INT_PTR r = DialogBoxParam(GetModuleHandle(nullptr),
			MAKEINTRESOURCE(IDD_NETPLAY_PICK), hParent, PickDialogProc,
			LPARAM(nCurrent));

	return (r == 1) ? s_chosen : -1;
}
