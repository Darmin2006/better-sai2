// SAI2启动器.exe - 拉起 SAI2 并自动把 saimod.dll 注进去
// 放在 saimod 文件夹里，sai_path.txt 可覆盖 SAI 路径
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdarg.h>

static void Printf(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        DWORD w = 0; WriteConsoleW(h, buf, (DWORD)wcslen(buf), &w, NULL);
    } else {
        char u8[4096];
        int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8), NULL, NULL);
        if (n > 0) { DWORD w = 0; WriteFile(h, u8, (DWORD)(n - 1), &w, NULL); }
    }
}
#define wprintf Printf

static BOOL Is64(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    BOOL wow = FALSE, ok = FALSE;
    typedef BOOL(WINAPI* F)(HANDLE, PBOOL);
    F fn = (F)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    if (fn && fn(h, &wow)) ok = !wow;
    CloseHandle(h);
    return ok;
}

struct Ctx { DWORD pid; HWND hwnd; };
static BOOL CALLBACK ByPid(HWND h, LPARAM lp) {
    Ctx* c = (Ctx*)lp;
    DWORD p = 0; GetWindowThreadProcessId(h, &p);
    if (p != c->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; GetWindowRect(h, &r);
    if (r.right - r.left < 200) return TRUE;
    c->hwnd = h; return FALSE;
}
static BOOL CALLBACK PanelWin(HWND h, LPARAM lp) {
    Ctx* c = (Ctx*)lp;
    DWORD p = 0; GetWindowThreadProcessId(h, &p);
    if (p != c->pid) return TRUE;
    wchar_t cls[64] = { 0 };
    GetClassNameW(h, cls, 64);
    if (_wcsicmp(cls, L"SaiModPanel") == 0) { c->hwnd = h; return FALSE; }
    return TRUE;
}

// 找 64 位且带窗口的 sai2.exe
static BOOL FindSai(DWORD* pid, HWND* win) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD bestPid = 0; HWND bestWin = NULL;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"sai2.exe") != 0) continue;
            if (!Is64(pe.th32ProcessID)) continue;
            Ctx c = { pe.th32ProcessID, NULL };
            EnumWindows(ByPid, (LPARAM)&c);
            if (c.hwnd) { bestPid = pe.th32ProcessID; bestWin = c.hwnd; break; }
            if (!bestPid) bestPid = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!bestPid) return FALSE;
    *pid = bestPid; *win = bestWin;
    return TRUE;
}

static BOOL Inject(DWORD pid, const wchar_t* dll) {
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                           FALSE, pid);
    if (!h) return FALSE;
    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(h, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(h); return FALSE; }
    BOOL ok = FALSE;
    if (WriteProcessMemory(h, remote, (void*)dll, bytes, NULL)) {
        LPTHREAD_START_ROUTINE f = (LPTHREAD_START_ROUTINE)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE th = CreateRemoteThread(h, NULL, 0, f, remote, 0, NULL);
        if (th) {
            WaitForSingleObject(th, 15000);
            DWORD ec = 0; GetExitCodeThread(th, &ec);
            ok = (ec != 0);
            CloseHandle(th);
        }
    }
    VirtualFreeEx(h, remote, 0, MEM_RELEASE);
    CloseHandle(h);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
    wchar_t dir[MAX_PATH]; wcscpy_s(dir, self);
    wchar_t* p = wcsrchr(dir, L'\\'); if (p) *p = 0;

    wchar_t dll[MAX_PATH]; swprintf_s(dll, L"%s\\saimod.dll", dir);

    // SAI 路径：优先 sai_path.txt
    wchar_t sai[MAX_PATH];
    swprintf_s(sai, L"%s\\sai2.exe", dir);
    wchar_t cfg[MAX_PATH]; swprintf_s(cfg, L"%s\\sai_path.txt", dir);
    if (GetFileAttributesW(cfg) != INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(cfg, L"r, ccs=UTF-8");
        if (f) {
            if (fgetws(sai, MAX_PATH, f)) {
                size_t n = wcslen(sai);
                while (n && (sai[n - 1] == L'\n' || sai[n - 1] == L'\r' || sai[n - 1] == L' ')) sai[--n] = 0;
            }
            fclose(f);
        }
    }
    if (argc >= 2) wcscpy_s(sai, argv[1]);

    wprintf(L"[*] SAI: %s\n", sai);
    if (GetFileAttributesW(sai) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[x] 找不到 SAI: %s\n", sai);
        MessageBoxW(NULL, sai, L"找不到 SAI 主程序", MB_ICONERROR);
        return 1;
    }

    // 已经开着就直接注入
    DWORD pid = 0; HWND win = NULL;
    if (!FindSai(&pid, &win)) {
        wprintf(L"[1/3] 启动 SAI ...\n");
        wchar_t wd[MAX_PATH]; wcscpy_s(wd, sai);
        wchar_t* q = wcsrchr(wd, L'\\'); if (q) *q = 0;

        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        wchar_t cmd[MAX_PATH + 8]; swprintf_s(cmd, L"\"%s\"", sai);
        if (!CreateProcessW(sai, cmd, NULL, NULL, FALSE, 0, NULL, wd, &si, &pi)) {
            wprintf(L"[x] 启动失败 (%lu)\n", GetLastError());
            MessageBoxW(NULL, L"启动 SAI 失败", L"SAI2启动器", MB_ICONERROR);
            return 1;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        wprintf(L"[1/3] SAI 已在运行 (PID=%lu)，直接注入\n", pid);
    }

    wprintf(L"[2/3] 等待 SAI 窗口就绪 ...\n");
    BOOL got = FALSE;
    for (int i = 0; i < 100; i++) {          // 最多 20 秒
        if (FindSai(&pid, &win)) { got = TRUE; if (win) break; }
        Sleep(200);
    }
    if (!got) {
        wprintf(L"[x] 等不到 64 位的 sai2.exe\n");
        MessageBoxW(NULL, L"等不到 SAI 进程，可能是启动失败或被拦截。", L"SAI2启动器", MB_ICONERROR);
        return 1;
    }
    Sleep(600);

    wprintf(L"[3/3] 注入 saimod.dll 到 PID=%lu ...\n", pid);
    if (!Inject(pid, dll)) {
        wprintf(L"[x] 注入失败\n");
        MessageBoxW(NULL, L"注入失败。可能被杀软拦截，或 SAI 版本不兼容。", L"SAI2启动器", MB_ICONERROR);
        return 1;
    }

    Ctx pc = { pid, NULL };
    for (int i = 0; i < 30 && !pc.hwnd; i++) { EnumWindows(PanelWin, (LPARAM)&pc); if (!pc.hwnd) Sleep(100); }
    if (pc.hwnd) wprintf(L"[+] 完成，面板已出现\n");
    else wprintf(L"[!] 注入成功但面板没找到\n");

    Sleep(2500);
    return 0;
}
