/*
 * tp_debug: launch tp.exe under the Win32 debug API and log what it does.
 *
 * - int3 breakpoints on a list of Windows API functions (armed once their DLL is loaded): file/registry access,
 *   window creation, DirectDraw, exits. File calls that fail are marked [FAILED].
 * - int3 breakpoints on chosen addresses inside tp.exe (see CODE_HOOKS): logs arguments and the return value.
 * - On a crash prints registers, the return addresses found on the stack and the game's world object table.
 * - Logs DLL loads and OutputDebugString text.
 * Everything goes to tp_debug.log next to the exe.
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

enum { K_PLAIN, K_FILE, K_REG, K_RET };   /* what to do when the call returns */
typedef struct {
    const char *dll, *fn;       /* dll NULL -> fixed address inside tp.exe */
    int strarg, str2;           /* 1-based stack arg that is a char*, 0 none */
    int kind;
    DWORD addr; BYTE orig; int on;
} Hook;

static Hook H[] = {
    {"ntdll.dll", "RtlExitUserProcess", 0, 0, K_PLAIN},
    {"ntdll.dll", "NtTerminateProcess", 0, 0, K_PLAIN},
    {"kernelbase.dll", "ExitProcess", 0, 0, K_PLAIN},
    {"kernelbase.dll", "TerminateProcess", 0, 0, K_PLAIN},
    {"kernelbase.dll", "CreateFileA", 1, 0, K_FILE},
    {"kernelbase.dll", "FindFirstFileA", 1, 0, K_FILE},
    {"kernelbase.dll", "GetFileAttributesA", 1, 0, K_FILE},
    {"kernelbase.dll", "LoadLibraryA", 1, 0, K_PLAIN},
    {"kernelbase.dll", "GetDriveTypeA", 1, 0, K_PLAIN},
    {"kernelbase.dll", "GetVolumeInformationA", 1, 0, K_PLAIN},
    {"kernelbase.dll", "CreateMutexA", 3, 0, K_PLAIN},
    {"kernelbase.dll", "RegOpenKeyExA", 2, 0, K_PLAIN},
    {"kernelbase.dll", "RegOpenKeyA", 2, 0, K_PLAIN},
    {"kernelbase.dll", "RegCreateKeyExA", 2, 0, K_PLAIN},
    {"kernelbase.dll", "RegQueryValueExA", 2, 0, K_REG},
    {"kernelbase.dll", "RegSetValueExA", 2, 0, K_PLAIN},
    {"user32.dll", "MessageBoxA", 2, 3, K_PLAIN},
    {"user32.dll", "CreateWindowExA", 2, 3, K_PLAIN},
    {"user32.dll", "PostQuitMessage", 0, 0, K_PLAIN},
    {"user32.dll", "DestroyWindow", 0, 0, K_PLAIN},
    {"user32.dll", "ChangeDisplaySettingsA", 0, 0, K_PLAIN},
    {"ddraw.dll", "DirectDrawCreate", 0, 0, K_PLAIN},
    {"ddraw.dll", "DirectDrawEnumerateA", 0, 0, K_PLAIN},
    {"ddraw.dll", "DirectDrawCreateEx", 0, 0, K_PLAIN},
};
/* Addresses inside the 1999-10-25 tp.exe (found by reverse engineering; see docs/findings.md). thiscall: ecx is logged too. */
static Hook CODE_HOOKS[] = {
    {NULL, "LoadLevel@407d70", 1, 0, K_RET, 0x407d70},
    {NULL, "LevelDescLoad@412f80", 1, 0, K_RET, 0x412f80},
    {NULL, "ParseEntry@413770", 0, 0, K_RET, 0x413770},
    {NULL, "LookupName@421150", 1, 0, K_RET, 0x421150},
    {NULL, "WorldInit@50bbc0", 1, 0, K_RET, 0x50bbc0},
};
#define NAPI (sizeof H / sizeof H[0])
#define NCODE (sizeof CODE_HOOKS / sizeof CODE_HOOKS[0])
#define NH_ALL (NAPI + NCODE)
static Hook *hook_at(size_t i) { return i < NAPI ? &H[i] : &CODE_HOOKS[i - NAPI]; }

static HANDLE hp;

static char *rdstr(DWORD a, char *buf, int n) {
    SIZE_T r = 0; memset(buf, 0, n);
    if (!a || !ReadProcessMemory(hp, (void *)(DWORD_PTR)a, buf, n - 1, &r)) strcpy(buf, "?");
    for (int i = 0; buf[i]; i++) if ((unsigned char)buf[i] < 32 || (unsigned char)buf[i] > 126) { buf[i] = 0; break; }
    return buf;
}
static void patch(DWORD addr, BYTE b) { SIZE_T n; WriteProcessMemory(hp, (void *)(DWORD_PTR)addr, &b, 1, &n); FlushInstructionCache(hp, (void *)(DWORD_PTR)addr, 1); }
static void arm(Hook *h) { patch(h->addr, 0xCC); h->on = 1; }
static void disarm(Hook *h) { patch(h->addr, h->orig); h->on = 0; }

