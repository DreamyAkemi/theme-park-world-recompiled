/* Direct3D 11 presentation for the ddraw layer: scales the game's finished frame and post-processes it on the GPU.
 * Everything is loaded dynamically (d3d11.dll, d3dcompiler_47.dll) so the DLL still starts on machines without them;
 * gpu_present() returns false whenever it cannot present and the caller falls back to GDI. */
#pragma once
#include <windows.h>

struct GpuOptions {
    int filter = 2;      /* 0 = nearest, 1 = bilinear, 2 = sharp bilinear */
    int aa = 1;          /* 1 = FXAA */
    int deband = 1;      /* 1 = smooth flat 16-bit colour bands */
    int sharpen = 0;     /* 0..100 */
    int vsync = 0;
};

typedef void (*GpuLogFn)(const char *fmt, ...);

/* bgrx: W*H pixels (0x00RRGGBB); dest = where the picture goes inside the client area (the rest is black). */
bool gpu_present(HWND hwnd, const DWORD *bgrx, int W, int H, int client_w, int client_h, const RECT &dest, const GpuOptions &opt, GpuLogFn log);
void gpu_shutdown();
bool gpu_failed();
