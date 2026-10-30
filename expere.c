// Expere - a tiny keyboard launcher for Windows.
//
// Memory budget is the whole point of this file:
//   * one native Win32 process, no browser engine, no script runtime
//   * no file index of our own: file search asks the Windows Search index that Windows
//     already keeps (or Everything, if its client is already running)
//   * the app list lives in a private heap that is freed in one HeapDestroy
//   * icons come from the shared system image list, resolved only for visible rows
//   * the window is drawn one row-high strip at a time, so the back buffer stays small
//   * the search connection and buffers are dropped and the working set trimmed on hide
//
// Transparency: DWM composites the window using the alpha channel of what we paint
// (the frame is extended over the whole client area). Every pixel is drawn here with
// premultiplied alpha; text goes through a coverage mask because GDI writes no alpha.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define COBJMACROS
#define _USE_MATH_DEFINES
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000006

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <powrprof.h>
#include <imm.h>
#include <oledb.h>
#include <msdasc.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>
#include "resource.h"

#define APP_NAME     L"Expere"
#define WND_CLASS    L"ExpereLauncher"
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define THEME_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"

#define WM_APP_TRAY  (WM_APP + 1)
#define WM_APP_APPS  (WM_APP + 2)
#define WM_APP_SHOW  (WM_APP + 3)
#define WM_APP_QUIT  (WM_APP + 4)
#define WM_APP_FILES (WM_APP + 5)
#define WM_APP_SETTINGS (WM_APP + 6)

#define HOTKEY_ID        1
#define TIMER_SEARCH     1
#define TIMER_TRIM       2
#define TIMER_CARET      3
#define SEARCH_DELAY_MS  90
#define TRIM_AGAIN_MS    20000  // shell and COM threads touch pages for a while after we hide
#define WS_MAX           24
#define MAX_ROWS         10
#define APP_REFRESH_MS   (10 * 60 * 1000)

// WM_COPYDATA tags. Everything replies carry EV_REPLY_BASE | query sequence.
#define COPY_QUERY       0x51505845u
#define EV_REPLY_BASE    0x45560000u

// Layout in 96-DPI pixels.
#define WIN_W   680
#define EDIT_H  58
#define ROW_H   50
#define FOOT_H  30
#define PAD     8
#define TEXT_X  22

// Paths that are noise in a launcher. Everything: a term containing "\" matches the full path.
// Windows Search ignores path filters in SQL, so its rows are filtered here instead; Start Menu
// is skipped because apps already come from the Apps folder. "\." covers .git, .gradle, .cargo...
#define EV_EXCLUDE L"!\\AppData\\ !\\$Recycle.Bin\\ !\\Windows\\ !\\node_modules\\ !\\.git\\"
static const WCHAR *WS_EXCLUDE[] = {L"\\AppData\\", L"\\node_modules\\", L"\\.", L"\\Start Menu\\", L"\\$Recycle.Bin\\"};
#define WS_FETCH 120

static const GUID kFolderAppsFolder = {0x1e87508d, 0x89c2, 0x42f0, {0x8a, 0x7e, 0x64, 0x5a, 0x0f, 0x50, 0xca, 0x58}};
static const GUID kBhidEnumItems    = {0x94f60519, 0x2850, 0x4924, {0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39}};
static const GUID kIidImageList     = {0x46eb5926, 0x582e, 0x4017, {0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50}};
static const GUID kDbguidDefault    = {0xc8b521fb, 0x5cf3, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};

enum { K_APP, K_FILE, K_FOLDER, K_CMD, K_CALC, K_WEB, K_SEARCH };
static const WCHAR *KIND_LABEL[] = {L"Application", L"File", L"Folder", L"Command", L"Calculator", L"Web", L"Search"};

enum { CMD_LOCK, CMD_SLEEP, CMD_RESTART, CMD_SHUTDOWN, CMD_SIGNOUT, CMD_RECYCLE, CMD_SETTINGS, CMD_QUIT };
// Glyphs are Segoe Fluent Icons code points (same in Segoe MDL2 Assets on Windows 10).
static const struct { const WCHAR *name, *sub; WCHAR glyph; } CMDS[] = {
    {L"Lock Screen",       L"Lock this PC",                                0xE72E},
    {L"Sleep",             L"Put this PC to sleep",                        0xE708},
    {L"Restart",           L"Restart Windows",                             0xE72C},
    {L"Shut Down",         L"Shut down Windows",                           0xE7E8},
    {L"Sign Out",          L"Sign out of Windows",                         0xF3B1},
    {L"Empty Recycle Bin", L"Permanently delete items in the Recycle Bin", 0xE74D},
    {L"Expere Settings",   L"Shortcut, appearance, and search options",    0xE713},
    {L"Quit Expere",       L"Close the launcher",                          0xE711},
};
#define GLYPH_CALC   0xE8EF
#define GLYPH_WEB    0xE774
#define GLYPH_SEARCH 0xE721

// ---------------------------------------------------------------- settings

enum { THEME_SYSTEM, THEME_DARK, THEME_LIGHT };
enum { BD_ACRYLIC, BD_GLASS, BD_SOLID };

static const struct { const WCHAR *name, *url; } ENGINES[] = {
    {L"Google",       L"https://www.google.com/search?q="},
    {L"Bing",         L"https://www.bing.com/search?q="},
    {L"DuckDuckGo",   L"https://duckduckgo.com/?q="},
    {L"Brave Search", L"https://search.brave.com/search?q="},
};

static const struct { const WCHAR *name; UINT mod; } HOTMODS[] = {
    {L"Alt", MOD_ALT}, {L"Ctrl", MOD_CONTROL}, {L"Ctrl + Alt", MOD_CONTROL | MOD_ALT},
    {L"Ctrl + Shift", MOD_CONTROL | MOD_SHIFT}, {L"Alt + Shift", MOD_ALT | MOD_SHIFT},
    {L"Win + Alt", MOD_WIN | MOD_ALT}, {L"Win + Shift", MOD_WIN | MOD_SHIFT},
};

typedef struct {
    UINT hotMod, hotKey;
    int theme, backdrop, opacity, maxResults, engine;
    BOOL files, keepQuery;
} Settings;

static Settings g_set = {MOD_ALT, VK_SPACE, THEME_SYSTEM, BD_ACRYLIC, 70, 8, 0, TRUE, FALSE};

typedef struct {
    COLORREF text, sub, accent, tint, border, sel, line;
    BYTE selA, lineA;
    BOOL dark;
} Palette;

static const Palette PAL_DARK  = {RGB(242, 242, 246), RGB(170, 170, 182), RGB(98, 124, 240), RGB(26, 26, 30),
                                  RGB(72, 72, 84), RGB(255, 255, 255), RGB(255, 255, 255), 26, 22, TRUE};
static const Palette PAL_LIGHT = {RGB(22, 22, 26), RGB(90, 90, 102), RGB(56, 90, 230), RGB(248, 248, 250),
                                  RGB(196, 196, 206), RGB(0, 0, 0), RGB(0, 0, 0), 20, 24, FALSE};

// ---------------------------------------------------------------- types

typedef struct { WCHAR *name, *parse; } App;
typedef struct { HANDLE heap; App *items; int n, cap; } AppIndex;

typedef struct {
    int kind, score, icon, cmd;  // icon: -2 = not resolved yet, -1 = none
    WCHAR title[160];
    WCHAR sub[400];
    WCHAR target[1100];
} Result;

typedef struct { WCHAR *t; int n; } Hist;

typedef struct { DWORD reply_hwnd, reply_copydata_message, search_flags, offset, max_results, request_flags, sort_type; } EvQuery2;
typedef struct { DWORD totitems, numitems, offset, request_flags, sort_type; } EvList2;
typedef struct { DWORD flags, data_offset; } EvItem2;

typedef struct { DBSTATUS pathStatus; DBLENGTH pathLen; WCHAR path[1024]; DBSTATUS attrStatus; ULONG attrs; } WsRow;
typedef struct { DWORD seq; int n; struct { ULONG attrs; WCHAR path[1024]; } it[WS_MAX]; } WsBatch;  // n < 0: failed

typedef struct { int AccentState, AccentFlags; DWORD GradientColor; int AnimationId; } AccentPolicy;
typedef struct { int Attribute; void *Data; SIZE_T SizeOfData; } WinCompAttrData;
typedef BOOL (WINAPI *SetWindowCompositionAttributeFn)(HWND, WinCompAttrData *);

typedef struct { HDC dc; HBITMAP bmp; HGDIOBJ old; DWORD *px; int w, h; } Dib;

// ---------------------------------------------------------------- state

static HINSTANCE g_inst;
static HWND g_hwnd, g_settingsDlg;
static HFONT g_fEdit, g_fTitle, g_fSub, g_fGlyph;  // g_fGlyph: icon font for commands
static HDC g_measureDC;
static int g_editTextH;
static HIMAGELIST g_iml;
static HICON g_icon, g_iconBig;
static UINT g_dpi = 96;
static RECT g_work;
static NOTIFYICONDATAW g_nid;
static UINT g_wmTaskbarCreated;
static POINT g_lastMouse;
static BOOL g_activated, g_hotkeyOk, g_acrylic, g_dragging;
static Palette g_pal;

// Strip renderer: g_band holds rows [g_bandY, g_bandY + g_bandH) of the window.
static Dib g_band, g_mask, g_icoK, g_icoW;
static int g_bandY, g_bandH;

// Search box.
static WCHAR g_in[512];
static int g_inLen, g_caret, g_anchor, g_scroll;
static BOOL g_caretOn = TRUE;

static AppIndex *g_apps;
static ULONGLONG g_appsAt;
static volatile LONG g_indexing;

static WCHAR g_query[512];
static DWORD g_seq;
static enum { FS_NONE, FS_EVERYTHING, FS_WINSEARCH } g_fileSource;

// Windows Search worker: the main thread posts the latest request, the worker coalesces.
static CRITICAL_SECTION g_wsLock;
static HANDLE g_wsWake;
static WCHAR g_wsQuery[512];
static DWORD g_wsSeq;
static BOOL g_wsPending, g_wsRelease;

static Result g_local[MAX_ROWS], g_files[MAX_ROWS], g_extra;
static int g_nlocal, g_nfiles;
static Result *g_shown[MAX_ROWS];
static int g_nshown, g_sel;

static Hist g_hist[256];
static int g_nhist;
static WCHAR g_dataDir[MAX_PATH], g_histPath[MAX_PATH], g_iniPath[MAX_PATH];

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }
static int Min(int a, int b) { return a < b ? a : b; }
static int Max(int a, int b) { return a > b ? a : b; }

// ---------------------------------------------------------------- matching

static WCHAR Fold(WCHAR c) {
    if (c >= L'A' && c <= L'Z') return c + 32;
    if (c < 128) return c;
    return (WCHAR)(UINT_PTR)CharLowerW((LPWSTR)(UINT_PTR)c);
}

static BOOL IsSep(WCHAR c) {
    return c == L' ' || c == L'-' || c == L'_' || c == L'.' || c == L'\\' || c == L'/' || c == L'(' || c == L'[';
}

static BOOL IsWordStart(const WCHAR *s, int i) {
    return i == 0 || IsSep(s[i - 1]) || (IsCharUpperW(s[i]) && IsCharLowerW(s[i - 1]));
}

