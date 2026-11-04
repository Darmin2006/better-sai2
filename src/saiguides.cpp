// SaiGuides.exe -- 给 PaintTool SAI Ver.2 加"参考线"（类似 PS 的辅助线）
// 原理：只读 SAI 进程内存里的视图变换，用一个点击穿透的置顶透明窗口把线画在画布上。
// 不注入、不修改 SAI、不影响绘画。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

// ---------------- 全局 ----------------
static HINSTANCE g_inst;
static HANDLE  g_proc = NULL;
static unsigned long long g_base = 0;
static DWORD   g_pid = 0;

static HWND g_main = NULL, g_overlay = NULL;
static HWND g_list = NULL, g_editV = NULL, g_editH = NULL, g_status = NULL;
static HWND g_chkShow = NULL, g_chkDrag = NULL;
static HWND g_comboUnit = NULL, g_editDpi = NULL;

static int    g_unit = 0;          // 0 = 像素, 1 = 毫米
static double g_dpi = 300.0;       // 文档分辨率，用于 px <-> mm

static double PxToMm(int px) { return (double)px * 25.4 / (g_dpi > 1.0 ? g_dpi : 300.0); }
static int    MmToPx(double mm) { return (int)lround(mm / 25.4 * (g_dpi > 1.0 ? g_dpi : 300.0)); }

static void FormatVal(int px, wchar_t* buf, size_t n) {
    if (g_unit == 0) _snwprintf_s(buf, n, _TRUNCATE, L"%d", px);
    else _snwprintf_s(buf, n, _TRUNCATE, L"%.2f", PxToMm(px));
}
static bool ParseVal(const wchar_t* s, int* outPx) {
    double v = _wtof(s);
    if (v < 0) return false;
    *outPx = (g_unit == 0) ? (int)lround(v) : MmToPx(v);
    return true;
}
static const wchar_t* UnitName(void) { return g_unit == 0 ? L"px" : L"mm"; }

static std::vector<int> g_vx;      // 竖线：文档 X
static std::vector<int> g_hy;      // 横线：文档 Y
static bool g_show = true;
static bool g_dragMode = false;
static int  g_dragSel = -1;        // 正在拖的条目（listbox 索引）
static bool g_dragIsV = false;

struct ViewT {
    bool   ok;
    int    dw, dh;        // 文档尺寸
    int    zoom;          // 0x10000 = 100%
    int    cw, ch;        // 画布客户区尺寸
    double scale;         // 文档像素 / 屏幕像素
    double cx, cy;        // 视图中心（文档坐标）
    double rot;           // 旋转角（弧度），暂只支持 0
    RECT   scr;           // 画布客户区（屏幕坐标）
};
static ViewT g_vt = { 0 };
static POINT g_cursorDoc = { -1, -1 };

// ---------------- 进程/内存 ----------------
static bool Rd(uintptr_t addr, void* out, size_t n) {
    if (!g_proc || !addr) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(g_proc, (LPCVOID)addr, out, n, &got) && got == n;
}
template <class T> static bool RdT(uintptr_t addr, T* out) { return Rd(addr, out, sizeof(T)); }

static HWND g_saiMain = NULL;
static HWND g_canvas = NULL;
static void Glog(const wchar_t* fmt, ...);
static BOOL CALLBACK EnumTopForPidProc(HWND h, LPARAM lp);

struct FindCtx { HWND best; int area; int seen; };
static BOOL CALLBACK FindCanvasProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    wchar_t cls[64] = { 0 };
    GetClassNameW(h, cls, 64);
    if (c->seen < 40) {
        RECT r;
        GetWindowRect(h, &r);
        Glog(L"[tree] %p cls=%s vis=%d %dx%d", h, cls, IsWindowVisible(h),
             r.right - r.left, r.bottom - r.top);
        c->seen++;
    }
    if (_wcsicmp(cls, L"sflChildWindow") != 0) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    RECT r;
    GetWindowRect(h, &r);
    int a = (r.right - r.left) * (r.bottom - r.top);
    if (a > c->area) { c->area = a; c->best = h; }
    return TRUE;
}

