// WinNotePlus_Full.cpp � Win32 single-file editor: Tabs + Sidebar + Autosave + simple Syntax Highlighting + Zoom + Font selection
// Features implemented:
// - Left sidebar (TreeView) listing open files and allowing open/close operations
// - TabControl with one RichEdit per tab (native, multiple documents)
// - Autosave to %TEMP% every 60 seconds (per-tab .autosave files)
// - Simple syntax highlighting for .cpp/.h and .html (keywords, strings, comments)
// - Zoom via Ctrl+MouseWheel, Font selection dialog, Statusbar with line/col
// - UTF-8 handling for plain text, RTF pass-through for .rtf files
// Build: Visual Studio (recommended) or g++ with -municode. Link libs as before.

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <algorithm>   // <-- neu: std::min/std::max
#include <cwctype>     // <-- neu: iswalpha/iswalnum für Wide-char Tests

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shlwapi.lib")

static HINSTANCE g_hInst = nullptr;
static HWND g_hMain = nullptr;
static HWND g_hStatus = nullptr;
static HWND g_hTree = nullptr;    // sidebar
static HWND g_hTabs = nullptr;    // tab control

static const wchar_t* APP_NAME = L"WinNotePlus Pro";
static const wchar_t* RICH_CLASS = L"RICHEDIT50W"; // use Msftedit

enum IDs {
    ID_FILE_OPEN = 4001, ID_FILE_SAVE, ID_FILE_SAVEAS, ID_FILE_CLOSE,
    ID_TABCONTROL = 5000, ID_TREE = 6000,
    ID_TIMER_AUTOSAVE = 7001, ID_TIMER_HIGHLIGHT = 7002,
    ID_VIEW_FONT = 8001
};

struct Doc {
    HWND hEdit;               // RichEdit window for this tab
    std::wstring path;        // file path (may be empty)
    bool modified = false;    // changed since last save
    int zoom = 100;           // percent
    bool isRtf = false;       // RTF file
    std::chrono::steady_clock::time_point lastEdit;
};

static std::vector<Doc> g_docs;
static int g_current = -1; // index into g_docs

// Utility: UTF-8 <-> wstring
static std::wstring Utf8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w; w.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string WToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s; s.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// File IO helpers
static bool ReadFileAll(const std::wstring& path, std::vector<unsigned char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end); size_t sz = (size_t)in.tellg(); in.seekg(0);
    out.resize(sz);
    if (sz) in.read((char*)out.data(), sz);
    return true;
}
static bool WriteFileAll(const std::wstring& path, const std::vector<unsigned char>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    if (!data.empty()) out.write((const char*)data.data(), data.size());
    return true;
}

// Neue: Hilfsstruktur + Callbacks für EM_STREAMIN/OUT (vermeidet statische Offsets in Lambdas)
struct StreamCookieIn {
	const std::vector<unsigned char>* data;
	size_t pos;
};
static DWORD CALLBACK RichEdit_StreamInCallback(DWORD_PTR dwCookie, LPBYTE pbBuff, LONG cb, LONG* pcb) {
	StreamCookieIn* sc = (StreamCookieIn*)dwCookie;
	if (!sc || !sc->data || pcb == nullptr) { if (pcb) *pcb = 0; return 1; }
	size_t available = sc->data->size() > sc->pos ? sc->data->size() - sc->pos : 0;
	size_t tocopy = std::min<size_t>(available, (size_t)cb);
	if (tocopy) memcpy(pbBuff, sc->data->data() + sc->pos, tocopy);
	sc->pos += tocopy;
	*pcb = (LONG)tocopy;
	return 0;
}
struct StreamCookieOut {
	std::vector<unsigned char>* out;
};
static DWORD CALLBACK RichEdit_StreamOutCallback(DWORD_PTR dwCookie, LPBYTE pbBuff, LONG cb, LONG* pcb) {
	StreamCookieOut* sc = (StreamCookieOut*)dwCookie;
	if (!sc || !sc->out || pcb == nullptr) { if (pcb) *pcb = 0; return 1; }
	sc->out->insert(sc->out->end(), pbBuff, pbBuff + cb);
	*pcb = cb;
	return 0;
}