// Subsequence match where every jump lands on a word start or stays inside the current word,
// so "vsc" finds "Visual Studio Code" but "report" does not match "dRivEr suPpOrT".
// Plain substrings always match; pure acronyms (every jump at a word start) score close to them.
// Returns -1 for no match; higher is better.
static int Fuzzy(const WCHAR *s, const WCHAR *q0) {
    int len = (int)wcslen(s);
    const WCHAR *sub = StrStrIW(s, q0);
    if (sub) {
        int at = (int)(sub - s);
        return 60 + (at == 0 ? 40 : IsWordStart(s, at) ? 20 : 0) - at / 2 - len / 10;
    }
    int score = 0, si = 0, last = -1;
    BOOL acronym = TRUE;
    for (const WCHAR *q = q0; *q; q++) {
        WCHAR qc = Fold(*q);
        if (qc == L' ') continue;
        if (last >= 0 && si == last + 1 && s[si] && Fold(s[si]) == qc) {
            score += 4;  // continues the run
        } else {
            // Jump: prefer the next word start; otherwise a later char in the same word.
            int found = -1;
            for (int i = si; s[i]; i++)
                if (Fold(s[i]) == qc && IsWordStart(s, i)) { found = i; break; }
            if (found < 0 && last >= 0) {
                for (int i = si; s[i] && !IsSep(s[i]); i++)
                    if (Fold(s[i]) == qc) { found = i; break; }
            }
            if (found < 0) return -1;
            si = found;
            if (IsWordStart(s, si)) score += 8;
            else { score += 1; acronym = FALSE; }
        }
        last = si++;
    }
    return last < 0 ? -1 : score + (acronym ? 40 : 0) - len / 10;
}

// ---------------------------------------------------------------- calculator

typedef struct { const WCHAR *p; BOOL ok; } Calc;
static double CExpr(Calc *c);
static double CUnary(Calc *c);

static void CWs(Calc *c) { while (*c->p == L' ') c->p++; }

static double CAtom(Calc *c) {
    CWs(c);
    WCHAR ch = *c->p;
    if (ch == L'(') {
        c->p++;
        double v = CExpr(c);
        CWs(c);
        if (*c->p == L')') c->p++; else c->ok = FALSE;
        return v;
    }
    if (iswalpha(ch)) {
        WCHAR id[8]; int n = 0;
        while (iswalpha(*c->p) && n < 7) id[n++] = Fold(*c->p++);
        id[n] = 0;
        if (!wcscmp(id, L"pi")) return M_PI;
        if (!wcscmp(id, L"e")) return M_E;
        double v = CAtom(c);
        if (!wcscmp(id, L"sqrt")) return sqrt(v);
        if (!wcscmp(id, L"sin")) return sin(v);
        if (!wcscmp(id, L"cos")) return cos(v);
        if (!wcscmp(id, L"tan")) return tan(v);
        if (!wcscmp(id, L"ln")) return log(v);
        if (!wcscmp(id, L"log")) return log10(v);
        if (!wcscmp(id, L"abs")) return fabs(v);
        c->ok = FALSE;
        return 0;
    }
    if (iswdigit(ch) || ch == L'.') {
        WCHAR *end;
        double v = wcstod(c->p, &end);
        if (end == c->p) c->ok = FALSE;
        c->p = end;
        return v;
    }
    c->ok = FALSE;
    return 0;
}

static double CPow(Calc *c) {
    double b = CAtom(c);
    CWs(c);
    if (*c->p == L'^') { c->p++; return pow(b, CUnary(c)); }
    return b;
}

static double CUnary(Calc *c) {
    CWs(c);
    if (*c->p == L'-') { c->p++; return -CUnary(c); }
    if (*c->p == L'+') { c->p++; return CUnary(c); }
    return CPow(c);
}

static double CTerm(Calc *c) {
    double v = CUnary(c);
    for (;;) {
        CWs(c);
        WCHAR op = *c->p;
        if (op == L'*' || op == 0x00D7) { c->p++; v *= CUnary(c); }
        else if (op == L'/' || op == 0x00F7) { c->p++; v /= CUnary(c); }
        else if (op == L'%') { c->p++; v = fmod(v, CUnary(c)); }
        else return v;
    }
}

static double CExpr(Calc *c) {
    double v = CTerm(c);
    for (;;) {
        CWs(c);
        if (*c->p == L'+') { c->p++; v += CTerm(c); }
        else if (*c->p == L'-') { c->p++; v -= CTerm(c); }
        else return v;
    }
}

static BOOL TryCalc(const WCHAR *q, double *out) {
    if (!wcspbrk(q, L"0123456789")) return FALSE;
    BOOL hasOp = wcspbrk(q, L"+-*/^%()\x00D7\x00F7") != NULL;
    for (const WCHAR *p = q; *p && !hasOp; p++) hasOp = iswalpha(*p);
    if (!hasOp) return FALSE;
    Calc c = {q, TRUE};
    double v = CExpr(&c);
    CWs(&c);
    if (!c.ok || *c.p || !isfinite(v)) return FALSE;
    *out = v == 0 ? 0 : v;
    return TRUE;
}

static void FormatNum(double v, WCHAR *out, int cap) {
    if (fabs(v) < 1e15 && v == floor(v)) _snwprintf(out, cap, L"%.0f", v);
    else _snwprintf(out, cap, L"%.10g", v);
    out[cap - 1] = 0;
}

// ---------------------------------------------------------------- files on disk: settings + history

static void InitPaths(void) {
    PWSTR roaming;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &roaming))) return;
    _snwprintf(g_dataDir, MAX_PATH, L"%ls\\Expere", roaming);
    g_dataDir[MAX_PATH - 1] = 0;
    _snwprintf(g_histPath, MAX_PATH, L"%ls\\history.txt", g_dataDir);
    _snwprintf(g_iniPath, MAX_PATH, L"%ls\\settings.ini", g_dataDir);
    g_histPath[MAX_PATH - 1] = g_iniPath[MAX_PATH - 1] = 0;
    CoTaskMemFree(roaming);
}

static int IniInt(const WCHAR *key, int def, int lo, int hi) {
    int v = (int)GetPrivateProfileIntW(APP_NAME, key, def, g_iniPath);
    return v < lo ? lo : v > hi ? hi : v;
}

static void IniPut(const WCHAR *key, int v) {
    WCHAR b[16];
    _snwprintf(b, 16, L"%d", v);
    b[15] = 0;
    WritePrivateProfileStringW(APP_NAME, key, b, g_iniPath);
}

static void LoadSettings(void) {
    if (!g_iniPath[0]) return;
    g_set.hotMod     = IniInt(L"HotkeyModifiers", MOD_ALT, 1, 15);
    g_set.hotKey     = IniInt(L"HotkeyKey", VK_SPACE, 1, 254);
    g_set.theme      = IniInt(L"Theme", THEME_SYSTEM, 0, 2);
    g_set.backdrop   = IniInt(L"Background", BD_ACRYLIC, 0, 2);
    g_set.opacity    = IniInt(L"Opacity", 70, 20, 100);
    g_set.maxResults = IniInt(L"MaxResults", 8, 3, MAX_ROWS);
    g_set.engine     = IniInt(L"SearchEngine", 0, 0, (int)ARRAYSIZE(ENGINES) - 1);
    g_set.files      = IniInt(L"FileSearch", 1, 0, 1);
    g_set.keepQuery  = IniInt(L"KeepQuery", 0, 0, 1);
}

static void SaveSettings(void) {
    if (!g_iniPath[0]) return;
    CreateDirectoryW(g_dataDir, NULL);
    IniPut(L"HotkeyModifiers", (int)g_set.hotMod);
    IniPut(L"HotkeyKey", (int)g_set.hotKey);
    IniPut(L"Theme", g_set.theme);
    IniPut(L"Background", g_set.backdrop);
    IniPut(L"Opacity", g_set.opacity);
    IniPut(L"MaxResults", g_set.maxResults);
    IniPut(L"SearchEngine", g_set.engine);
    IniPut(L"FileSearch", g_set.files);
    IniPut(L"KeepQuery", g_set.keepQuery);
}

static int HistCount(const WCHAR *prefix, const WCHAR *rest) {
    size_t pl = wcslen(prefix);
    for (int i = 0; i < g_nhist; i++)
        if (_wcsnicmp(g_hist[i].t, prefix, pl) == 0 && _wcsicmp(g_hist[i].t + pl, rest) == 0) return g_hist[i].n;
    return 0;
}

static int HistBonus(int n) { return (n > 20 ? 20 : n) * 6; }

static void LoadHistory(void) {
    FILE *f = g_histPath[0] ? _wfopen(g_histPath, L"rt, ccs=UTF-8") : NULL;
    if (!f) return;
    WCHAR line[1200];
    while (g_nhist < 256 && fgetws(line, 1200, f)) {
        WCHAR *tab = wcschr(line, L'\t');
        if (!tab) continue;
        *tab = 0;
        WCHAR *t = tab + 1;
        t[wcscspn(t, L"\r\n")] = 0;
        if (!*t) continue;
        g_hist[g_nhist].n = _wtoi(line);
        g_hist[g_nhist].t = _wcsdup(t);
        g_nhist++;
    }
    fclose(f);
}

static void SaveHistory(void) {
    if (!g_histPath[0]) return;
    CreateDirectoryW(g_dataDir, NULL);
    FILE *f = _wfopen(g_histPath, L"wt, ccs=UTF-8");
    if (!f) return;
    for (int i = 0; i < g_nhist; i++) fwprintf(f, L"%d\t%ls\n", g_hist[i].n, g_hist[i].t);
    fclose(f);
}

static void HistBump(const WCHAR *target) {
    int lo = 0;
    for (int i = 0; i < g_nhist; i++) {
        if (_wcsicmp(g_hist[i].t, target) == 0) { g_hist[i].n++; SaveHistory(); return; }
        if (g_hist[i].n < g_hist[lo].n) lo = i;
    }
    int i = g_nhist < 256 ? g_nhist++ : lo;
    if (i == lo && g_nhist == 256) free(g_hist[i].t);
    g_hist[i].t = _wcsdup(target);
    g_hist[i].n = 1;
    SaveHistory();
}

static void ClearHistory(void) {
    for (int i = 0; i < g_nhist; i++) free(g_hist[i].t);
    g_nhist = 0;
    if (g_histPath[0]) DeleteFileW(g_histPath);
}

// ---------------------------------------------------------------- app index (background thread)

static WCHAR *HeapDup(HANDLE heap, const WCHAR *s) {
    size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    WCHAR *d = HeapAlloc(heap, 0, n);
    if (d) memcpy(d, s, n);
    return d;
}

