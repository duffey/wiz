/* wiz - hotkey app switcher for Windows 11.
 *
 * Bind F-keys to programs from the tray menu. A bound key focuses the
 * program's window (cycling if it has several) or launches it.
 */
#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* settings */
#define NKEYS   12              /* F1 .. F12 */
#define KEYMOD  MOD_NOREPEAT    /* add MOD_ALT for Alt+F1 etc. (Alt+F4 closes windows) */
static const wchar_t *ignored[] = {   /* window classes never treated as programs */
	L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
	L"Windows.UI.Core.CoreWindow", L"XamlExplorerHostIslandWindow",
	L"TopLevelWindowForOverflowXamlIsland", L"Xaml_WindowedPopupClass",
	L"NotifyIconOverflowWindow", L"ForegroundStaging", L"MultitaskingViewFrame",
};

/* limits */
#define MAXWINDOWS   256
#define MAXPROGRAMS  64
#define TITLELEN     64
#define TEXTLEN      (MAX_PATH + TITLELEN + 8)

/* menu command ids: each key owns CMD_PERKEY ids, one per program plus one to unassign */
#define CMD_QUIT      1
#define CMD_STARTUP   2
#define CMD_BASE      100
#define CMD_PERKEY    100
#define CMD_UNASSIGN  (CMD_PERKEY - 1)
#define WM_TRAYICON   (WM_APP + 1)
#define RUNKEY        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

typedef struct {
	wchar_t path[MAX_PATH];     /* executable */
	wchar_t title[TITLELEN];    /* one of its window titles, for display */
	HWND window;                /* that window, for its icon */
} Program;

static HWND windows[MAXWINDOWS];           /* program windows, in order first seen */
static int nwindows;
static Program programs[MAXPROGRAMS];      /* programs listed in the open menu */
static int nprograms;
static wchar_t bindings[NKEYS][MAX_PATH];  /* exe path per F-key, "" if unassigned */
static wchar_t inipath[MAX_PATH];
static HBITMAP bitmaps[MAXPROGRAMS + NKEYS];
static int nbitmaps;
static HWND traywnd;
static HICON appicon;
static UINT taskbarcreated;
static BOOL menushowing;

static void
warn(const wchar_t *fmt, const wchar_t *arg)
{
	wchar_t buf[TEXTLEN];

	_snwprintf(buf, TEXTLEN, fmt, arg);
	buf[TEXTLEN - 1] = 0;
	MessageBoxW(NULL, buf, L"wiz", MB_OK | MB_ICONWARNING);
}

static const wchar_t *
filename(const wchar_t *path)
{
	const wchar_t *s = wcsrchr(path, L'\\');

	return s ? s + 1 : path;
}

/* menus treat '&' as a mnemonic prefix; double it to show literally */
static void
escape(wchar_t *dst, const wchar_t *src, size_t n)
{
	size_t i = 0;

	for (; *src && i < n - 2; src++) {
		if (*src == L'&')
			dst[i++] = L'&';
		dst[i++] = *src;
	}
	dst[i] = 0;
}

/* windows ------------------------------------------------------------------ */

static BOOL
processpath(HWND hwnd, wchar_t *buf)   /* buf[MAX_PATH] */
{
	DWORD pid = 0, len = MAX_PATH;
	HANDLE p;
	BOOL ok;

	GetWindowThreadProcessId(hwnd, &pid);
	if (!pid || !(p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)))
		return FALSE;
	ok = QueryFullProcessImageNameW(p, 0, buf, &len);
	CloseHandle(p);
	return ok;
}

/* an ordinary top-level program window: visible, titled, unowned, not a tool
 * window, not cloaked (on another virtual desktop) */
static BOOL
trackable(HWND hwnd)
{
	wchar_t cls[256];
	DWORD cloaked = 0;
	size_t i;

	if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd
	    || GetWindow(hwnd, GW_OWNER) || !GetWindowTextLengthW(hwnd))
		return FALSE;
	if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))
		return FALSE;
	if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
		return FALSE;
	GetClassNameW(hwnd, cls, 256);
	for (i = 0; i < sizeof ignored / sizeof *ignored; i++)
		if (!wcscmp(cls, ignored[i]))
			return FALSE;
	return TRUE;
}