/* ---- pending "on return" breakpoints ---- */
typedef struct { DWORD addr; int kind; char name[260], fn[48]; DWORD ptype, pdata, pcb; } Post;
static Post P[64]; static int np;
typedef struct { DWORD addr; BYTE orig; int on; } Armed;
static Armed A[64]; static int na;

static Armed *armed_find(DWORD addr) { for (int i = 0; i < na; i++) if (A[i].addr == addr) return &A[i]; return NULL; }
static void post_push(const Post *p) {
    if (np >= 64) return;
    Armed *a = armed_find(p->addr);
    if (!a) { if (na >= 64) return; a = &A[na++]; a->addr = p->addr; SIZE_T n; ReadProcessMemory(hp, (void *)(DWORD_PTR)p->addr, &a->orig, 1, &n); }
    if (!a->on) { patch(a->addr, 0xCC); a->on = 1; }
    P[np++] = *p;
}

static void install_for(const char *file, DWORD remote_base) {
    HMODULE mine = LoadLibraryA(file);
    if (!mine) return;
    for (size_t i = 0; i < NAPI; i++) {
        if (H[i].addr || _stricmp(H[i].dll, file)) continue;
        FARPROC f = GetProcAddress(mine, H[i].fn);
        if (!f) continue;
        H[i].addr = remote_base + (DWORD)((DWORD_PTR)f - (DWORD_PTR)mine);
        SIZE_T n; if (!ReadProcessMemory(hp, (void *)(DWORD_PTR)H[i].addr, &H[i].orig, 1, &n)) { H[i].addr = 0; continue; }
        arm(&H[i]);
    }
}