static unsigned long long ModuleBase(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me = { 0 };
    me.dwSize = sizeof(me);
    unsigned long long base = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, name) == 0) { base = (unsigned long long)me.modBaseAddr; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

// ---------------- 日志 ----------------
static wchar_t g_logPath[MAX_PATH];
static void Glog(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    FILE* f = NULL;
    if (_wfopen_s(&f, g_logPath, L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%s\n", buf);
        fclose(f);
    }
}

static void CloseSai(void) {
    if (g_proc) { CloseHandle(g_proc); g_proc = NULL; }
    g_base = 0; g_pid = 0; g_saiMain = NULL; g_canvas = NULL;
    g_vt.ok = false;
}

struct MainCtx { HWND best; int area; DWORD pid; };
static BOOL CALLBACK PickMainProc(HWND h, LPARAM lp) {
    MainCtx* c = (MainCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    wchar_t cls[64] = { 0 };
    GetClassNameW(h, cls, 64);
    if (_wcsicmp(cls, L"sflRootWindow") != 0) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    RECT r;
    GetWindowRect(h, &r);
    int a = (r.right - r.left) * (r.bottom - r.top);
    if (a > c->area) { c->area = a; c->best = h; }
    return TRUE;
}

static bool OpenSai(void) {
    HWND probe = FindWindowW(L"sflRootWindow", NULL);
    if (!probe) {
        Glog(L"[connect] FindWindow(sflRootWindow) = NULL");
        CloseSai();
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(probe, &pid);
    if (g_proc && g_pid == pid) return true;

    CloseSai();
    SetLastError(0);
    HANDLE p = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    DWORD e1 = GetLastError();
    if (!p) {
        SetLastError(0);
        p = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        Glog(L"[connect] OpenProcess(VM_READ|QUERY_INFO) failed err=%lu, retry err=%lu", e1, GetLastError());
    }
    if (!p) return false;

    // 同进程里可能有多个 sflRootWindow（比如隐藏的“工具首选项”），取最大的可见那个
    MainCtx mc = { NULL, 0, pid };
    EnumWindows(PickMainProc, (LPARAM)&mc);

    unsigned long long base = ModuleBase(pid, L"sai2.exe");
    if (!base) base = ModuleBase(pid, L"SAI2.exe");
    if (!base) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me = { 0 };
            me.dwSize = sizeof(me);
            if (Module32FirstW(snap, &me)) base = (unsigned long long)me.modBaseAddr;
            CloseHandle(snap);
        }
    }
    if (!base) base = 0x140000000ULL;

    g_proc = p; g_pid = pid; g_base = base;
    g_saiMain = mc.best ? mc.best : probe;
    Glog(L"[connect] pid=%lu base=0x%llX main=%p (area=%d)", pid, base, g_saiMain, mc.area);
    return true;
}

static BOOL CALLBACK EnumTopForPidProc(HWND h, LPARAM lp) {
    DWORD want = (DWORD)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != want) return TRUE;
    wchar_t cls[64] = { 0 }, txt[96] = { 0 };
    GetClassNameW(h, cls, 64);
    GetWindowTextW(h, txt, 96);
    RECT r;
    GetWindowRect(h, &r);
    Glog(L"[top] %p cls=%s vis=%d %dx%d at (%d,%d) '%s'", h, cls, IsWindowVisible(h),
         r.right - r.left, r.bottom - r.top, r.left, r.top, txt);
    FindCtx fc = { NULL, 0, 0 };
    EnumChildWindows(h, FindCanvasProc, (LPARAM)&fc);
    if (fc.best && !g_canvas) Glog(L"[top]   -> canvas candidate %p", fc.best);
    return TRUE;
}

// ---------------- 读视图变换 ----------------
static bool ReadTransform(ViewT* t) {
    if (!g_proc || !g_base) { Glog(L"[read] no proc/base"); return false; }
    unsigned long long doc = 0;
    if (!RdT(g_base + 0x320F18, &doc) || !doc) {
        Glog(L"[read] doc ptr read failed (base=0x%llX doc=0x%llX)", g_base, doc);
        return false;
    }
    unsigned long long view = 0;
    if (!RdT(doc + 0x120, &view) || !view) {
        Glog(L"[read] view ptr read failed (doc=0x%llX view=0x%llX)", doc, view);
        return false;
    }

    int dw = 0, dh = 0, zoom = 0;
    double cx = 0, cy = 0, scale = 0, rot = 0;
    if (!RdT((uintptr_t)(view + 0x340), &dw)) { Glog(L"[read] dw fail"); return false; }
    if (!RdT((uintptr_t)(view + 0x344), &dh)) { Glog(L"[read] dh fail"); return false; }
    if (!RdT((uintptr_t)(view + 0x348), &zoom)) { Glog(L"[read] zoom fail"); return false; }
    if (!RdT((uintptr_t)(view + 0x378), &cx)) { Glog(L"[read] cx fail"); return false; }
    if (!RdT((uintptr_t)(view + 0x380), &cy)) { Glog(L"[read] cy fail"); return false; }
    if (!RdT((uintptr_t)(view + 0x3C0), &scale)) { Glog(L"[read] scale fail"); return false; }
    if (!RdT((uintptr_t)(view + 0x3B0), &rot)) { Glog(L"[read] rot fail"); return false; }
    Glog(L"[read] doc=0x%llX view=0x%llX dw=%d dh=%d zoom=0x%X cx=%.1f cy=%.1f scale=%.4f rot=%.4f",
         doc, view, dw, dh, zoom, cx, cy, scale, rot);

    if (dw <= 0 || dh <= 0 || dw > 20000 || dh > 20000) { Glog(L"[read] bad doc size"); return false; }
    if (!(scale > 0.0001 && scale < 1000.0)) { Glog(L"[read] bad scale"); return false; }

    // 画布窗口
    FindCtx fc = { NULL, 0, 0 };
    if (g_saiMain) EnumChildWindows(g_saiMain, FindCanvasProc, (LPARAM)&fc);
    if (!fc.best) {
        Glog(L"[read] canvas not found under %p, dumping top-level windows of pid %lu", g_saiMain, g_pid);
        EnumWindows(EnumTopForPidProc, (LPARAM)(INT_PTR)g_pid);
    }
    Glog(L"[read] canvas=%p area=%d", fc.best, fc.area);
    if (!fc.best) return false;
    g_canvas = fc.best;

    RECT cr = { 0 };
    GetClientRect(g_canvas, &cr);
    POINT org = { 0, 0 };
    ClientToScreen(g_canvas, &org);

    t->ok = true;
    t->dw = dw; t->dh = dh; t->zoom = zoom;
    t->scale = scale; t->cx = cx; t->cy = cy; t->rot = rot;
    t->cw = cr.right - cr.left;
    t->ch = cr.bottom - cr.top;
    t->scr.left = org.x; t->scr.top = org.y;
    t->scr.right = org.x + t->cw; t->scr.bottom = org.y + t->ch;
    return true;
}

static double DocToClientX(double x) { return (x - g_vt.cx) / g_vt.scale + g_vt.cw * 0.5; }
static double DocToClientY(double y) { return (y - g_vt.cy) / g_vt.scale + g_vt.ch * 0.5; }
static double ClientToDocX(double x) { return (x - g_vt.cw * 0.5) * g_vt.scale + g_vt.cx; }
static double ClientToDocY(double y) { return (y - g_vt.ch * 0.5) * g_vt.scale + g_vt.cy; }

// ---------------- 覆盖窗口 ----------------
static HBITMAP g_dib = NULL;
static HDC     g_mdc = NULL;
static void*   g_bits = NULL;
static int     g_bw = 0, g_bh = 0;

static void FreeSurface(void) {
    if (g_mdc) { DeleteDC(g_mdc); g_mdc = NULL; }
    if (g_dib) { DeleteObject(g_dib); g_dib = NULL; }
    g_bits = NULL; g_bw = g_bh = 0;
}

static bool EnsureSurface(int w, int h) {
    if (w <= 0 || h <= 0 || w > 8000 || h > 8000) return false;
    if (g_bits && g_bw == w && g_bh == h) return true;
    FreeSurface();
    HDC dc = GetDC(NULL);
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!g_dib) return false;
    g_mdc = CreateCompatibleDC(NULL);
    SelectObject(g_mdc, g_dib);
    g_bw = w; g_bh = h;
    return true;
}

