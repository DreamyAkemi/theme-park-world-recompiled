/*
 * tpw-ddraw: a small, self-contained DirectDraw implementation for old 2D/software-rendered games on Windows 10/11.
 *
 * Written from the public DirectDraw API (ddraw.h). All surfaces live in system memory; the primary surface is shown by
 * scaling it into the game's window with GDI. Direct3D is deliberately not offered (QueryInterface for IDirect3D* fails), so
 * games with a software-renderer fallback use it.
 *
 * Options (ddraw.ini next to ddraw.dll):
 *   [ddraw]  scale=1        window scale of the display mode (windowed mode); 0 = largest window that fits the screen
 *            fullscreen=0   1 = borderless window covering the monitor, picture letterboxed
 *            border=1       0 = no title bar in windowed mode
 *            filter=0       1 = smooth (bilinear-like) scaling, 0 = nearest neighbour
 *            log=0          1 = write every call to ddraw_log.txt
 */
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>
#include <vector>
#include <algorithm>

/* ------------------------------------------------------------------ config + log */
static struct { int scale = 1, fullscreen = 0, border = 1, filter = 0, log = 0; } cfg;
static FILE *logf;
static CRITICAL_SECTION cs;
static HMODULE self_mod;

static void LOG(const char *fmt, ...) {
    if (!cfg.log) return;
    if (!logf) { char p[MAX_PATH]; GetModuleFileNameA(self_mod, p, MAX_PATH); char *s = strrchr(p, '\\'); if (s) *s = 0; strcat(p, "\\ddraw_log.txt"); logf = fopen(p, "w"); if (!logf) return; }
    va_list a; va_start(a, fmt); vfprintf(logf, fmt, a); va_end(a); fputc('\n', logf); fflush(logf);
}
static void LOGU(unsigned key, const char *fmt, ...) {
    if (!cfg.log) return; static std::vector<unsigned> seen; for (unsigned k : seen) if (k == key) return; if (seen.size() > 2000) return; seen.push_back(key);
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); LOG("%s", b);
}
struct Guard { Guard() { EnterCriticalSection(&cs); } ~Guard() { LeaveCriticalSection(&cs); } };

static void load_cfg() {
    char p[MAX_PATH]; GetModuleFileNameA(self_mod, p, MAX_PATH); char *s = strrchr(p, '\\'); if (s) *s = 0; strcat(p, "\\ddraw.ini");
    cfg.scale = GetPrivateProfileIntA("ddraw", "scale", 1, p); if (cfg.scale < 0) cfg.scale = 1; if (cfg.scale > 8) cfg.scale = 8;
    cfg.fullscreen = GetPrivateProfileIntA("ddraw", "fullscreen", 0, p);
    cfg.border = GetPrivateProfileIntA("ddraw", "border", 1, p);
    cfg.filter = GetPrivateProfileIntA("ddraw", "filter", 0, p);
    cfg.log = GetPrivateProfileIntA("ddraw", "log", 0, p);
}

/* ------------------------------------------------------------------ pixel formats */
struct Fmt { int bpp = 16; DWORD flags = DDPF_RGB; DWORD rm = 0xF800, gm = 0x07E0, bm = 0x001F, am = 0; int rs, gs, bs, as_, rb, gb, bb, ab; };
static void shiftbits(DWORD m, int &sh, int &bits) { sh = 0; bits = 0; if (!m) return; while (!(m & 1)) { m >>= 1; sh++; } while (m & 1) { m >>= 1; bits++; } }
static void fmt_finish(Fmt &f) { shiftbits(f.rm, f.rs, f.rb); shiftbits(f.gm, f.gs, f.gb); shiftbits(f.bm, f.bs, f.bb); shiftbits(f.am, f.as_, f.ab); }
static Fmt fmt_from_pf(const DDPIXELFORMAT &pf) {
    Fmt f; f.flags = pf.dwFlags;
    if (pf.dwFlags & DDPF_PALETTEINDEXED8) { f.bpp = 8; f.rm = f.gm = f.bm = 0; }
    else { f.bpp = pf.dwRGBBitCount; f.rm = pf.dwRBitMask; f.gm = pf.dwGBitMask; f.bm = pf.dwBBitMask; f.am = (pf.dwFlags & DDPF_ALPHAPIXELS) ? pf.dwRGBAlphaBitMask : 0; }
    if (pf.dwFlags & DDPF_ZBUFFER) { f.bpp = pf.dwZBufferBitDepth; f.rm = f.gm = f.bm = 0; }
    fmt_finish(f); return f;
}
static void pf_from_fmt(const Fmt &f, DDPIXELFORMAT &pf) {
    memset(&pf, 0, sizeof pf); pf.dwSize = sizeof pf; pf.dwFlags = f.flags;
    if (f.flags & DDPF_ZBUFFER) { pf.dwZBufferBitDepth = f.bpp; return; }
    pf.dwRGBBitCount = f.bpp; pf.dwRBitMask = f.rm; pf.dwGBitMask = f.gm; pf.dwBBitMask = f.bm; pf.dwRGBAlphaBitMask = f.am;
}
static Fmt display_fmt(int bpp) {
    Fmt f;
    if (bpp == 8) { f.bpp = 8; f.flags = DDPF_RGB | DDPF_PALETTEINDEXED8; f.rm = f.gm = f.bm = 0; }
    else if (bpp == 32 || bpp == 24) { f.bpp = bpp; f.rm = 0xFF0000; f.gm = 0xFF00; f.bm = 0xFF; }
    else if (bpp == 15) { f.bpp = 16; f.rm = 0x7C00; f.gm = 0x03E0; f.bm = 0x001F; }
    fmt_finish(f); return f;
}

/* ------------------------------------------------------------------ objects */
struct Surface; struct DDraw; struct Palette;

struct Palette : IDirectDrawPalette {
    LONG ref = 1; PALETTEENTRY e[256]; DWORD caps; virtual ~Palette() {}
    Palette() { memset(e, 0, sizeof e); caps = 0; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void **o) override { if (r == IID_IUnknown || r == IID_IDirectDrawPalette) { *o = this; AddRef(); return S_OK; } *o = 0; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&ref); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDWORD c) override { *c = caps; return DD_OK; }
    HRESULT STDMETHODCALLTYPE GetEntries(DWORD, DWORD b, DWORD n, LPPALETTEENTRY o) override { if (b + n > 256) return DDERR_INVALIDPARAMS; memcpy(o, e + b, n * sizeof(PALETTEENTRY)); return DD_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) override { return DDERR_ALREADYINITIALIZED; }
    HRESULT STDMETHODCALLTYPE SetEntries(DWORD, DWORD b, DWORD n, LPPALETTEENTRY i) override { if (b + n > 256) return DDERR_INVALIDPARAMS; memcpy(e + b, i, n * sizeof(PALETTEENTRY)); return DD_OK; }
};

