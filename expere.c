// Expere - a tiny keyboard launcher for Windows.
//
// Memory budget is the whole point of this file:
//   * one native Win32 process, no browser engine, no script runtime
//   * no file index of our own: file search asks the Windows Search index that Windows
//     already keeps (or Everything, if its client is already running)
//   * the app list lives in a private heap that is freed in one HeapDestroy
//   * icons come from the shared system image list, resolved only for visible rows
//   * the search connection is dropped and the working set trimmed every time the window hides

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
#include <oledb.h>
#include <msdasc.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>

#define APP_NAME     L"Expere"
#define WND_CLASS    L"ExpereLauncher"
#define RUN_KEY      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

#define WM_APP_TRAY  (WM_APP + 1)
#define WM_APP_APPS  (WM_APP + 2)
#define WM_APP_SHOW  (WM_APP + 3)
#define WM_APP_QUIT  (WM_APP + 4)
#define WM_APP_FILES (WM_APP + 5)

#define HOTKEY_ID        1
#define TIMER_SEARCH     1
#define TIMER_TRIM       2
#define TRIM_AGAIN_MS    20000  // shell and COM threads touch pages for a while after we hide
#define SEARCH_DELAY_MS  90
#define WS_MAX           24
#define MAX_RESULTS      8
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

#define C_BG     RGB(28, 28, 32)
#define C_SEL    RGB(52, 54, 66)
#define C_LINE   RGB(46, 46, 54)
#define C_BORDER RGB(64, 64, 76)
#define C_TEXT   RGB(240, 240, 245)
#define C_SUB    RGB(150, 150, 162)
#define C_ACCENT RGB(98, 124, 240)

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

enum { CMD_LOCK, CMD_SLEEP, CMD_RESTART, CMD_SHUTDOWN, CMD_SIGNOUT, CMD_RECYCLE, CMD_QUIT };
static const struct { const WCHAR *name, *sub; int siid; } CMDS[] = {
    {L"Lock Screen",       L"Lock this PC",                                47},
    {L"Sleep",             L"Put this PC to sleep",                        106},
    {L"Restart",           L"Restart Windows",                             106},
    {L"Shut Down",         L"Shut down Windows",                           106},
    {L"Sign Out",          L"Sign out of Windows",                         106},
    {L"Empty Recycle Bin", L"Permanently delete items in the Recycle Bin", 31},
    {L"Quit Expere",       L"Close the launcher",                          84},
};

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

static HINSTANCE g_inst;
static HWND g_hwnd, g_edit;
static HFONT g_fEdit, g_fTitle, g_fSub, g_fGlyph;
static int g_editTextH;
static HBRUSH g_bgBrush;
static HIMAGELIST g_iml;
static HICON g_icon, g_iconBig;
static UINT g_dpi = 96;
static RECT g_work;
static NOTIFYICONDATAW g_nid;
static UINT g_wmTaskbarCreated;
static POINT g_lastMouse;
static BOOL g_suppressChange, g_activated;

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
static Result g_local[MAX_RESULTS], g_files[MAX_RESULTS], g_extra;
static int g_nlocal, g_nfiles;
static Result *g_shown[MAX_RESULTS];
static int g_nshown, g_sel;

static Hist g_hist[256];
static int g_nhist;
static WCHAR g_histDir[MAX_PATH], g_histPath[MAX_PATH];

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

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
// Plain substrings always match. Returns -1 for no match; higher is better.
static int Fuzzy(const WCHAR *s, const WCHAR *q0) {
    int len = (int)wcslen(s);
    const WCHAR *sub = StrStrIW(s, q0);
    if (sub) {
        int at = (int)(sub - s);
        return 60 + (at == 0 ? 40 : IsWordStart(s, at) ? 20 : 0) - at / 2 - len / 10;
    }
    int score = 0, si = 0, last = -1;
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
            score += IsWordStart(s, si) ? 8 : 1;
        }
        last = si++;
    }
    return last < 0 ? -1 : score - len / 10;
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

// ---------------------------------------------------------------- history (frecency)