// Helpers to manage Tab captions
static void UpdateTabCaption(int idx) {
    if (idx < 0 || idx >= (int)g_docs.size()) return;
    wchar_t caption[512];
    const std::wstring& p = g_docs[idx].path;
    const wchar_t* name = p.empty() ? L"Unbenannt" : PathFindFileNameW(p.c_str());
    swprintf_s(caption, L"%s%s", g_docs[idx].modified ? L"*" : L"", name);
    TCITEM tie = { 0 }; tie.mask = TCIF_TEXT; tie.pszText = caption;
    TabCtrl_SetItem(g_hTabs, idx, &tie);
}

static void UpdateAllTabs() {
    for (int i = 0; i < (int)g_docs.size(); ++i) UpdateTabCaption(i);
}

// Create a new document/tab, optionally loading from path
static int CreateDoc(const std::wstring& path = L"") {
    // create RichEdit control
    EnsureMsftEditLoaded();
    HWND hEdit = CreateWindowExW(0, RICH_CLASS, L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL,
        0, 0, 0, 0, g_hTabs, nullptr, g_hInst, nullptr);
    SendMessageW(hEdit, EM_SETLIMITTEXT, 0, 0x7FFFFFFF);

    Doc d; d.hEdit = hEdit; d.path = path; d.modified = false; d.zoom = 100; d.isRtf = false;
    g_docs.push_back(d);
    int idx = (int)g_docs.size() - 1;

    // add tab
    wchar_t caption[256]; swprintf_s(caption, L"%s", path.empty() ? L"Unbenannt" : PathFindFileNameW(path.c_str()));
    TCITEM tie{}; tie.mask = TCIF_TEXT; tie.pszText = caption;
    TabCtrl_InsertItem(g_hTabs, idx, &tie);

    // set font default
    CHARFORMAT2 cf{}; cf.cbSize = sizeof(cf); cf.dwMask = CFM_FACE | CFM_SIZE; cf.yHeight = 240; // 12pt
    wcscpy_s(cf.szFaceName, L"Consolas");
    SendMessageW(hEdit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);

    // hook edit notifications via subclass
    SetWindowSubclass(hEdit, [](HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)->LRESULT {
        if (msg == EN_CHANGE) {
            // find which doc
            for (int i = 0; i < (int)g_docs.size(); ++i) if (g_docs[i].hEdit == h) {
                g_docs[i].modified = true; g_docs[i].lastEdit = std::chrono::steady_clock::now(); UpdateTabCaption(i);
                // schedule highlight timer
                SetTimer(g_hMain, ID_TIMER_HIGHLIGHT, 600, NULL);
                break;
            }
        }
        else if (msg == WM_MOUSEWHEEL) {
            // ctrl + wheel -> zoom
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                short delta = GET_WHEEL_DELTA_WPARAM(w);
                for (int i = 0; i < (int)g_docs.size(); ++i) if (g_docs[i].hEdit == h) {
                    int& z = g_docs[i].zoom;
                    z += (delta > 0) ? 10 : -10; if (z < 30) z = 30; if (z > 500) z = 500;
                    // set zoom by changing font size
                    CHARFORMAT2 cf{}; cf.cbSize = sizeof(cf); cf.dwMask = CFM_SIZE; cf.yHeight = z * 20; // approx
                    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION | SCF_ALL, (LPARAM)&cf);
                    UpdateTabCaption(i);
                    break;
                }
                return 0;
            }
        }
        return DefSubclassProc(h, msg, w, l);
        }, (UINT_PTR)idx + 1, 0);

    // load file if path provided
    if (!path.empty()) {
        std::vector<unsigned char> bytes; if (ReadFileAll(path, bytes)) {
            // detect rtf by extension
            wchar_t ext[_MAX_EXT] = {};
            _wsplitpath_s(path.c_str(), nullptr, 0, nullptr, 0, nullptr, 0, ext, _MAX_EXT);
            if (lstrcmpiW(ext, L".rtf") == 0) {
                // Verwende sicheren Cookie + Callback (kein statischer Offset)
				StreamCookieIn sc{ &bytes, 0 };
				EDITSTREAM es{}; es.dwCookie = (DWORD_PTR)&sc; es.pfnCallback = RichEdit_StreamInCallback;
				SendMessageW(hEdit, EM_STREAMIN, SF_RTF, (LPARAM)&es);
				g_docs[idx].isRtf = true;
            }
            else {
                // handle BOMs
                size_t off = 0; if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) off = 3;
                std::string s((char*)bytes.data() + off, bytes.size() - off);
                std::wstring w = Utf8ToW(s);
                SetWindowTextW(hEdit, w.c_str());
            }
        }
    }

    // select newly created tab
    TabCtrl_SetCurSel(g_hTabs, idx);
    // hide others and show this
    for (int i = 0; i < (int)g_docs.size(); ++i) {
        ShowWindow(g_docs[i].hEdit, i == idx ? SW_SHOW : SW_HIDE);
    }
    g_current = idx;
    UpdateAllTabs();
    return idx;
}

