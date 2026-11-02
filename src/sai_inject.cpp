// sai_inject.exe - 把 saimod.dll 注入到正确的 sai2.exe（必须 64 位那个）
// 用法: sai_inject.exe <saimod.dll 完整路径>
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <wchar.h>
#include <stdarg.h>

// 控制台直接走 WriteConsoleW（不受 chcp 影响），重定向走 UTF-8 字节
static void Printf(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        DWORD written = 0;
        WriteConsoleW(h, buf, (DWORD)wcslen(buf), &written, NULL);
    } else {
        char utf8[4096];
        int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8), NULL, NULL);
        if (n > 0) {
            DWORD written = 0;
            WriteFile(h, utf8, (DWORD)(n - 1), &written, NULL);
        }
    }
}
#define wprintf Printf

typedef BOOL(WINAPI* PFN_IsWow64Process)(HANDLE, PBOOL);

struct Cand {
    DWORD pid;
    BOOL  is64;      // 原生 64 位
    HWND  hwnd;      // 可见顶层窗口（可能为 NULL）
    DWORD threads;
};

static BOOL GetArch(DWORD pid, BOOL* is64) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) return FALSE;
    }
    BOOL wow = FALSE;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_IsWow64Process fn = (PFN_IsWow64Process)GetProcAddress(k32, "IsWow64Process");
    if (fn && fn(h, &wow)) {
        *is64 = !wow;
        CloseHandle(h);
        return TRUE;
    }
    CloseHandle(h);
    return FALSE;
}

