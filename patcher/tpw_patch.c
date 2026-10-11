/*
 * Theme Park World (Bullfrog, 1999) Windows 11 patcher.
 *
 * Works on YOUR OWN copy of the game. It contains no game code or assets: it repairs a damaged import table in
 * tp.exe by an algorithm, installs a small compatibility DLL built from this repo (embedded below), and writes
 * one registry value.
 *
 *   tpw_patch status [--game-dir DIR]
 *   tpw_patch apply  [--game-dir DIR] [--force]
 *   tpw_patch revert [--game-dir DIR]
 *
 * DIR defaults to the folder containing this program.
 */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "payload_weachatr.h"
#include "payload_ddraw.h"

#define KNOWN_ORIGINAL "00f9dfedcb2d4c01c3098f94fccbef6486a7f8aa40494d0921e8cdb8649f1409"
#define KNOWN_PATCHED  "b097f3ad9f0b69542697fe7d5307141ddb47d1d9dbebf0060047ea5f4385e32a"
#define REG_PATH "SOFTWARE\\Bullfrog Productions Ltd\\Theme Park World" /* 32-bit view (WOW6432Node) */
#define LANGUAGE_ENGLISH 9

typedef struct { unsigned char *p; size_t n; } Buf;

static int read_file(const char *path, Buf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); b->n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    b->p = malloc(b->n ? b->n : 1);
    if (fread(b->p, 1, b->n, f) != b->n) { fclose(f); free(b->p); return 0; }
    fclose(f); return 1;
}
static int write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static int exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
static void join(char *out, size_t n, const char *a, const char *b) { snprintf(out, n, "%s\\%s", a, b); }

