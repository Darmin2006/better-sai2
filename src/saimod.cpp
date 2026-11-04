// saimod.dll - 注入到 sai2.exe 里的"液化 / 渐变映射"悬浮面板
// 说明: 不改 SAI 内部代码，只在自己的窗口里截取画布 → 处理 → 通过剪贴板写回
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

// 日志固定写在 DLL 同目录的 saimod.log（不受 %TEMP% 重定向影响）
static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    wchar_t path[MAX_PATH] = { 0 };
    HMODULE mod = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&Log, &mod)) {
        GetModuleFileNameW(mod, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) *(slash + 1) = 0;
        wcscat_s(path, L"saimod.log");
    }

    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%s\n", buf);
        fclose(f);
    }
}

#pragma comment(lib, "comctl32.lib")

#define ID_MODE     100
#define ID_SIZE     101
#define ID_STR      102
#define ID_CAPTURE  103
#define ID_UNDO     104
#define ID_RESET    105
#define ID_WRITEBACK 106
#define ID_GMAP     110
#define ID_CANVAS   120
#define ID_GRAD     130
#define ID_GSEL     131
#define ID_GRESTORE 132
#define ID_CLIPCAP  133
#define ID_PIN      134
#define ID_BIG      135
#define ID_REC      136
#define ID_SAVEDISP 137
#define ID_APPLYDISP 138

static void FreeBackup(void);

static HINSTANCE g_inst;
static HWND g_sai;         // SAI 主窗口
static HWND g_main;        // 我们的面板
static HWND g_canvas;      // 图像预览区
static HWND g_btnSave = NULL, g_btnApply = NULL, g_lblState = NULL;

static int  g_w, g_h;      // 图像尺寸
static int  g_origW = 0, g_origH = 0;   // 抓图时的原始尺寸（写回要还原）
static uint32_t* g_src;    // 原始像素 (BGRA, 自上而下)
static uint32_t* g_warp;   // 当前结果像素
static float*    g_disp;   // 位移场 dx,dy
static HDC  g_srcDC, g_warpDC;
static HBITMAP g_srcBmp, g_warpBmp;
static void* g_srcBits; static void* g_warpBits;

#define MAXUNDO 12
static float* g_undo[MAXUNDO];
static int g_undoN = 0;

static int g_brush = 120, g_strength = 45;
static BOOL g_dragging = FALSE;
static POINT g_tip;
static BOOL g_haveDisp = FALSE;

// ---------------- 基础 ----------------
static inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

static void SampleBilinear(const uint32_t* src, int w, int h, float fx, float fy, BYTE out[4]) {
    if (fx < 0) fx = 0; else if (fx > w - 1) fx = (float)(w - 1);
    if (fy < 0) fy = 0; else if (fy > h - 1) fy = (float)(h - 1);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < w ? x0 + 1 : x0;
    int y1 = y0 + 1 < h ? y0 + 1 : y0;
    float ax = fx - x0, ay = fy - y0;
    const BYTE* p00 = (const BYTE*)&src[y0 * w + x0];
    const BYTE* p10 = (const BYTE*)&src[y0 * w + x1];
    const BYTE* p01 = (const BYTE*)&src[y1 * w + x0];
    const BYTE* p11 = (const BYTE*)&src[y1 * w + x1];
    for (int c = 0; c < 4; c++) {
        float a = p00[c] + (p10[c] - p00[c]) * ax;
        float b = p01[c] + (p11[c] - p01[c]) * ax;
        out[c] = (BYTE)(a + (b - a) * ay + 0.5f);
    }
}

static void AllocBuffers(int w, int h) {
    if (g_srcBmp) { DeleteObject(g_srcBmp); g_srcBmp = NULL; }
    if (g_warpBmp) { DeleteObject(g_warpBmp); g_warpBmp = NULL; }
    if (g_srcDC) { DeleteDC(g_srcDC); g_srcDC = NULL; }
    if (g_warpDC) { DeleteDC(g_warpDC); g_warpDC = NULL; }
    if (g_disp) { free(g_disp); g_disp = NULL; }
    for (int i = 0; i < g_undoN; i++) { free(g_undo[i]); g_undo[i] = NULL; }
    g_undoN = 0;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_srcDC = CreateCompatibleDC(NULL);
    g_srcBmp = CreateDIBSection(g_srcDC, &bi, DIB_RGB_COLORS, &g_srcBits, NULL, 0);
    SelectObject(g_srcDC, g_srcBmp);
    g_src = (uint32_t*)g_srcBits;

    g_warpDC = CreateCompatibleDC(NULL);
    g_warpBmp = CreateDIBSection(g_warpDC, &bi, DIB_RGB_COLORS, &g_warpBits, NULL, 0);
    SelectObject(g_warpDC, g_warpBmp);
    g_warp = (uint32_t*)g_warpBits;

    g_disp = (float*)calloc((size_t)w * h * 2, sizeof(float));
    g_w = w; g_h = h;
    g_haveDisp = FALSE;
}

// ---------------- SAI 视图变换（用于"按上次区域重抓"） ----------------
static BOOL SaiReadable(const void* p, size_t n) {
    if (!p) return FALSE;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return FALSE;
    if (mbi.State != MEM_COMMIT) return FALSE;
    if (mbi.Protect & PAGE_GUARD) return FALSE;
    DWORD pr = mbi.Protect & 0xFF;
    if (pr != PAGE_READWRITE && pr != PAGE_READONLY && pr != PAGE_EXECUTE_READ &&
        pr != PAGE_EXECUTE_READWRITE) return FALSE;
    return ((const BYTE*)p + n) <= ((const BYTE*)mbi.BaseAddress + mbi.RegionSize);
}

struct SaiViewT {
    BOOL   ok;
    double scale, cx, cy;
    int    cw, ch;
    POINT  org;
};
static SaiViewT g_view = { 0 };

static BOOL CALLBACK FindCanvasCb(HWND h, LPARAM lp) {
    HWND* best = (HWND*)lp;
    wchar_t cls[64] = { 0 };
    GetClassNameW(h, cls, 64);
    if (_wcsicmp(cls, L"sflChildWindow") != 0) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    RECT r; GetWindowRect(h, &r);
    int a = (r.right - r.left) * (r.bottom - r.top);
    if (a <= 0) return TRUE;
    if (*best) {
        RECT b; GetWindowRect(*best, &b);
        int ba = (b.right - b.left) * (b.bottom - b.top);
        if (a <= ba) return TRUE;
    }
    *best = h;
    return TRUE;
}

static BOOL ReadSaiView(SaiViewT* v) {
    if (!g_sai) return FALSE;
    BYTE* b = (BYTE*)GetModuleHandleW(NULL);
    void* doc = *(void**)(b + 0x320F18);
    if (!SaiReadable(doc, 0x2A0)) return FALSE;
    void* view = *(void**)((BYTE*)doc + 0x120);
    if (!SaiReadable(view, 0x3D0)) return FALSE;
    double cx = *(double*)((BYTE*)view + 0x378);
    double cy = *(double*)((BYTE*)view + 0x380);
    double sc = *(double*)((BYTE*)view + 0x3C0);
    if (!(sc > 0.0001 && sc < 1000.0)) return FALSE;

    HWND cv = NULL;
    EnumChildWindows(g_sai, FindCanvasCb, (LPARAM)&cv);
    if (!cv) return FALSE;
    RECT cr; GetClientRect(cv, &cr);
    POINT org = { 0, 0 };
    ClientToScreen(cv, &org);

    v->ok = TRUE;
    v->scale = sc; v->cx = cx; v->cy = cy;
    v->cw = cr.right - cr.left; v->ch = cr.bottom - cr.top;
    v->org = org;
    return TRUE;
}

static double Doc2ScrX(double x) { return g_view.org.x + (x - g_view.cx) / g_view.scale + g_view.cw * 0.5; }
static double Doc2ScrY(double y) { return g_view.org.y + (y - g_view.cy) / g_view.scale + g_view.ch * 0.5; }
static double Scr2DocX(double x) { return (x - g_view.org.x - g_view.cw * 0.5) * g_view.scale + g_view.cx; }
static double Scr2DocY(double y) { return (y - g_view.org.y - g_view.ch * 0.5) * g_view.scale + g_view.cy; }

static BOOL g_docSelValid = FALSE;
static RECT g_docSel = { 0, 0, 0, 0 };     // 文档坐标