static void EnsureMsftEditLoaded() {
    static HMODULE h = LoadLibraryW(L"Msftedit.dll"); (void)h;
}

// Save doc to path
static bool SaveDoc(int idx, const std::wstring& path) {
	if (idx < 0 || idx >= (int)g_docs.size()) return false;
	Doc& d = g_docs[idx];
	if (d.isRtf || (!path.empty() && PathMatchSpecW(path.c_str(), L"*.rtf"))) {
		// stream out rtf using sicheren Cookie
		std::vector<unsigned char> out;
		StreamCookieOut sc{ &out };
		EDITSTREAM es{}; es.dwCookie = (DWORD_PTR)&sc; es.pfnCallback = RichEdit_StreamOutCallback;
		LRESULT res = SendMessageW(d.hEdit, EM_STREAMOUT, SF_RTF, (LPARAM)&es);
		if (!res) return false; return WriteFileAll(path, out);
	}
	else {
		int len = (int)SendMessageW(d.hEdit, WM_GETTEXTLENGTH, 0, 0);
		std::wstring w; w.resize(len); GetWindowTextW(d.hEdit, &w[0], len + 1);
		std::string utf = WToUtf8(w);
		// write BOM + utf-8
		std::vector<unsigned char> out; out.push_back(0xEF); out.push_back(0xBB); out.push_back(0xBF);
		out.insert(out.end(), utf.begin(), utf.end());
		return WriteFileAll(path, out);
	}
}

// Autosave: save each modified doc to temp as name + .autosave
static void AutosaveAll() {
	wchar_t tmpPath[MAX_PATH]; GetTempPathW(MAX_PATH, tmpPath);
	for (int i = 0; i < (int)g_docs.size(); ++i) if (g_docs[i].modified) {
		std::wstring base = g_docs[i].path.empty() ? L"untitled" : PathFindFileNameW(g_docs[i].path.c_str());
		std::wstring safe = base;
		for (auto& c : safe) {
			// Ersetze Dateisystem-unfreundliche Zeichen
			if (c == L'\\' || c == L'/' || c == L':' || c == L'\'' || c == L'\"' || c == L' ' || c == L'\t') c = L'_';
		}
		std::wstring full = std::wstring(tmpPath) + safe + L"." + std::to_wstring(i) + L".autosave";
		// write plain text
		int len = (int)SendMessageW(g_docs[i].hEdit, WM_GETTEXTLENGTH, 0, 0);
		std::wstring w; w.resize(len); GetWindowTextW(g_docs[i].hEdit, &w[0], len + 1);
		std::string utf = WToUtf8(w);
		std::vector<unsigned char> out; out.push_back(0xEF); out.push_back(0xBB); out.push_back(0xBF);
		out.insert(out.end(), utf.begin(), utf.end());
		WriteFileAll(full, out);
	}
}