static void sha256_hex(const unsigned char *p, size_t n, char out[65]) {
    BCRYPT_ALG_HANDLE h = NULL; unsigned char d[32];
    BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    BCryptHash(h, NULL, 0, (PUCHAR)p, (ULONG)n, d, 32);
    BCryptCloseAlgorithmProvider(h, 0);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

/* ---- minimal PE32 reader ---- */
typedef struct { const unsigned char *d; size_t n; unsigned nsec; struct { DWORD va, size, raw; } sec[32]; DWORD dir[16][2]; } PE;
static unsigned rd32(const unsigned char *d) { unsigned v; memcpy(&v, d, 4); return v; }

static int pe_open(PE *pe, const unsigned char *d, size_t n) {
    memset(pe, 0, sizeof *pe); pe->d = d; pe->n = n;
    if (n < 0x100 || d[0] != 'M') return 0;
    unsigned o = rd32(d + 0x3C);
    if (o + 0x100 > n || memcmp(d + o, "PE\0\0", 4)) return 0;
    pe->nsec = *(const unsigned short *)(d + o + 6);
    unsigned optsz = *(const unsigned short *)(d + o + 20), opt = o + 24;
    if (*(const unsigned short *)(d + opt) != 0x10B || pe->nsec > 32) return 0;
    for (int i = 0; i < 16; i++) { pe->dir[i][0] = rd32(d + opt + 96 + 8 * i); pe->dir[i][1] = rd32(d + opt + 100 + 8 * i); }
    for (unsigned i = 0; i < pe->nsec; i++) {
        const unsigned char *s = d + opt + optsz + 40 * i;
        DWORD vs = rd32(s + 8), rs = rd32(s + 16);
        pe->sec[i].va = rd32(s + 12); pe->sec[i].size = vs > rs ? vs : rs; pe->sec[i].raw = rd32(s + 20);
    }
    return 1;
}
static long pe_off(const PE *pe, DWORD rva) {
    for (unsigned i = 0; i < pe->nsec; i++)
        if (rva >= pe->sec[i].va && rva < pe->sec[i].va + pe->sec[i].size) return (long)(pe->sec[i].raw + rva - pe->sec[i].va);
    return -1;
}
static const char *pe_str(const PE *pe, DWORD rva) { long o = pe_off(pe, rva); return o < 0 ? "" : (const char *)pe->d + o; }

typedef void (*ImportCb)(const char *dll, DWORD oft, DWORD ft, void *ctx);
static void pe_each_import(const PE *pe, ImportCb cb, void *ctx) {
    if (!pe->dir[1][0]) return;
    long o = pe_off(pe, pe->dir[1][0]);
    while (o >= 0 && (size_t)o + 20 <= pe->n) {
        DWORD oft = rd32(pe->d + o), name = rd32(pe->d + o + 12), ft = rd32(pe->d + o + 16);
        if (!name) break;
        cb(pe_str(pe, name), oft, ft, ctx);
        o += 20;
    }
}

/* count / repair slots where FirstThunk != OriginalFirstThunk */
typedef struct { unsigned char *w; int total; char desc[512]; } Iat;
static void iat_cb(const char *dll, DWORD oft, DWORD ft, void *ctx) {
    Iat *s = ctx; PE pe;
    if (!oft || !pe_open(&pe, s->w, 1 << 30)) return;
    int n = 0;
    for (DWORD k = 0;; k++) {
        long a = pe_off(&pe, oft + 4 * k), b = pe_off(&pe, ft + 4 * k);
        if (a < 0 || b < 0) break;
        unsigned va = rd32(s->w + a);
        if (!va) break;
        if (rd32(s->w + b) != va) { n++; if (s->desc[0] == 'R') memcpy(s->w + b, &va, 4); }
    }
    if (n) { s->total += n; char t[80]; snprintf(t, sizeof t, " %s:%d", dll, n); if (strlen(s->desc) + strlen(t) < sizeof s->desc - 1 && s->desc[0] != 'R') strcat(s->desc, t); }
}
static int iat_stale(const unsigned char *d, size_t n, char *desc, size_t dn, int repair, unsigned char *work) {
    PE pe; Iat s; memset(&s, 0, sizeof s);
    if (!pe_open(&pe, d, n)) return -1;
    s.w = work ? work : (unsigned char *)d; if (repair) s.desc[0] = 'R';
    /* iat_cb re-opens with the work buffer so offsets are consistent */
    pe_each_import(&pe, iat_cb, &s);
    if (desc && dn) snprintf(desc, dn, "%s", repair ? "" : s.desc);
    return s.total;
}

/* ---- weachatr handling ---- */
static const char *SHIM_FUNCS[] = { "?ParkShout@", "?CreatePark@", "?ParkInfo@" };

typedef struct { char names[64][160]; int n; } NameList;
static void collect_imports_cb(const char *dll, DWORD oft, DWORD ft, void *ctx);
static NameList g_need; static const PE *g_pe;
static void collect_imports_cb(const char *dll, DWORD oft, DWORD ft, void *ctx) {
    (void)ft; (void)ctx;
    if (_stricmp(dll, "weachatr.dll") || !oft) return;
    for (DWORD k = 0;; k++) {
        long o = pe_off(g_pe, oft + 4 * k); if (o < 0) break;
        unsigned v = rd32(g_pe->d + o); if (!v) break;
        if (v & 0x80000000u) continue;
        if (g_need.n < 64) snprintf(g_need.names[g_need.n++], 160, "%s", pe_str(g_pe, (v & 0x7FFFFFFF) + 2));
    }
}
static int has_export(const PE *pe, const char *name) {
    if (!pe->dir[0][0]) return 0;
    long o = pe_off(pe, pe->dir[0][0]); if (o < 0) return 0;
    unsigned nn = rd32(pe->d + o + 24); long names = pe_off(pe, rd32(pe->d + o + 32));
    for (unsigned i = 0; i < nn && names >= 0; i++)
        if (!strcmp(pe_str(pe, rd32(pe->d + names + 4 * i)), name)) return 1;
    return 0;
}
/* returns 1 needed, 0 not needed, -1 problem. msg filled. */
static int shim_needed(const char *game, char *msg, size_t mn) {
    char pexe[MAX_PATH], pdll[MAX_PATH]; Buf e, d; PE pe, pd;
    join(pexe, sizeof pexe, game, "tp.exe"); join(pdll, sizeof pdll, game, "weachatr.dll");
    if (!exists(pdll)) { snprintf(msg, mn, "weachatr.dll not found"); return 0; }
    if (!read_file(pexe, &e) || !read_file(pdll, &d)) { snprintf(msg, mn, "cannot read files"); return -1; }
    if (!pe_open(&pe, e.p, e.n) || !pe_open(&pd, d.p, d.n)) { snprintf(msg, mn, "not a PE file"); return -1; }
    g_need.n = 0; g_pe = &pe; pe_each_import(&pe, collect_imports_cb, NULL);
    int missing = 0;
    for (int i = 0; i < g_need.n; i++) {
        if (has_export(&pd, g_need.names[i])) continue;
        int ok = 0;
        for (int j = 0; j < 3; j++) if (!strncmp(g_need.names[i], SHIM_FUNCS[j], strlen(SHIM_FUNCS[j]))) ok = 1;
        if (!ok) { snprintf(msg, mn, "weachatr.dll lacks %s, which the shim cannot cover", g_need.names[i]); return -1; }
        missing++;
    }
    if (!missing) { snprintf(msg, mn, "weachatr.dll already exports everything tp.exe needs"); return 0; }
    snprintf(msg, mn, "weachatr.dll lacks %d export(s) the exe imports", missing);
    return 1;
}

/* ---- registry ---- */
static const char *registry_language(int write) {
    static char out[160]; HKEY k; DWORD type = 0, v = 0, sz = sizeof v;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, REG_PATH, 0, KEY_READ | KEY_WOW64_32KEY, &k) == ERROR_SUCCESS) {
        LONG r = RegQueryValueExA(k, "Language", NULL, &type, (BYTE *)&v, &sz);
        RegCloseKey(k);
        if (r == ERROR_SUCCESS && type == REG_DWORD) { snprintf(out, sizeof out, "present (DWORD %lu)", v); return out; }
    }
    if (!write) return "missing or wrong type";
    LONG r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, REG_PATH, 0, NULL, 0, KEY_WRITE | KEY_WOW64_32KEY, NULL, &k, NULL);
    if (r == ERROR_ACCESS_DENIED) return "FAILED: run this program as Administrator (it writes HKLM)";
    if (r != ERROR_SUCCESS) { snprintf(out, sizeof out, "FAILED (error %ld)", r); return out; }
    v = LANGUAGE_ENGLISH;
    r = RegSetValueExA(k, "Language", 0, REG_DWORD, (BYTE *)&v, sizeof v);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) { snprintf(out, sizeof out, "FAILED (error %ld)", r); return out; }
    snprintf(out, sizeof out, "written (DWORD %d = English)", LANGUAGE_ENGLISH);
    return out;
}