static DWORD WINAPI AppIndexThread(LPVOID unused) {
    (void)unused;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HANDLE heap = HeapCreate(HEAP_NO_SERIALIZE, 0, 0);
    AppIndex *ix = heap ? HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof *ix) : NULL;
    if (ix) {
        ix->heap = heap;
        IShellItem *folder;
        if (SUCCEEDED(SHGetKnownFolderItem(&kFolderAppsFolder, KF_FLAG_DEFAULT, NULL, &IID_IShellItem, (void **)&folder))) {
            IEnumShellItems *en;
            if (SUCCEEDED(IShellItem_BindToHandler(folder, NULL, &kBhidEnumItems, &IID_IEnumShellItems, (void **)&en))) {
                IShellItem *it;
                while (IEnumShellItems_Next(en, 1, &it, NULL) == S_OK) {
                    LPWSTR name = NULL, parse = NULL;
                    if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_NORMALDISPLAY, &name)) &&
                        SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_PARENTRELATIVEPARSING, &parse))) {
                        if (ix->n == ix->cap) {
                            int cap = ix->cap ? ix->cap * 2 : 256;
                            App *grown = ix->items ? HeapReAlloc(heap, 0, ix->items, cap * sizeof(App))
                                                   : HeapAlloc(heap, 0, cap * sizeof(App));
                            if (grown) { ix->items = grown; ix->cap = cap; }
                        }
                        if (ix->n < ix->cap) {
                            ix->items[ix->n].name = HeapDup(heap, name);
                            ix->items[ix->n].parse = HeapDup(heap, parse);
                            if (ix->items[ix->n].name && ix->items[ix->n].parse) ix->n++;
                        }
                    }
                    CoTaskMemFree(name);
                    CoTaskMemFree(parse);
                    IShellItem_Release(it);
                }
                IEnumShellItems_Release(en);
            }
            IShellItem_Release(folder);
        }
    }
    if (!ix || !PostMessageW(g_hwnd, WM_APP_APPS, 0, (LPARAM)ix)) {
        if (heap) HeapDestroy(heap);
        InterlockedExchange(&g_indexing, 0);
    }
    CoUninitialize();
    return 0;
}

static void StartAppIndex(void) {
    if (InterlockedExchange(&g_indexing, 1)) return;
    HANDLE t = CreateThread(NULL, 64 * 1024, AppIndexThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&g_indexing, 0);
}

// ---------------------------------------------------------------- results

// Keeps `a` sorted by score, capped at `cap`. Returns the slot to fill, or NULL if it does not rank.
static Result *Slot(Result *a, int *n, int cap, int score) {
    int i = *n;
    if (i == cap) {
        if (score <= a[cap - 1].score) return NULL;
        i = cap - 1;
    } else {
        (*n)++;
    }
    while (i > 0 && a[i - 1].score < score) { a[i] = a[i - 1]; i--; }
    memset(&a[i], 0, sizeof *a);
    a[i].score = score;
    a[i].icon = -2;
    return &a[i];
}