static int
indexof(HWND hwnd)
{
	int i;

	for (i = 0; i < nwindows; i++)
		if (windows[i] == hwnd)
			return i;
	return -1;
}

static BOOL CALLBACK
addwindow(HWND hwnd, LPARAM lp)
{
	(void)lp;
	if (nwindows < MAXWINDOWS && trackable(hwnd) && indexof(hwnd) < 0)
		windows[nwindows++] = hwnd;
	return TRUE;
}

/* drop closed windows, append new ones; existing order is kept so cycling is stable */
static void
sweep(void)
{
	int i, n = 0;

	for (i = 0; i < nwindows; i++)
		if (trackable(windows[i]))
			windows[n++] = windows[i];
	nwindows = n;
	EnumWindows(addwindow, 0);
}

static void
focus(HWND hwnd)
{
	DWORD fg, me = GetCurrentThreadId();
	BOOL attached;

	if (IsIconic(hwnd))
		ShowWindow(hwnd, SW_RESTORE);
	if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd)
		return;
	/* a background process may only take focus while sharing input with the foreground thread */
	fg = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
	attached = fg && fg != me && AttachThreadInput(fg, me, TRUE);
	BringWindowToTop(hwnd);
	SetForegroundWindow(hwnd);
	SetFocus(hwnd);
	if (attached)
		AttachThreadInput(fg, me, FALSE);
}

/* focus a window of exe, cycling through them; FALSE if it has none */
static BOOL
focusprogram(const wchar_t *exe)
{
	HWND matches[MAXWINDOWS], cur;
	wchar_t path[MAX_PATH];
	int i, n = 0, next = 0;

	sweep();
	cur = GetAncestor(GetForegroundWindow(), GA_ROOTOWNER);
	for (i = 0; i < nwindows; i++)
		if (processpath(windows[i], path) && !_wcsicmp(filename(path), exe))
			matches[n++] = windows[i];
	if (!n)
		return FALSE;
	for (i = 0; i < n; i++)
		if (matches[i] == cur)
			next = (i + 1) % n;
	focus(matches[next]);
	return TRUE;
}

/* one entry per executable that has a window */
static void
scanprograms(void)
{
	wchar_t path[MAX_PATH];
	int i, j;

	sweep();
	nprograms = 0;
	for (i = 0; i < nwindows && nprograms < MAXPROGRAMS; i++) {
		if (!processpath(windows[i], path))
			continue;
		for (j = 0; j < nprograms && _wcsicmp(programs[j].path, path); j++)
			;
		if (j < nprograms)
			continue;
		wcscpy(programs[j].path, path);
		GetWindowTextW(windows[i], programs[j].title, TITLELEN);
		programs[j].window = windows[i];
		nprograms++;
	}
}

/* bindings ----------------------------------------------------------------- */

static void
loadbindings(void)
{
	wchar_t key[8];
	int i;

	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", inipath, MAX_PATH - 16))
		return;
	wcscat(inipath, L"\\wiz");
	CreateDirectoryW(inipath, NULL);
	wcscat(inipath, L"\\wiz.ini");
	for (i = 0; i < NKEYS; i++) {
		_snwprintf(key, 8, L"F%d", i + 1);
		GetPrivateProfileStringW(L"bindings", key, L"", bindings[i], MAX_PATH, inipath);
	}
}

static void
savebinding(int i)
{
	wchar_t key[8];

	_snwprintf(key, 8, L"F%d", i + 1);
	WritePrivateProfileStringW(L"bindings", key, bindings[i][0] ? bindings[i] : NULL, inipath);
}

static void
registerkey(int i)
{
	wchar_t key[8];

	if (!bindings[i][0])
		return;
	if (!RegisterHotKey(NULL, i + 1, KEYMOD, VK_F1 + i)) {
		_snwprintf(key, 8, L"F%d", i + 1);
		warn(L"Could not register hotkey %s.\nAnother program may already be using it.", key);
	}
}

/* hotkey pressed: focus the bound program, or launch it */
static void
activate(int i)
{
	if (i < 0 || i >= NKEYS || !bindings[i][0] || focusprogram(filename(bindings[i])))
		return;
	if ((INT_PTR)ShellExecuteW(NULL, L"open", bindings[i], NULL, NULL, SW_SHOWNORMAL) <= 32)
		warn(L"Could not launch:\n%s", bindings[i]);
}