// Simple syntax highlighting (inefficient but illustrative)
static std::vector<std::wstring> cppKeywords = { L"int",L"float",L"double",L"char",L"if",L"else",L"for",L"while",L"return",L"void",L"class",L"struct",L"public",L"private",L"protected",L"include",L"using",L"namespace",L"std" };
static std::vector<std::wstring> htmlKeywords = { L"html",L"head",L"body",L"div",L"span",L"script",L"style",L"a",L"img",L"title" };

// Vollständig überarbeitete Highlight-Funktion (korrekte Parsing-Logik)
static void ApplyHighlightingToDoc(int idx) {
	if (idx < 0 || idx >= (int)g_docs.size()) return;
	HWND h = g_docs[idx].hEdit;
	int len = (int)SendMessageW(h, WM_GETTEXTLENGTH, 0, 0);
	std::wstring text; text.resize(len); GetWindowTextW(h, &text[0], len + 1);

	// Setze gesamten Text auf Default-Format
	CHARRANGE allSel{}; allSel.cpMin = 0; allSel.cpMax = -1; SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&allSel);
	CHARFORMAT2 cfDefault{}; cfDefault.cbSize = sizeof(cfDefault); cfDefault.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE; cfDefault.crTextColor = RGB(0, 0, 0); wcscpy_s(cfDefault.szFaceName, L"Consolas"); cfDefault.yHeight = 200;
	SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfDefault);

	bool isCpp = false, isHtml = false;
	if (!g_docs[idx].path.empty()) {
		wchar_t ext[_MAX_EXT] = {}; _wsplitpath_s(g_docs[idx].path.c_str(), nullptr, 0, nullptr, 0, nullptr, 0, ext, _MAX_EXT);
		if (lstrcmpiW(ext, L".cpp") == 0 || lstrcmpiW(ext, L".h") == 0 || lstrcmpiW(ext, L".c") == 0) isCpp = true;
		if (lstrcmpiW(ext, L".html") == 0 || lstrcmpiW(ext, L".htm") == 0) isHtml = true;
	}
	if (!isCpp && !isHtml) return;

	size_t pos = 0;
	while (pos < text.size()) {
		// C++ single-line comment //
		if (isCpp && pos + 1 < text.size() && text[pos] == L'/' && text[pos + 1] == L'/') {
			size_t start = pos;
			pos += 2;
			while (pos < text.size() && text[pos] != L'\n') ++pos;
			CHARRANGE cr{ (LONG)start, (LONG)pos };
			SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
			CHARFORMAT2 cf{}; cf.cbSize = sizeof(cf); cf.dwMask = CFM_COLOR; cf.crTextColor = RGB(0, 128, 0);
			SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
			continue;
		}
		// C++ multi-line comment /* ... */
		if (isCpp && pos + 1 < text.size() && text[pos] == L'/' && text[pos + 1] == L'*') {
			size_t start = pos;
			pos += 2;
			while (pos + 1 < text.size() && !(text[pos] == L'*' && text[pos + 1] == L'/')) ++pos;
			if (pos + 1 < text.size()) pos += 2; // include closing */
			CHARRANGE cr{ (LONG)start, (LONG)pos };
			SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
			CHARFORMAT2 cf{}; cf.cbSize = sizeof(cf); cf.dwMask = CFM_COLOR; cf.crTextColor = RGB(0, 128, 0);
			SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
			continue;
		}
		// Strings: "..." with escaping support
		if (text[pos] == L'\"') {
			size_t start = pos;
			++pos;
			while (pos < text.size()) {
				if (text[pos] == L'\\') { // escaped char
					pos += 2;
					continue;
				}
				if (text[pos] == L'\"') { ++pos; break; }
				++pos;
			}
			CHARRANGE cr{ (LONG)start, (LONG)pos };
			SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
			CHARFORMAT2 cf{}; cf.cbSize = sizeof(cf); cf.dwMask = CFM_COLOR; cf.crTextColor = RGB(163, 21, 21);
			SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
			continue;
		}
		// HTML tag
		if (isHtml && text[pos] == L'<') {
			size_t start = pos;
			++pos;
			while (pos < text.size() && text[pos] != L'>') ++pos;
			if (pos < text.size()) ++pos;
			CHARRANGE cr{ (LONG)start, (LONG)pos };
			SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
			CHARFORMAT2 cf{}; cf.cbSize = sizeof(cf); cf.dwMask = CFM_COLOR; cf.crTextColor = RGB(0, 0, 255);
			SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
			continue;
		}
		// Keywords (C++)
		if (isCpp && (iswalpha(text[pos]) || text[pos] == L'_')) {
			size_t start = pos;
			while (pos < text.size() && (iswalnum(text[pos]) || text[pos] == L'_')) ++pos;
			std::wstring w = text.substr(start, pos - start);
			for (auto& kw : cppKeywords) {
				if (w == kw) {
					CHARRANGE cr{ (LONG)start, (LONG)pos };
					SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
					CHARFORMAT2 cf{}; cf.cbSize = sizeof(cf); cf.dwMask = CFM_COLOR; cf.crTextColor = RGB(0, 0, 255);
					SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
					break;
				}
			}
			continue;
		}
		// andere Zeichen überspringen
		++pos;
	}
	// selection reset zurücksetzen
	CHARRANGE zeroSel{}; zeroSel.cpMin = zeroSel.cpMax = 0; SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&zeroSel);
}