static int HistCount(const WCHAR *prefix, const WCHAR *rest) {
    size_t pl = wcslen(prefix);
    for (int i = 0; i < g_nhist; i++)
        if (_wcsnicmp(g_hist[i].t, prefix, pl) == 0 && _wcsicmp(g_hist[i].t + pl, rest) == 0) return g_hist[i].n;
    return 0;
}

static int HistBonus(int n) { return (n > 20 ? 20 : n) * 6; }

static void LoadHistory(void) {
    PWSTR roaming;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &roaming))) return;
    _snwprintf(g_histDir, MAX_PATH, L"%ls\\Expere", roaming);
    _snwprintf(g_histPath, MAX_PATH, L"%ls\\history.txt", g_histDir);
    CoTaskMemFree(roaming);
    FILE *f = _wfopen(g_histPath, L"rt, ccs=UTF-8");
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
    CreateDirectoryW(g_histDir, NULL);
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

static HWND FindEverything(void) {
    HWND ev = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", NULL);
    return ev ? ev : FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION_(1.5a)", NULL);
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

static void Merge(void) {
    int a = 0, b = 0, n = 0, cap = g_query[0] ? MAX_RESULTS - 1 : 0;
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
    int eh = g_editTextH + S(2);
    MoveWindow(g_edit, S(22), (S(EDIT_H) - eh) / 2, w - S(44), eh, TRUE);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void OnQueryChanged(void) {
    WCHAR raw[512];
    GetWindowTextW(g_edit, raw, 512);
    const WCHAR *q = raw;
    while (*q == L' ') q++;
    wcscpy(g_query, q);
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
            Result *r = Slot(g_local, &g_nlocal, MAX_RESULTS, 1 << 20);
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
                s += 60 + HistBonus(HistCount(L"shell:AppsFolder\\", a->parse));
                Result *r = Slot(g_local, &g_nlocal, MAX_RESULTS, s);
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
            Result *r = Slot(g_local, &g_nlocal, MAX_RESULTS, s + 50);
            if (!r) continue;
            r->kind = K_CMD;
            r->cmd = i;
            wcscpy(r->title, CMDS[i].name);
            wcscpy(r->sub, CMDS[i].sub);
        }
        if (!calc && wcslen(g_query) >= 2) {
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
        wcscpy(g_extra.sub, L"Google");
        UrlEncode(g_query, enc, 1000, TRUE);
        _snwprintf(g_extra.target, 1100, L"https://www.google.com/search?q=%ls", enc);
    }
    Merge();
    Relayout();
}

static void AddFile(const WCHAR *name, const WCHAR *dir, const WCHAR *full, BOOL folder) {
    int s = Fuzzy(name, g_query);
    if (s < 0) s = 0;
    s += HistBonus(HistCount(L"", full)) - 40;
    Result *r = Slot(g_files, &g_nfiles, MAX_RESULTS, s);
    if (!r) return;
    r->kind = folder ? K_FOLDER : K_FILE;
    lstrcpynW(r->title, name, 160);
    lstrcpynW(r->sub, dir, 400);
    lstrcpynW(r->target, full, 1100);
}

// Used only when no file index answers: hand the query to Explorer.
static void AddExplorerSearch(void) {
    Result *r = Slot(g_local, &g_nlocal, MAX_RESULTS, -200);
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

// ---------------------------------------------------------------- actions

static void HideLauncher(void) {
    if (!IsWindowVisible(g_hwnd)) return;
    g_activated = FALSE;
    ShowWindow(g_hwnd, SW_HIDE);
    g_seq++;  // drop late search replies
    KillTimer(g_hwnd, TIMER_SEARCH);
    WsRequest(NULL, 0, TRUE);  // the worker drops its Windows Search connection
    Trim();
    SetTimer(g_hwnd, TIMER_TRIM, TRIM_AGAIN_MS, NULL);
}

static void CreateFonts(void);

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

    g_suppressChange = TRUE;
    SetWindowTextW(g_edit, text ? text : L"");
    g_suppressChange = FALSE;
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
    SetFocus(g_edit);
    int len = GetWindowTextLengthW(g_edit);
    SendMessageW(g_edit, EM_SETSEL, len, len);
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

static BOOL Confirm(const WCHAR *msg) {
    return MessageBoxW(NULL, msg, APP_NAME, MB_OKCANCEL | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) == IDOK;
}

static void RunCommand(int cmd) {
    switch (cmd) {
    case CMD_LOCK:     LockWorkStation(); break;
    case CMD_SLEEP:    SetSuspendState(FALSE, FALSE, FALSE); break;
    case CMD_RESTART:  if (Confirm(L"Restart Windows now?")) Launch(L"shutdown.exe", L"/r /t 0", SW_HIDE); break;
    case CMD_SHUTDOWN: if (Confirm(L"Shut down Windows now?")) Launch(L"shutdown.exe", L"/s /t 0", SW_HIDE); break;
    case CMD_SIGNOUT:  if (Confirm(L"Sign out of Windows now?")) ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_FLAG_PLANNED); break;
    case CMD_RECYCLE:  SHEmptyRecycleBinW(NULL, NULL, 0); break;
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

static void DeleteWordBack(void) {
    DWORD start, endSel;
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&endSel);
    if (start != endSel) { SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)L""); return; }
    WCHAR text[512];
    GetWindowTextW(g_edit, text, 512);
    DWORD i = start;
    while (i > 0 && text[i - 1] == L' ') i--;
    while (i > 0 && !IsSep(text[i - 1])) i--;
    SendMessageW(g_edit, EM_SETSEL, i, start);
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)L"");
}