/* run at sign-in via the per-user Run registry key */
static BOOL
startup(void)
{
	return RegGetValueW(HKEY_CURRENT_USER, RUNKEY, L"wiz", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void
togglestartup(void)
{
	wchar_t exe[MAX_PATH + 2] = L"\"";

	if (startup()) {
		RegDeleteKeyValueW(HKEY_CURRENT_USER, RUNKEY, L"wiz");
		return;
	}
	GetModuleFileNameW(NULL, exe + 1, MAX_PATH);
	wcscat(exe, L"\"");
	RegSetKeyValueW(HKEY_CURRENT_USER, RUNKEY, L"wiz", REG_SZ, exe, (DWORD)(wcslen(exe) + 1) * sizeof(wchar_t));
}

/* icons -------------------------------------------------------------------- */

static HICON
windowicon(HWND hwnd)   /* owned by the window; do not destroy */
{
	DWORD_PTR icon = 0;

	if (!hwnd)
		return NULL;
	if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &icon) && icon)
		return (HICON)icon;
	return (HICON)GetClassLongPtrW(hwnd, GCLP_HICONSM);
}

/* menu bitmap: 32-bit with premultiplied alpha, resampled to the small icon size */
static HBITMAP
icontobitmap(HICON icon)
{
	int size = GetSystemMetrics(SM_CXSMICON), w, h, i, x, y;
	DWORD alpha = 0, a, *src, *tmp, *px;
	ICONINFO ii;
	BITMAP bm;
	BITMAPINFO bi = { 0 };
	HBITMAP t, out = NULL;
	HGDIOBJ old;
	HDC dc;

	if (!GetIconInfo(icon, &ii))
		return NULL;
	GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
	w = bm.bmWidth;
	h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;   /* mono icons stack AND/XOR masks */
	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;   /* top-down */
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	dc = CreateCompatibleDC(NULL);
	src = calloc((size_t)w * h, 4);
	t = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&tmp, NULL, 0);
	if (!src || !t)
		goto done;

	if (ii.hbmColor && bm.bmBitsPixel == 32 && GetDIBits(dc, ii.hbmColor, 0, h, src, &bi, DIB_RGB_COLORS))
		for (i = 0; i < w * h; i++)
			alpha |= src[i] >> 24;
	if (alpha) {   /* straight alpha -> premultiplied */
		for (i = 0; i < w * h; i++) {
			a = src[i] >> 24;
			src[i] = a << 24 | ((src[i] >> 16 & 255) * a / 255) << 16
			       | ((src[i] >> 8 & 255) * a / 255) << 8 | (src[i] & 255) * a / 255;
		}
	} else {   /* no alpha channel: draw it, take opacity from the mask */
		old = SelectObject(dc, t);
		DrawIconEx(dc, 0, 0, icon, w, h, 0, NULL, DI_NORMAL);
		GdiFlush();
		SelectObject(dc, old);
		memcpy(src, tmp, (size_t)w * h * 4);
		GetDIBits(dc, ii.hbmMask, 0, h, tmp, &bi, DIB_RGB_COLORS);   /* white = transparent */
		for (i = 0; i < w * h; i++)
			src[i] = tmp[i] & 0xFFFFFF ? 0 : src[i] | 0xFF000000;
	}

	bi.bmiHeader.biWidth = size;
	bi.bmiHeader.biHeight = -size;
	if ((out = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0)))
		for (y = 0; y < size; y++)
			for (x = 0; x < size; x++)
				px[y * size + x] = src[y * h / size * w + x * w / size];
done:
	free(src);
	if (t)
		DeleteObject(t);
	DeleteObject(ii.hbmColor);
	DeleteObject(ii.hbmMask);
	DeleteDC(dc);
	return out;
}

/* the window's own icon (works for Store apps), else the exe's shell icon */
static HBITMAP
programbitmap(HWND hwnd, const wchar_t *path)
{
	SHFILEINFOW sfi = { 0 };
	HICON icon = windowicon(hwnd);
	HBITMAP bmp;

	if (!icon && SHGetFileInfoW(path, 0, &sfi, sizeof sfi, SHGFI_ICON | SHGFI_SMALLICON))
		icon = sfi.hIcon;
	if (!icon)
		return NULL;
	bmp = icontobitmap(icon);
	if (sfi.hIcon)
		DestroyIcon(sfi.hIcon);
	if (bmp)
		bitmaps[nbitmaps++] = bmp;
	return bmp;
}