struct Clipper : IDirectDrawClipper {
    LONG ref = 1; HWND hwnd = 0; virtual ~Clipper() {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void **o) override { if (r == IID_IUnknown || r == IID_IDirectDrawClipper) { *o = this; AddRef(); return S_OK; } *o = 0; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&ref); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetClipList(LPRECT, LPRGNDATA, LPDWORD sz) override { if (sz) *sz = 0; return DDERR_NOCLIPLIST; }
    HRESULT STDMETHODCALLTYPE GetHWnd(HWND *h) override { *h = hwnd; return DD_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE IsClipListChanged(WINBOOL *c) override { *c = FALSE; return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetClipList(LPRGNDATA, DWORD) override { return DD_OK; }
    HRESULT STDMETHODCALLTYPE SetHWnd(DWORD, HWND h) override { hwnd = h; return DD_OK; }
};

/* global display state */
static struct {
    HWND hwnd = 0; WNDPROC oldproc = 0; DWORD coop = 0;
    int w = 0, h = 0, bpp = 0;          /* current display mode (0 = never set) */
    int pw = 0, ph = 0;                 /* primary surface size */
    Surface *primary = 0;
    bool dirty = false;
} G;

static void present();

/* ---- host-process USER32 virtualisation: the game believes it owns a W x H screen at (0,0) ---- */
typedef BOOL (WINAPI *PFN_GetClientRect)(HWND, LPRECT);
typedef BOOL (WINAPI *PFN_GetWindowRect)(HWND, LPRECT);
typedef BOOL (WINAPI *PFN_ScreenToClient)(HWND, LPPOINT);
typedef BOOL (WINAPI *PFN_ClientToScreen)(HWND, LPPOINT);
typedef int (WINAPI *PFN_GetSystemMetrics)(int);
typedef BOOL (WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL (WINAPI *PFN_MoveWindow)(HWND, int, int, int, int, BOOL);
static PFN_GetClientRect o_GetClientRect; static PFN_GetWindowRect o_GetWindowRect; static PFN_ScreenToClient o_ScreenToClient;
static PFN_ClientToScreen o_ClientToScreen; static PFN_GetSystemMetrics o_GetSystemMetrics; static PFN_SetWindowPos o_SetWindowPos; static PFN_MoveWindow o_MoveWindow;
static bool hooks_installed;

static bool virt_active(HWND h) { return h && h == G.hwnd && G.primary && G.w > 0; }
/* real client rect -> aspect-fit rect of the picture inside it */
static void picture_rect(HWND h, RECT *d, int *cw, int *ch) {
    RECT cr = {0, 0, 0, 0}; (o_GetClientRect ? o_GetClientRect : GetClientRect)(h, &cr);
    *cw = cr.right; *ch = cr.bottom; int sw = G.pw ? G.pw : G.w, sh = G.ph ? G.ph : G.h;
    if (*cw <= 0 || *ch <= 0 || sw <= 0 || sh <= 0) { *d = cr; return; }
    if ((long long)*cw * sh >= (long long)*ch * sw) { int dw = *ch * sw / sh; d->left = (*cw - dw) / 2; d->right = d->left + dw; d->top = 0; d->bottom = *ch; }
    else { int dh = *cw * sh / sw; d->top = (*ch - dh) / 2; d->bottom = d->top + dh; d->left = 0; d->right = *cw; }
}
static BOOL WINAPI hk_GetClientRect(HWND h, LPRECT r) { if (virt_active(h) && r) { r->left = r->top = 0; r->right = G.pw; r->bottom = G.ph; return TRUE; } return o_GetClientRect(h, r); }
static BOOL WINAPI hk_GetWindowRect(HWND h, LPRECT r) { if (virt_active(h) && r) { r->left = r->top = 0; r->right = G.pw; r->bottom = G.ph; return TRUE; } return o_GetWindowRect(h, r); }
static BOOL WINAPI hk_ScreenToClient(HWND h, LPPOINT p) {
    BOOL ok = o_ScreenToClient(h, p);
    if (ok && p && virt_active(h)) { RECT d; int cw, ch; picture_rect(h, &d, &cw, &ch); int dw = d.right - d.left, dh = d.bottom - d.top; if (dw > 0 && dh > 0) { p->x = (p->x - d.left) * G.pw / dw; p->y = (p->y - d.top) * G.ph / dh; } }
    return ok;
}
static BOOL WINAPI hk_ClientToScreen(HWND h, LPPOINT p) {
    if (p && virt_active(h)) { RECT d; int cw, ch; picture_rect(h, &d, &cw, &ch); int dw = d.right - d.left, dh = d.bottom - d.top; if (dw > 0 && dh > 0) { p->x = p->x * dw / G.pw + d.left; p->y = p->y * dh / G.ph + d.top; } }
    return o_ClientToScreen(h, p);
}
static int WINAPI hk_GetSystemMetrics(int n) { if (G.w > 0 && G.primary) { if (n == SM_CXSCREEN) return G.w; if (n == SM_CYSCREEN) return G.h; } return o_GetSystemMetrics(n); }
static BOOL WINAPI hk_SetWindowPos(HWND h, HWND a, int x, int y, int cx, int cy, UINT f) { if (virt_active(h)) f |= SWP_NOMOVE | SWP_NOSIZE; return o_SetWindowPos(h, a, x, y, cx, cy, f); }
static BOOL WINAPI hk_MoveWindow(HWND h, int x, int y, int w, int hh, BOOL r) { if (virt_active(h)) return TRUE; return o_MoveWindow(h, x, y, w, hh, r); }

static void iat_hook(HMODULE mod, const char *dll, const char *fn, void *repl, void **orig) {
    BYTE *b = (BYTE *)mod; IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)b; if (dh->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + dh->e_lfanew); IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; if (!dd.VirtualAddress) return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + dd.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(b + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *oft = (IMAGE_THUNK_DATA *)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk)), *ft = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        for (; oft->u1.AddressOfData; oft++, ft++) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            const char *name = (const char *)((IMAGE_IMPORT_BY_NAME *)(b + oft->u1.AddressOfData))->Name;
            if (strcmp(name, fn)) continue;
            DWORD old; VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            if (!*orig) *orig = (void *)ft->u1.Function; ft->u1.Function = (DWORD_PTR)repl; VirtualProtect(&ft->u1.Function, sizeof(void *), old, &old);
            LOG("hooked %s!%s in host exe", dll, fn); return;
        }
    }
}
static void install_hooks() {
    if (hooks_installed) return; hooks_installed = true;
    HMODULE host = GetModuleHandle(NULL), u = GetModuleHandleA("user32.dll");
    o_GetClientRect = (PFN_GetClientRect)GetProcAddress(u, "GetClientRect"); o_GetWindowRect = (PFN_GetWindowRect)GetProcAddress(u, "GetWindowRect");
    o_ScreenToClient = (PFN_ScreenToClient)GetProcAddress(u, "ScreenToClient"); o_ClientToScreen = (PFN_ClientToScreen)GetProcAddress(u, "ClientToScreen");
    o_GetSystemMetrics = (PFN_GetSystemMetrics)GetProcAddress(u, "GetSystemMetrics"); o_SetWindowPos = (PFN_SetWindowPos)GetProcAddress(u, "SetWindowPos"); o_MoveWindow = (PFN_MoveWindow)GetProcAddress(u, "MoveWindow");
    void *tmp;
    tmp = 0; iat_hook(host, "USER32.dll", "GetClientRect", (void *)hk_GetClientRect, &tmp);
    tmp = 0; iat_hook(host, "USER32.dll", "GetWindowRect", (void *)hk_GetWindowRect, &tmp);
    tmp = 0; iat_hook(host, "USER32.dll", "ScreenToClient", (void *)hk_ScreenToClient, &tmp);
    tmp = 0; iat_hook(host, "USER32.dll", "ClientToScreen", (void *)hk_ClientToScreen, &tmp);
    tmp = 0; iat_hook(host, "USER32.dll", "GetSystemMetrics", (void *)hk_GetSystemMetrics, &tmp);
    tmp = 0; iat_hook(host, "USER32.dll", "SetWindowPos", (void *)hk_SetWindowPos, &tmp);
    tmp = 0; iat_hook(host, "USER32.dll", "MoveWindow", (void *)hk_MoveWindow, &tmp);
}

struct Surface {
    LONG ref = 1;
    DDraw *dd = 0;
    DDSURFACEDESC2 desc;                /* what we report */
    Fmt fmt; int w = 0, h = 0, pitch = 0;
    BYTE *mem = 0; bool ownmem = true;
    DWORD caps = 0, caps2 = 0;
    DDCOLORKEY ck_src_blt{}, ck_dst_blt{}; bool has_src = false, has_dst = false;
    Palette *pal = 0; Clipper *clip = 0;
    std::vector<Surface *> attached;    /* owned references */
    Surface *parent = 0;                /* weak: surface we are attached to */
    bool locked = false; HDC dc = 0; HBITMAP dib = 0; void *dibbits = 0; HGDIOBJ olddib = 0;
    bool isprimary() const { return (caps & DDSCAPS_PRIMARYSURFACE) != 0; }