// UI Setup
static HMENU BuildMenu() {
    HMENU m = CreateMenu();
    HMENU f = CreatePopupMenu();
    AppendMenuW(f, MF_STRING, ID_FILE_OPEN, L"&�ffnen...	Ctrl+O");
    AppendMenuW(f, MF_STRING, ID_FILE_SAVE, L"&Speichern	Ctrl+S");
    AppendMenuW(f, MF_STRING, ID_FILE_SAVEAS, L"Speichern &unter...");
    AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(f, MF_STRING, ID_FILE_CLOSE, L"&Schlie�en");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)f, L"&Datei");
    HMENU v = CreatePopupMenu(); AppendMenuW(v, MF_STRING, ID_VIEW_FONT, L"Schriftart..."); AppendMenuW(m, MF_POPUP, (UINT_PTR)v, L"&Ansicht");
    return m;
}

static void DoLayout() {
    RECT rc; GetClientRect(g_hMain, &rc);
    int statusH = 20;
    RECT tr; GetWindowRect(g_hStatus, &tr); statusH = tr.bottom - tr.top;
    int sidebarW = 260;
    MoveWindow(g_hTree, 0, 0, sidebarW, rc.bottom - statusH, TRUE);
    MoveWindow(g_hTabs, sidebarW, 0, rc.right - sidebarW, rc.bottom - statusH, TRUE);
    // resize current edit
    for (auto& d : g_docs) { MoveWindow(d.hEdit, sidebarW + 4, 24, rc.right - sidebarW - 8, rc.bottom - statusH - 28, TRUE); }
}

// Tree (sidebar) shows open docs
static void RefreshTree() {
    TreeView_DeleteAllItems(g_hTree);
    for (int i = 0; i < (int)g_docs.size(); ++i) {
        TVITEM ti{}; ti.mask = TVIF_TEXT | TVIF_PARAM; ti.pszText = (LPWSTR)(g_docs[i].path.empty() ? L"Unbenannt" : PathFindFileNameW(g_docs[i].path.c_str())); ti.lParam = i;
        TVINSERTSTRUCT ins{}; ins.item = ti; TreeView_InsertItem(g_hTree, &ins);
    }
}