static inline void Px(int x, int y, BYTE a, BYTE r, BYTE g, BYTE b) {
    if (x < 0 || y < 0 || x >= g_bw || y >= g_bh) return;
    BYTE* p = (BYTE*)g_bits + ((size_t)y * g_bw + x) * 4;
    // premultiplied BGRA
    p[0] = (BYTE)(b * a / 255);
    p[1] = (BYTE)(g * a / 255);
    p[2] = (BYTE)(r * a / 255);
    p[3] = a;
}

static void VLine(int x, BYTE a, BYTE r, BYTE g, BYTE b) {
    if (x < 0 || x >= g_bw) return;
    for (int y = 0; y < g_bh; y++) Px(x, y, a, r, g, b);
}
static void HLine(int y, BYTE a, BYTE r, BYTE g, BYTE b) {
    if (y < 0 || y >= g_bh) return;
    for (int x = 0; x < g_bw; x++) Px(x, y, a, r, g, b);
}

static void DrawOverlay(void) {
    if (!g_overlay) return;
    if (!g_vt.ok || !g_show) {
        ShowWindow(g_overlay, SW_HIDE);
        return;
    }
    if (!EnsureSurface(g_vt.cw, g_vt.ch)) return;
    memset(g_bits, 0, (size_t)g_bw * g_bh * 4);

    // 参考线：青色，1px 实线 + 相邻像素淡一点
    for (size_t i = 0; i < g_vx.size(); i++) {
        int sx = (int)lround(DocToClientX((double)g_vx[i]));
        VLine(sx, 230, 0, 200, 255);
    }
    for (size_t i = 0; i < g_hy.size(); i++) {
        int sy = (int)lround(DocToClientY((double)g_hy[i]));
        HLine(sy, 230, 0, 200, 255);
    }

    // 光标十字（跟随鼠标，帮助精确对位）
    POINT cp;
    if (GetCursorPos(&cp) && PtInRect(&g_vt.scr, cp)) {
        int lx = cp.x - g_vt.scr.left, ly = cp.y - g_vt.scr.top;
        for (int x = 0; x < g_bw; x += 2) Px(x, ly, 90, 255, 255, 255);
        for (int y = 0; y < g_bh; y += 2) Px(lx, y, 90, 255, 255, 255);
    }

    HDC screen = GetDC(NULL);
    POINT dst = { g_vt.scr.left, g_vt.scr.top };
    SIZE sz = { g_bw, g_bh };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_overlay, screen, &dst, &sz, g_mdc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
}