// Keys the edit control should not see. Returns TRUE if handled.
static BOOL HandleEditKey(WPARAM vk) {
    BOOL ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
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
        if (GetWindowTextLengthW(g_edit)) SetWindowTextW(g_edit, L"");
        else HideLauncher();
        return TRUE;
    case VK_BACK:
        if (!ctrl) return FALSE;
        DeleteWordBack();
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- painting

static void CreateFonts(void) {
    HFONT *fonts[] = {&g_fEdit, &g_fTitle, &g_fSub, &g_fGlyph};
    int px[] = {21, 15, 12, 18}, weight[] = {FW_NORMAL, FW_NORMAL, FW_NORMAL, FW_SEMIBOLD};
    for (int i = 0; i < 4; i++) {
        if (*fonts[i]) DeleteObject(*fonts[i]);
        *fonts[i] = CreateFontW(-S(px[i]), 0, 0, 0, weight[i], 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ old = SelectObject(dc, g_fEdit);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    g_editTextH = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(g_hwnd, dc);
    if (g_edit) SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fEdit, FALSE);
    SHGetImageList(g_dpi > 120 ? SHIL_EXTRALARGE : SHIL_LARGE, &kIidImageList, (void **)&g_iml);
}

static int ResolveIcon(Result *r) {
    if (r->icon != -2) return r->icon;
    r->icon = -1;
    SHFILEINFOW sfi = {0};
    SHSTOCKICONINFO sii = {sizeof sii};
    int siid = -1;
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
    case K_CMD:    siid = CMDS[r->cmd].siid; break;
    case K_WEB:    siid = 13; break;  // SIID_WORLD
    case K_SEARCH: siid = 22; break;  // SIID_FIND
    }
    if (siid >= 0 && SUCCEEDED(SHGetStockIconInfo((SHSTOCKICONID)siid, SHGSI_SYSICONINDEX, &sii))) r->icon = sii.iSysImageIndex;
    return r->icon;
}

static void FillSolid(HDC dc, const RECT *rc, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, rc, GetStockObject(DC_BRUSH));
}