// Status update: line/col
static void UpdateStatus() {
    if (g_current < 0 || g_current >= (int)g_docs.size()) return;
    HWND h = g_docs[g_current].hEdit; int len = (int)SendMessageW(h, WM_GETTEXTLENGTH, 0, 0);
    DWORD cp = (DWORD)SendMessageW(h, EM_GETSEL, 0, 0);
    DWORD line = (DWORD)SendMessageW(h, EM_LINEFROMCHAR, cp, 0);
    DWORD lineStart = (DWORD)SendMessageW(h, EM_LINEINDEX, line, 0);
    DWORD col = cp - lineStart;
    wchar_t buf[256]; swprintf_s(buf, L"%s � Zeichen: %d | Zeile: %d | Spalte: %d", APP_NAME, len, line + 1, col + 1);
    SendMessageW(g_hStatus, SB_SETTEXT, 0, (LPARAM)buf);
}

// Message handling
static LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_CREATE: {
            InitCommonControls(); EnsureMsftEditLoaded();
            g_hStatus = CreateStatusWindowW(WS_CHILD | WS_VISIBLE, L"Bereit", hWnd, 1);
            // TreeView
            g_hTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_SHOWSELALWAYS, 0, 0, 0, 0, hWnd, (HMENU)ID_TREE, g_hInst, nullptr);
            // Tab control (we'll fake the tab headers above the editors by reserving header area)
            g_hTabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | TCS_TABS, 0, 0, 0, 0, hWnd, (HMENU)ID_TABCONTROL, g_hInst, nullptr);
            // create initial doc
            CreateDoc(); RefreshTree();
            SetTimer(hWnd, ID_TIMER_AUTOSAVE, 60000, NULL); // 60s auto save
            return 0;
        }
        case WM_SIZE: DoLayout(); return 0;
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
            case ID_FILE_OPEN: {
                OPENFILENAMEW ofn{}; wchar_t buf[MAX_PATH] = L""; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hWnd; ofn.lpstrFilter = L"Alle Dateien *.* Textdateien *.txt;*.md;*.cpp;*.h;*.html RTF *.rtf  "; ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) { CreateDoc(buf); RefreshTree(); }
                break;
            }
            case ID_FILE_SAVE: {
                if (g_current >= 0 && g_current < (int)g_docs.size()) {
                    if (g_docs[g_current].path.empty()) {
                        // SaveAs
                        OPENFILENAMEW ofn{}; wchar_t buf[MAX_PATH] = L""; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hWnd; ofn.lpstrFilter = L"Textdateien *.txt RTF *.rtf Alle Dateien *.*  "; ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST; ofn.lpstrDefExt = L"txt";
                        if (GetSaveFileNameW(&ofn)) { if (SaveDoc(g_current, buf)) { g_docs[g_current].path = buf; g_docs[g_current].modified = false; UpdateTabCaption(g_current); RefreshTree(); } }
                    }
                    else { if (SaveDoc(g_current, g_docs[g_current].path)) { g_docs[g_current].modified = false; UpdateTabCaption(g_current); RefreshTree(); } }
                    UpdateStatus();
                }
                break;
            }
            case ID_FILE_SAVEAS: {
                if (g_current >= 0) {
                    OPENFILENAMEW ofn{}; wchar_t buf[MAX_PATH] = L""; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hWnd; ofn.lpstrFilter = L"Textdateien *.txt RTF *.rtf Alle Dateien *.*  "; ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST; ofn.lpstrDefExt = L"txt";
                    if (GetSaveFileNameW(&ofn)) { if (SaveDoc(g_current, buf)) { g_docs[g_current].path = buf; g_docs[g_current].modified = false; UpdateTabCaption(g_current); RefreshTree(); } }
                }
                break;
            }
            case ID_FILE_CLOSE: {
                if (g_current >= 0) {
                    // close tab
                    DestroyWindow(g_docs[g_current].hEdit);
                    g_docs.erase(g_docs.begin() + g_current);
                    TabCtrl_DeleteItem(g_hTabs, g_current);
                    if (g_docs.empty()) CreateDoc();
                    g_current = std::max(0, (int)g_docs.size() - 1);
                    for (int i = 0; i < (int)g_docs.size(); ++i) ShowWindow(g_docs[i].hEdit, i == g_current ? SW_SHOW : SW_HIDE);
                    UpdateAllTabs(); RefreshTree(); UpdateStatus();
                }
                break;
            }
            case ID_VIEW_FONT: {
                if (g_current >= 0) {
                    CHOOSEFONTW cf{}; LOGFONTW lf{}; GetObjectW((HFONT)GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
                    cf.lStructSize = sizeof(cf); cf.hwndOwner = hWnd; cf.lpLogFont = &lf; cf.Flags = CF_SCREENFONTS | CF_EFFECTS;
                    if (ChooseFontW(&cf)) {
                        CHARFORMAT2 chf{}; chf.cbSize = sizeof(chf); chf.dwMask = CFM_FACE | CFM_SIZE; wcscpy_s(chf.szFaceName, lf.lfFaceName); chf.yHeight = abs(lf.lfHeight) * 20;
                        SendMessageW(g_docs[g_current].hEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&chf);
                    }
                }
                break;
            }
            }
            break;
        }
        case WM_NOTIFY: {
            if (((LPNMHDR)lParam)->hwndFrom == g_hTabs) {
                if (((LPNMHDR)lParam)->code == TCN_SELCHANGE) {
                    int sel = TabCtrl_GetCurSel(g_hTabs);
                    if (sel >= 0 && sel < (int)g_docs.size()) {
                        // show selected
                        for (int i = 0; i < (int)g_docs.size(); ++i) ShowWindow(g_docs[i].hEdit, i == sel ? SW_SHOW : SW_HIDE);
                        g_current = sel; UpdateStatus(); RefreshTree();
                    }
                }
            }
            else if (((LPNMHDR)lParam)->hwndFrom == g_hTree) {
                if (((LPNMHDR)lParam)->code == TVN_SELCHANGED) {
                    NMTREEVIEWW* tv = (NMTREEVIEWW*)lParam; int idx = (int)tv->itemNew.lParam;
                    if (idx >= 0 && idx < (int)g_docs.size()) { TabCtrl_SetCurSel(g_hTabs, idx); SendMessageW(hWnd, WM_NOTIFY, 0, 0); }
                }
            }
            break;
        }
        case WM_TIMER: {
            if (wParam == ID_TIMER_AUTOSAVE) { AutosaveAll(); }
            else if (wParam == ID_TIMER_HIGHLIGHT) {
                KillTimer(hWnd, ID_TIMER_HIGHLIGHT);
                // apply highlight to all docs whose last edit was >300ms ago
                auto now = std::chrono::steady_clock::now();
                for (int i = 0; i < (int)g_docs.size(); ++i) {
                    if (g_docs[i].lastEdit + std::chrono::milliseconds(300) < now) ApplyHighlightingToDoc(i);
                }
            }
            break;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
        case WM_SETFOCUS: SetFocus(g_hTabs); return 0;
        case WM_MOUSEACTIVATE: return MA_ACTIVATE;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
        g_hInst = hInstance; InitCommonControls(); EnsureMsftEditLoaded();
        WNDCLASSEXW wc{ sizeof(wc) }; wc.style = CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = MainWndProc; wc.hInstance = hInstance; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"WinNotePlusMain";
        RegisterClassExW(&wc);
        g_hMain = CreateWindowExW(0, wc.lpszClassName, APP_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 700, NULL, BuildMenu(), hInstance, NULL);
        ShowWindow(g_hMain, nCmdShow); UpdateWindow(g_hMain);
        // initial layout and status
        DoLayout(); UpdateStatus();
        MSG msg; while (GetMessageW(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        return 0;
    }