// ---------------- 列表 ----------------
static void RefreshList(void) {
    if (!g_list) return;
    int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    wchar_t buf[96], val[48];
    for (size_t i = 0; i < g_vx.size(); i++) {
        FormatVal(g_vx[i], val, _countof(val));
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"竖线  X = %s %s", val, UnitName());
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)buf);
    }
    for (size_t i = 0; i < g_hy.size(); i++) {
        FormatVal(g_hy[i], val, _countof(val));
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"横线  Y = %s %s", val, UnitName());
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)buf);
    }
    if (sel >= 0 && sel < (int)(g_vx.size() + g_hy.size()))
        SendMessageW(g_list, LB_SETCURSEL, sel, 0);
}

static void AddGuide(bool vertical, int pos) {
    if (vertical) {
        if (std::find(g_vx.begin(), g_vx.end(), pos) == g_vx.end()) g_vx.push_back(pos);
    } else {
        if (std::find(g_hy.begin(), g_hy.end(), pos) == g_hy.end()) g_hy.push_back(pos);
    }
    RefreshList();
}

static void DeleteSelected(void) {
    int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    if (sel < (int)g_vx.size()) g_vx.erase(g_vx.begin() + sel);
    else {
        int k = sel - (int)g_vx.size();
        if (k >= 0 && k < (int)g_hy.size()) g_hy.erase(g_hy.begin() + k);
    }
    RefreshList();
}