static void DrawRow(HDC m, Result *r, BOOL sel, int top, int w) {
    int rh = S(ROW_H);
    SelectObject(m, GetStockObject(NULL_PEN));
    SelectObject(m, GetStockObject(DC_BRUSH));
    if (sel) {
        SetDCBrushColor(m, C_SEL);
        RoundRect(m, S(PAD), top, w - S(PAD), top + rh, S(12), S(12));
    }

    int isz = S(32), ix = S(PAD) + S(12), iy = top + (rh - isz) / 2;
    int idx = ResolveIcon(r);
    HICON hic = idx >= 0 && g_iml ? ImageList_GetIcon(g_iml, idx, ILD_NORMAL) : NULL;
    if (hic) {
        DrawIconEx(m, ix, iy, hic, isz, isz, 0, NULL, DI_NORMAL);
        DestroyIcon(hic);
    } else {
        SetDCBrushColor(m, C_ACCENT);
        RoundRect(m, ix, iy, ix + isz, iy + isz, S(8), S(8));
        RECT gr = {ix, iy, ix + isz, iy + isz};
        SelectObject(m, g_fGlyph);
        SetTextColor(m, RGB(255, 255, 255));
        DrawTextW(m, r->kind == K_CALC ? L"=" : L"›", -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    const WCHAR *badge = KIND_LABEL[r->kind];
    SIZE bs;
    SelectObject(m, g_fSub);
    GetTextExtentPoint32W(m, badge, (int)wcslen(badge), &bs);
    int right = w - S(PAD) - S(14);
    RECT br = {right - bs.cx, top, right, top + rh};
    SetTextColor(m, C_SUB);
    DrawTextW(m, badge, -1, &br, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);

    int tx = ix + isz + S(14), tr = br.left - S(16);
    SelectObject(m, g_fTitle);
    SetTextColor(m, C_TEXT);
    if (r->sub[0]) {
        RECT t1 = {tx, top, tr, top + rh / 2 + S(1)};
        DrawTextW(m, r->title, -1, &t1, DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT t2 = {tx, top + rh / 2 + S(2), tr, top + rh};
        SelectObject(m, g_fSub);
        SetTextColor(m, C_SUB);
        UINT ell = (r->kind == K_FILE || r->kind == K_FOLDER) ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS;
        DrawTextW(m, r->sub, -1, &t2, DT_SINGLELINE | DT_TOP | ell | DT_NOPREFIX);
    } else {
        RECT t = {tx, top, tr, top + rh};
        DrawTextW(m, r->title, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

static void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    HDC m = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(m, bmp);
    HGDIOBJ oldFont = SelectObject(m, g_fTitle);
    FillRect(m, &rc, g_bgBrush);
    SetBkMode(m, TRANSPARENT);

    if (g_nshown) {
        RECT line = {0, S(EDIT_H), w, S(EDIT_H) + S(1)};
        FillSolid(m, &line, C_LINE);
        int top0 = S(EDIT_H) + S(1) + S(PAD);
        for (int i = 0; i < g_nshown; i++) DrawRow(m, g_shown[i], i == g_sel, top0 + i * S(ROW_H), w);

        int fy = h - S(FOOT_H);
        RECT fl = {0, fy, w, fy + S(1)};
        FillSolid(m, &fl, C_LINE);
        RECT ft = {S(22), fy, w - S(22), h};
        SelectObject(m, g_fSub);
        SetTextColor(m, C_SUB);
        DrawTextW(m, L"↑↓  Navigate      ↵  Open      Ctrl+↵  Show in folder      Esc  Close", -1, &ft,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        const WCHAR *src = g_fileSource == FS_EVERYTHING ? L"Files via Everything"
                         : g_fileSource == FS_WINSEARCH  ? L"Files via Windows Search" : NULL;
        if (src) DrawTextW(m, src, -1, &ft, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }

    BitBlt(hdc, 0, 0, w, h, m, 0, 0, SRCCOPY);
    SelectObject(m, oldFont);
    SelectObject(m, oldBmp);
    DeleteObject(bmp);
    DeleteDC(m);
}

static int RowAt(int y) {
    int top0 = S(EDIT_H) + S(1) + S(PAD);
    if (y < top0) return -1;
    int i = (y - top0) / S(ROW_H);
    return i < g_nshown ? i : -1;
}

// ---------------------------------------------------------------- tray + startup

static void TrayAdd(BOOL balloon) {
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, L"Expere — Alt+Space");
    if (balloon) {
        g_nid.uFlags |= NIF_INFO;
        wcscpy(g_nid.szInfoTitle, L"Expere is running");
        wcscpy(g_nid.szInfo, L"Press Alt+Space to search apps, files, and commands.");
        g_nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    }
    Shell_NotifyIconW(NIM_ADD, &g_nid);
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
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Open Expere\tAlt+Space");
    AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), 2, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"Quit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (id == 1) ShowLauncher(NULL);
    else if (id == 2) SetStartup(!StartupEnabled());
    else if (id == 3) DestroyWindow(g_hwnd);
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
    case WM_COMMAND:
        if ((HWND)lp == g_edit && HIWORD(wp) == EN_CHANGE && !g_suppressChange) OnQueryChanged();
        return 0;
    case WM_ACTIVATE:
        // Only hide once the user has actually had the window; a failed activation must not close it.
        if (LOWORD(wp) != WA_INACTIVE) g_activated = TRUE;
        else if (g_activated) HideLauncher();
        return 0;
    case WM_SETFOCUS:
        SetFocus(g_edit);
        return 0;
    case WM_CLOSE:
        HideLauncher();
        return 0;
    case WM_MOUSEMOVE: {
        POINT pt;
        GetCursorPos(&pt);
        if (pt.x == g_lastMouse.x && pt.y == g_lastMouse.y) return 0;
        g_lastMouse = pt;
        int i = RowAt(GET_Y_LPARAM(lp));
        if (i >= 0 && i != g_sel) { g_sel = i; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int i = RowAt(GET_Y_LPARAM(lp));
        if (i >= 0) Activate(i, (wp & MK_CONTROL) != 0);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_BG);
        return (LRESULT)g_bgBrush;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DPICHANGED:
        if (LOWORD(wp) != g_dpi) { g_dpi = LOWORD(wp); CreateFonts(); Relayout(); }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
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
    BOOL background = FALSE, quit = FALSE;
    WCHAR query[512] = L"";
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--background")) background = TRUE;
        else if (!wcscmp(argv[i], L"--quit")) quit = TRUE;
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
            else if (query[0]) {
                COPYDATASTRUCT cds = {COPY_QUERY, (DWORD)((wcslen(query) + 1) * sizeof(WCHAR)), query};
                SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cds);
            } else if (!background) PostMessageW(w, WM_APP_SHOW, 0, 0);
        }
        return 0;
    }
    if (quit) return 0;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    SHSTOCKICONINFO sii = {sizeof sii};
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FIND, SHGSI_ICON | SHGSI_SMALLICON, &sii))) g_icon = sii.hIcon;
    if (SUCCEEDED(SHGetStockIconInfo(SIID_FIND, SHGSI_ICON | SHGSI_LARGEICON, &sii))) g_iconBig = sii.hIcon;

    g_bgBrush = CreateSolidBrush(C_BG);
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = WND_CLASS;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_icon;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, WND_CLASS, APP_NAME, WS_POPUP | WS_CLIPCHILDREN,
                             0, 0, 100, 100, NULL, NULL, inst, NULL);
    g_edit = CreateWindowExW(0, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10,
                             g_hwnd, NULL, inst, NULL);
    SendMessageW(g_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search apps, files, and commands…");
    SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);

    BOOL dark = TRUE;
    DWORD corner = 2;  // DWMWCP_ROUND
    COLORREF border = C_BORDER;
    DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof dark);       // DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute(g_hwnd, 33, &corner, sizeof corner);   // DWMWA_WINDOW_CORNER_PREFERENCE
    DwmSetWindowAttribute(g_hwnd, 34, &border, sizeof border);   // DWMWA_BORDER_COLOR

    g_dpi = GetDpiForWindow(g_hwnd);
    CreateFonts();

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_hwnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);

    BOOL hotkey = RegisterHotKey(g_hwnd, HOTKEY_ID, MOD_ALT | MOD_NOREPEAT, VK_SPACE);
    TrayAdd(!background && hotkey);
    if (!hotkey)
        MessageBoxW(NULL, L"Alt+Space is already used by another app, so the hotkey is off.\n"
                          L"Click the Expere tray icon to open the launcher.", APP_NAME, MB_OK | MB_ICONWARNING);

    InitializeCriticalSection(&g_wsLock);
    g_wsWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE ws = CreateThread(NULL, 256 * 1024, WsThread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (ws) CloseHandle(ws);

    LoadHistory();
    StartAppIndex();
    if (query[0]) ShowLauncher(query);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.hwnd == g_edit && msg.message == WM_KEYDOWN && HandleEditKey(msg.wParam)) continue;
        if (msg.hwnd == g_edit && msg.message == WM_CHAR && msg.wParam == 0x7F) continue;  // Ctrl+Backspace
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