static void UrlEncode(const WCHAR *in, WCHAR *out, int cap, BOOL plusForSpace) {
    char u8[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, u8, sizeof u8, NULL, NULL);
    int o = 0;
    for (int i = 0; i < n - 1 && o < cap - 4; i++) {
        unsigned char c = (unsigned char)u8[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = c;
        else if (c == ' ' && plusForSpace)
            out[o++] = L'+';
        else
            o += _snwprintf(out + o, 4, L"%%%02X", c);
    }
    out[o] = 0;
}

// ---------------------------------------------------------------- Windows Search (worker thread)

// Hands pages back to Windows. They stay on the standby list, so the next open soft-faults them in.
static void Trim(void) {
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void WsDisconnect(IDBInitialize **db, IDBCreateCommand **cc) {
    if (*cc) { IDBCreateCommand_Release(*cc); *cc = NULL; }
    if (*db) { IDBInitialize_Uninitialize(*db); IDBInitialize_Release(*db); *db = NULL; }
    CoFreeUnusedLibrariesEx(0, 0);
}

static BOOL WsConnect(IDBInitialize **db, IDBCreateCommand **cc) {
    IDataInitialize *di = NULL;
    IDBCreateSession *cs = NULL;
    BOOL ok = FALSE;
    if (SUCCEEDED(CoCreateInstance(&CLSID_MSDAINITIALIZE, NULL, CLSCTX_INPROC_SERVER, &IID_IDataInitialize, (void **)&di)) &&
        SUCCEEDED(IDataInitialize_GetDataSource(di, NULL, CLSCTX_INPROC_SERVER,
                  L"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';", &IID_IDBInitialize, (IUnknown **)db)) &&
        SUCCEEDED(IDBInitialize_Initialize(*db)) &&
        SUCCEEDED(IDBInitialize_QueryInterface(*db, &IID_IDBCreateSession, (void **)&cs)) &&
        SUCCEEDED(IDBCreateSession_CreateSession(cs, NULL, &IID_IDBCreateCommand, (IUnknown **)cc)))
        ok = TRUE;
    if (cs) IDBCreateSession_Release(cs);
    if (di) IDataInitialize_Release(di);
    if (!ok) WsDisconnect(db, cc);
    return ok;
}

// Each query word becomes a filename prefix term: "vs code" -> "vs*" AND "code*".
static BOOL WsBuildSql(const WCHAR *q, WCHAR *sql, int cap) {
    WCHAR terms[700] = L"";
    int t = 0;
    for (const WCHAR *p = q; *p;) {
        while (*p == L' ') p++;
        if (!*p) break;
        if (t && t < 680) t += _snwprintf(terms + t, 700 - t, L" AND ");
        if (t < 690) terms[t++] = L'"';
        for (; *p && *p != L' '; p++)
            if (*p != L'"' && *p != L'\'' && *p != L'*' && t < 690) terms[t++] = *p;
        if (t < 697) { terms[t++] = L'*'; terms[t++] = L'"'; }
        terms[t] = 0;
    }
    if (!t) return FALSE;
    _snwprintf(sql, cap, L"SELECT TOP %d System.ItemPathDisplay, System.FileAttributes FROM SystemIndex "
                         L"WHERE SCOPE='file:' AND CONTAINS(System.FileName, '%ls') ORDER BY System.DateModified DESC",
               WS_FETCH, terms);
    sql[cap - 1] = 0;
    return TRUE;
}

static WsBatch *WsRun(IDBCreateCommand *cc, const WCHAR *q, DWORD seq) {
    WsBatch *b = HeapAlloc(GetProcessHeap(), 0, sizeof *b);
    if (!b) return NULL;
    b->seq = seq;
    b->n = -1;
    WCHAR sql[1600];
    if (!WsBuildSql(q, sql, 1600)) { b->n = 0; return b; }

    ICommandText *ct = NULL;
    IRowset *rs = NULL;
    IAccessor *acc = NULL;
    HACCESSOR ha = 0;
    if (FAILED(IDBCreateCommand_CreateCommand(cc, NULL, &IID_ICommandText, (IUnknown **)&ct)) ||
        FAILED(ICommandText_SetCommandText(ct, &kDbguidDefault, sql)) ||
        FAILED(ICommandText_Execute(ct, NULL, &IID_IRowset, NULL, NULL, (IUnknown **)&rs)) || !rs ||
        FAILED(IRowset_QueryInterface(rs, &IID_IAccessor, (void **)&acc)))
        goto done;

    DBBINDING bind[2] = {0};
    bind[0].iOrdinal = 1;
    bind[0].obValue = offsetof(WsRow, path);
    bind[0].obLength = offsetof(WsRow, pathLen);
    bind[0].obStatus = offsetof(WsRow, pathStatus);
    bind[0].dwPart = DBPART_VALUE | DBPART_LENGTH | DBPART_STATUS;
    bind[0].cbMaxLen = sizeof(((WsRow *)0)->path);
    bind[0].wType = DBTYPE_WSTR;
    bind[1].iOrdinal = 2;
    bind[1].obValue = offsetof(WsRow, attrs);
    bind[1].obStatus = offsetof(WsRow, attrStatus);
    bind[1].dwPart = DBPART_VALUE | DBPART_STATUS;
    bind[1].cbMaxLen = sizeof(ULONG);
    bind[1].wType = DBTYPE_UI4;
    for (int i = 0; i < 2; i++) { bind[i].dwMemOwner = DBMEMOWNER_CLIENTOWNED; bind[i].eParamIO = DBPARAMIO_NOTPARAM; }
    if (FAILED(IAccessor_CreateAccessor(acc, DBACCESSOR_ROWDATA, 2, bind, sizeof(WsRow), &ha, NULL))) goto done;

    b->n = 0;
    for (BOOL more = TRUE; more && b->n < WS_MAX;) {
        HROW rows[30], *prows = rows;
        DBCOUNTITEM got = 0;
        HRESULT hr = IRowset_GetNextRows(rs, DB_NULL_HCHAPTER, 0, 30, &got, &prows);
        more = hr == S_OK && got == 30;  // DB_S_ENDOFROWSET is a success code that means "no more"
        for (DBCOUNTITEM i = 0; i < got && b->n < WS_MAX; i++) {
            WsRow r;
            if (FAILED(IRowset_GetData(rs, rows[i], ha, &r)) || r.pathStatus != DBSTATUS_S_OK) continue;
            BOOL skip = FALSE;
            for (int x = 0; x < (int)ARRAYSIZE(WS_EXCLUDE) && !skip; x++) skip = StrStrIW(r.path, WS_EXCLUDE[x]) != NULL;
            if (skip) continue;
            b->it[b->n].attrs = r.attrStatus == DBSTATUS_S_OK ? r.attrs : 0;
            lstrcpynW(b->it[b->n].path, r.path, 1024);
            b->n++;
        }
        if (got) IRowset_ReleaseRows(rs, got, rows, NULL, NULL, NULL);
    }
done:
    if (ha) IAccessor_ReleaseAccessor(acc, ha, NULL);
    if (acc) IAccessor_Release(acc);
    if (rs) IRowset_Release(rs);
    if (ct) ICommandText_Release(ct);
    return b;
}

static DWORD WINAPI WsThread(LPVOID unused) {
    (void)unused;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IDBInitialize *db = NULL;
    IDBCreateCommand *cc = NULL;
    for (;;) {
        WaitForSingleObject(g_wsWake, INFINITE);
        EnterCriticalSection(&g_wsLock);
        BOOL have = g_wsPending, release = g_wsRelease;
        WCHAR q[512];
        DWORD seq = g_wsSeq;
        wcscpy(q, g_wsQuery);
        g_wsPending = g_wsRelease = FALSE;
        LeaveCriticalSection(&g_wsLock);

        if (have) {
            WsBatch *b = NULL;
            if (cc || WsConnect(&db, &cc)) b = WsRun(cc, q, seq);
            if (!b && (b = HeapAlloc(GetProcessHeap(), 0, offsetof(WsBatch, it)))) { b->seq = seq; b->n = -1; }
            if (b && b->n < 0) WsDisconnect(&db, &cc);  // reconnect next time
            if (b && !PostMessageW(g_hwnd, WM_APP_FILES, 0, (LPARAM)b)) HeapFree(GetProcessHeap(), 0, b);
        } else if (release && (db || cc)) {
            WsDisconnect(&db, &cc);
            Trim();  // the provider's pages were touched after the main thread trimmed
        }
    }
    return 0;
}

static void WsRequest(const WCHAR *q, DWORD seq, BOOL release) {
    EnterCriticalSection(&g_wsLock);
    if (release) {
        g_wsRelease = TRUE;
        g_wsPending = FALSE;
    } else {
        lstrcpynW(g_wsQuery, q, 512);
        g_wsSeq = seq;
        g_wsPending = TRUE;
    }
    LeaveCriticalSection(&g_wsLock);
    SetEvent(g_wsWake);
}

// ---------------------------------------------------------------- Everything (only if already running)

static HWND FindEverything(void) {
    HWND ev = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", NULL);
    return ev ? ev : FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION_(1.5a)", NULL);
}

static BOOL QueryEverything(const WCHAR *q) {
    HWND ev = FindEverything();
    if (!ev) return FALSE;
    static BYTE buf[sizeof(EvQuery2) + 1024 * sizeof(WCHAR)];
    EvQuery2 *h = (EvQuery2 *)buf;
    WCHAR *s = (WCHAR *)(h + 1);
    _snwprintf(s, 1023, L"%ls %ls", q, EV_EXCLUDE);
    s[1023] = 0;
    h->reply_hwnd = (DWORD)(UINT_PTR)g_hwnd;
    h->reply_copydata_message = EV_REPLY_BASE | (g_seq & 0xFFFF);
    h->search_flags = 0;
    h->offset = 0;
    h->max_results = 48;
    h->request_flags = 1 | 2;  // name, path
    h->sort_type = 14;         // date modified, newest first
    COPYDATASTRUCT cds = {18 /* QUERY2W */, (DWORD)(sizeof(EvQuery2) + (wcslen(s) + 1) * sizeof(WCHAR)), buf};
    DWORD_PTR res = 0;
    return SendMessageTimeoutW(ev, WM_COPYDATA, (WPARAM)g_hwnd, (LPARAM)&cds, SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &res) && res;
}

// ---------------------------------------------------------------- query -> results

static void Merge(void) {
    int a = 0, b = 0, n = 0, cap = g_query[0] ? g_set.maxResults - 1 : 0;
    while (n < cap && (a < g_nlocal || b < g_nfiles)) {
        if (b >= g_nfiles || (a < g_nlocal && g_local[a].score >= g_files[b].score)) g_shown[n++] = &g_local[a++];
        else g_shown[n++] = &g_files[b++];
    }
    if (g_query[0]) g_shown[n++] = &g_extra;
    g_nshown = n;
    if (g_sel >= n) g_sel = n - 1;
    if (g_sel < 0) g_sel = 0;
}

static void Relayout(void) {
    int w = S(WIN_W), h = S(EDIT_H);
    if (g_nshown) h += S(1) + S(PAD) * 2 + g_nshown * S(ROW_H) + S(FOOT_H);
    int x = g_work.left + ((g_work.right - g_work.left) - w) / 2;
    int y = g_work.top + (g_work.bottom - g_work.top) / 5;
    SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void OnQueryChanged(void) {
    const WCHAR *q = g_in;
    while (*q == L' ') q++;
    lstrcpynW(g_query, q, 512);
    for (size_t n = wcslen(g_query); n && g_query[n - 1] == L' ';) g_query[--n] = 0;

    g_seq++;
    g_nlocal = g_nfiles = 0;
    g_sel = 0;
    g_fileSource = FS_NONE;
    KillTimer(g_hwnd, TIMER_SEARCH);

    if (g_query[0]) {
        double v;
        BOOL calc = TryCalc(g_query, &v);
        if (calc) {
            Result *r = Slot(g_local, &g_nlocal, MAX_ROWS, 1 << 20);
            r->kind = K_CALC;
            FormatNum(v, r->target, 64);
            _snwprintf(r->title, 160, L"= %ls", r->target);
            wcscpy(r->sub, L"Press Enter to copy the result");
        }
        if (g_apps) {
            for (int i = 0; i < g_apps->n; i++) {
                App *a = &g_apps->items[i];
                int s = Fuzzy(a->name, g_query);
                if (s < 0) continue;
                s += 30 + HistBonus(HistCount(L"shell:AppsFolder\\", a->parse));  // apps edge out files of equal quality
                Result *r = Slot(g_local, &g_nlocal, MAX_ROWS, s);
                if (!r) continue;
                r->kind = K_APP;
                lstrcpynW(r->title, a->name, 160);
                _snwprintf(r->target, 1100, L"shell:AppsFolder\\%ls", a->parse);
                r->target[1099] = 0;
            }
        }
        for (int i = 0; i < (int)ARRAYSIZE(CMDS); i++) {
            int s = Fuzzy(CMDS[i].name, g_query);
            if (s < 0) continue;
            Result *r = Slot(g_local, &g_nlocal, MAX_ROWS, s + 30);
            if (!r) continue;
            r->kind = K_CMD;
            r->cmd = i;
            wcscpy(r->title, CMDS[i].name);
            wcscpy(r->sub, CMDS[i].sub);
        }
        if (g_set.files && !calc && wcslen(g_query) >= 2) {
            if (QueryEverything(g_query)) {
                g_fileSource = FS_EVERYTHING;
            } else {
                g_fileSource = FS_WINSEARCH;  // debounced: typing fast sends one query, not one per key
                SetTimer(g_hwnd, TIMER_SEARCH, SEARCH_DELAY_MS, NULL);
            }
        }
        WCHAR enc[1000];
        memset(&g_extra, 0, sizeof g_extra);
        g_extra.kind = K_WEB;
        g_extra.icon = -2;
        _snwprintf(g_extra.title, 160, L"Search the web for “%ls”", g_query);
        wcscpy(g_extra.sub, ENGINES[g_set.engine].name);
        UrlEncode(g_query, enc, 1000, TRUE);
        _snwprintf(g_extra.target, 1100, L"%ls%ls", ENGINES[g_set.engine].url, enc);
        g_extra.target[1099] = 0;
    }
    Merge();
    Relayout();
}

static void AddFile(const WCHAR *name, const WCHAR *dir, const WCHAR *full, BOOL folder) {
    int s = Fuzzy(name, g_query);
    if (s < 0) s = 0;
    s += HistBonus(HistCount(L"", full)) - 10;
    Result *r = Slot(g_files, &g_nfiles, MAX_ROWS, s);
    if (!r) return;
    r->kind = folder ? K_FOLDER : K_FILE;
    lstrcpynW(r->title, name, 160);
    lstrcpynW(r->sub, dir, 400);
    lstrcpynW(r->target, full, 1100);
}

// Used only when no file index answers: hand the query to Explorer.
static void AddExplorerSearch(void) {
    Result *r = Slot(g_local, &g_nlocal, MAX_ROWS, -200);
    if (!r) return;
    WCHAR enc[1000];
    r->kind = K_SEARCH;
    _snwprintf(r->title, 160, L"Search files for “%ls”", g_query);
    wcscpy(r->sub, L"Open the search in File Explorer");
    UrlEncode(g_query, enc, 1000, FALSE);
    _snwprintf(r->target, 1100, L"search-ms:query=%ls", enc);
}

static void OnSearchResults(const WsBatch *b) {
    g_nfiles = 0;
    if (b->n < 0) {
        g_fileSource = FS_NONE;
        AddExplorerSearch();
    }
    for (int i = 0; i < b->n; i++) {
        const WCHAR *full = b->it[i].path, *name = PathFindFileNameW(full);
        WCHAR dir[1024];
        lstrcpynW(dir, full, (int)(name - full));  // drops the trailing separator
        if (name == full) dir[0] = 0;
        AddFile(name, dir, full, (b->it[i].attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
    Merge();
    Relayout();
}

static void OnEverythingReply(const COPYDATASTRUCT *cds) {
    if (cds->cbData < sizeof(EvList2)) return;
    const BYTE *base = cds->lpData, *end = base + cds->cbData;
    const EvList2 *l = (const EvList2 *)base;
    const EvItem2 *items = (const EvItem2 *)(l + 1);
    if (l->numitems > (cds->cbData - sizeof(EvList2)) / sizeof(EvItem2)) return;

    g_nfiles = 0;
    for (DWORD i = 0; i < l->numitems; i++) {
        if (items[i].flags & 2) continue;  // drive
        const BYTE *p = base + items[i].data_offset;
        const WCHAR *field[2];
        BOOL ok = TRUE;
        for (int f = 0; f < 2 && ok; f++) {
            if (p < base || p + sizeof(DWORD) > end) { ok = FALSE; break; }
            DWORD len = *(const DWORD *)p;
            p += sizeof(DWORD);
            if (len > 32767 || p + (len + 1) * sizeof(WCHAR) > end) { ok = FALSE; break; }
            field[f] = (const WCHAR *)p;
            p += (len + 1) * sizeof(WCHAR);
        }
        if (!ok) continue;
        WCHAR full[1100];
        _snwprintf(full, 1100, L"%ls\\%ls", field[1], field[0]);
        full[1099] = 0;
        AddFile(field[0], field[1], full, (items[i].flags & 1) != 0);
    }
    Merge();
    Relayout();
}

// ---------------------------------------------------------------- strip renderer (premultiplied BGRA)

static BOOL DibEnsure(Dib *d, int w, int h);

static void DibFree(Dib *d) {
    if (d->dc) { SelectObject(d->dc, d->old); DeleteDC(d->dc); }
    if (d->bmp) DeleteObject(d->bmp);
    memset(d, 0, sizeof *d);
}

static BOOL DibEnsure(Dib *d, int w, int h) {
    if (d->px && d->w == w && d->h == h) return TRUE;
    DibFree(d);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    void *bits = NULL;
    d->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!d->bmp) return FALSE;
    d->dc = CreateCompatibleDC(NULL);
    d->old = SelectObject(d->dc, d->bmp);
    d->px = bits;
    d->w = w;
    d->h = h;
    SetBkMode(d->dc, TRANSPARENT);
    return TRUE;
}

static void FreeBuffers(void) {
    DibFree(&g_band);
    DibFree(&g_mask);
    DibFree(&g_icoK);
    DibFree(&g_icoW);
}

static DWORD Premul(COLORREF c, int a) {
    return ((DWORD)a << 24) | ((DWORD)(GetRValue(c) * a / 255) << 16) | ((DWORD)(GetGValue(c) * a / 255) << 8) |
           (DWORD)(GetBValue(c) * a / 255);
}

// Source-over for premultiplied pixels.
static void Over(DWORD *d, DWORD s) {
    DWORD sa = s >> 24;
    if (!sa) return;
    if (sa == 255) { *d = s; return; }
    DWORD ia = 255 - sa, v = *d;
    DWORD a = sa + ((v >> 24) * ia + 127) / 255;
    DWORD r = ((s >> 16) & 255) + (((v >> 16) & 255) * ia + 127) / 255;
    DWORD g = ((s >> 8) & 255) + (((v >> 8) & 255) * ia + 127) / 255;
    DWORD b = (s & 255) + ((v & 255) * ia + 127) / 255;
    *d = (a << 24) | (r << 16) | (g << 8) | b;
}

// Anti-aliased rounded rectangle in window coordinates, clipped to the current strip.
static void FillRound(int l, int t, int r, int b, int rad, COLORREF c, int alpha) {
    int y0 = Max(t, g_bandY), y1 = Min(b, g_bandY + g_bandH), x0 = Max(l, 0), x1 = Min(r, g_band.w);
    DWORD full = Premul(c, alpha);
    for (int y = y0; y < y1; y++) {
        DWORD *row = g_band.px + (y - g_bandY) * g_band.w;
        float py = y + 0.5f, qy = fmaxf(fmaxf(t + rad - py, py - (b - rad)), 0.f);
        for (int x = x0; x < x1; x++) {
            if (rad > 0) {
                float px = x + 0.5f, qx = fmaxf(fmaxf(l + rad - px, px - (r - rad)), 0.f);
                float cov = 0.5f - (sqrtf(qx * qx + qy * qy) - rad);
                if (cov <= 0) continue;
                if (cov < 1) { Over(&row[x], Premul(c, (int)(alpha * cov))); continue; }
            }
            Over(&row[x], full);
        }
    }
}

// GDI writes no alpha, so text is drawn white-on-black into a mask and its coverage blended in.
static void PaintText(const WCHAR *s, int n, RECT r, HFONT f, UINT fmt, COLORREF c, const RECT *clip) {
    RECT a = r, band = {0, g_bandY, g_band.w, g_bandY + g_bandH};
    if (clip && !IntersectRect(&a, &a, clip)) return;
    if (!IntersectRect(&a, &a, &band)) return;
    for (int y = a.top; y < a.bottom; y++)
        memset(g_mask.px + (y - g_bandY) * g_mask.w + a.left, 0, (a.right - a.left) * sizeof(DWORD));
    SetViewportOrgEx(g_mask.dc, 0, -g_bandY, NULL);
    HGDIOBJ of = SelectObject(g_mask.dc, f);
    SetTextColor(g_mask.dc, RGB(255, 255, 255));
    DrawTextW(g_mask.dc, s, n, &r, fmt | DT_NOPREFIX);
    SelectObject(g_mask.dc, of);
    GdiFlush();
    int cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);
    for (int y = a.top; y < a.bottom; y++) {
        DWORD *m = g_mask.px + (y - g_bandY) * g_mask.w, *d = g_band.px + (y - g_bandY) * g_band.w;
        for (int x = a.left; x < a.right; x++) {
            DWORD v = m[x];
            int cov = Max(Max((v >> 16) & 255, (v >> 8) & 255), v & 255);
            if (cov) Over(&d[x], ((DWORD)cov << 24) | ((DWORD)(cr * cov / 255) << 16) | ((DWORD)(cg * cov / 255) << 8) | (DWORD)(cb * cov / 255));
        }
    }
}

// Alpha recovered from drawing the icon over black and over white: works for every icon format.
static void DrawIconAlpha(HICON hic, int x, int y, int sz) {
    if (y + sz <= g_bandY || y >= g_bandY + g_bandH) return;
    if (!DibEnsure(&g_icoK, sz, sz) || !DibEnsure(&g_icoW, sz, sz)) return;
    memset(g_icoK.px, 0, sz * sz * sizeof(DWORD));
    for (int i = 0; i < sz * sz; i++) g_icoW.px[i] = 0x00FFFFFF;
    DrawIconEx(g_icoK.dc, 0, 0, hic, sz, sz, 0, NULL, DI_NORMAL);
    DrawIconEx(g_icoW.dc, 0, 0, hic, sz, sz, 0, NULL, DI_NORMAL);
    GdiFlush();
    for (int yy = 0; yy < sz; yy++) {
        int by = y + yy - g_bandY;
        if (by < 0 || by >= g_bandH) continue;
        for (int xx = 0; xx < sz; xx++) {
            int bx = x + xx;
            if (bx < 0 || bx >= g_band.w) continue;
            DWORD k = g_icoK.px[yy * sz + xx], w = g_icoW.px[yy * sz + xx];
            int kr = (k >> 16) & 255, kg = (k >> 8) & 255, kb = k & 255;
            int diff = ((int)((w >> 16) & 255) - kr + (int)((w >> 8) & 255) - kg + (int)(w & 255) - kb) / 3;
            int a = 255 - diff;
            if (a <= 0) continue;
            if (a > 255) a = 255;
            Over(&g_band.px[by * g_band.w + bx],
                 ((DWORD)a << 24) | ((DWORD)Min(kr, a) << 16) | ((DWORD)Min(kg, a) << 8) | (DWORD)Min(kb, a));
        }
    }
}

// ---------------------------------------------------------------- search box

static int TextX(int i) {
    SIZE sz = {0};
    SelectObject(g_measureDC, g_fEdit);
    if (i > 0) GetTextExtentPoint32W(g_measureDC, g_in, i, &sz);
    return sz.cx;
}

static int FieldW(void) { return S(WIN_W) - 2 * S(TEXT_X); }

static int HitChar(int x) {
    int rel = x - S(TEXT_X) + g_scroll;
    if (rel <= 0 || !g_inLen) return 0;
    static int dx[512];
    int fit;
    SIZE sz;
    SelectObject(g_measureDC, g_fEdit);
    GetTextExtentExPointW(g_measureDC, g_in, g_inLen, 0, &fit, dx, &sz);
    for (int i = 0, prev = 0; i < g_inLen; prev = dx[i], i++)
        if (rel < (prev + dx[i]) / 2) return (i > 0 && IS_LOW_SURROGATE(g_in[i])) ? i - 1 : i;
    return g_inLen;
}

static void EnsureCaretVisible(void) {
    int fw = FieldW() - S(3), cx = TextX(g_caret), total = TextX(g_inLen);
    if (total <= fw) { g_scroll = 0; return; }
    if (cx - g_scroll > fw) g_scroll = cx - fw;
    if (cx < g_scroll) g_scroll = cx;
    if (g_scroll > total - fw) g_scroll = total - fw;
}

static void InvalidateInput(void) {
    RECT r = {0, 0, S(WIN_W), S(EDIT_H)};
    InvalidateRect(g_hwnd, &r, FALSE);
}

static void StartCaret(void) {
    g_caretOn = TRUE;
    UINT t = GetCaretBlinkTime();
    if (t && t != INFINITE) SetTimer(g_hwnd, TIMER_CARET, t, NULL);
}

static void CaretMoved(void) {
    StartCaret();
    EnsureCaretVisible();
    InvalidateInput();
}

static void InputReplace(int from, int to, const WCHAR *s, int n) {
    if (from == to && n == 0) return;
    int rest = g_inLen - (to - from);
    if (rest + n > 511) n = 511 - rest;
    if (n < 0) n = 0;
    memmove(g_in + from + n, g_in + to, (g_inLen - to + 1) * sizeof(WCHAR));
    memcpy(g_in + from, s, n * sizeof(WCHAR));
    g_inLen = rest + n;
    g_caret = g_anchor = from + n;
    CaretMoved();
    OnQueryChanged();
}

static int CharLeft(int i) {
    if (i <= 0) return 0;
    i--;
    if (i > 0 && IS_LOW_SURROGATE(g_in[i]) && IS_HIGH_SURROGATE(g_in[i - 1])) i--;
    return i;
}

static int CharRight(int i) {
    if (i >= g_inLen) return g_inLen;
    i++;
    if (i < g_inLen && IS_LOW_SURROGATE(g_in[i]) && IS_HIGH_SURROGATE(g_in[i - 1])) i++;
    return i;
}

static int WordLeft(int i) {
    while (i > 0 && g_in[i - 1] == L' ') i--;
    if (i > 0 && IsSep(g_in[i - 1])) return i - 1;
    while (i > 0 && !IsSep(g_in[i - 1])) i--;
    return i;
}

static int WordRight(int i) {
    if (i < g_inLen && IsSep(g_in[i]) && g_in[i] != L' ') i++;
    else while (i < g_inLen && !IsSep(g_in[i])) i++;
    while (i < g_inLen && g_in[i] == L' ') i++;
    return i;
}

static void CopyText(const WCHAR *s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h) {
        memcpy(GlobalLock(h), s, n);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

static void Paste(void) {
    WCHAR buf[512];
    int n = 0;
    if (!OpenClipboard(g_hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *s = h ? GlobalLock(h) : NULL;
    if (s) {
        for (; s[n] && n < 511; n++) buf[n] = (s[n] == L'\r' || s[n] == L'\n' || s[n] == L'\t') ? L' ' : s[n];
        GlobalUnlock(h);
    }
    CloseClipboard();
    if (n) InputReplace(Min(g_caret, g_anchor), Max(g_caret, g_anchor), buf, n);
}

// ---------------------------------------------------------------- appearance

static BOOL SystemUsesDark(void) {
    DWORD v = 1, cb = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, THEME_KEY, L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &cb);
    return v == 0;
}

static DWORD BackgroundPixel(void) {
    if (g_set.backdrop == BD_ACRYLIC && g_acrylic) return 0;  // DWM draws the blurred tint
    return Premul(g_pal.tint, g_set.backdrop == BD_SOLID ? 255 : g_set.opacity * 255 / 100);
}

static void ApplyAppearance(void) {
    BOOL dark = g_set.theme == THEME_DARK || (g_set.theme == THEME_SYSTEM && SystemUsesDark());
    g_pal = dark ? PAL_DARK : PAL_LIGHT;

    static SetWindowCompositionAttributeFn swca;
    if (!swca) swca = (SetWindowCompositionAttributeFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    AccentPolicy ap = {0};
    if (g_set.backdrop == BD_ACRYLIC) {
        DWORD a = (DWORD)(g_set.opacity * 255 / 100);
        ap.AccentState = 4;  // ACCENT_ENABLE_ACRYLICBLURBEHIND
        ap.AccentFlags = 2;
        ap.GradientColor = (a << 24) | ((DWORD)GetBValue(g_pal.tint) << 16) | ((DWORD)GetGValue(g_pal.tint) << 8) | GetRValue(g_pal.tint);
    }
    WinCompAttrData data = {19 /* WCA_ACCENT_POLICY */, &ap, sizeof ap};
    BOOL ok = swca && swca(g_hwnd, &data);
    g_acrylic = ok && g_set.backdrop == BD_ACRYLIC;

    MARGINS mg = {-1, -1, -1, -1};  // DWM honours our alpha everywhere in the client area
    DwmExtendFrameIntoClientArea(g_hwnd, &mg);
    DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof dark);              // DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute(g_hwnd, 33, &corner, sizeof corner);          // DWMWA_WINDOW_CORNER_PREFERENCE
    DwmSetWindowAttribute(g_hwnd, 34, &g_pal.border, sizeof g_pal.border);  // DWMWA_BORDER_COLOR
    if (IsWindowVisible(g_hwnd)) { Merge(); Relayout(); }
}

// ---------------------------------------------------------------- painting

// Built-in rows are drawn as a glyph on an accent tile instead of a shell icon.
static WCHAR GlyphFor(const Result *r) {
    switch (r->kind) {
    case K_CMD:    return CMDS[r->cmd].glyph;
    case K_CALC:   return GLYPH_CALC;
    case K_WEB:    return GLYPH_WEB;
    case K_SEARCH: return GLYPH_SEARCH;
    }
    return 0;
}

static int ResolveIcon(Result *r) {
    if (r->icon != -2) return r->icon;
    r->icon = -1;
    SHFILEINFOW sfi = {0};
    switch (r->kind) {
    case K_APP: {
        PIDLIST_ABSOLUTE pidl;
        if (SUCCEEDED(SHParseDisplayName(r->target, NULL, &pidl, 0, NULL))) {
            if (SHGetFileInfoW((LPCWSTR)pidl, 0, &sfi, sizeof sfi, SHGFI_PIDL | SHGFI_SYSICONINDEX)) r->icon = sfi.iIcon;
            CoTaskMemFree(pidl);
        }
        break;
    }
    case K_FILE:
    case K_FOLDER:
        if (SHGetFileInfoW(r->target, 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX)) r->icon = sfi.iIcon;
        if (r->icon < 0 || r->icon >= ImageList_GetImageCount(g_iml)) {
            // No icon in this list size (some handlers only register small icons): use the type's icon.
            DWORD attr = r->kind == K_FOLDER ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (SHGetFileInfoW(r->target, attr, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_USEFILEATTRIBUTES)) r->icon = sfi.iIcon;
        }
        break;
    }
    return r->icon;
}

static void DrawInput(void) {
    if (g_bandY >= S(EDIT_H)) return;
    int x0 = S(TEXT_X), th = g_editTextH, ty = (S(EDIT_H) - th) / 2;
    RECT field = {x0, ty, x0 + FieldW(), ty + th};
    if (!g_inLen) {
        PaintText(L"Search apps, files, and commands…", -1, field, g_fEdit, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
                  g_pal.sub, NULL);
    } else {
        int s0 = Min(g_caret, g_anchor), s1 = Max(g_caret, g_anchor);
        if (s0 != s1) {
            int a = Max(x0 + TextX(s0) - g_scroll, field.left), b = Min(x0 + TextX(s1) - g_scroll, field.right);
            FillRound(a, ty, b, ty + th, 0, g_pal.accent, 110);
        }
        RECT tr = {x0 - g_scroll, ty, x0 - g_scroll + TextX(g_inLen) + S(8), ty + th};
        PaintText(g_in, g_inLen, tr, g_fEdit, DT_SINGLELINE | DT_VCENTER | DT_LEFT, g_pal.text, &field);
    }
    if (g_caretOn) {
        int cx = x0 + TextX(g_caret) - g_scroll;
        if (cx >= field.left - 1 && cx <= field.right) FillRound(cx, ty + S(2), cx + Max(1, S(2)), ty + th - S(2), 0, g_pal.text, 255);
    }
}

static void DrawRow(Result *r, BOOL sel, int top, int w) {
    int rh = S(ROW_H);
    if (sel) FillRound(S(PAD), top, w - S(PAD), top + rh, S(8), g_pal.sel, g_pal.selA);

    int isz = S(32), ix = S(PAD) + S(12), iy = top + (rh - isz) / 2;
    WCHAR glyph = GlyphFor(r);
    int idx = glyph ? -1 : ResolveIcon(r);
    HICON hic = idx >= 0 && g_iml ? ImageList_GetIcon(g_iml, idx, ILD_NORMAL) : NULL;
    if (hic) {
        DrawIconAlpha(hic, ix, iy, isz);
        DestroyIcon(hic);
    } else {
        RECT gr = {ix, iy, ix + isz, iy + isz};
        FillRound(ix, iy, ix + isz, iy + isz, S(8), g_pal.accent, 255);
        PaintText(glyph ? &glyph : L"•", 1, gr, g_fGlyph, DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(255, 255, 255), NULL);
    }

    const WCHAR *badge = KIND_LABEL[r->kind];
    SIZE bs;
    SelectObject(g_measureDC, g_fSub);
    GetTextExtentPoint32W(g_measureDC, badge, (int)wcslen(badge), &bs);
    int right = w - S(PAD) - S(14);
    RECT br = {right - bs.cx, top, right, top + rh};
    PaintText(badge, -1, br, g_fSub, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, g_pal.sub, NULL);

    int tx = ix + isz + S(14), tr = br.left - S(16);
    if (r->sub[0]) {
        RECT t1 = {tx, top, tr, top + rh / 2 + S(1)}, t2 = {tx, top + rh / 2 + S(2), tr, top + rh};
        UINT ell = (r->kind == K_FILE || r->kind == K_FOLDER) ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS;
        PaintText(r->title, -1, t1, g_fTitle, DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS, g_pal.text, NULL);
        PaintText(r->sub, -1, t2, g_fSub, DT_SINGLELINE | DT_TOP | ell, g_pal.sub, NULL);
    } else {
        RECT t = {tx, top, tr, top + rh};
        PaintText(r->title, -1, t, g_fTitle, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, g_pal.text, NULL);
    }
}

static void DrawScene(int w, int h) {
    DrawInput();
    if (!g_nshown) return;
    int line = Max(1, S(1)), top0 = S(EDIT_H) + S(1) + S(PAD);
    FillRound(0, S(EDIT_H), w, S(EDIT_H) + line, 0, g_pal.line, g_pal.lineA);
    for (int i = 0; i < g_nshown; i++) {
        int top = top0 + i * S(ROW_H);
        if (top < g_bandY + g_bandH && top + S(ROW_H) > g_bandY) DrawRow(g_shown[i], i == g_sel, top, w);
    }
    int fy = h - S(FOOT_H);
    if (fy >= g_bandY + g_bandH || h <= g_bandY) return;
    FillRound(0, fy, w, fy + line, 0, g_pal.line, g_pal.lineA);
    RECT ft = {S(TEXT_X), fy, w - S(TEXT_X), h};
    PaintText(L"↑↓  Navigate      ↵  Open      Ctrl+↵  Show in folder      Ctrl+,  Settings", -1, ft,
              g_fSub, DT_SINGLELINE | DT_VCENTER | DT_LEFT, g_pal.sub, NULL);
    const WCHAR *src = g_fileSource == FS_EVERYTHING ? L"Files via Everything"
                     : g_fileSource == FS_WINSEARCH  ? L"Files via Windows Search" : NULL;
    if (src) PaintText(src, -1, ft, g_fSub, DT_SINGLELINE | DT_VCENTER | DT_RIGHT, g_pal.sub, NULL);
}

// The window is painted in horizontal strips (search box, each row, footer) through one
// row-sized buffer, so a tall window never needs a full-size back buffer.
static void PaintWindow(HDC hdc, const RECT *dirty) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom, ys[MAX_ROWS + 4], nb = 0;
    ys[nb++] = 0;
    if (g_nshown) {
        int top0 = S(EDIT_H) + S(1) + S(PAD);
        for (int i = 0; i <= g_nshown; i++) ys[nb++] = Min(top0 + i * S(ROW_H), h);
    }
    if (ys[nb - 1] < h) ys[nb++] = h;
    int maxH = 0;
    for (int i = 0; i + 1 < nb; i++) maxH = Max(maxH, ys[i + 1] - ys[i]);
    if (w <= 0 || maxH <= 0 || !DibEnsure(&g_band, w, maxH) || !DibEnsure(&g_mask, w, maxH)) return;

    DWORD bg = BackgroundPixel();
    for (int b = 0; b + 1 < nb; b++) {
        g_bandY = ys[b];
        g_bandH = ys[b + 1] - ys[b];
        if (g_bandH <= 0 || g_bandY + g_bandH <= dirty->top || g_bandY >= dirty->bottom) continue;
        for (int i = 0, n = w * g_bandH; i < n; i++) g_band.px[i] = bg;
        DrawScene(w, h);
        BitBlt(hdc, 0, g_bandY, w, g_bandH, g_band.dc, 0, 0, SRCCOPY);
    }
}

static int RowAt(int y) {
    int top0 = S(EDIT_H) + S(1) + S(PAD);
    if (y < top0) return -1;
    int i = (y - top0) / S(ROW_H);
    return i < g_nshown ? i : -1;
}

static HFONT MakeFont(int px, int weight, const WCHAR *face) {
    // Grayscale AA: ClearType needs an opaque background to blend against.
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
}

static void CreateFonts(void) {
    HFONT *fonts[] = {&g_fEdit, &g_fTitle, &g_fSub, &g_fGlyph};
    for (int i = 0; i < 4; i++) if (*fonts[i]) DeleteObject(*fonts[i]);
    g_fEdit = MakeFont(21, FW_NORMAL, L"Segoe UI");
    g_fTitle = MakeFont(15, FW_NORMAL, L"Segoe UI");
    g_fSub = MakeFont(12, FW_NORMAL, L"Segoe UI");
    g_fGlyph = MakeFont(16, FW_NORMAL, L"Segoe Fluent Icons");
    WCHAR face[LF_FACESIZE];
    SelectObject(g_measureDC, g_fGlyph);
    GetTextFaceW(g_measureDC, LF_FACESIZE, face);
    if (_wcsicmp(face, L"Segoe Fluent Icons")) {  // Windows 10
        DeleteObject(g_fGlyph);
        g_fGlyph = MakeFont(16, FW_NORMAL, L"Segoe MDL2 Assets");
    }
    TEXTMETRICW tm;
    SelectObject(g_measureDC, g_fEdit);
    GetTextMetricsW(g_measureDC, &tm);
    g_editTextH = tm.tmHeight;
    SHGetImageList(g_dpi > 120 ? SHIL_EXTRALARGE : SHIL_LARGE, &kIidImageList, (void **)&g_iml);
}

// ---------------------------------------------------------------- show / hide / actions

static void HideLauncher(void) {
    if (!IsWindowVisible(g_hwnd)) return;
    g_activated = FALSE;
    ShowWindow(g_hwnd, SW_HIDE);
    g_seq++;  // drop late search replies
    KillTimer(g_hwnd, TIMER_SEARCH);
    KillTimer(g_hwnd, TIMER_CARET);
    WsRequest(NULL, 0, TRUE);  // the worker drops its Windows Search connection
    FreeBuffers();
    Trim();
    SetTimer(g_hwnd, TIMER_TRIM, TRIM_AGAIN_MS, NULL);
}

static void ShowLauncher(const WCHAR *text) {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(mon, &mi);
    g_work = mi.rcWork;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    if (dx != g_dpi || !g_fEdit) { g_dpi = dx; CreateFonts(); }

    if (text) {
        lstrcpynW(g_in, text, 512);
        g_inLen = (int)wcslen(g_in);
        g_caret = g_anchor = g_inLen;
    } else if (!g_set.keepQuery) {
        g_in[0] = 0;
        g_inLen = g_caret = g_anchor = 0;
    } else {
        g_anchor = 0;  // reopen with the last search selected, ready to overtype
        g_caret = g_inLen;
    }
    g_scroll = 0;
    EnsureCaretVisible();
    OnQueryChanged();

    ShowWindow(g_hwnd, SW_SHOW);
    if (!SetForegroundWindow(g_hwnd)) {
        // Opened from a process without foreground rights (tray, second instance): borrow the input queue.
        DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), NULL), me = GetCurrentThreadId();
        if (fg && fg != me && AttachThreadInput(me, fg, TRUE)) {
            SetForegroundWindow(g_hwnd);
            BringWindowToTop(g_hwnd);
            AttachThreadInput(me, fg, FALSE);
        }
    }
    SetFocus(g_hwnd);
    StartCaret();
    GetCursorPos(&g_lastMouse);
    if (GetTickCount64() - g_appsAt > APP_REFRESH_MS) StartAppIndex();
}

static void Launch(const WCHAR *file, const WCHAR *args, int show) {
    AllowSetForegroundWindow(ASFW_ANY);
    SHELLEXECUTEINFOW sei = {sizeof sei};
    sei.fMask = SEE_MASK_NOASYNC;  // finish the hand-off before we hide and trim
    sei.lpFile = file;
    sei.lpParameters = args;
    sei.nShow = show;
    ShellExecuteExW(&sei);
}

static BOOL Confirm(const WCHAR *msg) {
    return MessageBoxW(NULL, msg, APP_NAME, MB_OKCANCEL | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDOK;
}

static void OpenSettings(void);

static void RunCommand(int cmd) {
    switch (cmd) {
    case CMD_LOCK:     LockWorkStation(); break;
    case CMD_SLEEP:    SetSuspendState(FALSE, FALSE, FALSE); break;
    case CMD_RESTART:  if (Confirm(L"Restart Windows now?")) Launch(L"shutdown.exe", L"/r /t 0", SW_HIDE); break;
    case CMD_SHUTDOWN: if (Confirm(L"Shut down Windows now?")) Launch(L"shutdown.exe", L"/s /t 0", SW_HIDE); break;
    case CMD_SIGNOUT:  if (Confirm(L"Sign out of Windows now?")) ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED); break;
    case CMD_RECYCLE:  SHEmptyRecycleBinW(NULL, NULL, 0); break;
    case CMD_SETTINGS: OpenSettings(); break;
    case CMD_QUIT:     DestroyWindow(g_hwnd); break;
    }
}

static void Activate(int i, BOOL reveal) {
    if (i < 0 || i >= g_nshown) return;
    Result r = *g_shown[i];
    switch (r.kind) {
    case K_CALC:
        CopyText(r.target);
        HideLauncher();
        break;
    case K_CMD:
        HideLauncher();
        RunCommand(r.cmd);
        break;
    case K_WEB:
    case K_SEARCH:
        Launch(r.target, NULL, SW_SHOWNORMAL);
        HideLauncher();
        break;
    default:
        if (reveal && r.kind != K_APP) {
            WCHAR args[1200];
            _snwprintf(args, 1200, L"/select,\"%ls\"", r.target);
            args[1199] = 0;
            Launch(L"explorer.exe", args, SW_SHOWNORMAL);
        } else {
            Launch(r.target, NULL, SW_SHOWNORMAL);
        }
        HistBump(r.target);
        HideLauncher();
    }
}

static BOOL OnKey(WPARAM vk) {
    BOOL ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    int s0 = Min(g_caret, g_anchor), s1 = Max(g_caret, g_anchor);
    switch (vk) {
    case VK_DOWN:
    case VK_UP:
    case VK_TAB:
        if (!g_nshown) return TRUE;
        if (vk == VK_UP || (vk == VK_TAB && shift)) g_sel = (g_sel + g_nshown - 1) % g_nshown;
        else g_sel = (g_sel + 1) % g_nshown;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return TRUE;
    case VK_RETURN:
        Activate(g_sel, ctrl || shift);
        return TRUE;
    case VK_ESCAPE:
        if (g_inLen) InputReplace(0, g_inLen, L"", 0);
        else HideLauncher();
        return TRUE;
    case VK_LEFT:
    case VK_RIGHT:
    case VK_HOME:
    case VK_END: {
        int c;
        if (vk == VK_HOME) c = 0;
        else if (vk == VK_END) c = g_inLen;
        else if (!shift && !ctrl && s0 != s1) c = vk == VK_LEFT ? s0 : s1;  // collapse the selection
        else if (vk == VK_LEFT) c = ctrl ? WordLeft(g_caret) : CharLeft(g_caret);
        else c = ctrl ? WordRight(g_caret) : CharRight(g_caret);
        g_caret = c;
        if (!shift) g_anchor = c;
        CaretMoved();
        return TRUE;
    }
    case VK_BACK:
        if (s0 != s1) InputReplace(s0, s1, L"", 0);
        else if (g_caret) InputReplace(ctrl ? WordLeft(g_caret) : CharLeft(g_caret), g_caret, L"", 0);
        return TRUE;
    case VK_DELETE:
        if (s0 != s1) InputReplace(s0, s1, L"", 0);
        else if (g_caret < g_inLen) InputReplace(g_caret, ctrl ? WordRight(g_caret) : CharRight(g_caret), L"", 0);
        return TRUE;
    }
    if (!ctrl) return FALSE;
    switch (vk) {
    case 'A':
        g_anchor = 0;
        g_caret = g_inLen;
        CaretMoved();
        return TRUE;
    case 'C':
    case 'X':
        if (s0 != s1) {
            WCHAR t[512];
            lstrcpynW(t, g_in + s0, s1 - s0 + 1);
            CopyText(t);
            if (vk == 'X') InputReplace(s0, s1, L"", 0);
        }
        return TRUE;
    case 'V':
        Paste();
        return TRUE;
    case VK_OEM_COMMA:
        OpenSettings();
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- hotkey + tray + startup

static void KeyName(UINT vk, WCHAR *out, int cap) {
    if (vk == VK_SPACE) lstrcpynW(out, L"Space", cap);
    else if (vk == VK_OEM_3) lstrcpynW(out, L"`", cap);
    else if (vk >= VK_F1 && vk <= VK_F24) _snwprintf(out, cap, L"F%u", vk - VK_F1 + 1);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) _snwprintf(out, cap, L"%c", (WCHAR)vk);
    else _snwprintf(out, cap, L"Key %u", vk);
    out[cap - 1] = 0;
}

static void HotkeyName(UINT mod, UINT vk, WCHAR *out, int cap) {
    WCHAR k[16];
    KeyName(vk, k, 16);
    _snwprintf(out, cap, L"%ls%ls%ls%ls%ls", mod & MOD_WIN ? L"Win+" : L"", mod & MOD_CONTROL ? L"Ctrl+" : L"",
               mod & MOD_ALT ? L"Alt+" : L"", mod & MOD_SHIFT ? L"Shift+" : L"", k);
    out[cap - 1] = 0;
}

static BOOL SetHotkey(UINT mod, UINT vk) {
    UnregisterHotKey(g_hwnd, HOTKEY_ID);
    return RegisterHotKey(g_hwnd, HOTKEY_ID, mod | MOD_NOREPEAT, vk);
}

static void TrayTip(void) {
    WCHAR hk[64];
    HotkeyName(g_set.hotMod, g_set.hotKey, hk, 64);
    _snwprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"Expere — %ls", hk);
    g_nid.szTip[ARRAYSIZE(g_nid.szTip) - 1] = 0;
}

static void TrayAdd(BOOL balloon) {
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon;
    TrayTip();
    if (balloon) {
        WCHAR hk[64];
        HotkeyName(g_set.hotMod, g_set.hotKey, hk, 64);
        g_nid.uFlags |= NIF_INFO;
        wcscpy(g_nid.szInfoTitle, L"Expere is running");
        _snwprintf(g_nid.szInfo, ARRAYSIZE(g_nid.szInfo), L"Press %ls to search apps, files, and commands.", hk);
        g_nid.szInfo[ARRAYSIZE(g_nid.szInfo) - 1] = 0;
        g_nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    }
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void TrayUpdate(void) {
    TrayTip();
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static BOOL StartupEnabled(void) {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void SetStartup(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        WCHAR exe[MAX_PATH], v[MAX_PATH + 32];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        _snwprintf(v, MAX_PATH + 32, L"\"%ls\" --background", exe);
        v[MAX_PATH + 31] = 0;
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

static void TrayMenu(void) {
    WCHAR hk[64], open[96];
    HotkeyName(g_set.hotMod, g_set.hotKey, hk, 64);
    _snwprintf(open, 96, L"Open Expere\t%ls", hk);
    open[95] = 0;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, open);
    AppendMenuW(m, MF_STRING, 2, L"Settings…");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Quit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (id == 1) ShowLauncher(NULL);
    else if (id == 2) OpenSettings();
    else if (id == 3) DestroyWindow(g_hwnd);
}

// ---------------------------------------------------------------- settings dialog

static void ComboAdd(HWND d, int id, const WCHAR *text, LPARAM data, BOOL select) {
    LRESULT i = SendDlgItemMessageW(d, id, CB_ADDSTRING, 0, (LPARAM)text);
    SendDlgItemMessageW(d, id, CB_SETITEMDATA, i, data);
    if (select) SendDlgItemMessageW(d, id, CB_SETCURSEL, i, 0);
}

static LPARAM ComboData(HWND d, int id) {
    LRESULT i = SendDlgItemMessageW(d, id, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : SendDlgItemMessageW(d, id, CB_GETITEMDATA, i, 0);
}

static void SettingsSync(HWND d) {
    int pos = (int)SendDlgItemMessageW(d, IDC_OPACITY, TBM_GETPOS, 0, 0);
    WCHAR t[8];
    _snwprintf(t, 8, L"%d%%", pos);
    t[7] = 0;
    SetDlgItemTextW(d, IDC_OPACITY_VAL, t);
    BOOL solid = ComboData(d, IDC_BACKDROP) == BD_SOLID;
    EnableWindow(GetDlgItem(d, IDC_OPACITY), !solid);
    EnableWindow(GetDlgItem(d, IDC_OPACITY_VAL), !solid);
}

static void SettingsInit(HWND d) {
    WCHAR name[32];
    BOOL found = FALSE;
    for (int i = 0; i < (int)ARRAYSIZE(HOTMODS); i++) {
        BOOL sel = HOTMODS[i].mod == g_set.hotMod;
        ComboAdd(d, IDC_HOTMOD, HOTMODS[i].name, HOTMODS[i].mod, sel);
        found |= sel;
    }
    if (!found) ComboAdd(d, IDC_HOTMOD, L"Custom", g_set.hotMod, TRUE);  // hand-edited settings.ini
    UINT keys[64];
    int nk = 0;
    keys[nk++] = VK_SPACE;
    keys[nk++] = VK_OEM_3;
    for (UINT k = 'A'; k <= 'Z'; k++) keys[nk++] = k;
    for (UINT k = '0'; k <= '9'; k++) keys[nk++] = k;
    for (UINT k = VK_F1; k <= VK_F12; k++) keys[nk++] = k;
    found = FALSE;
    for (int i = 0; i < nk; i++) {
        KeyName(keys[i], name, 32);
        ComboAdd(d, IDC_HOTKEY, name, keys[i], keys[i] == g_set.hotKey);
        found |= keys[i] == g_set.hotKey;
    }
    if (!found) { KeyName(g_set.hotKey, name, 32); ComboAdd(d, IDC_HOTKEY, name, g_set.hotKey, TRUE); }

    ComboAdd(d, IDC_THEME, L"Follow Windows", THEME_SYSTEM, g_set.theme == THEME_SYSTEM);
    ComboAdd(d, IDC_THEME, L"Dark", THEME_DARK, g_set.theme == THEME_DARK);
    ComboAdd(d, IDC_THEME, L"Light", THEME_LIGHT, g_set.theme == THEME_LIGHT);
    ComboAdd(d, IDC_BACKDROP, L"Acrylic (blurred)", BD_ACRYLIC, g_set.backdrop == BD_ACRYLIC);
    ComboAdd(d, IDC_BACKDROP, L"Transparent", BD_GLASS, g_set.backdrop == BD_GLASS);
    ComboAdd(d, IDC_BACKDROP, L"Solid", BD_SOLID, g_set.backdrop == BD_SOLID);
    SendDlgItemMessageW(d, IDC_OPACITY, TBM_SETRANGE, TRUE, MAKELPARAM(20, 100));
    SendDlgItemMessageW(d, IDC_OPACITY, TBM_SETPAGESIZE, 0, 10);
    SendDlgItemMessageW(d, IDC_OPACITY, TBM_SETPOS, TRUE, g_set.opacity);

    for (int n = 3; n <= MAX_ROWS; n++) {
        _snwprintf(name, 32, L"%d", n);
        ComboAdd(d, IDC_MAXRESULTS, name, n, n == g_set.maxResults);
    }
    for (int i = 0; i < (int)ARRAYSIZE(ENGINES); i++) ComboAdd(d, IDC_ENGINE, ENGINES[i].name, i, i == g_set.engine);
    CheckDlgButton(d, IDC_FILES, g_set.files ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_STARTUP, StartupEnabled() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(d, IDC_KEEPQUERY, g_set.keepQuery ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(d, WM_SETICON, ICON_SMALL, (LPARAM)g_icon);
    SendMessageW(d, WM_SETICON, ICON_BIG, (LPARAM)g_iconBig);
    SettingsSync(d);
}

static BOOL SettingsApply(HWND d) {
    Settings ns = g_set;
    ns.hotMod = (UINT)ComboData(d, IDC_HOTMOD);
    ns.hotKey = (UINT)ComboData(d, IDC_HOTKEY);
    ns.theme = (int)ComboData(d, IDC_THEME);
    ns.backdrop = (int)ComboData(d, IDC_BACKDROP);
    ns.opacity = (int)SendDlgItemMessageW(d, IDC_OPACITY, TBM_GETPOS, 0, 0);
    ns.maxResults = (int)ComboData(d, IDC_MAXRESULTS);
    ns.engine = (int)ComboData(d, IDC_ENGINE);
    ns.files = IsDlgButtonChecked(d, IDC_FILES) == BST_CHECKED;
    ns.keepQuery = IsDlgButtonChecked(d, IDC_KEEPQUERY) == BST_CHECKED;

    if (ns.hotMod != g_set.hotMod || ns.hotKey != g_set.hotKey || !g_hotkeyOk) {
        if (!SetHotkey(ns.hotMod, ns.hotKey)) {
            g_hotkeyOk = SetHotkey(g_set.hotMod, g_set.hotKey);
            WCHAR hk[64], msg[200];
            HotkeyName(ns.hotMod, ns.hotKey, hk, 64);
            _snwprintf(msg, 200, L"%ls is already used by another app. Choose a different shortcut.", hk);
            msg[199] = 0;
            MessageBoxW(d, msg, L"Expere Settings", MB_OK | MB_ICONWARNING);
            return FALSE;
        }
        g_hotkeyOk = TRUE;
    }
    BOOL startup = IsDlgButtonChecked(d, IDC_STARTUP) == BST_CHECKED;
    if (startup != StartupEnabled()) SetStartup(startup);

    g_set = ns;
    SaveSettings();
    ApplyAppearance();
    TrayUpdate();
    return TRUE;
}

static INT_PTR CALLBACK SettingsProc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG:
        SettingsInit(d);
        return TRUE;
    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(d, IDC_OPACITY)) SettingsSync(d);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            if (SettingsApply(d)) DestroyWindow(d);
            return TRUE;
        case IDC_APPLY:
            SettingsApply(d);
            return TRUE;
        case IDCANCEL:
            DestroyWindow(d);
            return TRUE;
        case IDC_BACKDROP:
            if (HIWORD(wp) == CBN_SELCHANGE) SettingsSync(d);
            return TRUE;
        case IDC_CLEARHIST:
            if (MessageBoxW(d, L"Forget which apps and files you open most? Ranking starts fresh.", L"Expere Settings",
                            MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                ClearHistory();
            return TRUE;
        }
        break;
    case WM_DESTROY:
        g_settingsDlg = NULL;
        SetTimer(g_hwnd, TIMER_TRIM, 1000, NULL);
        return TRUE;
    }
    return FALSE;
}

static void OpenSettings(void) {
    HideLauncher();
    if (!g_settingsDlg) {
        g_settingsDlg = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, SettingsProc, 0);
        if (!g_settingsDlg) return;
        ShowWindow(g_settingsDlg, SW_SHOW);
    }
    SetForegroundWindow(g_settingsDlg);
}

// ---------------------------------------------------------------- window

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (IsWindowVisible(hwnd) && GetForegroundWindow() == hwnd) HideLauncher();
        else ShowLauncher(NULL);
        return 0;
    case WM_APP_SHOW:
        ShowLauncher(NULL);
        return 0;
    case WM_APP_QUIT:
        DestroyWindow(hwnd);
        return 0;
    case WM_APP_SETTINGS:
        OpenSettings();
        return 0;
    case WM_APP_APPS: {
        AppIndex *old = g_apps;
        g_apps = (AppIndex *)lp;
        if (old) HeapDestroy(old->heap);
        g_appsAt = GetTickCount64();
        InterlockedExchange(&g_indexing, 0);
        if (IsWindowVisible(hwnd)) OnQueryChanged(); else Trim();
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_CARET) {
            g_caretOn = !g_caretOn;
            InvalidateInput();
            return 0;
        }
        KillTimer(hwnd, wp);
        if (wp == TIMER_SEARCH && g_query[0]) WsRequest(g_query, g_seq, FALSE);
        if (wp == TIMER_TRIM && !IsWindowVisible(hwnd)) Trim();
        return 0;
    case WM_APP_FILES: {
        WsBatch *b = (WsBatch *)lp;
        if (b->seq == g_seq && IsWindowVisible(hwnd)) OnSearchResults(b);
        HeapFree(GetProcessHeap(), 0, b);
        return 0;
    }
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (cds->dwData == (EV_REPLY_BASE | (g_seq & 0xFFFF))) {
            OnEverythingReply(cds);
        } else if (cds->dwData == COPY_QUERY && cds->cbData >= sizeof(WCHAR) && cds->cbData <= 1024) {
            WCHAR q[512];
            int n = (int)(cds->cbData / sizeof(WCHAR));
            memcpy(q, cds->lpData, n * sizeof(WCHAR));
            q[n < 512 ? n : 511] = 0;
            ShowLauncher(q);
        }
        return TRUE;
    }
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP) ShowLauncher(NULL);
        else if (LOWORD(lp) == WM_RBUTTONUP) TrayMenu();
        return 0;
    case WM_KEYDOWN:
        if (OnKey(wp)) return 0;
        break;
    case WM_CHAR:
        if (wp >= 32 && wp != 127) {
            WCHAR c = (WCHAR)wp;
            InputReplace(Min(g_caret, g_anchor), Max(g_caret, g_anchor), &c, 1);
        }
        return 0;
    case WM_IME_STARTCOMPOSITION: {
        HIMC imc = ImmGetContext(hwnd);
        if (imc) {
            COMPOSITIONFORM cf = {CFS_POINT, {S(TEXT_X) + TextX(g_caret) - g_scroll, (S(EDIT_H) - g_editTextH) / 2}, {0}};
            LOGFONTW lf;
            GetObjectW(g_fEdit, sizeof lf, &lf);
            ImmSetCompositionFontW(imc, &lf);
            ImmSetCompositionWindow(imc, &cf);
            ImmReleaseContext(hwnd, imc);
        }
        break;  // the default IME window shows the composition, then WM_CHAR delivers it
    }
    case WM_ACTIVATE:
        // Only hide once the user has actually had the window; a failed activation must not close it.
        if (LOWORD(wp) != WA_INACTIVE) g_activated = TRUE;
        else if (g_activated) HideLauncher();
        return 0;
    case WM_CLOSE:
        HideLauncher();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursorW(NULL, pt.y < S(EDIT_H) ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (g_dragging) {
            g_caret = HitChar(GET_X_LPARAM(lp));
            CaretMoved();
            return 0;
        }
        POINT pt;
        GetCursorPos(&pt);
        if (pt.x == g_lastMouse.x && pt.y == g_lastMouse.y) return 0;
        g_lastMouse = pt;
        int i = RowAt(GET_Y_LPARAM(lp));
        if (i >= 0 && i != g_sel) { g_sel = i; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int y = GET_Y_LPARAM(lp);
        if (y < S(EDIT_H)) {
            if (msg == WM_LBUTTONDBLCLK) {
                g_anchor = 0;
                g_caret = g_inLen;
            } else {
                g_caret = HitChar(GET_X_LPARAM(lp));
                if (!(wp & MK_SHIFT)) g_anchor = g_caret;
                g_dragging = TRUE;
                SetCapture(hwnd);
            }
            CaretMoved();
            return 0;
        }
        int i = RowAt(y);
        if (i >= 0 && msg == WM_LBUTTONDOWN) Activate(i, (wp & MK_CONTROL) != 0);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragging) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        g_dragging = FALSE;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintWindow(hdc, &ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DPICHANGED:
        if (LOWORD(wp) != g_dpi) { g_dpi = LOWORD(wp); CreateFonts(); Relayout(); }
        return 0;
    case WM_SETTINGCHANGE:
        if (g_set.theme == THEME_SYSTEM && lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) ApplyAppearance();
        break;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_settingsDlg) DestroyWindow(g_settingsDlg);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_wmTaskbarCreated && msg) { TrayAdd(FALSE); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdLine, int show) {
    (void)prev; (void)cmdLine; (void)show;
    g_inst = inst;

    int argc;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL background = FALSE, quit = FALSE, settings = FALSE;
    WCHAR query[512] = L"";
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--background")) background = TRUE;
        else if (!wcscmp(argv[i], L"--quit")) quit = TRUE;
        else if (!wcscmp(argv[i], L"--settings")) settings = TRUE;
        else {
            if (query[0]) wcsncat(query, L" ", 511 - wcslen(query));
            wcsncat(query, argv[i], 511 - wcslen(query));
        }
    }
    LocalFree(argv);

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\ExpereLauncherSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(WND_CLASS, NULL);
        if (w) {
            DWORD pid;
            GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid);
            if (quit) PostMessageW(w, WM_APP_QUIT, 0, 0);
            else if (settings) PostMessageW(w, WM_APP_SETTINGS, 0, 0);
            else if (query[0]) {
                COPYDATASTRUCT cds = {COPY_QUERY, (DWORD)((wcslen(query) + 1) * sizeof(WCHAR)), query};
                SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cds);
            } else if (!background) PostMessageW(w, WM_APP_SHOW, 0, 0);
        }
        return 0;
    }
    if (quit) return 0;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    InitPaths();
    LoadSettings();
    LoadHistory();

    SHSTOCKICONINFO sii = {sizeof sii};
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FIND, SHGSI_ICON | SHGSI_SMALLICON, &sii))) g_icon = sii.hIcon;
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FIND, SHGSI_ICON | SHGSI_LARGEICON, &sii))) g_iconBig = sii.hIcon;

    WNDCLASSEXW wc = {sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = WND_CLASS;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_icon;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, WND_CLASS, APP_NAME, WS_POPUP, 0, 0, 100, 100, NULL, NULL, inst, NULL);
    g_measureDC = CreateCompatibleDC(NULL);
    g_dpi = GetDpiForWindow(g_hwnd);
    CreateFonts();
    ApplyAppearance();

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_hwnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);

    g_hotkeyOk = SetHotkey(g_set.hotMod, g_set.hotKey);
    TrayAdd(!background && g_hotkeyOk);
    if (!g_hotkeyOk) {
        WCHAR hk[64], msg[256];
        HotkeyName(g_set.hotMod, g_set.hotKey, hk, 64);
        _snwprintf(msg, 256, L"%ls is already used by another app, so the shortcut is off.\n"
                             L"Right-click the Expere tray icon and open Settings to choose another.", hk);
        msg[255] = 0;
        MessageBoxW(NULL, msg, APP_NAME, MB_OK | MB_ICONWARNING);
    }

    InitializeCriticalSection(&g_wsLock);
    g_wsWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE ws = CreateThread(NULL, 256 * 1024, WsThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (ws) CloseHandle(ws);

    StartAppIndex();
    if (settings) OpenSettings();
    else if (query[0]) ShowLauncher(query);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_settingsDlg && IsDialogMessageW(g_settingsDlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