static void dump_crash(CONTEXT *c, EXCEPTION_RECORD *r, DWORD first) {
    L("[exception] code=0x%08lx addr=%p first=%lu", r->ExceptionCode, r->ExceptionAddress, first);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        L(" %s 0x%08lx", r->ExceptionInformation[0] == 0 ? "read" : r->ExceptionInformation[0] == 1 ? "write" : "exec", (unsigned long)r->ExceptionInformation[1]);
    L("\n      eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx\n",
      c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    DWORD sk[256] = {0}; SIZE_T n;
    ReadProcessMemory(hp, (void *)(DWORD_PTR)c->Esp, sk, sizeof sk, &n);
    L("      stack:"); for (int i = 0; i < 12; i++) L(" %08lx", sk[i]);
    L("\n      tp.exe return addresses:");
    for (int i = 0; i < 256; i++) if (sk[i] >= 0x401000 && sk[i] < 0xf85000) L(" 0x%08lx", sk[i]);
    L("\n");
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        /* tp.exe: global world object 0x7dbd54, handle words at +0x1da720.., object table at 0x7dc088 (20-byte entries) */
        DWORD w = 0; ReadProcessMemory(hp, (void *)0x7dbd54, &w, 4, &n);
        L("      world=0x%08lx\n", w);
        if (w) {
            WORD hs[16] = {0}; ReadProcessMemory(hp, (void *)(DWORD_PTR)(w + 0x1da720), hs, sizeof hs, &n);
            L("      handles @+0x1da720:"); for (int i = 0; i < 16; i++) L(" %04x", hs[i]);
            L("\n      table[handle]:");
            for (int i = 0; i < 16; i++) { DWORD pt = 0; ReadProcessMemory(hp, (void *)(DWORD_PTR)(0x7dc088 + 20 * (DWORD)hs[i]), &pt, 4, &n); L(" %08lx", pt); }
            L("\n");
        }
        DWORD f = 0; ReadProcessMemory(hp, (void *)0x77c23c, &f, 4, &n); L("      flag 0x77c23c=0x%08lx\n", f);
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
    DEBUG_EVENT e; int step_hook = -1, step_armed = -1;
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
            for (size_t i = 0; i < NCODE; i++) {
                Hook *h = &CODE_HOOKS[i]; SIZE_T n;
                if (ReadProcessMemory(hp, (void *)(DWORD_PTR)h->addr, &h->orig, 1, &n)) arm(h);
            }
            break;
        case EXCEPTION_DEBUG_EVENT: {
            EXCEPTION_RECORD *r = &e.u.Exception.ExceptionRecord;
            HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, e.dwThreadId);
            CONTEXT c; c.ContextFlags = CONTEXT_FULL; GetThreadContext(th, &c);
            DWORD xa = (DWORD)(DWORD_PTR)r->ExceptionAddress;
            int hit = -1, post = -1;
            if (r->ExceptionCode == EXCEPTION_BREAKPOINT) {
                for (size_t i = 0; i < NH_ALL; i++) { Hook *h = hook_at(i); if (h->on && h->addr == xa) hit = (int)i; }
                if (hit < 0) for (int i = np - 1; i >= 0; i--) if (P[i].addr == xa) { post = i; break; }
            }
            if (post >= 0) {
                Post *pp = &P[post]; SIZE_T n;
                if (pp->kind == K_FILE) {
                    if (c.Eax == 0xFFFFFFFFu) L("      [FAILED] %s(\"%s\") -> -1\n", pp->fn, pp->name);
                } else if (pp->kind == K_REG) {
                    DWORD ty = 0, cb = 0; BYTE dat[64] = {0};
                    if (pp->ptype) ReadProcessMemory(hp, (void *)(DWORD_PTR)pp->ptype, &ty, 4, &n);
                    if (pp->pcb) ReadProcessMemory(hp, (void *)(DWORD_PTR)pp->pcb, &cb, 4, &n);
                    if (pp->pdata) ReadProcessMemory(hp, (void *)(DWORD_PTR)pp->pdata, dat, cb < 63 ? cb : 63, &n);
                    char hex[200] = ""; for (DWORD i = 0; i < (cb < 24 ? cb : 24); i++) sprintf(hex + strlen(hex), "%02x ", dat[i]);
                    L("      -> result=%lu type=%lu cb=%lu data=[%s]\n", c.Eax, ty, cb, hex);
                } else if (pp->kind == K_RET) {
                    L("      <- %s returned eax=0x%08lx\n", pp->fn, c.Eax);
                }
                DWORD addr = pp->addr;
                for (int i = post; i < np - 1; i++) P[i] = P[i + 1];
                np--;
                Armed *a = armed_find(addr);
                patch(addr, a->orig); a->on = 0;
                int more = 0; for (int i = 0; i < np; i++) if (P[i].addr == addr) more = 1;
                c.Eip = addr;
                if (more) { c.EFlags |= 0x100; step_armed = (int)(a - A); }
                SetThreadContext(th, &c);
            } else if (hit >= 0) {
                Hook *h = hook_at((size_t)hit);
                DWORD st[8] = {0}; SIZE_T n; ReadProcessMemory(hp, (void *)(DWORD_PTR)c.Esp, st, sizeof st, &n);
                char s1[260] = "", s2[260] = "";
                if (h->strarg) rdstr(st[h->strarg], s1, sizeof s1);
                if (h->str2) rdstr(st[h->str2], s2, sizeof s2);
                if (!h->dll) L("[%s] ecx=0x%08lx a1=0x%lx a2=0x%lx%s%s%s\n", h->fn, c.Ecx, st[1], st[2], s1[0] ? " str=\"" : "", s1, s1[0] ? "\"" : "");
                else L("[%s] ret=0x%08lx a1=0x%lx a2=0x%lx%s%s%s\n", h->fn, st[0], st[1], st[2], s1[0] ? " str=\"" : "", s1, s1[0] ? "\"" : "");
                if (s2[0]) L("      str2=\"%s\"\n", s2);
                if (h->dll && (!strncmp(h->fn, "Rtl", 3) || !strncmp(h->fn, "Nt", 2) || !strncmp(h->fn, "Exit", 4) || !strncmp(h->fn, "Term", 4))) {
                    DWORD s3[64] = {0}; ReadProcessMemory(hp, (void *)(DWORD_PTR)c.Esp, s3, sizeof s3, &n);
                    L("      tp.exe addresses on stack:");
                    for (int i = 0; i < 64; i++) if (s3[i] >= 0x401000 && s3[i] < 0xf85000) L(" 0x%08lx", s3[i]);
                    L("\n");
                }
                if (h->kind != K_PLAIN) {
                    Post p; memset(&p, 0, sizeof p);
                    p.addr = st[0]; p.kind = h->kind;
                    snprintf(p.name, sizeof p.name, "%s", s1); snprintf(p.fn, sizeof p.fn, "%s", h->fn);
                    if (h->kind == K_REG) { p.ptype = st[4]; p.pdata = st[5]; p.pcb = st[6]; }
                    post_push(&p);
                }
                disarm(h);
                c.Eip = h->addr; c.EFlags |= 0x100; /* single step, then re-arm */
                SetThreadContext(th, &c); step_hook = hit;
            } else if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && (step_hook >= 0 || step_armed >= 0)) {
                if (step_hook >= 0) { arm(hook_at((size_t)step_hook)); step_hook = -1; }
                if (step_armed >= 0) { patch(A[step_armed].addr, 0xCC); A[step_armed].on = 1; step_armed = -1; }
                c.EFlags &= ~0x100u; SetThreadContext(th, &c);
            } else if (r->ExceptionCode == EXCEPTION_BREAKPOINT) {
                /* initial loader breakpoint */
            } else {
                dump_crash(&c, r, e.u.Exception.dwFirstChance);
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
