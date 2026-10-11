/*
 * tp_debug: launch tp.exe under the Win32 debug API and log what it does.
 * Sets int3 breakpoints on a list of API functions once their DLL is loaded, and logs each call
 * (file names, window creation, DirectDraw, message boxes, process exit with the caller address in tp.exe).
 * Also logs DLL loads, OutputDebugString text and exceptions to tp_debug.log next to the exe.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static FILE *lg;
static void L(const char *fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fputs(b, stdout); if (lg) { fputs(b, lg); fflush(lg); }
}

typedef struct { const char *dll, *fn; int strarg; /* 1-based stack arg that is a char*, 0 none */ int str2; DWORD addr; BYTE orig; int on; } Hook;
static Hook H[] = {
    {"ntdll.dll", "RtlExitUserProcess", 0, 0},
    {"ntdll.dll", "NtTerminateProcess", 0, 0},
    {"kernelbase.dll", "ExitProcess", 0, 0},
    {"kernelbase.dll", "TerminateProcess", 0, 0},
    {"kernelbase.dll", "CreateFileA", 1, 0},
    {"kernelbase.dll", "FindFirstFileA", 1, 0},
    {"kernelbase.dll", "GetFileAttributesA", 1, 0},
    {"kernelbase.dll", "LoadLibraryA", 1, 0},
    {"kernelbase.dll", "GetDriveTypeA", 1, 0},
    {"kernelbase.dll", "GetVolumeInformationA", 1, 0},
    {"kernelbase.dll", "CreateMutexA", 3, 0},
    {"kernelbase.dll", "RegOpenKeyExA", 2, 0},
    {"kernelbase.dll", "RegOpenKeyA", 2, 0},
    {"kernelbase.dll", "RegCreateKeyExA", 2, 0},
    {"kernelbase.dll", "RegQueryValueExA", 2, 0},
    {"kernelbase.dll", "RegSetValueExA", 2, 0},
    {"user32.dll", "MessageBoxA", 2, 3},
    {"user32.dll", "CreateWindowExA", 2, 3},
    {"user32.dll", "PostQuitMessage", 0, 0},
    {"user32.dll", "DestroyWindow", 0, 0},
    {"user32.dll", "ChangeDisplaySettingsA", 0, 0},
    {"ddraw.dll", "DirectDrawCreate", 0, 0},
    {"ddraw.dll", "DirectDrawEnumerateA", 0, 0},
    {"ddraw.dll", "DirectDrawCreateEx", 0, 0},
};
#define NH (sizeof H / sizeof H[0])

static HANDLE hp;
static DWORD post_addr, post_type, post_data, post_cb; static BYTE post_orig; static int post_on;

static char *rdstr(DWORD a, char *buf, int n) {
    SIZE_T r = 0; memset(buf, 0, n);
    if (!a || !ReadProcessMemory(hp, (void *)(DWORD_PTR)a, buf, n - 1, &r)) strcpy(buf, "?");
    return buf;
}
static void arm(Hook *h) { BYTE cc = 0xCC; SIZE_T n; WriteProcessMemory(hp, (void *)(DWORD_PTR)h->addr, &cc, 1, &n); FlushInstructionCache(hp, (void *)(DWORD_PTR)h->addr, 1); h->on = 1; }
static void disarm(Hook *h) { SIZE_T n; WriteProcessMemory(hp, (void *)(DWORD_PTR)h->addr, &h->orig, 1, &n); FlushInstructionCache(hp, (void *)(DWORD_PTR)h->addr, 1); h->on = 0; }

static void install_for(const char *file, DWORD remote_base) {
    HMODULE mine = LoadLibraryA(file);
    if (!mine) return;
    for (size_t i = 0; i < NH; i++) {
        if (H[i].addr || _stricmp(H[i].dll, file)) continue;
        FARPROC f = GetProcAddress(mine, H[i].fn);
        if (!f) continue;
        H[i].addr = remote_base + (DWORD)((DWORD_PTR)f - (DWORD_PTR)mine);
        SIZE_T n; if (!ReadProcessMemory(hp, (void *)(DWORD_PTR)H[i].addr, &H[i].orig, 1, &n)) { H[i].addr = 0; continue; }
        arm(&H[i]);
    }
}