// 按记住的文档区域重抓，缩放成 w×h
static BOOL RecaptureSavedRegion(int w, int h) {
    if (!g_docSelValid) return FALSE;
    if (!ReadSaiView(&g_view)) return FALSE;
    int sl = (int)floor(Doc2ScrX((double)g_docSel.left));
    int st = (int)floor(Doc2ScrY((double)g_docSel.top));
    int sr = (int)ceil(Doc2ScrX((double)g_docSel.right));
    int sb = (int)ceil(Doc2ScrY((double)g_docSel.bottom));
    if (sr - sl < 4 || sb - st < 4) return FALSE;

    AllocBuffers(w, h);
    ShowWindow(g_main, SW_HIDE);
    Sleep(120);
    HDC screen = GetDC(NULL);
    SetStretchBltMode(g_srcDC, HALFTONE);
    SetBrushOrgEx(g_srcDC, 0, 0, NULL);
    BOOL ok = StretchBlt(g_srcDC, 0, 0, w, h, screen, sl, st, sr - sl, sb - st, SRCCOPY);
    ReleaseDC(NULL, screen);
    ShowWindow(g_main, SW_SHOW);
    SetWindowPos(g_main, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (ok && g_disp) memset(g_disp, 0, (size_t)w * h * 2 * sizeof(float));
    Log(L"recapture: doc=(%d,%d)-(%d,%d) -> screen (%d,%d)-(%d,%d) size %dx%d ok=%d",
        g_docSel.left, g_docSel.top, g_docSel.right, g_docSel.bottom, sl, st, sr, sb, w, h, ok);
    return ok;
}

// ---------------- 截取画布 ----------------
static void DoCapture(RECT sel) {
    if (!g_sai) { MessageBoxW(g_main, L"没找到 SAI 窗口", L"saimod", MB_ICONWARNING); return; }

    int w = sel.right - sel.left, h = sel.bottom - sel.top;
    if (w < 8 || h < 8) { MessageBoxW(g_main, L"选区太小", L"saimod", MB_ICONWARNING); return; }

    // 记住这次框选对应的"文档区域"，之后可按同一区域重抓（保证多图层尺寸一致）
    if (ReadSaiView(&g_view)) {
        g_docSel.left = (LONG)floor(Scr2DocX((double)sel.left));
        g_docSel.top = (LONG)floor(Scr2DocY((double)sel.top));
        g_docSel.right = (LONG)ceil(Scr2DocX((double)sel.right));
        g_docSel.bottom = (LONG)ceil(Scr2DocY((double)sel.bottom));
        g_docSelValid = TRUE;
        Log(L"capture doc rect = (%d,%d)-(%d,%d)",
            g_docSel.left, g_docSel.top, g_docSel.right, g_docSel.bottom);
    }

    const int MAXSIDE = 4096;
    float s = 1.0f;
    if (w > MAXSIDE || h > MAXSIDE) s = (float)MAXSIDE / (w > h ? w : h);
    int dw = (int)(w * s), dh = (int)(h * s);
    if (dw < 8) dw = 8;
    if (dh < 8) dh = 8;

    AllocBuffers(dw, dh);
    FreeBackup();
    g_origW = w;
    g_origH = h;

    // 先藏起自己的面板，免得挡住画布；并从屏幕合成层抓（D2D/D3D 窗口用窗口 DC 抓是黑的）
    ShowWindow(g_main, SW_HIDE);
    Sleep(150);

    HDC screen = GetDC(NULL);
    SetStretchBltMode(g_srcDC, HALFTONE);
    SetBrushOrgEx(g_srcDC, 0, 0, NULL);
    BOOL ok = StretchBlt(g_srcDC, 0, 0, dw, dh, screen, sel.left, sel.top, w, h, SRCCOPY);
    ReleaseDC(NULL, screen);

    ShowWindow(g_main, SW_SHOW);
    SetWindowPos(g_main, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // 还是全黑就退回 PrintWindow
    long sum = 0; int n = 0;
    for (int i = 0; i < dw * dh; i += 13) {
        BYTE* q = (BYTE*)&g_src[i];
        sum += q[0] + q[1] + q[2]; n++;
    }
    double mean = n ? (double)sum / (3.0 * n) : 0;
    Log(L"capture screen: %dx%d rect=(%d,%d,%dx%d) ok=%d mean=%.1f",
        dw, dh, sel.left, sel.top, w, h, ok, mean);

    if (mean < 1.0) {
        RECT wr; GetWindowRect(g_sai, &wr);
        int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        HDC mem = CreateCompatibleDC(NULL);
        HBITMAP bmp = CreateCompatibleBitmap(mem, ww, wh);
        HGDIOBJ old = SelectObject(mem, bmp);
        PrintWindow(g_sai, mem, 2 /*PW_RENDERFULLCONTENT*/);
        SetStretchBltMode(g_srcDC, HALFTONE);
        StretchBlt(g_srcDC, 0, 0, dw, dh, mem,
                   sel.left - wr.left, sel.top - wr.top, w, h, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);

        sum = 0; n = 0;
        for (int i = 0; i < dw * dh; i += 13) {
            BYTE* q = (BYTE*)&g_src[i];
            sum += q[0] + q[1] + q[2]; n++;
        }
        mean = n ? (double)sum / (3.0 * n) : 0;
        Log(L"capture printwindow fallback: mean=%.1f", mean);
    }

    // 屏幕抓图没有 alpha（GDI 抓出来 alpha 全 0）。必须强制不透明，
    // 否则按 CF_DIBV5 写回时 SAI 会贴出一个全透明图层，看起来就是"空白图层"
    for (int i = 0; i < dw * dh; i++) ((BYTE*)&g_src[i])[3] = 0xFF;

    memcpy(g_warp, g_src, (size_t)dw * dh * 4);
    InvalidateRect(g_canvas, NULL, FALSE);
}

// ---------------- 剪贴板抓取（保留 alpha，不带工具栏） ----------------
// 解析 CF_DIBV5 / CF_DIB；像素起点用 GlobalSize - stride*h 反推
// （GDI 在 BI_BITFIELDS 下会把三个掩码再写一遍，仅凭头型判断会错）
static BOOL ReadClipboardImage(int* outW, int* outH, uint32_t** outPix) {
    *outW = 0; *outH = 0; *outPix = NULL;
    if (!OpenClipboard(g_main)) return FALSE;

    HANDLE h = GetClipboardData(CF_DIBV5);
    if (!h) h = GetClipboardData(CF_DIB);
    if (!h) { CloseClipboard(); return FALSE; }

    SIZE_T sz = GlobalSize(h);
    BYTE* p = (BYTE*)GlobalLock(h);
    BOOL ok = FALSE;

    if (p && sz >= 40) {
        DWORD hdr = *(DWORD*)p;
        BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)p;
        int w = bih->biWidth;
        int hh = bih->biHeight;
        BOOL topDown = hh < 0;
        if (hh < 0) hh = -hh;
        int bpp = bih->biBitCount;

        if (w > 0 && hh > 0 && (bpp == 32 || bpp == 24)) {
            int stride = ((w * bpp + 31) / 32) * 4;
            SIZE_T need = (SIZE_T)stride * hh;
            SIZE_T off = (sz > need) ? (sz - need) : (SIZE_T)hdr;
            if (off < hdr || off > (SIZE_T)hdr + 32) off = hdr;

            uint32_t* pix = (uint32_t*)malloc((size_t)w * hh * 4);
            if (pix) {
                BYTE* src = p + off;
                BOOL alphaAllZero = (bpp == 32);
                for (int y = 0; y < hh; y++) {
                    BYTE* row = src + (size_t)(topDown ? y : (hh - 1 - y)) * stride;
                    for (int x = 0; x < w; x++) {
                        BYTE b, g, r, a = 255;
                        if (bpp == 32) {
                            BYTE* s = row + x * 4;
                            b = s[0]; g = s[1]; r = s[2]; a = s[3];
                            if (a) alphaAllZero = FALSE;
                        } else {
                            BYTE* s = row + x * 3;
                            b = s[0]; g = s[1]; r = s[2];
                        }
                        pix[y * w + x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                                         ((uint32_t)g << 8) | (uint32_t)b;
                    }
                }
                if (bpp == 32 && alphaAllZero) {
                    for (int i = 0; i < w * hh; i++) pix[i] |= 0xFF000000u;
                }
                *outW = w; *outH = hh; *outPix = pix;
                ok = TRUE;
            }
        }
    }
    GlobalUnlock(h);
    CloseClipboard();
    return ok;
}

static void SendCtrl(WORD vk) {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = vk;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = vk;  in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// 自动在 SAI 里「全选 → 拷贝」，然后从剪贴板取图（alpha 保真）
static void CaptureFromClipboard(void) {
    if (!g_sai) { MessageBoxW(g_main, L"没找到 SAI 窗口", L"saimod", MB_ICONWARNING); return; }

    // 等热键的 Ctrl/Alt 松开，避免组合键污染
    for (int i = 0; i < 40; i++) {
        if (!(GetAsyncKeyState(VK_MENU) & 0x8000) &&
            !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) break;
        Sleep(10);
    }

    SetForegroundWindow(g_sai);
    Sleep(150);
    SendCtrl('A');          // 全选
    Sleep(120);
    SendCtrl('C');          // 拷贝
    Sleep(300);

    int w = 0, h = 0;
    uint32_t* pix = NULL;
    if (!ReadClipboardImage(&w, &h, &pix)) {
        MessageBoxW(g_main,
            L"剪贴板里没有图像。\n请先在 SAI 里选中图层，或改用「① 框选」屏幕抓取。",
            L"saimod", MB_ICONWARNING);
        return;
    }

    const int MAXSIDE = 4096;
    int dw = w, dh = h;
    if (w > MAXSIDE || h > MAXSIDE) {
        float s = (float)MAXSIDE / (w > h ? w : h);
        dw = (int)(w * s); dh = (int)(h * s);
        if (dw < 8) dw = 8;
        if (dh < 8) dh = 8;
    }

    AllocBuffers(dw, dh);
    FreeBackup();
    g_origW = w;
    g_origH = h;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((size_t)y * h / dh);
        for (int x = 0; x < dw; x++) {
            int sx = (int)((size_t)x * w / dw);
            g_src[y * dw + x] = pix[(size_t)sy * w + sx];
        }
    }
    free(pix);
    memcpy(g_warp, g_src, (size_t)dw * dh * 4);
    InvalidateRect(g_canvas, NULL, FALSE);
    Log(L"capture clipboard: %dx%d -> %dx%d", w, h, dw, dh);
}

// ---------------- 框选（可移动 / 可缩放 / 回车确认） ----------------
static RECT  g_selRect;        // 选择层客户区坐标
static RECT  g_selOrig;        // 开始拖动时的矩形
static POINT g_selAnchor;      // 拖动起点
static BOOL  g_selHas = FALSE; // 是否已框出有效选区
static int   g_selMode = 0;    // 0=空闲 1=新建 2=移动 3=缩放
static int   g_selEdge = 0;    // 1=左 2=上 4=右 8=下
static HWND  g_selWnd = NULL;

#define SELHIT 10

static RECT  g_selScreen = { 0, 0, 0, 0 };   // 上一次框选的屏幕矩形（给"精确覆盖"用）
static BOOL  g_hasSelRect = FALSE;
static BOOL  g_bigOverlay = FALSE;           // 大图窗是否处于"精确覆盖"模式

static void NormalizeRect(RECT* r) {
    if (r->left > r->right) { int t = r->left; r->left = r->right; r->right = t; }
    if (r->top > r->bottom) { int t = r->top; r->top = r->bottom; r->bottom = t; }
}

static int HitEdge(RECT r, POINT p) {
    if (p.x < r.left - SELHIT || p.x > r.right + SELHIT) return 0;
    if (p.y < r.top - SELHIT || p.y > r.bottom + SELHIT) return 0;
    int e = 0;
    if (p.x >= r.left - SELHIT && p.x <= r.left + SELHIT) e |= 1;
    else if (p.x >= r.right - SELHIT && p.x <= r.right + SELHIT) e |= 4;
    if (p.y >= r.top - SELHIT && p.y <= r.top + SELHIT) e |= 2;
    else if (p.y >= r.bottom - SELHIT && p.y <= r.bottom + SELHIT) e |= 8;
    return e;
}

static void SetSelCursor(int edge) {
    LPCSTR id = IDC_CROSS;
    if (edge == (1 | 2) || edge == (4 | 8))      id = IDC_SIZENWSE;
    else if (edge == (4 | 2) || edge == (1 | 8)) id = IDC_SIZENESW;
    else if (edge & 1 || edge & 4)               id = IDC_SIZEWE;
    else if (edge & 2 || edge & 8)               id = IDC_SIZENS;
    SetCursor(LoadCursor(NULL, id));
}

static void ConfirmSel(HWND h) {
    RECT wr; GetWindowRect(h, &wr);          // 选择层覆盖整个虚拟屏
    RECT sel;

    if (g_selHas &&
        g_selRect.right - g_selRect.left >= 8 &&
        g_selRect.bottom - g_selRect.top >= 8) {
        sel.left   = wr.left + g_selRect.left;
        sel.top    = wr.top  + g_selRect.top;
        sel.right  = wr.left + g_selRect.right;
        sel.bottom = wr.top  + g_selRect.bottom;
    } else {
        // 没框 → 退回整个 SAI 客户区
        RECT cr; GetClientRect(g_sai, &cr);
        POINT a = { cr.left, cr.top }, b = { cr.right, cr.bottom };
        ClientToScreen(g_sai, &a);
        ClientToScreen(g_sai, &b);
        sel.left = a.x; sel.top = a.y; sel.right = b.x; sel.bottom = b.y;
    }

    DestroyWindow(h);
    g_selWnd = NULL;
    g_selRect = sel;
    g_selScreen = sel;
    g_hasSelRect = TRUE;
    PostMessageW(g_main, WM_APP + 10, 0, 0);
}

static LRESULT CALLBACK SelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
        Log(L"Sel: down (%d,%d) has=%d mode=%d", pt.x, pt.y, g_selHas, g_selMode);
        g_selAnchor = pt;
        g_selOrig = g_selRect;
        int edge = g_selHas ? HitEdge(g_selRect, pt) : 0;
        if (g_selHas && edge) {
            g_selMode = 3; g_selEdge = edge;
        } else if (g_selHas && PtInRect(&g_selRect, pt)) {
            g_selMode = 2;                       // 拖动整个框
        } else {
            g_selMode = 1;                       // 重新框
            g_selRect.left = g_selRect.right = pt.x;
            g_selRect.top = g_selRect.bottom = pt.y;
            g_selHas = FALSE;
        }
        SetCapture(h);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
        if (g_selMode == 1) {
            g_selRect.left = g_selAnchor.x < pt.x ? g_selAnchor.x : pt.x;
            g_selRect.right = g_selAnchor.x > pt.x ? g_selAnchor.x : pt.x;
            g_selRect.top = g_selAnchor.y < pt.y ? g_selAnchor.y : pt.y;
            g_selRect.bottom = g_selAnchor.y > pt.y ? g_selAnchor.y : pt.y;
        } else if (g_selMode == 2) {
            int dx = pt.x - g_selAnchor.x, dy = pt.y - g_selAnchor.y;
            g_selRect.left = g_selOrig.left + dx;
            g_selRect.right = g_selOrig.right + dx;
            g_selRect.top = g_selOrig.top + dy;
            g_selRect.bottom = g_selOrig.bottom + dy;
        } else if (g_selMode == 3) {
            if (g_selEdge & 1) g_selRect.left = pt.x;
            if (g_selEdge & 4) g_selRect.right = pt.x;
            if (g_selEdge & 2) g_selRect.top = pt.y;
            if (g_selEdge & 8) g_selRect.bottom = pt.y;
            NormalizeRect(&g_selRect);
        } else {
            SetSelCursor(g_selHas ? HitEdge(g_selRect, pt) : 0);
            return 0;
        }
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }

    case WM_LBUTTONUP:
        ReleaseCapture();
        if (g_selMode == 1) {
            NormalizeRect(&g_selRect);
            if (g_selRect.right - g_selRect.left < 8 ||
                g_selRect.bottom - g_selRect.top < 8) {
                // 只是点了一下 → 直接按"整个客户区"抓取（旧行为）
                ConfirmSel(h);
                return 0;
            }
            g_selHas = TRUE;
        }
        g_selMode = 0;
        InvalidateRect(h, NULL, TRUE);
        return 0;

    case WM_LBUTTONDBLCLK: {
        POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
        if (g_selHas && PtInRect(&g_selRect, pt)) ConfirmSel(h);
        else { g_selMode = 1; g_selAnchor = pt;
               g_selRect.left = g_selRect.right = pt.x;
               g_selRect.top = g_selRect.bottom = pt.y; g_selHas = FALSE; SetCapture(h); }
        return 0;
    }

    case WM_KEYDOWN:
        if (w == VK_RETURN) { ConfirmSel(h); return 0; }
        if (w == VK_ESCAPE) { DestroyWindow(h); g_selWnd = NULL; return 0; }
        return 0;

    case WM_RBUTTONDOWN:
        DestroyWindow(h); g_selWnd = NULL;
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT cr; GetClientRect(h, &cr);
        SetBkMode(dc, TRANSPARENT);

        // 屏幕四周画一圈亮边，明确提示"已进入框选模式"
        HPEN pen0 = CreatePen(PS_SOLID, 4, RGB(64, 160, 255));
        HGDIOBJ op0 = SelectObject(dc, pen0);
        HGDIOBJ ob0 = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 2, 2, cr.right - 2, cr.bottom - 2);
        SelectObject(dc, op0);
        SelectObject(dc, ob0);
        DeleteObject(pen0);

        if (g_selHas || g_selMode != 0) {
            int x0 = g_selRect.left, y0 = g_selRect.top;
            int x1 = g_selRect.right, y1 = g_selRect.bottom;

            HPEN pen = CreatePen(PS_SOLID, 4, RGB(64, 160, 255));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, x0, y0, x1, y1);
            SelectObject(dc, oldPen); SelectObject(dc, oldBr); DeleteObject(pen);

            wchar_t info[96];
            swprintf_s(info, L"%d × %d", x1 - x0, y1 - y0);
            SetTextColor(dc, RGB(255, 255, 255));
            TextOutW(dc, x0 + 8, y0 + 8, info, (int)wcslen(info));
        }

        SetTextColor(dc, RGB(220, 230, 245));
        const wchar_t* tip = g_selHas
            ? L"框内拖动 = 移动   拖边角 = 缩放   回车 / 双击 = 确认   Esc / 右键 = 取消"
            : L"拖拽框选画布区域（不框直接回车 = 整个窗口）";
        TextOutW(dc, 40, 40, tip, (int)wcslen(tip));

        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void StartSelect(void) {
    Log(L"StartSelect: g_sai=%p g_selWnd=%p", g_sai, g_selWnd);
    if (!g_sai) { MessageBoxW(g_main, L"没找到 SAI 窗口", L"saimod", MB_ICONWARNING); return; }
    if (g_selWnd) { SetForegroundWindow(g_selWnd); return; }

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = SelProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"SaiModSelect";
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);

    g_selWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"SaiModSelect", L"选择区域", WS_POPUP,
        vx, vy, vw, vh, NULL, NULL, g_inst, NULL);
    if (!g_selWnd) { Log(L"StartSelect: CreateWindow 失败 err=%lu", GetLastError()); return; }
    Log(L"StartSelect: created %p", g_selWnd);

    g_selHas = FALSE;
    g_selMode = 0;
    SetRectEmpty(&g_selRect);

    SetLayeredWindowAttributes(g_selWnd, 0, 90, LWA_ALPHA);
    ShowWindow(g_selWnd, SW_SHOW);
    Log(L"StartSelect: shown");
    SetForegroundWindow(g_selWnd);
    SetFocus(g_selWnd);
}