static void
freebitmaps(void)
{
	while (nbitmaps)
		DeleteObject(bitmaps[--nbitmaps]);
}

/* menu --------------------------------------------------------------------- */

static void
additem(HMENU menu, UINT id, HMENU sub, const wchar_t *text, HBITMAP bmp, BOOL checked)
{
	MENUITEMINFOW mi = { sizeof mi };

	mi.fMask = MIIM_STRING | MIIM_BITMAP | MIIM_STATE | (sub ? MIIM_SUBMENU : MIIM_ID);
	mi.wID = id;
	mi.hSubMenu = sub;
	mi.dwTypeData = (wchar_t *)text;
	mi.hbmpItem = bmp;
	mi.fState = checked ? MFS_CHECKED : MFS_ENABLED;
	InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mi);
}

static HMENU
buildmenu(void)
{
	HMENU menu = CreatePopupMenu(), sub;
	HBITMAP icons[MAXPROGRAMS], bmp;
	wchar_t text[TEXTLEN], esc[TEXTLEN * 2], name[MAX_PATH];
	size_t len;
	int k, i;
	BOOL cur;

	scanprograms();
	freebitmaps();   /* the previous menu is gone by now */
	for (i = 0; i < nprograms; i++)
		icons[i] = programbitmap(programs[i].window, programs[i].path);

	for (k = 0; k < NKEYS; k++) {
		sub = CreatePopupMenu();
		bmp = NULL;
		if (!nprograms)
			AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"(no windows open)");
		for (i = 0; i < nprograms; i++) {
			if ((cur = !_wcsicmp(programs[i].path, bindings[k])))
				bmp = icons[i];
			wcscpy(name, filename(programs[i].path));
			len = wcslen(name);
			if (len > 4 && !_wcsicmp(name + len - 4, L".exe"))
				name[len - 4] = 0;
			_snwprintf(text, TEXTLEN, L"%s - %s", name, programs[i].title);
			text[TEXTLEN - 1] = 0;
			escape(esc, text, TEXTLEN * 2);
			additem(sub, CMD_BASE + k * CMD_PERKEY + i, NULL, esc, icons[i], cur);
		}
		AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
		additem(sub, CMD_BASE + k * CMD_PERKEY + CMD_UNASSIGN, NULL, L"Unassigned", NULL, !bindings[k][0]);

		/* bound program not running: icon from its exe on disk. '\t' puts F<n> in the accelerator column. */
		if (bindings[k][0] && !bmp)
			bmp = programbitmap(NULL, bindings[k]);
		_snwprintf(text, TEXTLEN, L"%s\tF%d", bindings[k][0] ? filename(bindings[k]) : L"(unassigned)", k + 1);
		text[TEXTLEN - 1] = 0;
		escape(esc, text, TEXTLEN * 2);
		additem(menu, 0, sub, esc, bmp, FALSE);
	}
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING | (startup() ? MF_CHECKED : 0), CMD_STARTUP, L"Run at startup");
	AppendMenuW(menu, MF_STRING, CMD_QUIT, L"Quit");
	return menu;
}

static void
command(UINT id)
{
	int k = (id - CMD_BASE) / CMD_PERKEY, choice = (id - CMD_BASE) % CMD_PERKEY;

	if (id == CMD_QUIT) {
		PostQuitMessage(0);
		return;
	}
	if (id == CMD_STARTUP) {
		togglestartup();
		return;
	}
	if (id < CMD_BASE || k >= NKEYS)
		return;
	UnregisterHotKey(NULL, k + 1);
	if (choice == CMD_UNASSIGN)
		bindings[k][0] = 0;
	else if (choice < nprograms)
		wcscpy(bindings[k], programs[choice].path);
	savebinding(k);
	registerkey(k);
}

static void
showmenu(void)
{
	HMENU menu;
	POINT pt;
	UINT id;

	if (menushowing)   /* one click can arrive as two notifications */
		return;
	menushowing = TRUE;
	menu = buildmenu();
	GetCursorPos(&pt);
	SetForegroundWindow(traywnd);   /* so the menu closes on a click elsewhere */
	id = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
	                      pt.x, pt.y, traywnd, NULL);
	PostMessageW(traywnd, WM_NULL, 0, 0);
	DestroyMenu(menu);
	menushowing = FALSE;
	if (id)
		command(id);
}