// ---------------- 状态栏文本 ----------------
static void UpdateStatus(void) {
    if (!g_status) return;
    wchar_t buf[512];
    if (!g_vt.ok) {
        wcscpy_s(buf, L"未连接 SAI —— 请先打开 SAI2 并打开一个文档");
    } else {
        double pct = 100.0 / g_vt.scale;
        double wmm = PxToMm(g_vt.dw), hmm = PxToMm(g_vt.dh);
        if (g_cursorDoc.x >= 0) {
            double cmx = PxToMm(g_cursorDoc.x), cmy = PxToMm(g_cursorDoc.y);
            _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                L"画布 %d×%d px (%.1f×%.1f mm)  %.0f%%  光标 %d,%d px / %.2f,%.2f mm",
                g_vt.dw, g_vt.dh, wmm, hmm, pct,
                g_cursorDoc.x, g_cursorDoc.y, cmx, cmy);
        } else {
            _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                L"画布 %d×%d px (%.1f×%.1f mm)  %.0f%%  光标 —",
                g_vt.dw, g_vt.dh, wmm, hmm, pct);
        }
    }
    SetWindowTextW(g_status, buf);
}

// ---------------- 覆盖窗窗口过程 ----------------
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_LBUTTONDOWN: {
        if (!g_dragMode || !g_vt.ok) break;
        int cx = GET_X_LPARAM(l), cy = GET_Y_LPARAM(l);
        // 找最近的线（8 屏幕像素内）
        int best = -1, bestD = 9;
        bool isV = false;
        for (size_t i = 0; i < g_vx.size(); i++) {
            int d = abs((int)lround(DocToClientX((double)g_vx[i])) - cx);
            if (d < bestD) { bestD = d; best = (int)i; isV = true; }
        }
        for (size_t i = 0; i < g_hy.size(); i++) {
            int d = abs((int)lround(DocToClientY((double)g_hy[i])) - cy);
            if (d < bestD) { bestD = d; best = (int)i; isV = false; }
        }
        if (best >= 0) { g_dragSel = best; g_dragIsV = isV; SetCapture(h); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_dragSel < 0) break;
        POINT sp;
        GetCursorPos(&sp);
        POINT cp = { sp.x - g_vt.scr.left, sp.y - g_vt.scr.top };
        if (g_dragIsV) {
            int v = (int)lround(ClientToDocX(cp.x));
            if (g_dragSel < (int)g_vx.size()) g_vx[g_dragSel] = v;
        } else {
            int v = (int)lround(ClientToDocY(cp.y));
            if (g_dragSel < (int)g_hy.size()) g_hy[g_dragSel] = v;
        }
        RefreshList();
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragSel >= 0) { g_dragSel = -1; ReleaseCapture(); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------- 主窗口 ----------------
#define IDC_EDITV   101
#define IDC_ADDNV   102
#define IDC_EDITH   103
#define IDC_ADDH    104
#define IDC_LIST    105
#define IDC_DEL     106
#define IDC_CLEAR   107
#define IDC_PICK    108
#define IDC_SHOW    109
#define IDC_DRAG    110
#define IDC_CONNECT 111
#define IDC_UNIT    112
#define IDC_DPI     113

static HWND MakeBtn(HWND p, const wchar_t* t, int id, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, w, h, p, (HMENU)(INT_PTR)id, g_inst, NULL);
}