// ---------------- 渲染 ----------------
static void RenderRegion(int x0, int y0, int x1, int y1) {
    if (!g_src) return;
    x0 = clampi(x0, 0, g_w); y0 = clampi(y0, 0, g_h);
    x1 = clampi(x1, 0, g_w); y1 = clampi(y1, 0, g_h);
    BYTE px[4];
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            int i = (y * g_w + x) * 2;
            if (g_disp[i] == 0 && g_disp[i + 1] == 0) continue;
            SampleBilinear(g_src, g_w, g_h, x - g_disp[i], y - g_disp[i + 1], px);
            memcpy(&g_warp[y * g_w + x], px, 4);
        }
    }
}

static void RenderAll(void) {
    if (!g_src) return;
    memcpy(g_warp, g_src, (size_t)g_w * g_h * 4);
    RenderRegion(0, 0, g_w, g_h);
}

// ---------------- 多图层：记住形变 + 套用到当前抓取的图 ----------------
static float* g_savedDisp = NULL;
static int    g_savedW = 0, g_savedH = 0;

static void SaveDispField(void) {
    if (!g_disp || g_w <= 0 || g_h <= 0) {
        MessageBoxW(g_main, L"还没有形变可记（先液化一下）", L"saimod", MB_ICONINFORMATION);
        return;
    }
    size_t n = (size_t)g_w * g_h * 2;
    float* p = (float*)realloc(g_savedDisp, n * sizeof(float));
    if (!p) { MessageBoxW(g_main, L"内存不足", L"saimod", MB_ICONERROR); return; }
    g_savedDisp = p;
    memcpy(g_savedDisp, g_disp, n * sizeof(float));
    g_savedW = g_w; g_savedH = g_h;
    if (g_lblState) {
        wchar_t s[64];
        _snwprintf_s(s, _countof(s), _TRUNCATE, L"✓ 已记住 %dx%d", g_savedW, g_savedH);
        SetWindowTextW(g_lblState, s);
    }
    MessageBoxW(g_main, L"形变已记住。\n\n接下来：\n1. 在 SAI 里切到另一个图层\n2. 选中它 → Ctrl+A → Ctrl+C\n3. 点 ② 剪贴板\n4. 点 ② 套用到当前图\n5. ④ 写回",
                L"saimod", MB_ICONINFORMATION);
}

static void ApplyDispField(void) {
    if (!g_savedDisp) {
        MessageBoxW(g_main,
            L"还没有记住形变。\n\n顺序是：\n"
            L"1. 先在某个图层上液化\n"
            L"2. 点 ① 记住形变\n"
            L"3. 再切到别的图层，Ctrl+A / Ctrl+C，点 ② 剪贴板\n"
            L"4. 最后点 ② 套用到当前图",
            L"saimod", MB_ICONINFORMATION);
        return;
    }
    // 尺寸不一致：把当前抓到的图缩放到记住的尺寸再套用（不依赖重抓）
    if (!g_src || !g_warp) {
        MessageBoxW(g_main,
            L"当前没有可套用的图。\n\n请先在 SAI 里选中目标图层 → Ctrl+A → Ctrl+C → 点 ② 剪贴板，再点这个按钮。",
            L"saimod", MB_ICONWARNING);
        return;
    }
    if (g_w != g_savedW || g_h != g_savedH) {
        int nw = g_savedW, nh = g_savedH;
        int ow = g_w, oh = g_h;
        uint32_t* tmp = (uint32_t*)malloc((size_t)nw * nh * 4);
        if (!tmp) { MessageBoxW(g_main, L"内存不足", L"saimod", MB_ICONERROR); return; }
        for (int y = 0; y < nh; y++) {
            int sy = (int)((long long)y * oh / nh);
            if (sy >= oh) sy = oh - 1;
            for (int x = 0; x < nw; x++) {
                int sx = (int)((long long)x * ow / nw);
                if (sx >= ow) sx = ow - 1;
                tmp[(size_t)y * nw + x] = g_src[(size_t)sy * ow + sx];
            }
        }
        AllocBuffers(nw, nh);
        if (!g_src || !g_warp || !g_disp) { free(tmp); return; }
        memcpy(g_src, tmp, (size_t)nw * nh * 4);
        free(tmp);
        Log(L"[apply] 尺寸不同，已把 %dx%d 缩放为 %dx%d 再套用", ow, oh, nw, nh);
    }
    if (!g_disp) return;
    memcpy(g_disp, g_savedDisp, (size_t)g_w * g_h * 2 * sizeof(float));
    RenderAll();
    if (g_canvas) InvalidateRect(g_canvas, NULL, FALSE);
    HWND bw = FindWindowW(L"SaiModBig", NULL);
    if (bw) InvalidateRect(bw, NULL, FALSE);
    if (g_lblState) SetWindowTextW(g_lblState, L"✓ 已套用，可写回");
}

// ---------------- 笔刷 ----------------
static float Falloff(float d, float r, float hard) {
    if (d >= r) return 0.f;
    float t = 1.f - d / r;
    float p = 0.6f + (1.f - hard) * 2.4f;
    return (float)pow(t, p);
}

static void PushUndo(void) {
    if (!g_disp) return;
    if (g_undoN == MAXUNDO) { free(g_undo[0]); for (int i = 1; i < MAXUNDO; i++) g_undo[i - 1] = g_undo[i]; g_undoN--; }
    float* copy = (float*)malloc((size_t)g_w * g_h * 2 * sizeof(float));
    memcpy(copy, g_disp, (size_t)g_w * g_h * 2 * sizeof(float));
    g_undo[g_undoN++] = copy;
}

static void UndoOnce(void) {
    if (g_undoN <= 0 || !g_disp) return;
    memcpy(g_disp, g_undo[--g_undoN], (size_t)g_w * g_h * 2 * sizeof(float));
    free(g_undo[g_undoN]);
    g_undo[g_undoN] = NULL;
    RenderAll();
    InvalidateRect(g_canvas, NULL, FALSE);
    HWND bw = FindWindowW(L"SaiModBig", NULL);
    if (bw) InvalidateRect(bw, NULL, FALSE);
}

static void ApplyBrush(int cx, int cy, float dx, float dy, int mode) {
    if (!g_src) return;
    float r = (float)g_brush;
    float strength = g_strength / 100.f;
    int x0 = clampi((int)(cx - r), 0, g_w), x1 = clampi((int)(cx + r) + 1, 0, g_w);
    int y0 = clampi((int)(cy - r), 0, g_h), y1 = clampi((int)(cy + r) + 1, 0, g_h);

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float ddx = x - cx, ddy = y - cy;
            float d = sqrtf(ddx * ddx + ddy * ddy);
            float f = Falloff(d, r, 0.5f);
            if (f <= 0) continue;
            int i = (y * g_w + x) * 2;
            float vx = 0, vy = 0;
            switch (mode) {
            case 0: vx = dx * f * strength; vy = dy * f * strength; break;              // 推进
            case 1: { float k = 1.f + f * strength * 2.2f;
                      vx = g_disp[i] * k - g_disp[i]; vy = g_disp[i + 1] * k - g_disp[i + 1]; break; } // 膨胀
            case 2: { float k = 1.f - f * strength * 0.75f;
                      vx = g_disp[i] * k - g_disp[i]; vy = g_disp[i + 1] * k - g_disp[i + 1]; break; } // 收缩
            case 3: case 4: {
                float a = (mode == 3 ? 1.f : -1.f) * f * strength * 0.22f;
                float ca = cosf(a), sa = sinf(a);
                vx = g_disp[i] * ca - g_disp[i + 1] * sa - g_disp[i];
                vy = g_disp[i] * sa + g_disp[i + 1] * ca - g_disp[i + 1];
                break;
            }
            case 5: {   // 平滑：位移场向邻域平均靠拢
                float ax = 0, ay = 0; int cnt = 0;
                for (int oy = -2; oy <= 2; oy += 2) {
                    for (int ox = -2; ox <= 2; ox += 2) {
                        int nx = x + ox, ny = y + oy;
                        if (nx < 0 || ny < 0 || nx >= g_w || ny >= g_h) continue;
                        int j = (ny * g_w + nx) * 2;
                        ax += g_disp[j]; ay += g_disp[j + 1]; cnt++;
                    }
                }
                if (cnt) {
                    ax /= cnt; ay /= cnt;
                    vx = (ax - g_disp[i]) * f * strength * 0.5f;
                    vy = (ay - g_disp[i + 1]) * f * strength * 0.5f;
                }
                break;
            }
            case 6: {   // 重建：位移场往 0 收敛，抹掉形变
                vx = -g_disp[i] * f * strength * 0.25f;
                vy = -g_disp[i + 1] * f * strength * 0.25f;
                break;
            }
            }
            g_disp[i] += vx;
            g_disp[i + 1] += vy;
            float m = sqrtf(g_disp[i] * g_disp[i] + g_disp[i + 1] * g_disp[i + 1]);
            float lim = r * 2.5f;
            if (m > lim) { g_disp[i] *= lim / m; g_disp[i + 1] *= lim / m; }
        }
    }
    g_haveDisp = TRUE;
    RenderRegion(x0, y0, x1, y1);
}