    struct V7 : IDirectDrawSurface7 { Surface *s;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void **o) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
        HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE7 a) override;
        HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE Blt(LPRECT d, LPDIRECTDRAWSURFACE7 src, LPRECT sr, DWORD f, LPDDBLTFX fx) override;
        HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH, DWORD, DWORD) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 src, LPRECT sr, DWORD t) override;
        HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE7 a) override;
        HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb) override;
        HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK7) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE7 t, DWORD f) override;
        HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS2 c, LPDIRECTDRAWSURFACE7 *o) override;
        HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS2 c) override { memset(c, 0, sizeof *c); c->dwCaps = s->caps; c->dwCaps2 = s->caps2; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetClipper(LPDIRECTDRAWCLIPPER *c) override { if (!s->clip) return DDERR_NOCLIPPERATTACHED; s->clip->AddRef(); *c = s->clip; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetColorKey(DWORD f, LPDDCOLORKEY k) override;
        HRESULT STDMETHODCALLTYPE GetDC(HDC *dc) override;
        HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG, LPLONG) override { return DDERR_NOTAOVERLAYSURFACE; }
        HRESULT STDMETHODCALLTYPE GetPalette(LPDIRECTDRAWPALETTE *p) override { if (!s->pal) return DDERR_NOPALETTEATTACHED; s->pal->AddRef(); *p = s->pal; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT p) override { pf_from_fmt(s->fmt, *p); return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC2 d) override { *d = s->desc; d->dwSize = sizeof *d; return DD_OK; }
        HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, LPDDSURFACEDESC2) override { return DDERR_ALREADYINITIALIZED; }
        HRESULT STDMETHODCALLTYPE IsLost() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE Lock(LPRECT r, LPDDSURFACEDESC2 d, DWORD f, HANDLE e) override;
        HRESULT STDMETHODCALLTYPE ReleaseDC(HDC dc) override;
        HRESULT STDMETHODCALLTYPE Restore() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE SetClipper(LPDIRECTDRAWCLIPPER c) override;
        HRESULT STDMETHODCALLTYPE SetColorKey(DWORD f, LPDDCOLORKEY k) override;
        HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG, LONG) override { return DDERR_NOTAOVERLAYSURFACE; }
        HRESULT STDMETHODCALLTYPE SetPalette(LPDIRECTDRAWPALETTE p) override;
        HRESULT STDMETHODCALLTYPE Unlock(LPRECT r) override;
        HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE7, LPRECT, DWORD, LPDDOVERLAYFX) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE7) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE GetDDInterface(LPVOID *o) override;
        HRESULT STDMETHODCALLTYPE PageLock(DWORD) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE PageUnlock(DWORD) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE SetSurfaceDesc(LPDDSURFACEDESC2, DWORD) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, LPVOID, DWORD, DWORD) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, LPVOID, LPDWORD) override { return DDERR_NOTFOUND; }
        HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return DDERR_NOTFOUND; }
        HRESULT STDMETHODCALLTYPE GetUniquenessValue(LPDWORD v) override { *v = 1; return DD_OK; }
        HRESULT STDMETHODCALLTYPE ChangeUniquenessValue() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE SetPriority(DWORD) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetPriority(LPDWORD p) override { *p = 0; return DD_OK; }
        HRESULT STDMETHODCALLTYPE SetLOD(DWORD) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetLOD(LPDWORD l) override { *l = 0; return DD_OK; }
    } v7;
    struct V3 : IDirectDrawSurface3 { Surface *s;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void **o) override { return s->v7.QueryInterface(r, o); }
        ULONG STDMETHODCALLTYPE AddRef() override { return s->v7.AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return s->v7.Release(); }
        HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE3 a) override { return s->v7.AddAttachedSurface(a ? &((Surface::V3 *)a)->s->v7 : 0); }
        HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE Blt(LPRECT d, LPDIRECTDRAWSURFACE3 src, LPRECT sr, DWORD f, LPDDBLTFX fx) override { return s->v7.Blt(d, src ? &((V3 *)src)->s->v7 : 0, sr, f, fx); }
        HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH, DWORD, DWORD) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE3 src, LPRECT sr, DWORD t) override { return s->v7.BltFast(x, y, src ? &((V3 *)src)->s->v7 : 0, sr, t); }
        HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD f, LPDIRECTDRAWSURFACE3 a) override { return s->v7.DeleteAttachedSurface(f, a ? &((V3 *)a)->s->v7 : 0); }
        HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) override;
        HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE3 t, DWORD f) override { return s->v7.Flip(t ? &((V3 *)t)->s->v7 : 0, f); }
        HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS c, LPDIRECTDRAWSURFACE3 *o) override;
        HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS c) override { c->dwCaps = s->caps; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetClipper(LPDIRECTDRAWCLIPPER *c) override { return s->v7.GetClipper(c); }
        HRESULT STDMETHODCALLTYPE GetColorKey(DWORD f, LPDDCOLORKEY k) override { return s->v7.GetColorKey(f, k); }
        HRESULT STDMETHODCALLTYPE GetDC(HDC *dc) override { return s->v7.GetDC(dc); }
        HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG, LPLONG) override { return DDERR_NOTAOVERLAYSURFACE; }
        HRESULT STDMETHODCALLTYPE GetPalette(LPDIRECTDRAWPALETTE *p) override { return s->v7.GetPalette(p); }
        HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT p) override { return s->v7.GetPixelFormat(p); }
        HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC d) override;
        HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) override { return DDERR_ALREADYINITIALIZED; }
        HRESULT STDMETHODCALLTYPE IsLost() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE Lock(LPRECT r, LPDDSURFACEDESC d, DWORD f, HANDLE e) override;
        HRESULT STDMETHODCALLTYPE ReleaseDC(HDC dc) override { return s->v7.ReleaseDC(dc); }
        HRESULT STDMETHODCALLTYPE Restore() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE SetClipper(LPDIRECTDRAWCLIPPER c) override { return s->v7.SetClipper(c); }
        HRESULT STDMETHODCALLTYPE SetColorKey(DWORD f, LPDDCOLORKEY k) override { return s->v7.SetColorKey(f, k); }
        HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG, LONG) override { return DDERR_NOTAOVERLAYSURFACE; }
        HRESULT STDMETHODCALLTYPE SetPalette(LPDIRECTDRAWPALETTE p) override { return s->v7.SetPalette(p); }
        HRESULT STDMETHODCALLTYPE Unlock(LPVOID) override { return s->v7.Unlock(0); }
        HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE3, LPRECT, DWORD, LPDDOVERLAYFX) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE3) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE GetDDInterface(LPVOID *o) override { return s->v7.GetDDInterface(o); }
        HRESULT STDMETHODCALLTYPE PageLock(DWORD) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE PageUnlock(DWORD) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE SetSurfaceDesc(LPDDSURFACEDESC, DWORD) override { return DDERR_UNSUPPORTED; }
    } v3;

    Surface() { v7.s = this; v3.s = this; memset(&desc, 0, sizeof desc); }
    ~Surface();
    void release_gdi();
};

/* DDSURFACEDESC <-> DDSURFACEDESC2 */
static void to2(const DDSURFACEDESC *a, DDSURFACEDESC2 *b) {
    memset(b, 0, sizeof *b); b->dwSize = sizeof *b;
    memcpy(&b->dwFlags, &a->dwFlags, offsetof(DDSURFACEDESC, ddsCaps) - offsetof(DDSURFACEDESC, dwFlags));
    b->ddsCaps.dwCaps = a->ddsCaps.dwCaps;
}
static void from2(const DDSURFACEDESC2 *b, DDSURFACEDESC *a) {
    memset(a, 0, sizeof *a); a->dwSize = sizeof *a;
    memcpy(&a->dwFlags, &b->dwFlags, offsetof(DDSURFACEDESC, ddsCaps) - offsetof(DDSURFACEDESC, dwFlags));
    a->ddsCaps.dwCaps = b->ddsCaps.dwCaps;
}

/* ------------------------------------------------------------------ pixel access */
static inline DWORD getpx(const BYTE *p, int bpp) {
    switch (bpp) { case 8: return *p; case 16: return *(const WORD *)p; case 24: return p[0] | (p[1] << 8) | (p[2] << 16); default: return *(const DWORD *)p; }
}
static inline void putpx(BYTE *p, int bpp, DWORD v) {
    switch (bpp) { case 8: *p = (BYTE)v; break; case 16: *(WORD *)p = (WORD)v; break; case 24: p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); break; default: *(DWORD *)p = v; }
}
static inline void unpack(const Fmt &f, DWORD v, const Palette *pal, int &r, int &g, int &b, int &a) {
    if (f.bpp == 8 && (f.flags & DDPF_PALETTEINDEXED8)) { if (pal) { r = pal->e[v & 255].peRed; g = pal->e[v & 255].peGreen; b = pal->e[v & 255].peBlue; } else r = g = b = (int)(v & 255); a = 255; return; }
    auto ex = [&](DWORD m, int sh, int bits) -> int { if (!bits) return 0; DWORD x = (v & m) >> sh; return bits >= 8 ? (int)(x >> (bits - 8)) : (int)((x * 255) / ((1u << bits) - 1)); };
    r = ex(f.rm, f.rs, f.rb); g = ex(f.gm, f.gs, f.gb); b = ex(f.bm, f.bs, f.bb); a = f.ab ? ex(f.am, f.as_, f.ab) : 255;
}
static inline DWORD pack(const Fmt &f, int r, int g, int b, int a) {
    auto pk = [&](int c, int sh, int bits) -> DWORD { if (!bits) return 0; return ((DWORD)(c >> (8 - (bits < 8 ? bits : 8))) << sh); };
    return pk(r, f.rs, f.rb) | pk(g, f.gs, f.gb) | pk(b, f.bs, f.bb) | (f.ab ? pk(a, f.as_, f.ab) : 0);
}
static bool same_fmt(const Fmt &a, const Fmt &b) { return a.bpp == b.bpp && a.rm == b.rm && a.gm == b.gm && a.bm == b.bm && a.am == b.am && ((a.flags ^ b.flags) & DDPF_PALETTEINDEXED8) == 0; }

/* ------------------------------------------------------------------ presentation */
static int g_client_w, g_client_h;
static LRESULT CALLBACK hook_proc(HWND h, UINT m, WPARAM w, LPARAM l);

static void dest_rect(int cw, int ch, int sw, int sh, RECT *r) {
    if (cw * sh >= ch * sw) { int dw = ch * sw / sh; r->left = (cw - dw) / 2; r->right = r->left + dw; r->top = 0; r->bottom = ch; }
    else { int dh = cw * sh / sw; r->top = (ch - dh) / 2; r->bottom = r->top + dh; r->left = 0; r->right = cw; }
}