struct WinCtx { DWORD pid; HWND hwnd; };
static BOOL CALLBACK WinEnum(HWND h, LPARAM lp) {
    WinCtx* c = (WinCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; GetWindowRect(h, &r);
    if (r.right - r.left < 200) return TRUE;
    c->hwnd = h;
    return FALSE;
}

int wmain(int argc, wchar_t** argv) {
    setlocale(LC_ALL, "");
    if (argc < 2) {
        wprintf(L"用法: sai_inject.exe <saimod.dll 完整路径>\n");
        return 1;
    }

    DWORD len = GetFullPathNameW(argv[1], 0, NULL, NULL);
    wchar_t* full = (wchar_t*)malloc(len * sizeof(wchar_t));
    GetFullPathNameW(argv[1], len, full, NULL);
    if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[x] 找不到 DLL: %s\n", full);
        free(full); return 1;
    }

    // 目标进程名：默认 sai2.exe，可用第二个参数覆盖（测试用）
    const wchar_t* targetName = (argc >= 3) ? argv[2] : L"sai2.exe";

    // ---- 收集所有目标进程 ----
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { wprintf(L"[x] 快照失败\n"); free(full); return 1; }

    Cand list[16]; int n = 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, targetName) != 0) continue;
            if (n >= 16) break;
            BOOL is64 = FALSE;
            if (!GetArch(pe.th32ProcessID, &is64)) continue;
            WinCtx c = { pe.th32ProcessID, NULL };
            EnumWindows(WinEnum, (LPARAM)&c);
            list[n].pid = pe.th32ProcessID;
            list[n].is64 = is64;
            list[n].hwnd = c.hwnd;
            list[n].threads = pe.cntThreads;
            n++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    if (n == 0) {
        wprintf(L"[x] 没找到 sai2.exe 进程，先把 SAI 打开\n");
        free(full); return 1;
    }

    wprintf(L"[*] 发现 %d 个 sai2.exe：\n", n);
    for (int i = 0; i < n; i++) {
        wprintf(L"    [%d] PID=%-6lu  %s  窗口=%s  线程=%lu\n",
                i, list[i].pid,
                list[i].is64 ? L"64位 (可用)" : L"32位 (外壳，不能注入)",
                list[i].hwnd ? L"有" : L"无",
                list[i].threads);
    }

    // ---- 选目标：优先 64 位 + 有窗口 ----
    int pick = -1;
    for (int i = 0; i < n; i++) if (list[i].is64 && list[i].hwnd) { pick = i; break; }
    if (pick < 0) for (int i = 0; i < n; i++) if (list[i].is64) { pick = i; break; }

    if (pick < 0) {
        wprintf(L"\n[x] 找到的 sai2.exe 全是 32 位，不能注入 64 位 DLL\n");
        wprintf(L"    请确认打开的是真正的 64 位 SAI2 主程序\n");
        free(full); return 1;
    }

    DWORD pid = list[pick].pid;
    wprintf(L"\n[*] 目标 PID = %lu (%s)\n", pid, list[pick].hwnd ? L"正好是主窗口进程" : L"无窗口，可能是子进程");

    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc) {
        DWORD e = GetLastError();
        wprintf(L"[x] OpenProcess 失败，错误码 %lu\n", e);
        if (e == 5) wprintf(L"    → 权限不足，右键 sai_inject.exe 用管理员身份运行\n");
        free(full); return 1;
    }

    SIZE_T bytes = (wcslen(full) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hProc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { wprintf(L"[x] VirtualAllocEx 失败 (%lu)\n", GetLastError()); CloseHandle(hProc); free(full); return 1; }

    if (!WriteProcessMemory(hProc, remote, full, bytes, NULL)) {
        wprintf(L"[x] WriteProcessMemory 失败 (%lu)\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE); CloseHandle(hProc); free(full); return 1;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    HANDLE th = CreateRemoteThread(hProc, NULL, 0, loadLib, remote, 0, NULL);
    if (!th) {
        wprintf(L"[x] CreateRemoteThread 失败 (%lu)\n", GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE); CloseHandle(hProc); free(full); return 1;
    }

    WaitForSingleObject(th, 15000);
    DWORD ec = 0; GetExitCodeThread(th, &ec);
    CloseHandle(th);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);

    if (ec == 0) {
        wprintf(L"[x] DLL 加载失败（LoadLibrary 返回 0）\n");
        wprintf(L"    常见原因：位数不匹配 / 被外壳校验拦了 / DLL 被杀软拦截\n");
        CloseHandle(hProc); free(full); return 1;
    }

    wprintf(L"[+] 注入成功，模块基址 = 0x%lX\n", ec);

    // ---- 验证模块真的在目标进程里（轮询 3 秒，等 LoadLibrary 完成注入）----
    BOOL found = FALSE;
    for (int t = 0; t < 30 && !found; t++) {
        Sleep(100);
        HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (ms == INVALID_HANDLE_VALUE) continue;
        MODULEENTRY32W me = { sizeof(me) };
        if (Module32FirstW(ms, &me)) {
            do { if (_wcsicmp(me.szModule, L"saimod.dll") == 0) { found = TRUE; break; } }
            while (Module32NextW(ms, &me));
        }
        CloseHandle(ms);
    }
    wprintf(found ? L"[+] 已确认 saimod.dll 在目标进程中\n" : L"[!] 模块列表里没看到 saimod.dll\n");

    // ---- 检查面板窗口是否真的建出来了 ----
    struct PanelCtx { DWORD pid; HWND hwnd; };
    static PanelCtx pc;
    pc.pid = pid; pc.hwnd = NULL;
    for (int t = 0; t < 20 && !pc.hwnd; t++) {
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            PanelCtx* c = (PanelCtx*)lp;
            DWORD p = 0; GetWindowThreadProcessId(h, &p);
            if (p != c->pid) return TRUE;
            wchar_t cls[64] = { 0 };
            GetClassNameW(h, cls, 64);
            if (_wcsicmp(cls, L"SaiModPanel") == 0) { c->hwnd = h; return FALSE; }
            return TRUE;
        }, (LPARAM)&pc);
        if (!pc.hwnd) Sleep(150);
    }
    if (pc.hwnd) wprintf(L"[+] 面板窗口已创建 hwnd = 0x%p\n", pc.hwnd);
    else wprintf(L"[!] 没找到面板窗口：DLL 加载了但窗口没建出来\n");
    wprintf(L"[+] 切回 SAI，右侧应该出现面板\n");

    CloseHandle(hProc);
    free(full);
    return 0;
}