// ---------------- 渐变映射（可编辑色标） ----------------
struct GStop { float t; COLORREF c; int id; };
static GStop g_stops[16];
static int   g_stopN = 0;
static int   g_selId = -1;
static int   g_stopSeq = 0;
static HWND  g_gradBar = NULL;
static uint32_t* g_srcBak = NULL;
static BOOL  g_gradDrag = FALSE;

static void FreeBackup(void) {
    if (g_srcBak) { free(g_srcBak); g_srcBak = NULL; }
}
static void EnsureBackup(void) {
    if (!g_src || g_srcBak) return;
    g_srcBak = (uint32_t*)malloc((size_t)g_w * g_h * 4);
    if (g_srcBak) memcpy(g_srcBak, g_src, (size_t)g_w * g_h * 4);
}
static void SortStops(void) {
    for (int i = 1; i < g_stopN; i++) {
        GStop k = g_stops[i];
        int j = i - 1;
        while (j >= 0 && g_stops[j].t > k.t) { g_stops[j + 1] = g_stops[j]; j--; }
        g_stops[j + 1] = k;
    }
}
static int SelIndex(void) {
    for (int i = 0; i < g_stopN; i++) if (g_stops[i].id == g_selId) return i;
    return -1;
}

static COLORREF ColorAt(float t) {
    if (g_stopN <= 0) return RGB(0, 0, 0);
    if (t <= g_stops[0].t) return g_stops[0].c;
    if (t >= g_stops[g_stopN - 1].t) return g_stops[g_stopN - 1].c;
    for (int i = 0; i < g_stopN - 1; i++) {
        if (t >= g_stops[i].t && t <= g_stops[i + 1].t) {
            float span = g_stops[i + 1].t - g_stops[i].t;
            float u = (span <= 0.0001f) ? 0.f : (t - g_stops[i].t) / span;
            int r = (int)(GetRValue(g_stops[i].c) + (GetRValue(g_stops[i + 1].c) - GetRValue(g_stops[i].c)) * u + 0.5f);
            int g = (int)(GetGValue(g_stops[i].c) + (GetGValue(g_stops[i + 1].c) - GetGValue(g_stops[i].c)) * u + 0.5f);
            int b = (int)(GetBValue(g_stops[i].c) + (GetBValue(g_stops[i + 1].c) - GetBValue(g_stops[i].c)) * u + 0.5f);
            return RGB(r, g, b);
        }
    }
    return g_stops[g_stopN - 1].c;
}

static void ApplyGradient(void) {
    if (!g_src || g_stopN < 2) return;
    EnsureBackup();
    if (!g_srcBak) return;
    BYTE lut[256 * 3];
    for (int i = 0; i < 256; i++) {
        COLORREF c = ColorAt(i / 255.f);
        lut[i * 3 + 0] = GetRValue(c);
        lut[i * 3 + 1] = GetGValue(c);
        lut[i * 3 + 2] = GetBValue(c);
    }
    for (int i = 0; i < g_w * g_h; i++) {
        BYTE* p = (BYTE*)&g_srcBak[i];        // 始终从原图重算，非破坏
        BYTE* q = (BYTE*)&g_src[i];
        int lum = (int)(p[0] * 0.0722f + p[1] * 0.7152f + p[2] * 0.2126f);
        q[0] = lut[lum * 3 + 2];
        q[1] = lut[lum * 3 + 1];
        q[2] = lut[lum * 3 + 0];
        q[3] = p[3];
    }
    RenderAll();
    InvalidateRect(g_canvas, NULL, FALSE);
    InvalidateRect(g_gradBar, NULL, FALSE);
}

static void RestoreOriginal(void) {
    if (!g_src || !g_srcBak) return;
    memcpy(g_src, g_srcBak, (size_t)g_w * g_h * 4);
    RenderAll();
    InvalidateRect(g_canvas, NULL, FALSE);
}

static void PresetToStops(int idx) {
    static const BYTE ramps[5][9] = {
        { 0x3a,0x0f,0x2b,  0x7a,0xb3,0xff,  0x00,0x00,0x00 },
        { 0x4a,0x1e,0x0a,  0xff,0xe8,0xa8,  0x00,0x00,0x00 },
        { 0x00,0x00,0x00,  0xff,0xb0,0x00,  0xff,0xff,0xff },
        { 0x2b,0x2b,0x0d,  0x7a,0x8f,0x8a,  0xc8,0xe0,0xf3 },
        { 0x6b,0x30,0x14,  0xb0,0xf3,0xff,  0x00,0x00,0x00 },
    };
    if (idx < 0 || idx > 4) idx = 0;
    g_stopN = 0;
    const BYTE* r = ramps[idx];
    for (int i = 0; i < 3; i++) {
        g_stops[g_stopN].t = i / 2.0f;
        g_stops[g_stopN].c = RGB(r[i * 3 + 0], r[i * 3 + 1], r[i * 3 + 2]);
        g_stops[g_stopN].id = ++g_stopSeq;
        g_stopN++;
    }
    g_selId = -1;
}

static void ApplyGradientMap(int preset) {
    PresetToStops(preset);
    ApplyGradient();
}

static void PickStopColor(int idx) {
    if (idx < 0 || idx >= g_stopN) return;
    static COLORREF custom[16] = { 0 };
    CHOOSECOLORW cc = { sizeof(cc) };
    cc.hwndOwner = g_main;
    cc.rgbResult = g_stops[idx].c;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        g_stops[idx].c = cc.rgbResult;
        g_selId = g_stops[idx].id;
        ApplyGradient();
    }
}

static void GradPaint(HDC dc, int w, int h) {
    int barH = 26;
    for (int x = 0; x < w; x++) {
        COLORREF c = ColorAt(w > 1 ? (float)x / (w - 1) : 0.f);
        HBRUSH b = CreateSolidBrush(c);
        RECT r = { x, 2, x + 1, barH };
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 86, 96));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 2, w, barH);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);

    for (int i = 0; i < g_stopN; i++) {
        int x = (int)(g_stops[i].t * (w - 1));
        POINT tri[3] = { { x - 5, barH + 2 }, { x + 5, barH + 2 }, { x, barH + 10 } };
        HBRUSH br = CreateSolidBrush(i == SelIndex() ? RGB(64, 160, 255) : RGB(200, 205, 215));
        HGDIOBJ ob2 = SelectObject(dc, br);
        HGDIOBJ op2 = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, tri, 3);
        SelectObject(dc, ob2);
        SelectObject(dc, op2);
        DeleteObject(br);
    }
    (void)h;
}

static int HitStop(int mx, int w) {
    for (int i = 0; i < g_stopN; i++) {
        int x = (int)(g_stops[i].t * (w - 1));
        if (mx >= x - 7 && mx <= x + 7) return i;
    }
    return -1;
}

static LRESULT CALLBACK GradProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        GradPaint(dc, rc.right, rc.bottom);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        RECT rc;
        GetClientRect(h, &rc);
        int mx = (short)LOWORD(lp);
        int idx = HitStop(mx, rc.right);
        if (idx < 0) {
            if (g_stopN >= 16) return 0;
            float t = rc.right > 1 ? (float)mx / (rc.right - 1) : 0.f;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            g_stops[g_stopN].t = t;
            g_stops[g_stopN].c = ColorAt(t);
            g_stops[g_stopN].id = ++g_stopSeq;
            g_selId = g_stops[g_stopN].id;
            g_stopN++;
            SortStops();
            idx = SelIndex();
        } else {
            g_selId = g_stops[idx].id;
        }
        g_gradDrag = TRUE;
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        (void)idx;
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_gradDrag) return 0;
        RECT rc;
        GetClientRect(h, &rc);
        int idx = SelIndex();
        if (idx >= 0) {
            float t = rc.right > 1 ? (float)(short)LOWORD(lp) / (rc.right - 1) : 0.f;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            g_stops[idx].t = t;
            SortStops();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_gradDrag) {
            g_gradDrag = FALSE;
            ReleaseCapture();
            ApplyGradient();
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        RECT rc;
        GetClientRect(h, &rc);
        int idx = HitStop((short)LOWORD(lp), rc.right);
        if (idx >= 0) { g_selId = g_stops[idx].id; PickStopColor(idx); }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        RECT rc;
        GetClientRect(h, &rc);
        int idx = HitStop((short)LOWORD(lp), rc.right);
        if (idx >= 0 && g_stopN > 2) {
            for (int i = idx; i < g_stopN - 1; i++) g_stops[i] = g_stops[i + 1];
            g_stopN--;
            g_selId = -1;
            InvalidateRect(h, NULL, FALSE);
            ApplyGradient();
        }
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------- 写回 SAI ----------------
static void WriteBack(void) {
    if (!g_warp) { MessageBoxW(g_main, L"先抓图（框选或剪贴板）", L"saimod", MB_ICONWARNING); return; }

    // 抓图时可能为了性能缩小过，写回必须还原成原始尺寸，否则 SAI 里大小不对
    int outW  = (g_origW > 0) ? g_origW : g_w;
    int outH  = (g_origH > 0) ? g_origH : g_h;
    uint32_t* src = g_warp;
    uint32_t* tmp = NULL;

    if (outW != g_w || outH != g_h) {
        tmp = (uint32_t*)malloc((size_t)outW * outH * 4);
        if (tmp) {
            for (int y = 0; y < outH; y++) {
                int sy = (int)((long long)y * g_h / outH);
                if (sy >= g_h) sy = g_h - 1;
                for (int x = 0; x < outW; x++) {
                    int sx = (int)((long long)x * g_w / outW);
                    if (sx >= g_w) sx = g_w - 1;
                    tmp[(size_t)y * outW + x] = g_warp[(size_t)sy * g_w + sx];
                }
            }
            src = tmp;
            Log(L"writeback: scaled %dx%d -> %dx%d", g_w, g_h, outW, outH);
        }
    }

    // 兜底：如果整张图 alpha 全 0，强制不透明，避免贴出空白图层
    {
        BOOL anyAlpha = FALSE;
        for (int i = 0; i < g_w * g_h && !anyAlpha; i++)
            if (((BYTE*)&g_warp[i])[3] != 0) anyAlpha = TRUE;
        if (!anyAlpha) {
            for (int i = 0; i < g_w * g_h; i++) ((BYTE*)&g_warp[i])[3] = 0xFF;
            Log(L"writeback: alpha 全 0，已强制不透明");
        }
    }

    int stride = outW * 4;
    SIZE_T pixBytes = (SIZE_T)stride * outH;

    // --- CF_DIBV5：带 alpha，SAI 透明图层能保真 ---
    HGLOBAL m5 = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + pixBytes);
    if (m5) {
        BITMAPV5HEADER* v5 = (BITMAPV5HEADER*)GlobalLock(m5);
        memset(v5, 0, sizeof(*v5));
        v5->bV5Size = sizeof(BITMAPV5HEADER);
        v5->bV5Width = outW;
        v5->bV5Height = outH;
        v5->bV5Planes = 1;
        v5->bV5BitCount = 32;
        v5->bV5Compression = BI_BITFIELDS;
        v5->bV5SizeImage = (DWORD)pixBytes;
        v5->bV5RedMask = 0x00FF0000;
        v5->bV5GreenMask = 0x0000FF00;
        v5->bV5BlueMask = 0x000000FF;
        v5->bV5AlphaMask = 0xFF000000;
        v5->bV5CSType = 0x73524742;              // 'sRGB'
        BYTE* d5 = (BYTE*)v5 + sizeof(BITMAPV5HEADER);
        for (int y = 0; y < outH; y++)
            memcpy(d5 + (size_t)(outH - 1 - y) * stride, (BYTE*)src + (size_t)y * stride, stride);
        GlobalUnlock(m5);
    }

    // --- CF_DIB：兜底，给只认老格式的程序 ---
    HGLOBAL m1 = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixBytes);
    if (m1) {
        BYTE* p = (BYTE*)GlobalLock(m1);
        BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)p;
        memset(bih, 0, sizeof(*bih));
        bih->biSize = sizeof(*bih); bih->biWidth = outW; bih->biHeight = outH;
        bih->biPlanes = 1; bih->biBitCount = 32; bih->biCompression = BI_RGB;
        bih->biSizeImage = (DWORD)pixBytes;
        BYTE* dst = p + sizeof(*bih);
        for (int y = 0; y < outH; y++)
            memcpy(dst + (size_t)(outH - 1 - y) * stride, (BYTE*)src + (size_t)y * stride, stride);
        GlobalUnlock(m1);
    }

    if (!OpenClipboard(g_main)) {
        if (m5) GlobalFree(m5);
        if (m1) GlobalFree(m1);
        return;
    }
    EmptyClipboard();
    if (m5) SetClipboardData(CF_DIBV5, m5);
    if (m1) SetClipboardData(CF_DIB, m1);
    CloseClipboard();

    if (g_sai) {
        SetForegroundWindow(g_sai);
        Sleep(120);
        INPUT in[4] = {};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 'V';
        in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = 'V'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(4, in, sizeof(INPUT));
    }
    if (tmp) free(tmp);
}

