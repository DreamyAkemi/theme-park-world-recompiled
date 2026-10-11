/*
 * tp_debug: launch tp.exe under the Win32 debug API and log what it does.
 * Logs OutputDebugString text (the game prints "FAILED, exiting ..." there), DLL loads,
 * exceptions, and -- via a breakpoint on ExitProcess -- the caller address and arguments
 * of the exit. Writes tp_debug.log next to the exe. Build (32-bit): see tools/README.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static FILE *lg;
static void L(const char *fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    fputs(b, stdout); if (lg) { fputs(b, lg); fflush(lg); }
}

int main(int argc, char **argv) {
    const char *exe = argc > 1 ? argv[1] : "tp.exe";
    lg = fopen("tp_debug.log", "w");
    STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &si, &pi)) {
        L("CreateProcess failed: %lu\n", GetLastError()); return 1;
    }
    BYTE *bp[3] = {0}; BYTE orig[3] = {0};
    const char *names[3] = {"ExitProcess", "TerminateProcess", "ExitThread"};
    HMODULE k = GetModuleHandleA("kernel32.dll");
    for (int i = 0; i < 3; i++) bp[i] = (BYTE *)GetProcAddress(k, names[i]);
    for (int i = 0; i < 3; i++) if (bp[i]) {
        BYTE cc = 0xCC; SIZE_T n;
        ReadProcessMemory(pi.hProcess, bp[i], &orig[i], 1, &n);
        WriteProcessMemory(pi.hProcess, bp[i], &cc, 1, &n);
        FlushInstructionCache(pi.hProcess, bp[i], 1);
    }
    DEBUG_EVENT e; int firstbp = 1;
    for (;;) {
        if (!WaitForDebugEvent(&e, INFINITE)) break;
        DWORD cont = DBG_CONTINUE;
        switch (e.dwDebugEventCode) {
        case OUTPUT_DEBUG_STRING_EVENT: {
            OUTPUT_DEBUG_STRING_INFO *o = &e.u.DebugString; char buf[512] = {0};
            SIZE_T n, len = o->nDebugStringLength < 511 ? o->nDebugStringLength : 511;
            if (!o->fUnicode && ReadProcessMemory(pi.hProcess, o->lpDebugStringData, buf, len, &n)) L("[dbgstr] %s\n", buf);
            break; }
        case LOAD_DLL_DEBUG_EVENT: {
            char p[MAX_PATH] = "?";
            if (e.u.LoadDll.hFile) GetFinalPathNameByHandleA(e.u.LoadDll.hFile, p, MAX_PATH, 0);
            L("[dll] %s\n", p); break; }
        case EXCEPTION_DEBUG_EVENT: {
            EXCEPTION_RECORD *r = &e.u.Exception.ExceptionRecord;
            BYTE *a = (BYTE *)r->ExceptionAddress; int hit = -1;
            for (int i = 0; i < 3; i++) if (bp[i] && a == bp[i] && r->ExceptionCode == EXCEPTION_BREAKPOINT) hit = i;
            if (hit >= 0) {
                CONTEXT c; c.ContextFlags = CONTEXT_FULL; HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, e.dwThreadId);
                GetThreadContext(th, &c);
                DWORD st[8] = {0}; SIZE_T n; ReadProcessMemory(pi.hProcess, (void *)c.Esp, st, sizeof st, &n);
                L("[%s] return=0x%08lx arg1=0x%08lx arg2=0x%08lx\n", names[hit], st[0], st[1], st[2]);
                DWORD s2[64] = {0}; ReadProcessMemory(pi.hProcess, (void *)c.Ebp, s2, sizeof s2, &n);
                L("   esp=0x%08lx ebp=0x%08lx stack:", c.Esp, c.Ebp);
                DWORD s3[48] = {0}; ReadProcessMemory(pi.hProcess, (void *)c.Esp, s3, sizeof s3, &n);
                for (int i = 0; i < 48; i++) if (s3[i] >= 0x401000 && s3[i] < 0xf85000) L(" 0x%08lx", s3[i]);
                L("\n");
                WriteProcessMemory(pi.hProcess, bp[hit], &orig[hit], 1, &n);
                FlushInstructionCache(pi.hProcess, bp[hit], 1);
                c.Eip = (DWORD)(DWORD_PTR)bp[hit]; SetThreadContext(th, &c); CloseHandle(th);
            } else if (r->ExceptionCode == EXCEPTION_BREAKPOINT && firstbp) { firstbp = 0;
            } else {
                L("[exception] code=0x%08lx addr=%p first=%lu\n", r->ExceptionCode, r->ExceptionAddress, e.u.Exception.dwFirstChance);
                if (r->ExceptionCode != EXCEPTION_BREAKPOINT) cont = DBG_EXCEPTION_NOT_HANDLED;
            }
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