int main(int argc, char **argv) {
    const char *exe = argc > 1 ? argv[1] : "tp.exe";
    lg = fopen("tp_debug.log", "w");
    STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi)) {
        L("CreateProcess failed: %lu\n", GetLastError()); return 1;
    }
    hp = pi.hProcess;
    DEBUG_EVENT e; int stepping = -1;
    for (;;) {
        if (!WaitForDebugEvent(&e, INFINITE)) break;
        DWORD cont = DBG_CONTINUE;
        switch (e.dwDebugEventCode) {
        case OUTPUT_DEBUG_STRING_EVENT: {
            OUTPUT_DEBUG_STRING_INFO *o = &e.u.DebugString; char buf[512] = {0};
            SIZE_T n, len = o->nDebugStringLength < 511 ? o->nDebugStringLength : 511;
            if (!o->fUnicode && ReadProcessMemory(hp, o->lpDebugStringData, buf, len, &n)) L("[dbgstr] %s\n", buf);
            break; }
        case LOAD_DLL_DEBUG_EVENT: {
            char p[MAX_PATH] = "?";
            if (e.u.LoadDll.hFile) GetFinalPathNameByHandleA(e.u.LoadDll.hFile, p, MAX_PATH, 0);
            char *base = strrchr(p, '\\'); base = base ? base + 1 : p;
            L("[dll] %s @%p\n", p, e.u.LoadDll.lpBaseOfDll);
            install_for(base, (DWORD)(DWORD_PTR)e.u.LoadDll.lpBaseOfDll);
            break; }
        case CREATE_PROCESS_DEBUG_EVENT:
            install_for("ntdll.dll", 0); /* placeholder; real base filled when ntdll load event arrives */
            for (size_t i = 0; i < NH; i++) if (!strcmp(H[i].dll, "ntdll.dll") && H[i].addr) { H[i].addr = 0; H[i].on = 0; }
            break;
        case EXCEPTION_DEBUG_EVENT: {
            EXCEPTION_RECORD *r = &e.u.Exception.ExceptionRecord;
            HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, e.dwThreadId);
            CONTEXT c; c.ContextFlags = CONTEXT_FULL; GetThreadContext(th, &c);
            int hit = -1;
            if (r->ExceptionCode == EXCEPTION_BREAKPOINT)
                for (size_t i = 0; i < NH; i++) if (H[i].on && (DWORD)(DWORD_PTR)r->ExceptionAddress == H[i].addr) hit = (int)i;
            if (hit < 0 && post_on && r->ExceptionCode == EXCEPTION_BREAKPOINT && (DWORD)(DWORD_PTR)r->ExceptionAddress == post_addr) {
                SIZE_T n; DWORD ty = 0, cb = 0; BYTE dat[64] = {0};
                if (post_type) ReadProcessMemory(hp, (void *)(DWORD_PTR)post_type, &ty, 4, &n);
                if (post_cb) ReadProcessMemory(hp, (void *)(DWORD_PTR)post_cb, &cb, 4, &n);
                if (post_data) ReadProcessMemory(hp, (void *)(DWORD_PTR)post_data, dat, cb < 63 ? cb : 63, &n);
                char hex[200] = ""; for (DWORD i = 0; i < (cb < 24 ? cb : 24); i++) sprintf(hex + strlen(hex), "%02x ", dat[i]);
                L("      -> result=%lu type=%lu cb=%lu data=[%s] \"%.40s\"\n", c.Eax, ty, cb, hex, dat);
                WriteProcessMemory(hp, (void *)(DWORD_PTR)post_addr, &post_orig, 1, &n);
                FlushInstructionCache(hp, (void *)(DWORD_PTR)post_addr, 1);
                c.Eip = post_addr; SetThreadContext(th, &c); post_on = 0;
            } else if (hit >= 0) {
                DWORD st[8] = {0}; SIZE_T n; ReadProcessMemory(hp, (void *)(DWORD_PTR)c.Esp, st, sizeof st, &n);
                char s1[260] = "", s2[260] = "";
                if (H[hit].strarg) rdstr(st[H[hit].strarg], s1, sizeof s1);
                if (H[hit].str2) rdstr(st[H[hit].str2], s2, sizeof s2);
                L("[%s] ret=0x%08lx a1=0x%lx a2=0x%lx%s%s%s%s\n", H[hit].fn, st[0], st[1], st[2],
                  s1[0] ? " str=\"" : "", s1, s1[0] ? "\"" : "", "");
                if (s2[0]) L("      str2=\"%s\"\n", s2);
                if (!strncmp(H[hit].fn, "Rtl", 3) || !strncmp(H[hit].fn, "Nt", 2) || !strncmp(H[hit].fn, "Exit", 4) || !strncmp(H[hit].fn, "Term", 4)) {
                    DWORD s3[64] = {0}; ReadProcessMemory(hp, (void *)(DWORD_PTR)c.Esp, s3, sizeof s3, &n);
                    L("      tp.exe addresses on stack:");
                    for (int i = 0; i < 64; i++) if (s3[i] >= 0x401000 && s3[i] < 0xf85000) L(" 0x%08lx", s3[i]);
                    L("\n");
                }
                if (!strcmp(H[hit].fn, "RegQueryValueExA")) {
                    DWORD s3[96] = {0}; ReadProcessMemory(hp, (void *)(DWORD_PTR)c.Esp, s3, sizeof s3, &n);
                    L("      chain:"); for (int i = 0; i < 96; i++) if (s3[i] >= 0x401000 && s3[i] < 0xf85000) L(" 0x%08lx", s3[i]);
                    DWORD cbv = 0; ReadProcessMemory(hp, (void *)(DWORD_PTR)st[6], &cbv, 4, &n);
                    L("\n      bufsize=%lu\n", cbv);
                    post_addr = st[0]; post_type = st[4]; post_data = st[5]; post_cb = st[6];
                    if (!post_on) { ReadProcessMemory(hp, (void *)(DWORD_PTR)post_addr, &post_orig, 1, &n);
                        BYTE cc = 0xCC; WriteProcessMemory(hp, (void *)(DWORD_PTR)post_addr, &cc, 1, &n);
                        FlushInstructionCache(hp, (void *)(DWORD_PTR)post_addr, 1); post_on = 1; }
                }
                disarm(&H[hit]);
                c.Eip = H[hit].addr; c.EFlags |= 0x100; /* single step, then re-arm */
                SetThreadContext(th, &c); stepping = hit;
            } else if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && stepping >= 0) {
                arm(&H[stepping]); stepping = -1;
                c.EFlags &= ~0x100u; SetThreadContext(th, &c);
            } else if (r->ExceptionCode == EXCEPTION_BREAKPOINT) {
                /* initial loader breakpoint */
            } else {
                L("[exception] code=0x%08lx addr=%p first=%lu", r->ExceptionCode, r->ExceptionAddress, e.u.Exception.dwFirstChance);
                if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
                    L(" %s 0x%08lx", r->ExceptionInformation[0] == 0 ? "read" : r->ExceptionInformation[0] == 1 ? "write" : "exec", (unsigned long)r->ExceptionInformation[1]);
                L("\n      eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx\n",
                  c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Ebp, c.Esp);
                {
                    DWORD sk[256] = {0}; SIZE_T n;
                    ReadProcessMemory(hp, (void *)(DWORD_PTR)c.Esp, sk, sizeof sk, &n);
                    L("      stack:"); for (int i = 0; i < 12; i++) L(" %08lx", sk[i]);
                    L("\n      tp.exe return addresses:");
                    for (int i = 0; i < 256; i++) if (sk[i] >= 0x401000 && sk[i] < 0xf85000) L(" 0x%08lx", sk[i]);
                    L("\n");
                    DWORD fr[2] = {0}, ebp = c.Ebp; L("      frame chain:");
                    for (int d = 0; d < 24 && ebp; d++) {
                        if (!ReadProcessMemory(hp, (void *)(DWORD_PTR)ebp, fr, 8, &n) || n < 8) break;
                        L(" 0x%08lx", fr[1]); if (fr[0] <= ebp) break; ebp = fr[0];
                    }
                    L("\n");
                }
                cont = DBG_EXCEPTION_NOT_HANDLED;
            }
            CloseHandle(th);
            break; }
        case EXIT_PROCESS_DEBUG_EVENT:
            L("[exit] code=%lu (0x%lx)\n", e.u.ExitProcess.dwExitCode, e.u.ExitProcess.dwExitCode);
            ContinueDebugEvent(e.dwProcessId, e.dwThreadId, DBG_CONTINUE);
            return 0;
        }
        ContinueDebugEvent(e.dwProcessId, e.dwThreadId, cont);
    }
    return 0;
}