/* tray --------------------------------------------------------------------- */

/* the tray icon: a lightning bolt (U+26A1) from Segoe UI Emoji, white on a dark
 * taskbar or black on a light one. GDI renders emoji fonts as plain outlines,
 * so the glyph takes whatever colour we give it. */
static HICON
makeicon(void)
{
	int size = GetSystemMetrics(SM_CXSMICON), i;
	BITMAPINFO bi = { 0 };
	ICONINFO ii = { TRUE };
	RECT r = { 0, 0, size, size };
	DWORD *px, a, light = 0, len = sizeof light, rgb;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bmp;
	HFONT font;
	HGDIOBJ old;
	HICON icon = NULL;
	void *zeros = calloc((size_t)size, 8);   /* empty mono mask; alpha does the work */

	bi.bmiHeader.biSize = sizeof bi.bmiHeader;
	bi.bmiHeader.biWidth = size;
	bi.bmiHeader.biHeight = -size;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
	font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
	                   ANTIALIASED_QUALITY, 0, L"Segoe UI Emoji");
	if (bmp && font && zeros) {
		old = SelectObject(dc, bmp);
		SelectObject(dc, font);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(255, 255, 255));
		DrawTextW(dc, L"\x26A1", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
		GdiFlush();
		SelectObject(dc, old);
		RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		             L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &light, &len);
		rgb = light ? 0x000000 : 0xFFFFFF;
		for (i = 0; i < size * size; i++) {   /* grey level = coverage -> alpha; colour it */
			a = px[i] & 255;
			px[i] = a << 24 | rgb;
		}
		ii.hbmColor = bmp;
		ii.hbmMask = CreateBitmap(size, size, 1, 1, zeros);
		icon = CreateIconIndirect(&ii);
		DeleteObject(ii.hbmMask);
	}
	if (font)
		DeleteObject(font);
	if (bmp)
		DeleteObject(bmp);
	free(zeros);
	DeleteDC(dc);
	return icon ? icon : LoadIconW(NULL, IDI_APPLICATION);
}

static void
addicon(void)
{
	NOTIFYICONDATAW nid = { sizeof nid };

	nid.hWnd = traywnd;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_TRAYICON;
	nid.hIcon = appicon;
	wcscpy(nid.szTip, L"wiz");
	Shell_NotifyIconW(NIM_ADD, &nid);
	nid.uVersion = NOTIFYICON_VERSION_4;
	Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void
removeicon(void)
{
	NOTIFYICONDATAW nid = { sizeof nid };

	nid.hWnd = traywnd;
	nid.uID = 1;
	Shell_NotifyIconW(NIM_DELETE, &nid);
}

static LRESULT CALLBACK
wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_TRAYICON) {
		switch (LOWORD(lp)) {
		case WM_CONTEXTMENU: case NIN_SELECT: case NIN_KEYSELECT:
		case WM_LBUTTONUP: case WM_RBUTTONUP:
			showmenu();
		}
		return 0;
	}
	if (msg == taskbarcreated) {   /* Explorer restarted; the icon is gone */
		addicon();
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI
wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
	WNDCLASSW wc = { 0 };
	MSG msg;
	int i;

	(void)prev; (void)cmd; (void)show;
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	CreateMutexW(NULL, TRUE, L"Local\\wiz");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		MessageBoxW(NULL, L"wiz is already running.", L"wiz", MB_OK | MB_ICONINFORMATION);
		return 1;
	}

	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.lpszClassName = L"wiz";
	RegisterClassW(&wc);
	taskbarcreated = RegisterWindowMessageW(L"TaskbarCreated");
	traywnd = CreateWindowExW(0, L"wiz", L"wiz", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
	if (!traywnd)
		return 1;

	loadbindings();
	for (i = 0; i < NKEYS; i++)
		registerkey(i);
	appicon = makeicon();
	addicon();

	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		if (msg.message == WM_HOTKEY)
			activate((int)msg.wParam - 1);
		else
			DispatchMessageW(&msg);
	}
	removeicon();
	return 0;
}