static void present() {
    Guard pg;
    Surface *p = G.primary;
    if (!p || !p->mem || !G.hwnd || !IsWindow(G.hwnd)) return;
    int W = p->w, H = p->h;
    static std::vector<DWORD> buf; buf.resize((size_t)W * H);
    for (int y = 0; y < H; y++) {
        const BYTE *row = p->mem + (size_t)y * p->pitch; DWORD *o = &buf[(size_t)y * W];
        if (p->fmt.bpp == 32 && p->fmt.rm == 0xFF0000 && p->fmt.gm == 0xFF00 && p->fmt.bm == 0xFF) { memcpy(o, row, (size_t)W * 4); continue; }
        if (p->fmt.bpp == 16 && p->fmt.rm == 0xF800 && p->fmt.gm == 0x07E0 && p->fmt.bm == 0x001F) {
            const WORD *s = (const WORD *)row;
            for (int x = 0; x < W; x++) { DWORD v = s[x]; DWORD r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31; o[x] = (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2)); }
            continue;
        }
        for (int x = 0; x < W; x++) { int r, g, b, a; unpack(p->fmt, getpx(row + x * (p->fmt.bpp / 8), p->fmt.bpp), p->pal, r, g, b, a); o[x] = (r << 16) | (g << 8) | b; }
    }
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(G.hwnd); RECT cr; (o_GetClientRect ? o_GetClientRect : GetClientRect)(G.hwnd, &cr);
    int cw = cr.right, ch = cr.bottom; g_client_w = cw; g_client_h = ch;
    RECT d; dest_rect(cw, ch, W, H, &d);
    if (d.left > 0 || d.top > 0) { HBRUSH bk = (HBRUSH)GetStockObject(BLACK_BRUSH); RECT a = {0, 0, cw, d.top}, b = {0, d.bottom, cw, ch}, c = {0, d.top, d.left, d.bottom}, e = {d.right, d.top, cw, d.bottom}; FillRect(dc, &a, bk); FillRect(dc, &b, bk); FillRect(dc, &c, bk); FillRect(dc, &e, bk); }
    SetStretchBltMode(dc, cfg.filter ? HALFTONE : COLORONCOLOR); if (cfg.filter) SetBrushOrgEx(dc, 0, 0, NULL);
    if (d.right - d.left == W && d.bottom - d.top == H) SetDIBitsToDevice(dc, d.left, d.top, W, H, 0, 0, 0, H, buf.data(), &bi, DIB_RGB_COLORS);
    else StretchDIBits(dc, d.left, d.top, d.right - d.left, d.bottom - d.top, 0, 0, W, H, buf.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(G.hwnd, dc); G.dirty = false;
}

static void apply_window() {
    if (!G.hwnd || !IsWindow(G.hwnd) || !G.w) return;
    HWND h = G.hwnd;
    if (cfg.fullscreen) {
        MONITORINFO mi = {sizeof mi}; GetMonitorInfo(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongPtr(h, GWL_STYLE, (GetWindowLongPtr(h, GWL_STYLE) & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER)) | WS_POPUP | WS_VISIBLE);
        (o_SetWindowPos ? o_SetWindowPos : SetWindowPos)(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        SetForegroundWindow(h); SetFocus(h);
        return;
    }
    LONG_PTR st = GetWindowLongPtr(h, GWL_STYLE);
    st &= ~(WS_POPUP | WS_THICKFRAME | WS_MAXIMIZEBOX);
    st |= cfg.border ? (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE) : (WS_POPUP | WS_VISIBLE);
    SetWindowLongPtr(h, GWL_STYLE, st);
    MONITORINFO mi = {sizeof mi}; GetMonitorInfo(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    int cwid = G.w * (cfg.scale ? cfg.scale : 1), chei = G.h * (cfg.scale ? cfg.scale : 1);
    if (cfg.scale == 0) {   /* largest window that fits the work area, aspect kept */
        RECT fr = {0, 0, 0, 0}; AdjustWindowRect(&fr, (DWORD)st, FALSE);
        int maxw = (mi.rcWork.right - mi.rcWork.left) - (fr.right - fr.left), maxh = (mi.rcWork.bottom - mi.rcWork.top) - (fr.bottom - fr.top);
        cwid = maxw; chei = maxw * G.h / G.w; if (chei > maxh) { chei = maxh; cwid = maxh * G.w / G.h; }
    }
    RECT r = {0, 0, cwid, chei}; AdjustWindowRect(&r, (DWORD)st, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2, y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
    if (x < mi.rcWork.left) { x = mi.rcWork.left; }
    if (y < mi.rcWork.top) { y = mi.rcWork.top; }
    (o_SetWindowPos ? o_SetWindowPos : SetWindowPos)(h, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetForegroundWindow(h); SetFocus(h); SetActiveWindow(h);
}

static LRESULT CALLBACK hook_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    WNDPROC old = G.oldproc;
    switch (m) {
    case WM_ERASEBKGND: if (G.primary) return 1; break;
    case WM_PAINT: if (G.primary) { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); present(); return 0; } break;
    case WM_SIZE: G.dirty = true; break;
    case WM_NCDESTROY: SetWindowLongPtr(h, GWLP_WNDPROC, (LONG_PTR)old); G.oldproc = 0; G.hwnd = 0; return CallWindowProc(old, h, m, w, l);
    default:
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && G.primary && g_client_w > 0) {
            RECT d; int cw2, ch2; picture_rect(h, &d, &cw2, &ch2);
            int dw = d.right - d.left, dh = d.bottom - d.top;
            if (dw > 0 && dh > 0 && (dw != G.primary->w || dh != G.primary->h || d.left || d.top)) {
                int x = (short)LOWORD(l), y = (short)HIWORD(l);
                x = (x - d.left) * G.primary->w / dw; y = (y - d.top) * G.primary->h / dh;
                x = std::max(0, std::min(x, G.primary->w - 1)); y = std::max(0, std::min(y, G.primary->h - 1));
                l = MAKELPARAM(x, y);
            }
        }
    }
    return old ? CallWindowProc(old, h, m, w, l) : DefWindowProc(h, m, w, l);
}

static void hook_window(HWND h) {
    if (!h || h == G.hwnd) return;
    if (G.hwnd && G.oldproc) SetWindowLongPtr(G.hwnd, GWLP_WNDPROC, (LONG_PTR)G.oldproc);
    G.hwnd = h; G.oldproc = (WNDPROC)SetWindowLongPtr(h, GWLP_WNDPROC, (LONG_PTR)hook_proc);
}

/* ------------------------------------------------------------------ Surface implementation */
static void clip_rect(RECT *r, int w, int h) { if (r->left < 0) r->left = 0; if (r->top < 0) r->top = 0; if (r->right > w) r->right = w; if (r->bottom > h) r->bottom = h; }

Surface::~Surface() {
    release_gdi();
    for (Surface *a : attached) { a->parent = 0; a->v7.Release(); }
    if (pal) pal->Release();
    if (clip) clip->Release();
    if (ownmem && mem) VirtualFree(mem, 0, MEM_RELEASE);
    if (G.primary == this) G.primary = 0;
}
void Surface::release_gdi() { if (dc) { SelectObject(dc, olddib); DeleteDC(dc); dc = 0; } if (dib) { DeleteObject(dib); dib = 0; } dibbits = 0; }

HRESULT Surface::V7::QueryInterface(REFIID r, void **o) {
    *o = 0;
    if (r == IID_IUnknown || r == IID_IDirectDrawSurface7 || r == IID_IDirectDrawSurface4) *o = &s->v7;
    else if (r == IID_IDirectDrawSurface || r == IID_IDirectDrawSurface2 || r == IID_IDirectDrawSurface3) *o = &s->v3;
    else return E_NOINTERFACE;   /* IDirect3DTexture etc. are not offered */
    AddRef(); return S_OK;
}
ULONG Surface::V7::AddRef() { return InterlockedIncrement(&s->ref); }
ULONG Surface::V7::Release() { LONG n = InterlockedDecrement(&s->ref); if (!n) { Guard l; delete s; } return n; }

HRESULT Surface::V7::AddAttachedSurface(LPDIRECTDRAWSURFACE7 a) {
    if (!a) { return DDERR_INVALIDPARAMS; }
    Surface *t = ((V7 *)a)->s;
    Guard l; s->attached.push_back(t); t->v7.AddRef(); t->parent = s; return DD_OK;
}
HRESULT Surface::V7::DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE7 a) {
    Guard l; Surface *t = a ? ((V7 *)a)->s : 0;
    for (size_t i = 0; i < s->attached.size(); i++) if (!t || s->attached[i] == t) { Surface *x = s->attached[i]; s->attached.erase(s->attached.begin() + i); x->parent = 0; x->v7.Release(); return DD_OK; }
    return DDERR_SURFACENOTATTACHED;
}
static bool caps_match(const Surface *a, DWORD want) { return (a->caps & want) == want; }
HRESULT Surface::V7::GetAttachedSurface(LPDDSCAPS2 c, LPDIRECTDRAWSURFACE7 *o) {
    Guard l;
    for (Surface *a : s->attached) if (caps_match(a, c->dwCaps)) { a->v7.AddRef(); *o = &a->v7; return DD_OK; }
    *o = 0; return DDERR_NOTFOUND;
}
HRESULT Surface::V3::GetAttachedSurface(LPDDSCAPS c, LPDIRECTDRAWSURFACE3 *o) {
    Guard l;
    for (Surface *a : s->attached) if (caps_match(a, c->dwCaps)) { a->v7.AddRef(); *o = &a->v3; return DD_OK; }
    *o = 0; return DDERR_NOTFOUND;
}
HRESULT Surface::V7::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK7 cb) {
    std::vector<Surface *> list; { Guard l; list = s->attached; }
    for (Surface *a : list) { a->v7.AddRef(); DDSURFACEDESC2 d = a->desc; d.dwSize = sizeof d; if (cb(&a->v7, &d, ctx) == DDENUMRET_CANCEL) break; }
    return DD_OK;
}
HRESULT Surface::V3::EnumAttachedSurfaces(LPVOID ctx, LPDDENUMSURFACESCALLBACK cb) {
    std::vector<Surface *> list; { Guard l; list = s->attached; }
    for (Surface *a : list) { a->v7.AddRef(); DDSURFACEDESC d; from2(&a->desc, &d); if (cb((LPDIRECTDRAWSURFACE)&a->v3, &d, ctx) == DDENUMRET_CANCEL) break; }
    return DD_OK;
}
HRESULT Surface::V3::GetSurfaceDesc(LPDDSURFACEDESC d) { from2(&s->desc, d); return DD_OK; }
HRESULT Surface::V3::Lock(LPRECT r, LPDDSURFACEDESC d, DWORD f, HANDLE e) {
    DDSURFACEDESC2 d2; memset(&d2, 0, sizeof d2); d2.dwSize = sizeof d2;
    HRESULT hr = s->v7.Lock(r, &d2, f, e); if (hr == DD_OK) from2(&d2, d); return hr;
}