/* ---- commands ---- */
/* ---- ddraw.dll (our DirectDraw layer) ---- */
static const char DDRAW_INI[] =
    "[ddraw]\r\n; Integer window scale of the game's display mode (windowed mode). 1 = native size, 2 = double, ...\r\nscale=1\r\n"
    "; 1 = borderless window covering the monitor with the picture letterboxed (aspect ratio kept)\r\nfullscreen=0\r\n"
    "; 0 = no title bar in windowed mode\r\nborder=1\r\n; 0 = sharp scaling, 1 = smooth scaling\r\nfilter=0\r\n"
    "; 1 = write every DirectDraw call to ddraw_log.txt (for bug reports)\r\nlog=0\r\n";
/* 1 = file is exactly our embedded ddraw.dll, 0 = a different file, -1 = missing */
static int ddraw_state(const char *game) {
    char p[MAX_PATH]; Buf b; join(p, sizeof p, game, "ddraw.dll");
    if (!exists(p)) return -1;
    if (!read_file(p, &b)) return 0;
    int same = b.n == ddraw_dll_len && !memcmp(b.p, ddraw_dll, b.n); free(b.p); return same;
}

static int cmd_status(const char *game) {
    char path[MAX_PATH], desc[512], msg[256], hex[65]; Buf b;
    join(path, sizeof path, game, "tp.exe");
    if (!read_file(path, &b)) { printf("tp.exe not found in %s\n", game); return 1; }
    sha256_hex(b.p, b.n, hex);
    printf("tp.exe sha256 %s\n  known: %s\n", hex,
           !strcmp(hex, KNOWN_ORIGINAL) ? "original 1999-10-25 build (stale KERNEL32/USER32 import slots)" :
           !strcmp(hex, KNOWN_PATCHED) ? "patched build (import table repaired)" : "UNKNOWN build");
    int st = iat_stale(b.p, b.n, desc, sizeof desc, 0, NULL);
    if (st < 0) printf("  not a valid PE file\n"); else printf("  stale import slots: %d%s\n", st, st ? desc : "");
    join(path, sizeof path, game, "weachatr_orig.dll");
    if (exists(path)) printf("weachatr shim: installed\n");
    else { int n = shim_needed(game, msg, sizeof msg); printf("weachatr shim: %s - %s\n", n == 1 ? "needed" : n == 0 ? "not needed" : "PROBLEM", msg); }
    join(path, sizeof path, game, "Data\\Language\\English");
    printf("Data\\Language\\English: %s\n", exists(path) ? "yes" : "NO");
    printf("registry Language: %s\n", registry_language(0));
    { int d = ddraw_state(game); printf("ddraw.dll: %s\n", d == 1 ? "ours (tpw-ddraw)" : d == 0 ? "a different DirectDraw wrapper (cnc-ddraw/dgVoodoo?)" : "missing (Windows 11's own DirectDraw cannot run the game)"); }
    return 0;
}