// ---------------- 预览区 ----------------
static void CanvasPaint(HDC dc, int cw, int ch) {
    RECT full = { 0, 0, cw, ch };
    {
        const int CS = 12;
        for (int y = 0; y < ch; y += CS) {
            for (int x = 0; x < cw; x += CS) {
                BOOL odd = (((x / CS) + (y / CS)) & 1);
                HBRUSH br = CreateSolidBrush(odd ? RGB(200, 200, 200) : RGB(240, 240, 240));
                RECT r = { x, y, x + CS, y + CS };
                FillRect(dc, &r, br);
                DeleteObject(br);
            }
        }
    }
    if (!g_warp) return;
    float sx = (float)cw / g_w, sy = (float)ch / g_h;
    float s = sx < sy ? sx : sy;
    int dw = (int)(g_w * s), dh = (int)(g_h * s);
    int ox = (cw - dw) / 2, oy = (ch - dh) / 2;
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    // 用 AlphaBlend 把 alpha 也画出来（透明区显示棋盘格，而不是黑）
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, ox, oy, dw, dh, g_warpDC, 0, 0, g_w, g_h, bf);
}

static void CanvasToImage(int mx, int my, float* ix, float* iy) {
    RECT rc; GetClientRect(g_canvas, &rc);
    int cw = rc.right, ch = rc.bottom;
    float sx = (float)cw / g_w, sy = (float)ch / g_h;
    float s = sx < sy ? sx : sy;
    int dw = (int)(g_w * s), dh = (int)(g_h * s);
    int ox = (cw - dw) / 2, oy = (ch - dh) / 2;
    *ix = (mx - ox) / s;
    *iy = (my - oy) / s;
}

static LRESULT CALLBACK CanvasProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        CanvasPaint(mem, rc.right, rc.bottom);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN:
        if (w == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) { UndoOnce(); return 0; }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!g_src) return 0;
        SetFocus(h);
        SetCapture(h);
        PushUndo();
        g_dragging = TRUE;
        float ix, iy; CanvasToImage((short)LOWORD(l), (short)HIWORD(l), &ix, &iy);
        g_tip.x = (LONG)ix; g_tip.y = (LONG)iy;
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_dragging || !g_src) return 0;
        float ix, iy; CanvasToImage((short)LOWORD(l), (short)HIWORD(l), &ix, &iy);
        float dx = ix - g_tip.x, dy = iy - g_tip.y;
        int mode = (int)SendMessageW(GetDlgItem(g_main, ID_MODE), CB_GETCURSEL, 0, 0);
        if (mode < 0) mode = 0;
        int steps = (int)(sqrtf(dx * dx + dy * dy) / (g_brush * 0.25f)) + 1;
        if (steps > 40) steps = 40;
        for (int s = 1; s <= steps; s++) {
            float px = g_tip.x + dx * s / steps;
            float py = g_tip.y + dy * s / steps;
            ApplyBrush((int)px, (int)py, dx / steps, dy / steps, mode);
        }
        g_tip.x = (LONG)ix; g_tip.y = (LONG)iy;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragging) { g_dragging = FALSE; ReleaseCapture(); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------- 主面板 ----------------
// ---------------- 全屏大图绘制窗 ----------------
static HWND  g_bigWnd = NULL;
static float g_bigScale = 1.f, g_bigOffX = 0, g_bigOffY = 0;
static BOOL  g_bigDrawing = FALSE, g_bigPanning = FALSE;
static float g_bigTipX = 0, g_bigTipY = 0;
static POINT g_bigAnchor;
static float g_bigOffX0 = 0, g_bigOffY0 = 0;
static HDC     g_bigDC = NULL;      // 双缓冲，避免每帧刷黑
static HBITMAP g_bigBmp = NULL;
static int     g_bigBW = 0, g_bigBH = 0;

static void BigEnsureBuf(HDC ref, int w, int h) {
    if (g_bigDC && g_bigBW == w && g_bigBH == h) return;
    if (g_bigBmp) { DeleteObject(g_bigBmp); g_bigBmp = NULL; }
    if (g_bigDC) { DeleteDC(g_bigDC); g_bigDC = NULL; }
    if (w <= 0 || h <= 0) return;
    g_bigDC = CreateCompatibleDC(ref);
    g_bigBmp = CreateCompatibleBitmap(ref, w, h);
    SelectObject(g_bigDC, g_bigBmp);
    g_bigBW = w;
    g_bigBH = h;
}

static void BigToImage(HWND h, int mx, int my, float* ix, float* iy) {
    *ix = (mx - g_bigOffX) / g_bigScale;
    *iy = (my - g_bigOffY) / g_bigScale;
}

static void BigPaintRect(HWND h, HDC dc, RECT prc) {
    // 棋盘格背景（PS 式透明底），避免黑底把线稿吞掉
    {
        const int CS = 12;
        for (int y = prc.top; y < prc.bottom; y += CS) {
            for (int x = prc.left; x < prc.right; x += CS) {
                BOOL odd = (((x / CS) + (y / CS)) & 1);
                HBRUSH br = CreateSolidBrush(odd ? RGB(200, 200, 200) : RGB(240, 240, 240));
                RECT r = { x, y, x + CS, y + CS };
                FillRect(dc, &r, br);
                DeleteObject(br);
            }
        }
    }
    if (!g_warp) return;

    int dx = (int)g_bigOffX, dy = (int)g_bigOffY;
    int dw = (int)(g_w * g_bigScale), dh = (int)(g_h * g_bigScale);
    RECT img = { dx, dy, dx + dw, dy + dh };
    RECT in;
    if (!IntersectRect(&in, &prc, &img)) return;

    int sx = (int)((in.left - dx) / g_bigScale);
    int sy = (int)((in.top - dy) / g_bigScale);
    int sw = (int)((in.right - in.left) / g_bigScale) + 1;
    int sh = (int)((in.bottom - in.top) / g_bigScale) + 1;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx + sw > g_w) sw = g_w - sx;
    if (sy + sh > g_h) sh = g_h - sy;
    if (sw <= 0 || sh <= 0) return;

    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    BLENDFUNCTION bf2 = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    AlphaBlend(dc, in.left, in.top, in.right - in.left, in.bottom - in.top,
               g_warpDC, sx, sy, sw, sh, bf2);

    (void)h;
}

static void BigPaintTip(HDC dc) {
    wchar_t tip[160];
    if (g_bigOverlay)
        swprintf_s(tip, L"精确覆盖   %d%%   左键涂抹 · 回车/Esc 返回（位置已对齐原画）", (int)(g_bigScale * 100));
    else
        swprintf_s(tip, L"大图绘制   %d%%   滚轮缩放 · 右键拖动平移 · 回车/Esc 返回", (int)(g_bigScale * 100));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(120, 200, 255));
    TextOutW(dc, 24, 16, tip, (int)wcslen(tip));
}

// 兼容旧调用：整窗重绘
static void BigPaint(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    BigPaintRect(h, dc, rc);
    BigPaintTip(dc);
}