HRESULT Surface::V7::Lock(LPRECT r, LPDDSURFACEDESC2 d, DWORD lf, HANDLE) {
    Guard l; LOGU(0x4000000u ^ lf ^ (s->isprimary() ? 0x80000000u : 0), "Lock %s flags=0x%lx", s->isprimary() ? "primary" : "surface", lf); if (!s->mem) return DDERR_SURFACELOST;
    if (s->locked) return DDERR_SURFACEBUSY;
    s->locked = true;
    if (d) {
        *d = s->desc; d->dwSize = sizeof *d;
        d->dwFlags |= DDSD_LPSURFACE | DDSD_PITCH | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_CAPS;
        d->lPitch = s->pitch; d->dwWidth = s->w; d->dwHeight = s->h;
        BYTE *p = s->mem; if (r) p += (size_t)r->top * s->pitch + (size_t)r->left * (s->fmt.bpp / 8);
        d->lpSurface = p; pf_from_fmt(s->fmt, d->ddpfPixelFormat);
    }
    return DD_OK;
}
HRESULT Surface::V7::Unlock(LPRECT) {
    Guard l; if (!s->locked) return DDERR_NOTLOCKED; s->locked = false;
    if (s->isprimary()) present();
    return DD_OK;
}
HRESULT Surface::V7::GetColorKey(DWORD f, LPDDCOLORKEY k) {
    if (f & DDCKEY_SRCBLT) { if (!s->has_src) return DDERR_NOCOLORKEY; *k = s->ck_src_blt; return DD_OK; }
    if (f & DDCKEY_DESTBLT) { if (!s->has_dst) return DDERR_NOCOLORKEY; *k = s->ck_dst_blt; return DD_OK; }
    return DDERR_NOCOLORKEY;
}
HRESULT Surface::V7::SetColorKey(DWORD f, LPDDCOLORKEY k) {
    if (f & DDCKEY_SRCBLT) { s->has_src = k != 0; if (k) s->ck_src_blt = *k; }
    else if (f & DDCKEY_DESTBLT) { s->has_dst = k != 0; if (k) s->ck_dst_blt = *k; }
    return DD_OK;
}
HRESULT Surface::V7::SetPalette(LPDIRECTDRAWPALETTE p) { Palette *n = (Palette *)p; if (n) n->AddRef(); if (s->pal) s->pal->Release(); s->pal = n; if (s->isprimary()) present(); return DD_OK; }
HRESULT Surface::V7::SetClipper(LPDIRECTDRAWCLIPPER c) { Clipper *n = (Clipper *)c; if (n) n->AddRef(); if (s->clip) s->clip->Release(); s->clip = n; return DD_OK; }

HRESULT Surface::V7::GetDC(HDC *out) {
    Guard l; if (!s->mem) return DDERR_SURFACELOST;
    if (s->dc) return DDERR_DCALREADYCREATED;
    int bpp = s->fmt.bpp; if (bpp != 16 && bpp != 32 && bpp != 24 && bpp != 8) return DDERR_CANTCREATEDC;
    struct { BITMAPINFOHEADER h; DWORD m[3]; RGBQUAD pal[256]; } bi; memset(&bi, 0, sizeof bi);
    bi.h.biSize = sizeof bi.h; bi.h.biWidth = s->w; bi.h.biHeight = -s->h; bi.h.biPlanes = 1; bi.h.biBitCount = (WORD)bpp;
    if (bpp == 16 || bpp == 32) { bi.h.biCompression = BI_BITFIELDS; bi.m[0] = s->fmt.rm; bi.m[1] = s->fmt.gm; bi.m[2] = s->fmt.bm; }
    if (bpp == 8) { for (int i = 0; i < 256; i++) { if (s->pal) { bi.pal[i].rgbRed = s->pal->e[i].peRed; bi.pal[i].rgbGreen = s->pal->e[i].peGreen; bi.pal[i].rgbBlue = s->pal->e[i].peBlue; } } bi.h.biClrUsed = 256; }
    s->dib = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &s->dibbits, NULL, 0);
    if (!s->dib) return DDERR_CANTCREATEDC;
    s->dc = CreateCompatibleDC(NULL); s->olddib = SelectObject(s->dc, s->dib);
    int rowbytes = ((s->w * bpp + 31) / 32) * 4;
    for (int y = 0; y < s->h; y++) memcpy((BYTE *)s->dibbits + (size_t)y * rowbytes, s->mem + (size_t)y * s->pitch, std::min(rowbytes, s->pitch));
    *out = s->dc; return DD_OK;
}
HRESULT Surface::V7::ReleaseDC(HDC) {
    Guard l; if (!s->dc) return DDERR_INVALIDPARAMS;
    int bpp = s->fmt.bpp, rowbytes = ((s->w * bpp + 31) / 32) * 4;
    GdiFlush();
    for (int y = 0; y < s->h; y++) memcpy(s->mem + (size_t)y * s->pitch, (BYTE *)s->dibbits + (size_t)y * rowbytes, std::min(rowbytes, s->pitch));
    s->release_gdi(); if (s->isprimary()) present(); return DD_OK;
}

HRESULT Surface::V7::Flip(LPDIRECTDRAWSURFACE7 target, DWORD fl) {
    Guard l; LOGU(0x3000000u ^ fl, "Flip flags=0x%lx", fl);
    Surface *front = s; Surface *to = target ? ((V7 *)target)->s : 0;
    if (!to) { for (Surface *a : front->attached) if (a->caps & DDSCAPS_BACKBUFFER) { to = a; break; } }
    if (!to) return DDERR_NOTFLIPPABLE;
    /* rotate buffers along the chain front -> b1 -> b2 ... -> front so that 'to' becomes the new front */
    std::vector<Surface *> chain; chain.push_back(front);
    for (Surface *c = front; ;) { Surface *nx = 0; for (Surface *a : c->attached) if (a->caps & DDSCAPS_BACKBUFFER) { nx = a; break; } if (!nx || nx == front) break; chain.push_back(nx); c = nx; }
    if (to != chain[1 % chain.size()] && chain.size() > 2) { for (size_t i = 1; i < chain.size(); i++) if (chain[i] == to) { std::rotate(chain.begin() + 1, chain.begin() + i, chain.end()); break; } }
    BYTE *first = chain[0]->mem;
    for (size_t i = 0; i + 1 < chain.size(); i++) chain[i]->mem = chain[i + 1]->mem;
    chain.back()->mem = first;
    if (front->isprimary()) present();
    return DD_OK;
}

static bool in_chain_front(Surface *s) { return s->isprimary(); }

