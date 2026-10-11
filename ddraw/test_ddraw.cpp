// Exercises tpw-ddraw through the public API: mode set, flip chain, lock/blit/colour-key/palette/GDI DC.
// Run from the folder containing ddraw.dll. Exit code 0 = all checks passed.
#define WIN32_LEAN_AND_MEAN
#define DIRECTDRAW_VERSION 0x0700
#define DIRECTINPUT_VERSION 0x0500
#include <windows.h>
#include <ddraw.h>
#include <dinput.h>
#include <stdio.h>

static int fails;
static const GUID IID_D3D3 = {0xbb223240, 0xe72b, 0x11d0, {0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc1, 0xa7, 0xf0}};
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } else printf("ok:   %s\n", msg); } while (0)

static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProc(h, m, w, l); }
typedef HRESULT (WINAPI *PFN_Create)(GUID *, LPDIRECTDRAW *, IUnknown *);

int main(int argc, char **argv) {
    char dir[MAX_PATH]; GetModuleFileNameA(NULL, dir, MAX_PATH); *strrchr(dir, '\\') = 0;
    char dll[MAX_PATH]; snprintf(dll, sizeof dll, "%s\\ddraw.dll", dir);
    HMODULE m = LoadLibraryA(dll); CHECK(m != NULL, "load ddraw.dll from test folder");
    if (!m) return 1;
    PFN_Create create = (PFN_Create)GetProcAddress(m, "DirectDrawCreate");
    CHECK(create != NULL, "export DirectDrawCreate");

    WNDCLASSA wc = {0}; wc.lpfnWndProc = wp; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "tpwtest"; RegisterClassA(&wc);
    HWND hw = CreateWindowA("tpwtest", "tpw ddraw test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

    IDirectDraw *dd1 = NULL; CHECK(create(NULL, &dd1, NULL) == DD_OK && dd1, "DirectDrawCreate");
    IDirectDraw7 *dd = NULL; CHECK(dd1->QueryInterface(IID_IDirectDraw4, (void **)&dd) == S_OK && dd, "QI IDirectDraw4");
    IDirectDraw2 *dd2 = NULL; CHECK(dd1->QueryInterface(IID_IDirectDraw2, (void **)&dd2) == S_OK, "QI IDirectDraw2");
    void *junk = NULL; CHECK(dd1->QueryInterface(IID_D3D3, &junk) == E_NOINTERFACE, "Direct3D is refused (software path)");

    DDDEVICEIDENTIFIER2 id; CHECK(dd->GetDeviceIdentifier(&id, 0) == DD_OK, "GetDeviceIdentifier");
    CHECK(dd->SetCooperativeLevel(hw, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWMODEX | DDSCL_FPUSETUP) == DD_OK, "SetCooperativeLevel");
    CHECK(dd->SetDisplayMode(640, 480, 16, 0, 0) == DD_OK, "SetDisplayMode 640x480x16");
    DDSURFACEDESC2 cur; memset(&cur, 0, sizeof cur); cur.dwSize = sizeof cur;
    CHECK(dd->GetDisplayMode(&cur) == DD_OK && cur.dwWidth == 640 && cur.ddpfPixelFormat.dwRGBBitCount == 16, "GetDisplayMode reports 640x480x16");

    DDSURFACEDESC2 sd; memset(&sd, 0, sizeof sd); sd.dwSize = sizeof sd;
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT; sd.dwBackBufferCount = 1;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    IDirectDrawSurface7 *prim = NULL; CHECK(dd->CreateSurface(&sd, &prim, NULL) == DD_OK && prim, "CreateSurface primary + backbuffer");
    DDSCAPS2 caps; memset(&caps, 0, sizeof caps); caps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface7 *back = NULL; CHECK(prim->GetAttachedSurface(&caps, &back) == DD_OK && back, "GetAttachedSurface(BACKBUFFER)");

    // the game's view of the window must be the display mode, whatever the real window size is
    RECT cr; GetClientRect(hw, &cr); CHECK(cr.right == 640 && cr.bottom == 480, "hooked GetClientRect reports 640x480");
    CHECK(GetSystemMetrics(SM_CXSCREEN) == 640 && GetSystemMetrics(SM_CYSCREEN) == 480, "hooked GetSystemMetrics reports the display mode");
    BOOL (WINAPI *realGCR0)(HWND, LPRECT) = (BOOL (WINAPI *)(HWND, LPRECT))GetProcAddress(GetModuleHandleA("user32.dll"), "GetClientRect"); { RECT b4; realGCR0(hw, &b4); printf("      real client before game SetWindowPos: %ldx%ld\n", b4.right, b4.bottom); }
    SetWindowPos(hw, NULL, 5, 5, 123, 77, SWP_NOZORDER); RECT rr; BOOL (WINAPI *realGCR)(HWND, LPRECT) = (BOOL (WINAPI *)(HWND, LPRECT))GetProcAddress(GetModuleHandleA("user32.dll"), "GetClientRect"); realGCR(hw, &rr);
    CHECK(rr.right > 300, "game's SetWindowPos on the owned window is ignored");
    {   // mouse: a real client point (200,300) must reach the game as (200,300)/scale
        BOOL (WINAPI *realC2S)(HWND, LPPOINT) = (BOOL (WINAPI *)(HWND, LPPOINT))GetProcAddress(GetModuleHandleA("user32.dll"), "ClientToScreen");
        int scale = rr.right / 640; POINT o = {0, 0}; realC2S(hw, &o); POINT p = {o.x + 200, o.y + 300}; ScreenToClient(hw, &p);
        printf("      real client %ldx%ld (scale %d) -> ScreenToClient gives %ld,%ld\n", rr.right, rr.bottom, scale, p.x, p.y);
        CHECK(scale >= 1 && p.x == 200 / scale && p.y == 300 / scale, "hooked ScreenToClient maps the mouse into game coordinates");
    }

    // the game must not be able to maximize the window we own
    ShowWindow(hw, SW_SHOWMAXIMIZED); { MSG m2; for (int i = 0; i < 20; i++) { while (PeekMessage(&m2, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m2); DispatchMessage(&m2); } Sleep(5); } }
    CHECK(!IsZoomed(hw), "ShowWindow(SW_SHOWMAXIMIZED) on the owned window does not maximize it");
    // DirectInput mouse: exclusive request must work through the proxy (and be downgraded to shared)
    { LPDIRECTINPUTA di = NULL; HRESULT hr = DirectInputCreateA(GetModuleHandle(NULL), 0x0500, &di, NULL); CHECK(hr == DI_OK && di, "DirectInputCreateA through the proxy");
      LPDIRECTINPUTDEVICEA ms = NULL; if (di) hr = di->CreateDevice(GUID_SysMouse, &ms, NULL); CHECK(di && hr == DI_OK && ms, "CreateDevice(mouse)");
      if (ms) { CHECK(ms->SetDataFormat(&c_dfDIMouse) == DI_OK, "mouse SetDataFormat"); hr = ms->SetCooperativeLevel(hw, DISCL_EXCLUSIVE | DISCL_FOREGROUND); printf("      mouse SetCooperativeLevel(exclusive) -> 0x%08lx\n", (unsigned long)hr); CHECK(hr == DI_OK, "mouse SetCooperativeLevel accepted"); ms->Release(); }
      if (di) di->Release(); }

    // draw a test pattern into the back buffer
    DDSURFACEDESC2 ld; memset(&ld, 0, sizeof ld); ld.dwSize = sizeof ld;
    CHECK(back->Lock(NULL, &ld, DDLOCK_WAIT, NULL) == DD_OK, "Lock backbuffer");
    CHECK(ld.dwWidth == 640 && ld.dwHeight == 480 && ld.lPitch >= 1280 && ld.ddpfPixelFormat.dwRBitMask == 0xF800, "lock desc: size/pitch/565 masks");
    for (int y = 0; y < 480; y++) { WORD *row = (WORD *)((BYTE *)ld.lpSurface + y * ld.lPitch); for (int x = 0; x < 640; x++) row[x] = (x < 320) ? 0xF800 : ((y < 240) ? 0x07E0 : 0x001F); }
    CHECK(back->Unlock(NULL) == DD_OK, "Unlock backbuffer");

    // colour-keyed sprite: 32x32 magenta key with a white 16x16 centre, BltFast onto the back buffer at (10,10)
    DDSURFACEDESC2 od; memset(&od, 0, sizeof od); od.dwSize = sizeof od; od.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT; od.dwWidth = 32; od.dwHeight = 32;
    od.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    IDirectDrawSurface7 *spr = NULL; CHECK(dd->CreateSurface(&od, &spr, NULL) == DD_OK, "CreateSurface offscreen");
    DDBLTFX fx; memset(&fx, 0, sizeof fx); fx.dwSize = sizeof fx; fx.dwFillColor = 0xF81F; // magenta key
    CHECK(spr->Blt(NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx) == DD_OK, "Blt colour fill");
    RECT c = {8, 8, 24, 24}; fx.dwFillColor = 0xFFFF; CHECK(spr->Blt(&c, NULL, NULL, DDBLT_COLORFILL, &fx) == DD_OK, "Blt colour fill rect");
    DDCOLORKEY ck = {0xF81F, 0xF81F}; CHECK(spr->SetColorKey(DDCKEY_SRCBLT, &ck) == DD_OK, "SetColorKey");
    CHECK(back->BltFast(10, 10, spr, NULL, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT) == DD_OK, "BltFast with source colour key");
    // verify: corner of the sprite area keeps the pattern (red), centre is white
    memset(&ld, 0, sizeof ld); ld.dwSize = sizeof ld; back->Lock(NULL, &ld, 0, NULL);
    WORD corner = *(WORD *)((BYTE *)ld.lpSurface + 10 * ld.lPitch + 10 * 2), centre = *(WORD *)((BYTE *)ld.lpSurface + 26 * ld.lPitch + 26 * 2);
    back->Unlock(NULL);
    CHECK(corner == 0xF800, "colour-keyed pixels were skipped");
    CHECK(centre == 0xFFFF, "opaque pixels were copied");

    // stretch blit + mirror
    RECT dr = {200, 300, 264, 364}; CHECK(back->Blt(&dr, spr, NULL, DDBLT_WAIT | DDBLT_KEYSRC, NULL) == DD_OK, "stretch Blt 32->64 with key");

    // GDI text through GetDC
    HDC dc; CHECK(back->GetDC(&dc) == DD_OK && dc, "GetDC");
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 0)); TextOutA(dc, 400, 20, "TPW DDRAW TEST", 14);
    CHECK(back->ReleaseDC(dc) == DD_OK, "ReleaseDC");

    // flip -> the pattern must now be on the primary
    CHECK(prim->Flip(NULL, DDFLIP_WAIT) == DD_OK, "Flip");
    memset(&ld, 0, sizeof ld); ld.dwSize = sizeof ld; prim->Lock(NULL, &ld, 0, NULL);
    WORD p0 = *(WORD *)((BYTE *)ld.lpSurface + 100 * ld.lPitch + 100 * 2), p1 = *(WORD *)((BYTE *)ld.lpSurface + 100 * ld.lPitch + 500 * 2), p2 = *(WORD *)((BYTE *)ld.lpSurface + 400 * ld.lPitch + 500 * 2);
    prim->Unlock(NULL);
    CHECK(p0 == 0xF800 && p1 == 0x07E0 && p2 == 0x001F, "after Flip the primary holds the pattern");

    // texture-like surface with a 4444 pixel format and a mip chain
    DDSURFACEDESC2 td; memset(&td, 0, sizeof td); td.dwSize = sizeof td; td.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT;
    td.dwWidth = 64; td.dwHeight = 64; td.dwMipMapCount = 4; td.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY;
    td.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT); td.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS; td.ddpfPixelFormat.dwRGBBitCount = 16;
    td.ddpfPixelFormat.dwRGBAlphaBitMask = 0xF000; td.ddpfPixelFormat.dwRBitMask = 0x0F00; td.ddpfPixelFormat.dwGBitMask = 0x00F0; td.ddpfPixelFormat.dwBBitMask = 0x000F;
    IDirectDrawSurface7 *tex = NULL; CHECK(dd->CreateSurface(&td, &tex, NULL) == DD_OK, "CreateSurface 4444 texture with mips");
    DDSCAPS2 mc; memset(&mc, 0, sizeof mc); mc.dwCaps = DDSCAPS_MIPMAP; IDirectDrawSurface7 *mip = NULL;
    CHECK(tex->GetAttachedSurface(&mc, &mip) == DD_OK && mip, "mip level 1 attached");
    DDSURFACEDESC2 md; memset(&md, 0, sizeof md); md.dwSize = sizeof md; mip->GetSurfaceDesc(&md); CHECK(md.dwWidth == 32, "mip level 1 is 32 wide");

    // older interface views
    IDirectDrawSurface3 *s3 = NULL; CHECK(spr->QueryInterface(IID_IDirectDrawSurface3, (void **)&s3) == S_OK && s3, "QI IDirectDrawSurface3");
    DDSURFACEDESC d1; memset(&d1, 0, sizeof d1); d1.dwSize = sizeof d1; CHECK(s3->GetSurfaceDesc(&d1) == DD_OK && d1.dwWidth == 32, "v3 GetSurfaceDesc");

    // palette + 8-bit surface
    PALETTEENTRY pe[256]; for (int i = 0; i < 256; i++) { pe[i].peRed = i; pe[i].peGreen = 255 - i; pe[i].peBlue = 0; pe[i].peFlags = 0; }
    IDirectDrawPalette *pal = NULL; CHECK(dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, NULL) == DD_OK, "CreatePalette");

    CHECK(dd->GetAvailableVidMem(NULL, NULL, NULL) == DD_OK, "GetAvailableVidMem");
    // diagnostics: an all-white primary must be reported in the log (ini has log=1 in the test folder)
    { DDBLTFX wf; memset(&wf, 0, sizeof wf); wf.dwSize = sizeof wf; wf.dwFillColor = 0xFFFF; prim->Blt(NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &wf);
      char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\ddraw_log.txt", dir); FILE *f = fopen(lp, "r"); bool found = false; char line[512];
      while (f && fgets(line, sizeof line, f)) if (strstr(line, "WHITE FRAME")) found = true; if (f) fclose(f);
      CHECK(found, "an all-white frame is reported in ddraw_log.txt"); }
    printf("presented; holding window for screenshot\n"); fflush(stdout);
    DWORD t0 = GetTickCount(); MSG msg;
    while (GetTickCount() - t0 < (argc > 1 ? (DWORD)atoi(argv[1]) : 1500)) { while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); } Sleep(10); }
    if (pal) pal->Release(); if (s3) s3->Release(); if (mip) mip->Release(); if (tex) tex->Release(); if (spr) spr->Release(); if (back) back->Release(); if (prim) prim->Release();
    dd2->Release(); dd->Release(); dd1->Release();
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL CHECKS PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