static LRESULT CALLBACK BigProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        BigEnsureBuf(dc, rc.right, rc.bottom);
        if (g_bigDC) {
            BigPaintRect(h, g_bigDC, ps.rcPaint);
            BigPaintTip(g_bigDC);
            BitBlt(dc,
                   ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top,
                   g_bigDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        } else {
            BigPaint(h, dc);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        if (!g_src) return 0;
        float ix, iy;
        BigToImage(h, (short)LOWORD(lp), (short)HIWORD(lp), &ix, &iy);
        PushUndo();
        g_bigDrawing = TRUE;
        g_bigTipX = ix;
        g_bigTipY = iy;
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_bigPanning) {
            g_bigOffX = g_bigOffX0 + ((short)LOWORD(lp) - g_bigAnchor.x);
            g_bigOffY = g_bigOffY0 + ((short)HIWORD(lp) - g_bigAnchor.y);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (!g_bigDrawing || !g_src) return 0;
        float ix, iy;
        BigToImage(h, (short)LOWORD(lp), (short)HIWORD(lp), &ix, &iy);
        float dx = ix - g_bigTipX, dy = iy - g_bigTipY;
        int mode = (int)SendMessageW(GetDlgItem(g_main, ID_MODE), CB_GETCURSEL, 0, 0);
        if (mode < 0) mode = 0;
        int steps = (int)(sqrtf(dx * dx + dy * dy) / (g_brush * 0.25f)) + 1;
        if (steps > 40) steps = 40;
        for (int s = 1; s <= steps; s++) {
            float px = g_bigTipX + dx * s / steps;
            float py = g_bigTipY + dy * s / steps;
            ApplyBrush((int)px, (int)py, dx / steps, dy / steps, mode);
        }
        g_bigTipX = ix;
        g_bigTipY = iy;
        // 只重绘笔刷影响的那一块，避免整屏闪烁
        {
            int rr = (int)(g_brush * g_bigScale) + 6;
            RECT d;
            d.left = (int)(ix * g_bigScale + g_bigOffX) - rr;
            d.top = (int)(iy * g_bigScale + g_bigOffY) - rr;
            d.right = d.left + rr * 2;
            d.bottom = d.top + rr * 2;
            InvalidateRect(h, &d, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_bigDrawing) { g_bigDrawing = FALSE; ReleaseCapture(); }
        return 0;
    case WM_RBUTTONDOWN:
        g_bigPanning = TRUE;
        g_bigAnchor.x = (short)LOWORD(lp);
        g_bigAnchor.y = (short)HIWORD(lp);
        g_bigOffX0 = g_bigOffX;
        g_bigOffY0 = g_bigOffY;
        SetCapture(h);
        return 0;
    case WM_RBUTTONUP:
        if (g_bigPanning) { g_bigPanning = FALSE; ReleaseCapture(); }
        return 0;
    case WM_MOUSEWHEEL: {
        if (g_bigOverlay) return 0;          // 精确覆盖模式不允许缩放，保持 1:1 对齐
        short delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(h, &pt);
        float ix, iy;
        BigToImage(h, pt.x, pt.y, &ix, &iy);
        float f = (delta > 0) ? 1.15f : (1.f / 1.15f);
        float ns = g_bigScale * f;
        if (ns < 0.05f) ns = 0.05f;
        if (ns > 8.f) ns = 8.f;
        g_bigScale = ns;
        // 保持鼠标下的像素不动
        g_bigOffX = pt.x - ix * g_bigScale;
        g_bigOffY = pt.y - iy * g_bigScale;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_ESCAPE) { DestroyWindow(h); return 0; }
        if (wp == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) { UndoOnce(); return 0; }
        return 0;
    case WM_LBUTTONDBLCLK:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_bigWnd = NULL;
        if (g_bigBmp) { DeleteObject(g_bigBmp); g_bigBmp = NULL; }
        if (g_bigDC) { DeleteDC(g_bigDC); g_bigDC = NULL; }
        g_bigBW = g_bigBH = 0;
        InvalidateRect(g_canvas, NULL, FALSE);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void StartBig(void) {
    if (!g_src || !g_warp) {
        MessageBoxW(g_main, L"先抓一张图（① 或 ②）", L"saimod", MB_ICONWARNING);
        return;
    }
    if (g_bigWnd) { SetForegroundWindow(g_bigWnd); return; }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = BigProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"SaiModBig";
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // 有上次框选的矩形、且还在 SAI 窗口里 → 精确覆盖那一块
    BOOL overlay = FALSE;
    int x = vx, y = vy, w = vw, h = vh;
    if (g_hasSelRect && (g_selScreen.right - g_selScreen.left) > 16 &&
        (g_selScreen.bottom - g_selScreen.top) > 16) {
        RECT wr;
        GetWindowRect(g_sai, &wr);
        int cx = (g_selScreen.left + g_selScreen.right) / 2;
        int cy = (g_selScreen.top + g_selScreen.bottom) / 2;
        if (cx >= wr.left && cx <= wr.right && cy >= wr.top && cy <= wr.bottom) {
            overlay = TRUE;
            x = g_selScreen.left;
            y = g_selScreen.top;
            w = g_selScreen.right - g_selScreen.left;
            h = g_selScreen.bottom - g_selScreen.top;
        }
    }
    g_bigOverlay = overlay;

    g_bigWnd = CreateWindowExW(WS_EX_TOPMOST,
        L"SaiModBig", L"大图绘制", WS_POPUP,
        x, y, w, h, NULL, NULL, g_inst, NULL);
    if (!g_bigWnd) return;

    // 按窗口适配（精确覆盖模式下就是 1:1 对齐到原画位置）
    float sx = (float)w / g_w, sy = (float)h / g_h;
    g_bigScale = sx < sy ? sx : sy;
    g_bigOffX = (w - g_w * g_bigScale) * 0.5f;
    g_bigOffY = (h - g_h * g_bigScale) * 0.5f;

    ShowWindow(g_bigWnd, SW_SHOW);
    SetForegroundWindow(g_bigWnd);
    SetFocus(g_bigWnd);
}

// ---------------- 笔刷位置校准 ----------------
static BOOL  g_calibrate = FALSE;   // 按 F9 后进入，下一次点击记为"液化笔刷"
static BOOL  g_hasBrush = FALSE;
static float g_brushNY = 0.5f;      // 归一化 Y（0..1）
static int   g_palW = 0, g_palH = 0; // 校准时那个面板的尺寸

static void SaveConfig(void) {
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(g_inst, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(path, L"liquify.txt");

    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"w, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"trigger=F8\n");
    fwprintf(f, L"exit=B,E,G,P,V\n");
    fwprintf(f, L"brush=%.4f,%d,%d\n", g_brushNY, g_palW, g_palH);
    fclose(f);
}

static void HandleSaiClick(HWND h, POINT pt) {
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, hh = r.bottom - r.top;
    if (w <= 0 || hh <= 0) return;
    float nx = (float)(pt.x - r.left) / w;
    float ny = (float)(pt.y - r.top) / hh;

    wchar_t cls[64] = { 0 };
    GetClassNameW(h, cls, 64);
    Log(L"CLICK (%d,%d) cls=%s size=%dx%d rel=(%.3f,%.3f)", pt.x, pt.y, cls, w, hh, nx, ny);

    if (g_calibrate) {
        g_brushNY = ny;
        g_palW = w;
        g_palH = hh;
        g_hasBrush = TRUE;
        g_calibrate = FALSE;
        SaveConfig();
        Log(L"CALIBRATED: brush ny=%.3f panel=%dx%d", g_brushNY, g_palW, g_palH);
        MessageBoxW(g_main, L"已记录为「液化笔刷」。\n\n以后点这个位置就自动进液化，点同一面板的其他位置会弹确认。",
                    L"校准完成", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return;
    }

    if (!g_hasBrush) return;

    // 只在"校准时那个尺寸的面板"里判断（容忍 12px 误差）
    if (abs(w - g_palW) > 12 || abs(hh - g_palH) > 12) return;

    float dyn = ny - g_brushNY;
    if (dyn < 0) dyn = -dyn;
    BOOL onBrush = (dyn <= 0.07f);

    if (onBrush) {
        if (!g_bigWnd) PostMessageW(g_main, WM_APP + 20, 0, 0);
    } else {
        if (g_bigWnd) PostMessageW(g_main, WM_APP + 21, 0, 0);
    }
}
static int g_trigKey = VK_F8;      // 唤起液化的键
static int g_exitKeys[8] = { 0 };
static int g_exitN = 0;

static int VkFromName(const wchar_t* s) {
    if (!s || !s[0]) return 0;
    if (s[1] == 0 && s[0] >= L'A' && s[0] <= L'Z') return (int)s[0];
    if (s[1] == 0 && s[0] >= L'0' && s[0] <= L'9') return (int)s[0];
    if (s[0] == L'F' || s[0] == L'f') {
        int n = _wtoi(s + 1);
        if (n >= 1 && n <= 12) return VK_F1 + n - 1;
    }
    return 0;
}

static void LoadLiquifyConfig(void) {
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW(g_inst, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(path, L"liquify.txt");

    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"r, ccs=UTF-8") != 0 || !f) {
        g_exitKeys[0] = 'B'; g_exitKeys[1] = 'E'; g_exitKeys[2] = 'G';
        g_exitKeys[3] = 'P'; g_exitKeys[4] = 'V';
        g_exitN = 5;
        return;
    }
    wchar_t line[256];
    while (fgetws(line, 256, f)) {
        size_t n = wcslen(line);
        while (n && (line[n - 1] == L'\n' || line[n - 1] == L'\r' || line[n - 1] == L' ')) line[--n] = 0;
        wchar_t* eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        wchar_t* key = line;
        wchar_t* val = eq + 1;
        if (_wcsicmp(key, L"trigger") == 0) {
            int vk = VkFromName(val);
            if (vk) g_trigKey = vk;
        } else if (_wcsicmp(key, L"brush") == 0) {
            double ny = 0; int pw = 0, ph = 0;
            if (swscanf_s(val, L"%lf,%d,%d", &ny, &pw, &ph) == 3) {
                g_brushNY = (float)ny;
                g_palW = pw;
                g_palH = ph;
                g_hasBrush = TRUE;
            }
        } else if (_wcsicmp(key, L"exit") == 0) {
            g_exitN = 0;
            wchar_t* ctx = NULL;
            wchar_t* tok = wcstok_s(val, L", ", &ctx);
            while (tok && g_exitN < 8) {
                int vk = VkFromName(tok);
                if (vk) g_exitKeys[g_exitN++] = vk;
                tok = wcstok_s(NULL, L", ", &ctx);
            }
        }
    }
    fclose(f);
}

// 每 30ms 轮询一次按键（不用键盘钩子，避免被杀软当成键盘记录器）
static DWORD WINAPI KeyWatch(LPVOID) {
    BOOL prevTrig = FALSE;
    BOOL prevExit = FALSE;
    BOOL prevQuit = FALSE;
    for (;;) {
        Sleep(30);
        if (!g_sai || !g_main) continue;

        // 只有本进程（SAI）在前台才处理
        HWND fg = GetForegroundWindow();
        if (!fg) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid != GetCurrentProcessId()) {
            prevTrig = FALSE;
            prevExit = FALSE;
            prevQuit = FALSE;
            continue;
        }

        // F9：进入液化
        BOOL down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (down && !prevTrig) PostMessageW(g_main, WM_APP + 20, 0, 0);
        prevTrig = down;

        // F10：退出液化（弹确认）
        BOOL ex = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        if (ex && !prevExit) PostMessageW(g_main, WM_APP + 23, 0, 0);
        prevExit = ex;

        // Ctrl+Alt+Z：撤销
        (void)prevQuit;
    }
    return 0;
}

// 诊断：把 SAI 的子窗口结构写进日志（为"点笔刷"检测做评估）
static BOOL CALLBACK DumpChild(HWND h, LPARAM lp) {
    wchar_t cls[128] = { 0 }, txt[128] = { 0 };
    GetClassNameW(h, cls, 128);
    GetWindowTextW(h, txt, 128);
    RECT r;
    GetWindowRect(h, &r);
    Log(L"child %p cls=%-24s txt=%-16s rect=(%d,%d) %dx%d vis=%d",
        h, cls, txt, r.left, r.top, r.right - r.left, r.bottom - r.top,
        IsWindowVisible(h));
    return TRUE;
}

// 子类化 SAI 的所有子窗口：点一下就把「窗口文本」记进日志（校准用）
static LRESULT CALLBACK ChildHook(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                  UINT_PTR id, DWORD_PTR ref) {
    if (m == WM_LBUTTONDOWN) {
        wchar_t cls[128] = { 0 }, txt[256] = { 0 };
        GetClassNameW(h, cls, 128);
        GetWindowTextW(h, txt, 256);
        RECT r;
        GetWindowRect(h, &r);
        Log(L"HIT %p cls=%s txt=[%s] rect=(%d,%d) %dx%d",
            h, cls, txt, r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
    return DefSubclassProc(h, m, wp, lp);
}

// 钩最顶层窗口：SAI 在它自己的 wndproc 里做命中测试，子窗口不吃点击。
// 所以按屏幕坐标反查"这个点落在哪个子窗口里"，再读那个子窗口的文本。
struct HitCtx { POINT pt; HWND out; int best; };

static BOOL CALLBACK HitEnum(HWND h, LPARAM lp) {
    HitCtx* c = (HitCtx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    RECT r;
    GetWindowRect(h, &r);
    if (r.right <= r.left || r.bottom <= r.top) return TRUE;
    if (c->pt.x >= r.left && c->pt.x < r.right &&
        c->pt.y >= r.top && c->pt.y < r.bottom) {
        int area = (r.right - r.left) * (r.bottom - r.top);
        if (area < c->best) { c->best = area; c->out = h; }
    }
    return TRUE;
}

// 跨线程钩主窗口：SetWindowSubclass 不能跨线程（实测返回 0），
// 改用 SetWindowLongPtr 替换窗口过程，这个可以跨线程。
struct Hooked { HWND h; WNDPROC orig; };
static Hooked g_hooked[64];
static int    g_hookedN = 0;

static WNDPROC OrigFor(HWND h) {
    for (int i = 0; i < g_hookedN; i++)
        if (g_hooked[i].h == h) return g_hooked[i].orig;
    return NULL;
}

static void LogClickAt(HWND root, POINT pt) {
    HitCtx c;
    c.pt = pt;
    c.out = NULL;
    c.best = 0x7FFFFFFF;
    EnumChildWindows(root, HitEnum, (LPARAM)&c);
    if (c.out) {
        wchar_t cls[128] = { 0 }, txt[256] = { 0 };
        GetClassNameW(c.out, cls, 128);
        GetWindowTextW(c.out, txt, 256);
        RECT r;
        GetWindowRect(c.out, &r);
        Log(L"CLICK (%d,%d) txt=[%s] cls=%s rect=(%d,%d) %dx%d",
            pt.x, pt.y, txt, cls, r.left, r.top, r.right - r.left, r.bottom - r.top);
    } else {
        wchar_t rc0[64] = { 0 };
        GetClassNameW(root, rc0, 64);
        Log(L"CLICK (%d,%d) root=%s 没命中子窗口", pt.x, pt.y, rc0);
    }
}

static LRESULT CALLBACK SaiWndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_LBUTTONDOWN) {
        POINT pt;
        GetCursorPos(&pt);
        HandleSaiClick(h, pt);
    }
    WNDPROC o = (WNDPROC)GetPropW(h, L"saimod.orig");
    return o ? CallWindowProcW(o, h, m, wp, lp) : DefWindowProcW(h, m, wp, lp);
}

static void HookWindow(HWND h) {
    if (!h || h == g_main) return;
    WNDPROC cur = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (cur == SaiWndProc) return;
    WNDPROC orig = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)SaiWndProc);
    if (!orig) return;
    SetPropW(h, L"saimod.orig", (HANDLE)orig);
    if (GetPropW(h, L"saimod.logged")) return;
    SetPropW(h, L"saimod.logged", (HANDLE)1);
    wchar_t cls[64] = { 0 }, txt[128] = { 0 };
    GetClassNameW(h, cls, 64);
    GetWindowTextW(h, txt, 128);
    RECT r;
    GetWindowRect(h, &r);
    Log(L"hook %p cls=%s txt=[%s] rect=(%d,%d) %dx%d",
        h, cls, txt, r.left, r.top, r.right - r.left, r.bottom - r.top);
}

static BOOL CALLBACK HookTopEnum(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) HookWindow(h);
    return TRUE;
}

static BOOL CALLBACK SubAll(HWND h, LPARAM lp) {
    HookWindow(h);
    return TRUE;
}

// 给本进程的所有顶层窗口尝试挂钩，并记录结果
static BOOL CALLBACK SubTop(HWND h, LPARAM lp) {
    HookWindow(h);
    return TRUE;
}

// ---------------- 缩时摄影 ----------------
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

#define ID_REC_START 140
#define ID_REC_FOLD  141
#define ID_REC_INT   142

static HWND     g_recWnd = NULL;
static volatile LONG g_recording = 0;
static HANDLE   g_recThread = NULL;
static int      g_recInterval = 2;      // 秒
static volatile LONG g_recFrames = 0;
static wchar_t  g_recPath[MAX_PATH] = { 0 };
static BOOL     g_recFolded = FALSE;
static RECT     g_recRect = { 0, 0, 0, 0 };
static int      g_recW = 0, g_recH = 0;

static void GetSaiDir(wchar_t* out, int cch) {
    GetModuleFileNameW(NULL, out, cch);      // SAI 主 exe
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *(slash + 1) = 0;
}

// 找画布窗口的屏幕矩形：优先用框选记录，否则取最大的可见子窗口
static void FindCanvasRect(RECT* out) {
    if (g_hasSelRect && (g_selScreen.right - g_selScreen.left) > 64) {
        *out = g_selScreen;
        return;
    }
    struct C { RECT best; long area; } c;
    c.area = 0;
    SetRect(&c.best, 0, 0, 0, 0);

    // 手工枚举：用 EnumChildWindows 的回调无法捕获，这里用静态变量
    static RECT  s_best;
    static long  s_area;
    s_area = 0;
    SetRect(&s_best, 0, 0, 0, 0);

    EnumChildWindows(g_sai, [](HWND h, LPARAM) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;
        RECT r;
        GetWindowRect(h, &r);
        long w = r.right - r.left, hh = r.bottom - r.top;
        if (w < 200 || hh < 200) return TRUE;
        long a = w * hh;
        if (a > s_area) { s_area = a; s_best = r; }
        return TRUE;
    }, 0);

    if (s_area > 0) *out = s_best;
    else {
        GetClientRect(g_sai, out);
        POINT a = { out->left, out->top }, b = { out->right, out->bottom };
        ClientToScreen(g_sai, &a);
        ClientToScreen(g_sai, &b);
        out->left = a.x; out->top = a.y; out->right = b.x; out->bottom = b.y;
    }
}