/* generic blit */
static HRESULT do_blit(Surface *dst, RECT *dr, Surface *src, RECT *sr, DWORD flags, DDBLTFX *fx, bool fast_ck_src, bool fast_ck_dst) {
    if (!dst->mem) return DDERR_SURFACELOST;
    RECT d = dr ? *dr : RECT{0, 0, dst->w, dst->h};
    int dbpp = dst->fmt.bpp / 8;
    if (flags & DDBLT_COLORFILL) {
        DWORD c = fx ? fx->dwFillColor : 0; RECT cr = d; clip_rect(&cr, dst->w, dst->h);
        for (int y = cr.top; y < cr.bottom; y++) { BYTE *p = dst->mem + (size_t)y * dst->pitch + (size_t)cr.left * dbpp; for (int x = cr.left; x < cr.right; x++, p += dbpp) putpx(p, dst->fmt.bpp, c); }
        return DD_OK;
    }
    if ((flags & DDBLT_ROP) && fx && !src) {
        DWORD rop = fx->dwROP; if (rop == BLACKNESS || rop == WHITENESS) { RECT cr = d; clip_rect(&cr, dst->w, dst->h); DWORD c = rop == WHITENESS ? 0xFFFFFFFFu : 0; for (int y = cr.top; y < cr.bottom; y++) { BYTE *p = dst->mem + (size_t)y * dst->pitch + (size_t)cr.left * dbpp; for (int x = cr.left; x < cr.right; x++, p += dbpp) putpx(p, dst->fmt.bpp, c); } }
        return DD_OK;
    }
    if (!src || !src->mem) return DDERR_INVALIDPARAMS;
    RECT s = sr ? *sr : RECT{0, 0, src->w, src->h};
    int sw = s.right - s.left, sh = s.bottom - s.top, dw = d.right - d.left, dh = d.bottom - d.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return DD_OK;
    bool mirx = fx && (flags & DDBLT_DDFX) && (fx->dwDDFX & DDBLTFX_MIRRORLEFTRIGHT), miry = fx && (flags & DDBLT_DDFX) && (fx->dwDDFX & DDBLTFX_MIRRORUPDOWN);
    bool use_src_ck = (flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) || fast_ck_src, use_dst_ck = (flags & (DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE)) || fast_ck_dst;
    DDCOLORKEY sk = src->ck_src_blt, dk = dst->ck_dst_blt;
    if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) sk = fx->ddckSrcColorkey;
    if ((flags & DDBLT_KEYDESTOVERRIDE) && fx) dk = fx->ddckDestColorkey;
    if (use_src_ck && !(flags & DDBLT_KEYSRCOVERRIDE) && !src->has_src) use_src_ck = false;
    if (use_dst_ck && !(flags & DDBLT_KEYDESTOVERRIDE) && !dst->has_dst) use_dst_ck = false;
    bool same = same_fmt(src->fmt, dst->fmt), plain = same && sw == dw && sh == dh && !mirx && !miry && !use_src_ck && !use_dst_ck;
    /* clip destination to the surface, adjusting the source proportionally */
    int dx0 = std::max(0, (int)-d.left), dy0 = std::max(0, (int)-d.top), dx1 = std::min(dw, (int)(dst->w - d.left)), dy1 = std::min(dh, (int)(dst->h - d.top));
    if (dx1 <= dx0 || dy1 <= dy0) return DD_OK;
    int sbpp = src->fmt.bpp / 8;
    for (int y = dy0; y < dy1; y++) {
        int sy = miry ? s.top + (dh - 1 - y) * sh / dh : s.top + y * sh / dh; if (sy < 0 || sy >= src->h) continue;
        const BYTE *srow = src->mem + (size_t)sy * src->pitch; BYTE *drow = dst->mem + (size_t)(d.top + y) * dst->pitch;
        if (plain) { int x0 = dx0, x1 = std::min(dx1, (int)(src->w - s.left)); if (x1 > x0 && s.left + x0 >= 0) memmove(drow + (size_t)(d.left + x0) * dbpp, srow + (size_t)(s.left + x0) * sbpp, (size_t)(x1 - x0) * dbpp); continue; }
        for (int x = dx0; x < dx1; x++) {
            int sx = mirx ? s.left + (dw - 1 - x) * sw / dw : s.left + x * sw / dw; if (sx < 0 || sx >= src->w) continue;
            DWORD sv = getpx(srow + (size_t)sx * sbpp, src->fmt.bpp);
            if (use_src_ck && sv >= sk.dwColorSpaceLowValue && sv <= sk.dwColorSpaceHighValue) continue;
            BYTE *dp = drow + (size_t)(d.left + x) * dbpp;
            if (use_dst_ck) { DWORD dv = getpx(dp, dst->fmt.bpp); if (dv < dk.dwColorSpaceLowValue || dv > dk.dwColorSpaceHighValue) continue; }
            if (same) putpx(dp, dst->fmt.bpp, sv);
            else { int r, g, b, a; unpack(src->fmt, sv, src->pal, r, g, b, a); putpx(dp, dst->fmt.bpp, pack(dst->fmt, r, g, b, a)); }
        }
    }
    return DD_OK;
}

HRESULT Surface::V7::Blt(LPRECT d, LPDIRECTDRAWSURFACE7 src, LPRECT sr, DWORD f, LPDDBLTFX fx) {
    Guard l; Surface *ss = src ? ((V7 *)src)->s : 0;
    LOGU(0x1000000u ^ f ^ (s->isprimary() ? 0x80000000u : 0), "Blt dst=%s flags=0x%lx src=%s%s", s->isprimary() ? "primary" : "surface", f, ss ? (ss->isprimary() ? "primary" : "surface") : "none", (f & DDBLT_COLORFILL) && fx ? "" : "");
    if ((f & DDBLT_COLORFILL) && fx) LOGU(0x2000000u ^ fx->dwFillColor ^ (s->isprimary() ? 0x4000000u : 0), "  colorfill 0x%08lx on %s %dx%d", fx->dwFillColor, s->isprimary() ? "primary" : "surface", s->w, s->h);
    HRESULT hr = do_blit(s, d, ss, sr, f, fx, false, false);
    if (hr == DD_OK && s->isprimary()) present();
    return hr;
}
HRESULT Surface::V7::BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE7 src, LPRECT sr, DWORD t) {
    Guard l; Surface *ss = src ? ((V7 *)src)->s : 0; if (!ss) return DDERR_INVALIDPARAMS;
    RECT sRect = sr ? *sr : RECT{0, 0, ss->w, ss->h}; RECT d = {(LONG)x, (LONG)y, (LONG)x + (sRect.right - sRect.left), (LONG)y + (sRect.bottom - sRect.top)};
    HRESULT hr = do_blit(s, &d, ss, &sRect, 0, 0, (t & DDBLTFAST_SRCCOLORKEY) != 0, (t & DDBLTFAST_DESTCOLORKEY) != 0);
    if (hr == DD_OK && s->isprimary()) present();
    return hr;
}

/* ------------------------------------------------------------------ DirectDraw object */
static const struct { int w, h; } k_modes[] = {{320, 200}, {320, 240}, {400, 300}, {512, 384}, {640, 400}, {640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 960}, {1280, 1024}, {1600, 1200}, {1920, 1080}, {2048, 1536}};
static const int k_bpps[] = {8, 16, 32};