static int cmd_apply(const char *game, int force, int replace_ddraw) {
    char pexe[MAX_PATH], path[MAX_PATH], bak[MAX_PATH], desc[512], msg[256], hex[65]; Buf b;
    join(pexe, sizeof pexe, game, "tp.exe");
    if (!read_file(pexe, &b)) { printf("tp.exe not found in %s\n", game); return 1; }
    sha256_hex(b.p, b.n, hex);
    if (strcmp(hex, KNOWN_ORIGINAL) && strcmp(hex, KNOWN_PATCHED) && !force) {
        printf("Unknown tp.exe build (%s).\nRe-run with --force to patch anyway.\n", hex); return 2;
    }
    int st = iat_stale(b.p, b.n, desc, sizeof desc, 0, NULL);
    if (st < 0) { printf("tp.exe is not a valid PE file\n"); return 2; }
    if (st) {
        snprintf(bak, sizeof bak, "%s.orig", pexe);
        if (!exists(bak)) { if (!CopyFileA(pexe, bak, TRUE)) { printf("cannot create backup %s\n", bak); return 5; } printf("backup: %s\n", bak); }
        unsigned char *w = malloc(b.n); memcpy(w, b.p, b.n);
        iat_stale(b.p, b.n, NULL, 0, 1, w);
        if (iat_stale(w, b.n, NULL, 0, 0, NULL) != 0) { printf("internal error: repair incomplete\n"); return 5; }
        if (!write_file(pexe, w, b.n)) { printf("cannot write %s\n", pexe); return 5; }
        printf("repaired %d import slots:%s\n", st, desc);
    } else printf("tp.exe import table already fine\n");

    join(path, sizeof path, game, "weachatr_orig.dll");
    if (exists(path)) printf("weachatr: shim already installed\n");
    else {
        int n = shim_needed(game, msg, sizeof msg);
        if (n < 0) { printf("ERROR: %s\n", msg); return 3; }
        if (n) {
            char dll[MAX_PATH]; join(dll, sizeof dll, game, "weachatr.dll");
            if (!MoveFileExA(dll, path, 0)) { printf("cannot rename weachatr.dll\n"); return 5; }
            if (!write_file(dll, shim_dll, shim_dll_len)) { printf("cannot write shim\n"); return 5; }
            printf("installed weachatr shim (original kept as weachatr_orig.dll)\n");
        } else printf("weachatr: %s\n", msg);
    }
    printf("registry Language: %s\n", registry_language(1));
    {
        int d = ddraw_state(game); char dd[MAX_PATH], ini[MAX_PATH], bak[MAX_PATH];
        join(dd, sizeof dd, game, "ddraw.dll"); join(ini, sizeof ini, game, "ddraw.ini"); join(bak, sizeof bak, game, "ddraw_orig.dll");
        if (d == 1) printf("ddraw.dll: ours, already installed\n");
        else if (d == 0 && !replace_ddraw) printf("ddraw.dll: a different DirectDraw wrapper is installed; left alone (add --replace-ddraw to switch to ours, the old one is kept as ddraw_orig.dll)\n");
        else {
            if (d == 0) { if (exists(bak)) DeleteFileA(bak); if (!MoveFileExA(dd, bak, 0)) { printf("cannot back up existing ddraw.dll\n"); return 5; } }
            if (!write_file(dd, ddraw_dll, ddraw_dll_len)) { printf("cannot write ddraw.dll\n"); return 5; }
            if (!exists(ini)) write_file(ini, DDRAW_INI, sizeof DDRAW_INI - 1);
            printf("installed ddraw.dll (our DirectDraw layer) and ddraw.ini%s\n", d == 0 ? "; previous one kept as ddraw_orig.dll" : "");
        }
    }
    return 0;
}