static void RecSetStatus(const wchar_t* extra) {
    if (!g_recWnd) return;
    wchar_t buf[256];
    if (g_recording)
        swprintf_s(buf, L"● 录制中   %ld 帧   %s", g_recFrames, extra ? extra : L"");
    else
        swprintf_s(buf, L"○ 待机     %s", extra ? extra : L"按「开始」录缩时");
    SetDlgItemTextW(g_recWnd, 150, buf);
}

// 录制线程：抓帧 → Media Foundation 写 MP4
struct RecCtx { int w, h; };

static DWORD WINAPI RecThread(LPVOID param) {
    RecCtx* ctx = (RecCtx*)param;
    int w = ctx->w, h = ctx->h;
    free(ctx);

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    IMFSinkWriter* writer = NULL;
    DWORD stream = 0;
    BOOL ok = FALSE;

    hr = MFCreateSinkWriterFromURL(g_recPath, NULL, NULL, &writer);
    if (SUCCEEDED(hr)) {
        IMFMediaType* pOut = NULL;
        MFCreateMediaType(&pOut);
        pOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        pOut->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
        pOut->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(pOut, MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(pOut, MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(pOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->AddStream(pOut, &stream);
        pOut->Release();
    }
    if (SUCCEEDED(hr)) {
        IMFMediaType* pIn = NULL;
        MFCreateMediaType(&pIn);
        pIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        pIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(pIn, MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(pIn, MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(pIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->SetInputMediaType(stream, pIn, NULL);
        pIn->Release();
    }
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    if (SUCCEEDED(hr)) ok = TRUE;

    // 抓帧用的 DIB（自下而上，和 RGB32 一致）
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC memDC = CreateCompatibleDC(NULL);
    void* bits = NULL;
    HBITMAP dib = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(memDC, dib);

    BYTE* prev = (BYTE*)malloc((size_t)w * h * 4);
    int frame = 0;
    Log(L"rec: start %dx%d -> %s", w, h, g_recPath);

    while (g_recording) {
        // 分片睡眠，随时响应停止
        for (int s = 0; s < g_recInterval * 10 && g_recording; s++) Sleep(100);
        if (!g_recording) break;

        HDC screen = GetDC(NULL);
        SetStretchBltMode(memDC, HALFTONE);
        SetBrushOrgEx(memDC, 0, 0, NULL);
        StretchBlt(memDC, 0, 0, w, h, screen,
                   g_recRect.left, g_recRect.top,
                   g_recRect.right - g_recRect.left,
                   g_recRect.bottom - g_recRect.top, SRCCOPY);
        ReleaseDC(NULL, screen);

        BYTE* cur = (BYTE*)bits;
        if (prev) {   // 和上一帧比较，几乎没变就跳过
            long long diff = 0;
            int n = w * h * 4;
            for (int i = 0; i < n; i += 64) diff += abs((int)cur[i] - (int)prev[i]);
            if (diff < (long long)(n / 64) * 1) continue;
            memcpy(prev, cur, (size_t)n);
        }

        if (ok) {
            DWORD bytes = (DWORD)(w * h * 4);
            IMFMediaBuffer* buf = NULL;
            IMFSample* samp = NULL;
            if (SUCCEEDED(MFCreateMemoryBuffer(bytes, &buf))) {
                BYTE* dst = NULL;
                DWORD maxLen = 0;
                if (SUCCEEDED(buf->Lock(&dst, &maxLen, NULL))) {
                    memcpy(dst, cur, bytes);
                    buf->Unlock();
                    buf->SetCurrentLength(bytes);
                    if (SUCCEEDED(MFCreateSample(&samp))) {
                        samp->AddBuffer(buf);
                        LONGLONG dur = 10000000LL / 30;
                        samp->SetSampleTime(frame * dur);
                        samp->SetSampleDuration(dur);
                        writer->WriteSample(stream, samp);
                        frame++;
                        InterlockedIncrement(&g_recFrames);
                        samp->Release();
                    }
                }
                buf->Release();
            }
        }
    }

    if (writer) {
        if (ok) writer->Finalize();
        writer->Release();
    }
    if (prev) free(prev);
    if (dib) DeleteObject(dib);
    if (memDC) DeleteDC(memDC);
    MFShutdown();
    if (SUCCEEDED(hr)) CoUninitialize();

    Log(L"rec: stop, %d frames written, hr=0x%08X", frame, hr);
    return 0;
}

static void StartRec(void) {
    if (g_recording) return;

    FindCanvasRect(&g_recRect);
    int w = g_recRect.right - g_recRect.left;
    int h = g_recRect.bottom - g_recRect.top;
    if (w < 64 || h < 64) {
        MessageBoxW(g_recWnd, L"找不到画布区域", L"缩时摄影", MB_ICONWARNING);
        return;
    }
    // 长边限制 1920，够看，编码也快
    const int MAXSIDE = 1920;
    if (w > MAXSIDE || h > MAXSIDE) {
        float s = (float)MAXSIDE / (w > h ? w : h);
        w = (int)(w * s);
        h = (int)(h * s);
    }
    w &= ~1; h &= ~1;            // H.264 要求偶数
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    g_recW = w;
    g_recH = h;

    // 输出目录：SAI 目录下的 timelapse 子文件夹
    wchar_t dir[MAX_PATH];
    GetSaiDir(dir, MAX_PATH);
    wcscat_s(dir, L"timelapse");
    CreateDirectoryW(dir, NULL);

    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(g_recPath, L"%s\\timelapse_%04d%02d%02d_%02d%02d%02d.mp4",
               dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    InterlockedExchange(&g_recFrames, 0);
    InterlockedExchange(&g_recording, 1);

    RecCtx* ctx = (RecCtx*)malloc(sizeof(RecCtx));
    ctx->w = w;
    ctx->h = h;
    g_recThread = CreateThread(NULL, 0, RecThread, ctx, 0, NULL);

    RecSetStatus(L"");
    Log(L"rec: stop-request cleared, interval=%d", g_recInterval);
}

static void StopRec(void) {
    if (!g_recording) return;
    InterlockedExchange(&g_recording, 0);
    if (g_recThread) {
        WaitForSingleObject(g_recThread, 15000);
        CloseHandle(g_recThread);
        g_recThread = NULL;
    }
    wchar_t msg[64];
    swprintf_s(msg, L"已保存 %ld 帧", g_recFrames);
    RecSetStatus(msg);
}

static LRESULT CALLBACK RecProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC", L"○ 待机", WS_CHILD | WS_VISIBLE,
                        12, 34, 300, 20, h, (HMENU)150, g_inst, NULL);
        HWND cb = CreateWindowExW(0, L"COMBOBOX", NULL,
                        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                        12, 60, 110, 200, h, (HMENU)ID_REC_INT, g_inst, NULL);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"每 1 秒");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"每 2 秒");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"每 5 秒");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"每 10 秒");
        SendMessageW(cb, CB_SETCURSEL, 1, 0);

        CreateWindowExW(0, L"BUTTON", L"● 开始录制", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        132, 60, 100, 26, h, (HMENU)ID_REC_START, g_inst, NULL);
        CreateWindowExW(0, L"BUTTON", L"收起", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        240, 60, 72, 26, h, (HMENU)ID_REC_FOLD, g_inst, NULL);

        CreateWindowExW(0, L"STATIC", L"输出到 SAI 目录 timelapse\\",
                        WS_CHILD | WS_VISIBLE, 12, 92, 300, 18, h, (HMENU)160, g_inst, NULL);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_REC_START) {
            if (g_recording) StopRec();
            else StartRec();
            SetDlgItemTextW(h, ID_REC_START, g_recording ? L"■ 停止并保存" : L"● 开始录制");
            return 0;
        }
        if (id == ID_REC_FOLD) {
            RECT r;
            GetWindowRect(h, &r);
            int w = r.right - r.left;
            g_recFolded = !g_recFolded;
            SetWindowPos(h, NULL, r.left, r.top, w, g_recFolded ? 28 : 122,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(GetDlgItem(h, 150), g_recFolded ? SW_HIDE : SW_SHOW);
            ShowWindow(GetDlgItem(h, ID_REC_INT), g_recFolded ? SW_HIDE : SW_SHOW);
            ShowWindow(GetDlgItem(h, ID_REC_START), g_recFolded ? SW_HIDE : SW_SHOW);
            ShowWindow(GetDlgItem(h, 160), g_recFolded ? SW_HIDE : SW_SHOW);
            SetDlgItemTextW(h, ID_REC_FOLD, g_recFolded ? L"展开" : L"收起");
            return 0;
        }
        if (id == ID_REC_INT) {
            int sel = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
            const int iv[4] = { 1, 2, 5, 10 };
            if (sel >= 0 && sel < 4) g_recInterval = iv[sel];
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_recording) {
            if (MessageBoxW(h, L"正在录制，先停止并保存？", L"缩时摄影",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) StopRec();
            else return 0;
        }
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void OpenRecWindow(void) {
    if (g_recWnd) { ShowWindow(g_recWnd, SW_SHOW); SetForegroundWindow(g_recWnd); return; }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = RecProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SaiModRec";
    RegisterClassExW(&wc);

    RECT wr;
    GetWindowRect(g_sai, &wr);
    g_recWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"SaiModRec", L"缩时摄影",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        wr.left + 20, wr.bottom - 160, 332, 122, NULL, NULL, g_inst, NULL);
    if (g_recWnd) ShowWindow(g_recWnd, SW_SHOW);
}

static BOOL g_follow = TRUE;      // 用户拖动过面板就停掉自动跟随

static void Follow(void) {
    if (!g_follow) return;
    if (!g_sai) return;
    RECT r; GetWindowRect(g_sai, &r);
    RECT mr; GetWindowRect(g_main, &mr);
    int mw = mr.right - mr.left, mh = mr.bottom - mr.top;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = r.right + 8;
    if (x + mw > wa.right) x = r.left - mw - 8;
    if (x < wa.left) x = wa.left;
    int y = r.top;
    if (y + mh > wa.bottom) y = wa.bottom - mh;
    if (y < wa.top) y = wa.top;
    SetWindowPos(g_main, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

static HWND MakeBtn(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           x, y, w, h, parent, (HMENU)(INT_PTR)id, g_inst, NULL);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        CreateWindowExW(0, L"STATIC", L"模式", WS_CHILD | WS_VISIBLE,
                        10, 8, 60, 18, h, NULL, g_inst, NULL);
        HWND cb = CreateWindowExW(0, L"COMBOBOX", NULL,
                         WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                         70, 6, 370, 220, h, (HMENU)ID_MODE, g_inst, NULL);
        const wchar_t* modes[] = { L"向前推进", L"膨胀", L"收缩", L"顺时针", L"逆时针", L"平滑", L"重建" };
        for (int i = 0; i < 7; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)modes[i]);
        SendMessageW(cb, CB_SETCURSEL, 0, 0);

        CreateWindowExW(0, L"STATIC", L"笔刷", WS_CHILD | WS_VISIBLE,
                        10, 38, 60, 18, h, NULL, g_inst, NULL);
        HWND tb1 = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
                         WS_CHILD | WS_VISIBLE | TBS_HORZ,
                         70, 34, 370, 30, h, (HMENU)ID_SIZE, g_inst, NULL);
        SendMessageW(tb1, TBM_SETRANGE, TRUE, MAKELONG(10, 400));
        SendMessageW(tb1, TBM_SETPOS, TRUE, g_brush);

        CreateWindowExW(0, L"STATIC", L"强度", WS_CHILD | WS_VISIBLE,
                        10, 68, 60, 18, h, NULL, g_inst, NULL);
        HWND tb2 = CreateWindowExW(0, TRACKBAR_CLASSW, NULL,
                         WS_CHILD | WS_VISIBLE | TBS_HORZ,
                         70, 64, 370, 30, h, (HMENU)ID_STR, g_inst, NULL);
        SendMessageW(tb2, TBM_SETRANGE, TRUE, MAKELONG(1, 100));
        SendMessageW(tb2, TBM_SETPOS, TRUE, g_strength);

        MakeBtn(h, L"① 框选", ID_CAPTURE, 8, 100, 95, 30);
        MakeBtn(h, L"② 剪贴板", ID_CLIPCAP, 108, 100, 95, 30);
        MakeBtn(h, L"④ 写回", ID_WRITEBACK, 208, 100, 95, 30);
        MakeBtn(h, L"● 缩时", ID_REC, 308, 100, 130, 30);

        MakeBtn(h, L"撤销", ID_UNDO, 10, 136, 80, 28);
        MakeBtn(h, L"重置液化", ID_RESET, 96, 136, 80, 28);
        MakeBtn(h, L"放大绘制", ID_BIG, 182, 136, 120, 28);
        MakeBtn(h, L"贴回 SAI", ID_PIN, 308, 136, 130, 28);

        CreateWindowExW(0, L"STATIC", L"渐变映射（拖色标 = 移位置，双击 = 改色，右键 = 删除）",
                         WS_CHILD | WS_VISIBLE, 10, 172, 430, 18, h, NULL, g_inst, NULL);
        const wchar_t* names[] = { L"紫橙", L"蓝青", L"热成像", L"胶片", L"蓝黄" };
        for (int i = 0; i < 5; i++)
            MakeBtn(h, names[i], ID_GMAP + i, 10 + i * 88, 192, 84, 26);

        g_gradBar = CreateWindowExW(0, L"SaiModGrad", NULL, WS_CHILD | WS_VISIBLE,
                        10, 224, 430, 40, h, (HMENU)ID_GRAD, g_inst, NULL);

        MakeBtn(h, L"改颜色", ID_GSEL, 10, 270, 210, 26);
        MakeBtn(h, L"还原原图", ID_GRESTORE, 230, 270, 210, 26);

        g_canvas = CreateWindowExW(0, L"SaiModCanvas", NULL,
                         WS_CHILD | WS_VISIBLE,
                         10, 304, 430, 340, h, (HMENU)ID_CANVAS, g_inst, NULL);

        g_btnSave = MakeBtn(h, L"① 记住形变", ID_SAVEDISP, 10, 650, 130, 28);
        g_btnApply = MakeBtn(h, L"② 套用到当前图", ID_APPLYDISP, 148, 650, 160, 28);
        g_lblState = CreateWindowExW(0, L"STATIC", L"（未记住形变）", WS_CHILD | WS_VISIBLE,
                                    316, 656, 140, 18, h, NULL, g_inst, NULL);

        SetTimer(h, 1, 250, NULL);
        return 0;
    }
    case WM_APP + 22: {                 // F9：进入校准，下一次点击记为液化笔刷
        g_calibrate = TRUE;
        Log(L"calibrate mode ON");
        SetWindowTextW(g_main, L"SAI 液化 / 渐变映射  —— 请在 SAI 里点一下你的液化笔刷");
        return 0;
    }
    case WM_APP + 23: {                 // F10：退出液化 + 确认
        if (!g_bigWnd) return 0;
        int r = MessageBoxW(g_main,
            L"结束这次液化？\n\n「是」= 把结果写回 SAI\n「否」= 放弃，不写回",
            L"退出液化", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
        if (r == IDYES) WriteBack();
        SendMessageW(g_bigWnd, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_APP + 20: {                 // 触发键：唤起液化
        if (g_bigWnd) { SetForegroundWindow(g_bigWnd); return 0; }
        CaptureFromClipboard();
        if (g_src) StartBig();
        return 0;
    }
    case WM_APP + 21: {                 // 切到别的笔刷：确认
        if (!g_bigWnd) return 0;
        int r = MessageBoxW(g_main,
            L"检测到你切到了别的笔刷。\n\n「是」= 把液化结果写回 SAI\n「否」= 放弃这次液化",
            L"液化确认", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST);
        if (r == IDYES) WriteBack();
        SendMessageW(g_bigWnd, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_TIMER: {
        static int tick = 0;
        // 跟随 SAI 最小化：只动主面板，液化/框选进行中不干预
        {
            static BOOL s_min = FALSE;
            BOOL min = (g_sai && IsWindow(g_sai)) ? IsIconic(g_sai) : FALSE;
            if (min != s_min) {
                s_min = min;
                if (!g_bigWnd && !g_selWnd) {
                    ShowWindow(g_main, min ? SW_MINIMIZE : SW_RESTORE);
                    Log(L"[follow] panel minimize=%d", min);
                }
            }
        }
        if (++tick % 8 == 0 && g_sai) {
            EnumChildWindows(g_sai, SubAll, 0);
            EnumWindows(HookTopEnum, 0);
        }
        Follow();
        return 0;
    }
    case WM_KEYDOWN:
        if (w == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) { UndoOnce(); return 0; }
        return 0;
    case WM_ENTERSIZEMOVE: g_follow = FALSE; return 0;   // 用户开始拖动 → 停止跟随
    case WM_HOTKEY:
        if (w == 1) CaptureFromClipboard();
        else if (w == 2) WriteBack();
        else if (w == 3) SendMessageW(h, WM_COMMAND, ID_UNDO, 0);
        return 0;
    case WM_APP + 10: DoCapture(g_selRect); return 0;
    case WM_HSCROLL: {
        HWND src = (HWND)l;
        if ((HWND)w == GetDlgItem(h, ID_SIZE)) g_brush = (int)SendMessageW(src, TBM_GETPOS, 0, 0);
        if ((HWND)w == GetDlgItem(h, ID_STR))  g_strength = (int)SendMessageW(src, TBM_GETPOS, 0, 0);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_CAPTURE) { StartSelect(); return 0; }
        if (id == ID_REC) { OpenRecWindow(); return 0; }
        if (id == ID_BIG) { StartBig(); return 0; }
        if (id == ID_PIN) { g_follow = TRUE; Follow(); return 0; }
        if (id == ID_GSEL) { PickStopColor(SelIndex()); return 0; }
        if (id == ID_GRESTORE) { RestoreOriginal(); return 0; }
        if (id == ID_CLIPCAP) { CaptureFromClipboard(); return 0; }
        if (id == ID_WRITEBACK) { WriteBack(); return 0; }
        if (id == ID_SAVEDISP) { SaveDispField(); return 0; }
        if (id == ID_APPLYDISP) { ApplyDispField(); return 0; }
        if (id == ID_UNDO) { UndoOnce(); return 0; }
        if (id == ID_RESET) {
            if (g_disp) { memset(g_disp, 0, (size_t)g_w * g_h * 2 * sizeof(float)); RenderAll(); InvalidateRect(g_canvas, NULL, FALSE); }
            return 0;
        }
        if (id >= ID_GMAP && id <= ID_GMAP + 4) { ApplyGradientMap(id - ID_GMAP); return 0; }
        return 0;
    }
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    case WM_DESTROY: KillTimer(h, 1); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------- 入口 ----------------// 自检线程：DLL 目录下有 autocap.txt 时，注入后 1.2 秒自动抓一次并写日志
static DWORD WINAPI AutoCapProc(LPVOID) {
    Sleep(1200);
    if (!g_sai) return 0;
    RECT cr; GetClientRect(g_sai, &cr);
    POINT a = { cr.left, cr.top }, b = { cr.right, cr.bottom };
    ClientToScreen(g_sai, &a);
    ClientToScreen(g_sai, &b);
    RECT sel = { a.x, a.y, b.x, b.y };
    DoCapture(sel);
    return 0;
}

static DWORD WINAPI Entry(LPVOID) {
    // 等 SAI 主窗口就绪
    DWORD pid = GetCurrentProcessId();
    for (int i = 0; i < 100 && !g_sai; i++) {
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            DWORD p = 0; GetWindowThreadProcessId(h, &p);
            if (p != GetCurrentProcessId()) return TRUE;
            if (!IsWindowVisible(h)) return TRUE;
            if (GetWindow(h, GW_OWNER)) return TRUE;
            RECT r; GetWindowRect(h, &r);
            if (r.right - r.left < 300) return TRUE;
            *(HWND*)lp = h;
            return FALSE;
        }, (LPARAM)&g_sai);
        if (!g_sai) Sleep(100);
    }
    if (!g_sai) return 0;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SaiModPanel";
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);

    WNDCLASSEXW cc = { sizeof(cc) };
    cc.lpfnWndProc = CanvasProc;
    cc.hInstance = g_inst;
    cc.hCursor = LoadCursor(NULL, IDC_CROSS);
    cc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    cc.lpszClassName = L"SaiModCanvas";
    RegisterClassExW(&cc);

    WNDCLASSEXW gc = { sizeof(gc) };
    gc.lpfnWndProc = GradProc;
    gc.hInstance = g_inst;
    gc.hCursor = LoadCursor(NULL, IDC_HAND);
    gc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    gc.lpszClassName = L"SaiModGrad";
    gc.style = CS_DBLCLKS;
    RegisterClassExW(&gc);

    g_main = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"SaiModPanel", L"SAI 液化 / 渐变映射",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        100, 100, 462, 720, NULL, NULL, g_inst, NULL);
    if (!g_main) return 0;

    PresetToStops(0);

    // 全局热键（SAI 在前台也能用）
    RegisterHotKey(g_main, 1, MOD_CONTROL | MOD_ALT, 'G');   // 抓取
    RegisterHotKey(g_main, 2, MOD_CONTROL | MOD_ALT, 'A');   // 写回
    RegisterHotKey(g_main, 3, MOD_CONTROL | MOD_ALT, 'Z');   // 撤销

    ShowWindow(g_main, SW_SHOW);
    Follow();

    LoadLiquifyConfig();
    CreateThread(NULL, 0, KeyWatch, NULL, 0, NULL);
    {
        wchar_t cls[64] = { 0 };
        GetClassNameW(g_sai, cls, 64);
        Log(L"g_sai=%p cls=%s", g_sai, cls);
        EnumWindows(HookTopEnum, 0);
    }
    Log(L"liquify: trigger=%d exits=%d", g_trigKey, g_exitN);
    EnumChildWindows(g_sai, DumpChild, 0);

    {
        wchar_t flag[MAX_PATH] = { 0 };
        GetModuleFileNameW(g_inst, flag, MAX_PATH);
        wchar_t* slash = wcsrchr(flag, L'\\');
        if (slash) *(slash + 1) = 0;
        wcscat_s(flag, L"autocap.txt");
        if (GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES) {
            Log(L"autocap enabled");
            CreateThread(NULL, 0, AutoCapProc, NULL, 0, NULL);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = h;
        DisableThreadLibraryCalls(h);
        CreateThread(NULL, 0, Entry, NULL, 0, NULL);
    }
    return TRUE;
}