struct DDraw {
    LONG ref = 1;
    struct V7 : IDirectDraw7 { DDraw *d;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void **o) override;
        ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&d->ref); }
        ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&d->ref); if (!n) delete d; return n; }
        HRESULT STDMETHODCALLTYPE Compact() override { return DDERR_NOEXCLUSIVEMODE; }
        HRESULT STDMETHODCALLTYPE CreateClipper(DWORD, LPDIRECTDRAWCLIPPER *c, IUnknown *) override { *c = new Clipper(); return DD_OK; }
        HRESULT STDMETHODCALLTYPE CreatePalette(DWORD f, LPPALETTEENTRY e, LPDIRECTDRAWPALETTE *p, IUnknown *) override { Palette *n = new Palette(); n->caps = f; if (e) memcpy(n->e, e, (f & DDPCAPS_8BIT ? 256 : f & DDPCAPS_4BIT ? 16 : 256) * sizeof(PALETTEENTRY)); *p = n; return DD_OK; }
        HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC2 sd, LPDIRECTDRAWSURFACE7 *o, IUnknown *) override;
        HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE7, LPDIRECTDRAWSURFACE7 *) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD f, LPDDSURFACEDESC2 sd, LPVOID ctx, LPDDENUMMODESCALLBACK2 cb) override;
        HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC2, LPVOID, LPDDENUMSURFACESCALLBACK7) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS drv, LPDDCAPS hel) override;
        HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC2 sd) override;
        HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD n, LPDWORD) override { *n = 0; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE7 *o) override { if (!G.primary) return DDERR_NOTFOUND; G.primary->v7.AddRef(); *o = &G.primary->v7; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD f) override { *f = 60; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD l) override { *l = 0; return DDERR_VERTICALBLANKINPROGRESS; }
        HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(WINBOOL *b) override { *b = FALSE; return DD_OK; }
        HRESULT STDMETHODCALLTYPE Initialize(GUID *) override { return DDERR_ALREADYINITIALIZED; }
        HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override;
        HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD, DWORD) override;
        HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override { Sleep(1); return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetAvailableVidMem(LPDDSCAPS2, LPDWORD t, LPDWORD f) override { if (t) *t = 64u << 20; if (f) *f = 48u << 20; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetSurfaceFromDC(HDC, LPDIRECTDRAWSURFACE7 *) override { return DDERR_NOTFOUND; }
        HRESULT STDMETHODCALLTYPE RestoreAllSurfaces() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetDeviceIdentifier(LPDDDEVICEIDENTIFIER2 id, DWORD) override {
            memset(id, 0, sizeof *id); strcpy(id->szDriver, "tpw-ddraw"); strcpy(id->szDescription, "TPW DirectDraw layer (software)"); id->dwVendorId = 0; id->dwDeviceId = 0; return DD_OK; }
        HRESULT STDMETHODCALLTYPE StartModeTest(LPSIZE, DWORD, DWORD) override { return DDERR_NOTFOUND; }
        HRESULT STDMETHODCALLTYPE EvaluateMode(DWORD, DWORD *) override { return DDERR_NOTFOUND; }
    } v7;
    struct V2 : IDirectDraw2 { DDraw *d;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void **o) override { return d->v7.QueryInterface(r, o); }
        ULONG STDMETHODCALLTYPE AddRef() override { return d->v7.AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return d->v7.Release(); }
        HRESULT STDMETHODCALLTYPE Compact() override { return DDERR_NOEXCLUSIVEMODE; }
        HRESULT STDMETHODCALLTYPE CreateClipper(DWORD f, LPDIRECTDRAWCLIPPER *c, IUnknown *u) override { return d->v7.CreateClipper(f, c, u); }
        HRESULT STDMETHODCALLTYPE CreatePalette(DWORD f, LPPALETTEENTRY e, LPDIRECTDRAWPALETTE *p, IUnknown *u) override { return d->v7.CreatePalette(f, e, p, u); }
        HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC sd, LPDIRECTDRAWSURFACE *o, IUnknown *u) override;
        HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE *) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD f, LPDDSURFACEDESC sd, LPVOID ctx, LPDDENUMMODESCALLBACK cb) override;
        HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS a, LPDDCAPS b) override { return d->v7.GetCaps(a, b); }
        HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC sd) override { DDSURFACEDESC2 d2; memset(&d2, 0, sizeof d2); d2.dwSize = sizeof d2; HRESULT hr = d->v7.GetDisplayMode(&d2); from2(&d2, sd); return hr; }
        HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD n, LPDWORD) override { *n = 0; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE *o) override { if (!G.primary) return DDERR_NOTFOUND; G.primary->v7.AddRef(); *o = (LPDIRECTDRAWSURFACE)&G.primary->v3; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD f) override { *f = 60; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD l) override { *l = 0; return DDERR_VERTICALBLANKINPROGRESS; }
        HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(WINBOOL *b) override { *b = FALSE; return DD_OK; }
        HRESULT STDMETHODCALLTYPE Initialize(GUID *) override { return DDERR_ALREADYINITIALIZED; }
        HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override { return d->v7.SetCooperativeLevel(h, f); }
        HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD b, DWORD r, DWORD f) override { return d->v7.SetDisplayMode(w, h, b, r, f); }
        HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override { Sleep(1); return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetAvailableVidMem(LPDDSCAPS, LPDWORD t, LPDWORD f) override { if (t) *t = 64u << 20; if (f) *f = 48u << 20; return DD_OK; }
    } v2;
    struct V1 : IDirectDraw { DDraw *d;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void **o) override { return d->v7.QueryInterface(r, o); }
        ULONG STDMETHODCALLTYPE AddRef() override { return d->v7.AddRef(); }
        ULONG STDMETHODCALLTYPE Release() override { return d->v7.Release(); }
        HRESULT STDMETHODCALLTYPE Compact() override { return DDERR_NOEXCLUSIVEMODE; }
        HRESULT STDMETHODCALLTYPE CreateClipper(DWORD f, LPDIRECTDRAWCLIPPER *c, IUnknown *u) override { return d->v7.CreateClipper(f, c, u); }
        HRESULT STDMETHODCALLTYPE CreatePalette(DWORD f, LPPALETTEENTRY e, LPDIRECTDRAWPALETTE *p, IUnknown *u) override { return d->v7.CreatePalette(f, e, p, u); }
        HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC sd, LPDIRECTDRAWSURFACE *o, IUnknown *u) override { return d->v2.CreateSurface(sd, o, u); }
        HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE *) override { return DDERR_UNSUPPORTED; }
        HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD f, LPDDSURFACEDESC sd, LPVOID ctx, LPDDENUMMODESCALLBACK cb) override { return d->v2.EnumDisplayModes(f, sd, ctx, cb); }
        HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE FlipToGDISurface() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS a, LPDDCAPS b) override { return d->v7.GetCaps(a, b); }
        HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC sd) override { return d->v2.GetDisplayMode(sd); }
        HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD n, LPDWORD) override { *n = 0; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE *o) override { return d->v2.GetGDISurface(o); }
        HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD f) override { *f = 60; return DD_OK; }
        HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD l) override { *l = 0; return DDERR_VERTICALBLANKINPROGRESS; }
        HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(WINBOOL *b) override { *b = FALSE; return DD_OK; }
        HRESULT STDMETHODCALLTYPE Initialize(GUID *) override { return DDERR_ALREADYINITIALIZED; }
        HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override { return DD_OK; }
        HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override { return d->v7.SetCooperativeLevel(h, f); }
        HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD b) override { return d->v7.SetDisplayMode(w, h, b, 0, 0); }
        HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) override { Sleep(1); return DD_OK; }
    } v1;
    DDraw() { v7.d = this; v2.d = this; v1.d = this; }
};

HRESULT DDraw::V7::QueryInterface(REFIID r, void **o) {
    *o = 0;
    if (r == IID_IUnknown || r == IID_IDirectDraw7 || r == IID_IDirectDraw4) *o = &d->v7;
    else if (r == IID_IDirectDraw2) *o = &d->v2;
    else if (r == IID_IDirectDraw) *o = &d->v1;
    else { LOG("DD::QueryInterface refused {%08lx-%04x-%04x}", r.Data1, r.Data2, r.Data3); return E_NOINTERFACE; }   /* IDirect3D* -> software renderer path */
    AddRef(); return S_OK;
}

HRESULT DDraw::V7::SetCooperativeLevel(HWND h, DWORD f) {
    Guard l; LOG("SetCooperativeLevel hwnd=%p flags=0x%lx", h, f);
    G.coop = f; if (h) hook_window(h);
    return DD_OK;
}
HRESULT DDraw::V7::SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD, DWORD) {
    Guard l; LOG("SetDisplayMode %lux%lux%lu", w, h, bpp);
    if (!w || !h || (bpp != 8 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32)) return DDERR_INVALIDMODE;
    G.w = w; G.h = h; G.bpp = bpp; apply_window();
    return DD_OK;
}
HRESULT DDraw::V7::GetDisplayMode(LPDDSURFACEDESC2 sd) {
    int w = G.w, h = G.h, bpp = G.bpp;
    if (!w) { w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); bpp = 32; }
    memset(sd, 0, sizeof *sd); sd->dwSize = sizeof *sd; sd->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    sd->dwWidth = w; sd->dwHeight = h; sd->lPitch = w * (bpp == 15 ? 2 : bpp / 8); sd->dwRefreshRate = 60; Fmt f = display_fmt(bpp); pf_from_fmt(f, sd->ddpfPixelFormat); return DD_OK;
}
HRESULT DDraw::V7::EnumDisplayModes(DWORD, LPDDSURFACEDESC2, LPVOID ctx, LPDDENUMMODESCALLBACK2 cb) {
    for (auto &m : k_modes) for (int b : k_bpps) {
        DDSURFACEDESC2 sd; memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd; sd.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        sd.dwWidth = m.w; sd.dwHeight = m.h; sd.lPitch = m.w * b / 8; sd.dwRefreshRate = 60; Fmt f = display_fmt(b); pf_from_fmt(f, sd.ddpfPixelFormat);
        if (cb(&sd, ctx) == DDENUMRET_CANCEL) return DD_OK;
    }
    return DD_OK;
}
HRESULT DDraw::V2::EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID ctx, LPDDENUMMODESCALLBACK cb) {
    for (auto &m : k_modes) for (int b : k_bpps) {
        DDSURFACEDESC2 s2; memset(&s2, 0, sizeof s2); s2.dwSize = sizeof s2; s2.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
        s2.dwWidth = m.w; s2.dwHeight = m.h; s2.lPitch = m.w * b / 8; s2.dwRefreshRate = 60; Fmt f = display_fmt(b); pf_from_fmt(f, s2.ddpfPixelFormat);
        DDSURFACEDESC sd; from2(&s2, &sd); if (cb(&sd, ctx) == DDENUMRET_CANCEL) return DD_OK;
    }
    return DD_OK;
}
HRESULT DDraw::V7::GetCaps(LPDDCAPS drv, LPDDCAPS hel) {
    auto fill = [&](LPDDCAPS c) {
        if (!c) { return; }
        DWORD sz = c->dwSize ? c->dwSize : sizeof(DDCAPS); memset(c, 0, sz); c->dwSize = sz;
        c->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_COLORKEY | DDCAPS_BLTSTRETCH | DDCAPS_CANBLTSYSMEM | DDCAPS_PALETTE | DDCAPS_NOHARDWARE * 0;
        c->dwCKeyCaps = DDCKEYCAPS_SRCBLT | DDCKEYCAPS_DESTBLT;
        c->dwVidMemTotal = 64u << 20; c->dwVidMemFree = 48u << 20; c->ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY;
    };
    fill(drv); fill(hel); return DD_OK;
}