static void PickFromCursor(void) {
    POINT sp;
    if (!GetCursorPos(&sp)) return;
    POINT cp = { sp.x - g_vt.scr.left, sp.y - g_vt.scr.top };
    if (!g_vt.ok || cp.x < 0 || cp.y < 0 || cp.x >= g_vt.cw || cp.y >= g_vt.ch) return;
    wchar_t buf[48];
    FormatVal((int)lround(ClientToDocX(cp.x)), buf, _countof(buf));
    SetWindowTextW(g_editV, buf);
    FormatVal((int)lround(ClientToDocY(cp.y)), buf, _countof(buf));
    SetWindowTextW(g_editH, buf);
}

static void ConnectSai(void) {
    if (OpenSai() && ReadTransform(&g_vt)) {
        Glog(L"[connect] OK doc=%dx%d scale=%.4f center=(%.1f,%.1f) canvas=%p",
             g_vt.dw, g_vt.dh, g_vt.scale, g_vt.cx, g_vt.cy, g_canvas);
        UpdateStatus();
        MessageBoxW(g_main, L"已连接 SAI。\n状态栏会显示画布尺寸、缩放和光标坐标。", L"SaiGuides", MB_ICONINFORMATION);
        return;
    }
    g_vt.ok = false;
    UpdateStatus();
    if (!g_proc) {
        MessageBoxW(g_main,
            L"打不开 SAI 进程。\n\n最常见原因：SAI 是“以管理员身份”启动的，而本工具不是。\n"
            L"请右键 saiguides.exe → 以管理员身份运行，再点“连接 SAI”。\n\n"
            L"（诊断日志在同目录 saiguides.log）",
            L"SaiGuides", MB_ICONWARNING);
    } else {
        MessageBoxW(g_main,
            L"能打开 SAI 进程，但读不到视图数据。\n\n"
            L"可能：还没打开文档 / 版本不同。\n可以先打开一个文档再点一次。\n\n"
            L"（诊断日志在同目录 saiguides.log）",
            L"SaiGuides", MB_ICONWARNING);
    }
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        int y = 12;

        // 单位 + DPI
        CreateWindowExW(0, L"STATIC", L"单位", WS_CHILD | WS_VISIBLE, 12, y + 4, 40, 18, h, NULL, g_inst, NULL);
        g_comboUnit = CreateWindowExW(0, L"COMBOBOX", NULL,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            54, y, 120, 200, h, (HMENU)IDC_UNIT, g_inst, NULL);
        SendMessageW(g_comboUnit, CB_ADDSTRING, 0, (LPARAM)L"像素 px");
        SendMessageW(g_comboUnit, CB_ADDSTRING, 0, (LPARAM)L"毫米 mm");
        SendMessageW(g_comboUnit, CB_SETCURSEL, 0, 0);

        CreateWindowExW(0, L"STATIC", L"DPI", WS_CHILD | WS_VISIBLE, 190, y + 4, 40, 18, h, NULL, g_inst, NULL);
        g_editDpi = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"300", WS_CHILD | WS_VISIBLE,
            228, y - 1, 70, 26, h, (HMENU)IDC_DPI, g_inst, NULL);
        CreateWindowExW(0, L"STATIC", L"(换算用)", WS_CHILD | WS_VISIBLE,
            304, y + 4, 90, 18, h, NULL, g_inst, NULL);
        y += 42;

        CreateWindowExW(0, L"STATIC", L"竖线 X", WS_CHILD | WS_VISIBLE, 12, y + 4, 70, 18, h, NULL, g_inst, NULL);
        g_editV = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"512", WS_CHILD | WS_VISIBLE,
            86, y - 3, 100, 26, h, (HMENU)IDC_EDITV, g_inst, NULL);
        MakeBtn(h, L"添加竖线", IDC_ADDNV, 200, y - 3, 110, 28);
        y += 38;

        CreateWindowExW(0, L"STATIC", L"横线 Y", WS_CHILD | WS_VISIBLE, 12, y + 4, 70, 18, h, NULL, g_inst, NULL);
        g_editH = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"512", WS_CHILD | WS_VISIBLE,
            86, y - 3, 100, 26, h, (HMENU)IDC_EDITH, g_inst, NULL);
        MakeBtn(h, L"添加横线", IDC_ADDH, 200, y - 3, 110, 28);
        y += 40;

        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
            12, y, 410, 170, h, (HMENU)IDC_LIST, g_inst, NULL);
        y += 180;

        MakeBtn(h, L"取鼠标位置", IDC_PICK, 12, y, 130, 30);
        MakeBtn(h, L"删除选中", IDC_DEL, 152, y, 130, 30);
        MakeBtn(h, L"全部清空", IDC_CLEAR, 292, y, 130, 30);
        y += 40;

        g_chkShow = CreateWindowExW(0, L"BUTTON", L"显示参考线",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 12, y, 130, 24, h, (HMENU)IDC_SHOW, g_inst, NULL);
        SendMessageW(g_chkShow, BM_SETCHECK, BST_CHECKED, 0);
        g_chkDrag = CreateWindowExW(0, L"BUTTON", L"拖动模式（可拖线）",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 156, y, 210, 24, h, (HMENU)IDC_DRAG, g_inst, NULL);
        y += 34;

        MakeBtn(h, L"连接 SAI", IDC_CONNECT, 12, y, 140, 30);
        y += 42;

        g_status = CreateWindowExW(0, L"STATIC", L"未连接 SAI", WS_CHILD | WS_VISIBLE,
            12, y, 410, 44, h, NULL, g_inst, NULL);

        SetTimer(h, 1, 40, NULL);
        return 0;
    }
    case WM_TIMER: {
        if (g_pid == 0 || !IsWindow(g_saiMain)) OpenSai();
        // 跟随 SAI 最小化 / 还原
        {
            static BOOL s_saiMin = FALSE;
            BOOL min = (g_saiMain && IsWindow(g_saiMain)) ? IsIconic(g_saiMain) : FALSE;
            if (min != s_saiMin) {
                s_saiMin = min;
                if (min) {
                    ShowWindow(g_overlay, SW_HIDE);
                    ShowWindow(g_main, SW_MINIMIZE);
                } else {
                    ShowWindow(g_main, SW_RESTORE);
                }
                Glog(L"[follow] sai minimized=%d", min);
            }
            if (min) return 0;          // 最小化期间不做任何绘制
        }
        ViewT t = { 0 };
        bool ok = false;
        if (g_proc) {
            if (!IsWindow(g_saiMain)) { HWND mm = FindWindowW(L"sflRootWindow", NULL);
                if (mm) { DWORD pid2 = 0; GetWindowThreadProcessId(mm, &pid2);
                          if (pid2 != g_pid) { CloseSai(); } else g_saiMain = mm; } }
            ok = ReadTransform(&t);
        }
        g_vt = t;
        if (g_vt.ok) {
            POINT sp;
            if (GetCursorPos(&sp) && PtInRect(&g_vt.scr, sp)) {
                g_cursorDoc.x = (int)lround(ClientToDocX(sp.x - g_vt.scr.left));
                g_cursorDoc.y = (int)lround(ClientToDocY(sp.y - g_vt.scr.top));
            } else {
                g_cursorDoc.x = g_cursorDoc.y = -1;
            }
        }
        UpdateStatus();
        DrawOverlay();
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_UNIT && HIWORD(w) == CBN_SELCHANGE) {
            g_unit = (int)SendMessageW(g_comboUnit, CB_GETCURSEL, 0, 0);
            if (g_unit < 0) g_unit = 0;
            // 把输入框里的数值按新单位重写
            wchar_t buf[48];
            if (g_vt.ok) {
                int cx = (int)lround(g_vt.cx), cy = (int)lround(g_vt.cy);
                FormatVal(cx, buf, _countof(buf)); SetWindowTextW(g_editV, buf);
                FormatVal(cy, buf, _countof(buf)); SetWindowTextW(g_editH, buf);
            }
            RefreshList();
            UpdateStatus();
            return 0;
        }
        if (id == IDC_DPI && HIWORD(w) == EN_CHANGE) {
            wchar_t buf[32] = { 0 };
            GetWindowTextW(g_editDpi, buf, 32);
            double v = _wtof(buf);
            if (v >= 20 && v <= 5000) { g_dpi = v; RefreshList(); UpdateStatus(); }
            return 0;
        }
        if (id == IDC_ADDNV) {
            wchar_t buf[48] = { 0 };
            GetWindowTextW(g_editV, buf, 48);
            int v = 0;
            if (!ParseVal(buf, &v)) return 0;
            if (!g_vt.ok) { MessageBoxW(h, L"还没连上 SAI", L"SaiGuides", MB_ICONINFORMATION); return 0; }
            if (v < 0 || v >= g_vt.dw) {
                MessageBoxW(h, L"超出画布宽度范围", L"SaiGuides", MB_ICONINFORMATION);
                return 0;
            }
            AddGuide(true, v);
            return 0;
        }
        if (id == IDC_ADDH) {
            wchar_t buf[48] = { 0 };
            GetWindowTextW(g_editH, buf, 48);
            int v = 0;
            if (!ParseVal(buf, &v)) return 0;
            if (!g_vt.ok) { MessageBoxW(h, L"还没连上 SAI", L"SaiGuides", MB_ICONINFORMATION); return 0; }
            if (v < 0 || v >= g_vt.dh) {
                MessageBoxW(h, L"超出画布高度范围", L"SaiGuides", MB_ICONINFORMATION);
                return 0;
            }
            AddGuide(false, v);
            return 0;
        }
        if (id == IDC_DEL) { DeleteSelected(); return 0; }
        if (id == IDC_CLEAR) { g_vx.clear(); g_hy.clear(); RefreshList(); return 0; }
        if (id == IDC_PICK) { PickFromCursor(); return 0; }
        if (id == IDC_SHOW) {
            g_show = (SendMessageW(g_chkShow, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        }
        if (id == IDC_DRAG) {
            g_dragMode = (SendMessageW(g_chkDrag, BM_GETCHECK, 0, 0) == BST_CHECKED);
            LONG ex = GetWindowLongW(g_overlay, GWL_EXSTYLE);
            if (g_dragMode) ex &= ~WS_EX_TRANSPARENT;
            else ex |= WS_EX_TRANSPARENT;
            SetWindowLongW(g_overlay, GWL_EXSTYLE, ex);
            return 0;
        }
        if (id == IDC_CONNECT) { ConnectSai(); return 0; }
        if (id == IDC_LIST && HIWORD(w) == LBN_DBLCLK) {
            int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
            int px = -1;
            if (sel >= 0 && sel < (int)g_vx.size()) px = g_vx[sel];
            else if (sel >= 0) {
                int k = sel - (int)g_vx.size();
                if (k >= 0 && k < (int)g_hy.size()) px = g_hy[k];
            }
            if (px >= 0) {
                wchar_t buf[48];
                FormatVal(px, buf, _countof(buf));
                SetWindowTextW(g_editV, buf);
                SetWindowTextW(g_editH, buf);
            }
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        if (g_overlay) DestroyWindow(g_overlay);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SaiGuidesMain";
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"SaiGuidesMain",
        L"SAI 参考线", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        40, 120, 444, 520, NULL, NULL, inst, NULL);
    if (!g_main) return 0;

    WNDCLASSEXW oc = { sizeof(oc) };
    oc.lpfnWndProc = OverlayProc;
    oc.hInstance = inst;
    oc.lpszClassName = L"SaiGuidesOverlay";
    RegisterClassExW(&oc);
    g_overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        L"SaiGuidesOverlay", NULL, WS_POPUP,
        0, 0, 10, 10, NULL, NULL, inst, NULL);

    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t* s = wcsrchr(exe, L'\\');
    if (s) *(s + 1) = 0;
    wcscpy_s(g_logPath, exe);
    wcscat_s(g_logPath, L"saiguides.log");
    Glog(L"==== SaiGuides start ====");

    OpenSai();
    ReadTransform(&g_vt);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