static int cmd_revert(const char *game) {
    char pexe[MAX_PATH], bak[MAX_PATH], orig[MAX_PATH], cur[MAX_PATH];
    join(pexe, sizeof pexe, game, "tp.exe"); snprintf(bak, sizeof bak, "%s.orig", pexe);
    if (exists(bak)) { MoveFileExA(bak, pexe, MOVEFILE_REPLACE_EXISTING); printf("restored tp.exe\n"); }
    join(orig, sizeof orig, game, "weachatr_orig.dll"); join(cur, sizeof cur, game, "weachatr.dll");
    if (exists(orig)) { MoveFileExA(orig, cur, MOVEFILE_REPLACE_EXISTING); printf("restored weachatr.dll\n"); }
    { char dd[MAX_PATH], bak[MAX_PATH]; join(dd, sizeof dd, game, "ddraw.dll"); join(bak, sizeof bak, game, "ddraw_orig.dll");
      if (ddraw_state(game) == 1) { DeleteFileA(dd); printf("removed our ddraw.dll\n"); if (exists(bak)) { MoveFileExA(bak, dd, 0); printf("restored previous ddraw.dll\n"); } } }
    printf("registry value left in place\n");
    return 0;
}

int main(int argc, char **argv) {
    char game[MAX_PATH]; int force = 0, replace_ddraw = 0; const char *cmd = NULL;
    GetModuleFileNameA(NULL, game, sizeof game);
    char *s = strrchr(game, '\\'); if (s) *s = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc) snprintf(game, sizeof game, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--replace-ddraw")) replace_ddraw = 1;
        else if (!cmd) cmd = argv[i];
    }
    size_t l = strlen(game); while (l > 3 && (game[l - 1] == '\\' || game[l - 1] == '/')) game[--l] = 0;
    if (!cmd) { puts("usage: tpw_patch status|apply|revert [--game-dir DIR] [--force] [--replace-ddraw]"); return 64; }
    if (!strcmp(cmd, "status")) return cmd_status(game);
    if (!strcmp(cmd, "apply")) return cmd_apply(game, force, replace_ddraw);
    if (!strcmp(cmd, "revert")) return cmd_revert(game);
    puts("unknown command"); return 64;
}