HRESULT DDraw::V7::CreateSurface(LPDDSURFACEDESC2 sd, LPDIRECTDRAWSURFACE7 *out, IUnknown *) {
    Guard l; if (!sd || !out) return DDERR_INVALIDPARAMS;
    DWORD fl = sd->dwFlags, caps = (fl & DDSD_CAPS) ? sd->ddsCaps.dwCaps : 0, caps2 = (fl & DDSD_CAPS) ? sd->ddsCaps.dwCaps2 : 0;
    LOG("CreateSurface flags=0x%lx caps=0x%lx %lux%lu bb=%lu", fl, caps, (fl & DDSD_WIDTH) ? sd->dwWidth : 0, (fl & DDSD_HEIGHT) ? sd->dwHeight : 0, (fl & DDSD_BACKBUFFERCOUNT) ? sd->dwBackBufferCount : 0);
    auto make = [&](int w, int h, const Fmt &f, DWORD c, DWORD c2) -> Surface * {
        Surface *s = new Surface(); s->dd = 0; s->w = w; s->h = h; s->fmt = f; s->caps = c; s->caps2 = c2;
        int bytes = f.bpp > 0 ? (f.bpp + 7) / 8 : 2; s->pitch = (w * bytes + 3) & ~3;
        size_t sz = (size_t)s->pitch * h; s->mem = (BYTE *)VirtualAlloc(NULL, sz ? sz : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!s->mem) { delete s; return 0; }
        s->desc.dwSize = sizeof s->desc; s->desc.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
        s->desc.dwWidth = w; s->desc.dwHeight = h; s->desc.lPitch = s->pitch; pf_from_fmt(f, s->desc.ddpfPixelFormat);
        s->desc.ddsCaps.dwCaps = c; s->desc.ddsCaps.dwCaps2 = c2; return s;
    };
    Surface *s = 0;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        int w = G.w ? G.w : GetSystemMetrics(SM_CXSCREEN), h = G.h ? G.h : GetSystemMetrics(SM_CYSCREEN), bpp = G.bpp ? G.bpp : 32;
        if (!G.w && G.hwnd) { RECT r; GetClientRect(G.hwnd, &r); if (r.right > 0 && r.bottom > 0) { w = r.right; h = r.bottom; } }
        s = make(w, h, display_fmt(bpp), caps, caps2); if (!s) return DDERR_OUTOFMEMORY;
        int nb = (fl & DDSD_BACKBUFFERCOUNT) ? (int)sd->dwBackBufferCount : 0;
        Surface *prev = s;
        for (int i = 0; i < nb; i++) {
            Surface *b = make(w, h, display_fmt(bpp), DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY * 0, 0); if (!b) { s->v7.Release(); return DDERR_OUTOFMEMORY; }
            prev->attached.push_back(b); b->parent = prev; prev = b;   /* parent owns the reference (ref starts at 1) */
        }
        if (nb) s->caps |= DDSCAPS_FRONTBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX, s->desc.ddsCaps.dwCaps = s->caps;
        s->dd = this->d; G.primary = s; G.pw = w; G.ph = h;
        if (nb) { for (Surface *a : s->attached) a->dd = this->d; }
        memset(s->mem, 0, (size_t)s->pitch * s->h);
    } else {
        int w = (fl & DDSD_WIDTH) ? sd->dwWidth : 0, h = (fl & DDSD_HEIGHT) ? sd->dwHeight : 0;
        Fmt f;
        if (caps & DDSCAPS_ZBUFFER) { f.bpp = (fl & DDSD_ZBUFFERBITDEPTH) ? (int)sd->dwMipMapCount : 16; if (fl & DDSD_PIXELFORMAT) f = fmt_from_pf(sd->ddpfPixelFormat); f.flags = DDPF_ZBUFFER; }
        else if (fl & DDSD_PIXELFORMAT) f = fmt_from_pf(sd->ddpfPixelFormat);
        else f = display_fmt(G.bpp ? G.bpp : 32);
        if (w <= 0 || h <= 0) return DDERR_INVALIDPARAMS;
        s = make(w, h, f, caps, caps2); if (!s) return DDERR_OUTOFMEMORY; s->dd = this->d;
        if ((fl & DDSD_CKSRCBLT)) { s->ck_src_blt = sd->ddckCKSrcBlt; s->has_src = true; }
        if ((fl & DDSD_CKDESTBLT)) { s->ck_dst_blt = sd->ddckCKDestBlt; s->has_dst = true; }
        if ((fl & DDSD_MIPMAPCOUNT) && (caps & DDSCAPS_MIPMAP) && sd->dwMipMapCount > 1) {
            Surface *prev = s; int mw = w, mh = h;
            for (DWORD i = 1; i < sd->dwMipMapCount && mw > 1 && mh > 1; i++) { mw >>= 1; mh >>= 1; Surface *m = make(mw, mh, f, caps & ~DDSCAPS_ALLOCONLOAD, caps2); if (!m) break; m->dd = this->d; prev->attached.push_back(m); m->parent = prev; prev = m; }
        }
    }
    *out = &s->v7; return DD_OK;
}
HRESULT DDraw::V2::CreateSurface(LPDDSURFACEDESC sd, LPDIRECTDRAWSURFACE *o, IUnknown *u) {
    DDSURFACEDESC2 d2; to2(sd, &d2); LPDIRECTDRAWSURFACE7 p = 0;
    HRESULT hr = d->v7.CreateSurface(&d2, &p, u); if (hr == DD_OK) *o = (LPDIRECTDRAWSURFACE)&((Surface::V7 *)p)->s->v3; return hr;
}
HRESULT Surface::V7::GetDDInterface(LPVOID *o) { if (!s->dd) return DDERR_INVALIDOBJECT; s->dd->v7.AddRef(); *o = &s->dd->v7; return DD_OK; }

/* ------------------------------------------------------------------ exports */
static HRESULT create_dd(void **out, REFIID iid) {
    install_hooks();
    DDraw *d = new DDraw(); HRESULT hr = d->v7.QueryInterface(iid, out); d->v7.Release(); return hr;
}
extern "C" {
HRESULT WINAPI DirectDrawCreate(GUID *, LPDIRECTDRAW *out, IUnknown *) { LOG("DirectDrawCreate"); return create_dd((void **)out, IID_IDirectDraw); }
HRESULT WINAPI DirectDrawCreateEx(GUID *, LPVOID *out, REFIID iid, IUnknown *) { LOG("DirectDrawCreateEx"); return create_dd(out, iid); }
HRESULT WINAPI DirectDrawCreateClipper(DWORD, LPDIRECTDRAWCLIPPER *c, IUnknown *) { *c = new Clipper(); return DD_OK; }
HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) { cb(NULL, (LPSTR)"Primary Display Driver", (LPSTR)"display", ctx); return DD_OK; }
HRESULT WINAPI DirectDrawEnumerateW(LPDDENUMCALLBACKW cb, LPVOID ctx) { cb(NULL, (LPWSTR)L"Primary Display Driver", (LPWSTR)L"display", ctx); return DD_OK; }
HRESULT WINAPI DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD) { cb(NULL, (LPSTR)"Primary Display Driver", (LPSTR)"display", ctx, 0); return DD_OK; }
HRESULT WINAPI DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW cb, LPVOID ctx, DWORD) { cb(NULL, (LPWSTR)L"Primary Display Driver", (LPWSTR)L"display", ctx, 0); return DD_OK; }
void WINAPI AcquireDDThreadLock() { EnterCriticalSection(&cs); }
void WINAPI ReleaseDDThreadLock() { LeaveCriticalSection(&cs); }
HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, LPVOID *o) { *o = 0; return CLASS_E_CLASSNOTAVAILABLE; }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { self_mod = h; DisableThreadLibraryCalls(h); InitializeCriticalSection(&cs); load_cfg(); }
    return TRUE;
}
