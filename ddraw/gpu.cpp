#include "gpu.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>

static const char *HLSL = R"HLSL(
cbuffer C : register(b0) {
    float4 src;    // source w, h, 1/w, 1/h
    float4 dst;    // target w, h, 1/w, 1/h
    float4 rect;   // picture rectangle in target pixels: left, top, width, height
    float4 opt;    // filter, aa, deband, sharpen
};
Texture2D tex : register(t0);
SamplerState lin : register(s0);
SamplerState pnt : register(s1);
struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; };

VO vs(uint id : SV_VertexID) {
    VO o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}

// pass 1: place the game frame inside the target with the chosen scaling filter
float4 ps_scale(VO i) : SV_Target {
    float2 rel = (i.p.xy - rect.xy) / rect.zw;
    if (any(rel < 0) || any(rel >= 1)) return float4(0, 0, 0, 1);
    float2 t = rel * src.xy;
    if (opt.x < 0.5) return tex.SampleLevel(pnt, (floor(t) + 0.5) * src.zw, 0);
    if (opt.x < 1.5) return tex.SampleLevel(lin, t * src.zw, 0);
    float2 s = max(rect.zw / src.xy, 1.0);          // sharp bilinear: only blend inside a thin band at texel edges
    float2 fl = floor(t);
    float2 c = fl + saturate((t - fl - 0.5) * s + 0.5);
    return tex.SampleLevel(lin, c * src.zw, 0);
}

float3 S(float2 uv) { return tex.SampleLevel(lin, uv, 0).rgb; }
float L(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float hash(float2 p) { float3 q = frac(float3(p.xyx) * 0.1031); q += dot(q, q.yzx + 33.33); return frac((q.x + q.y) * q.z); }

// FXAA 3.11 (quality, subpixel 0.75, edge threshold 0.125 / min 0.0312)
float3 fxaa(float2 uv) {
    float2 px = dst.zw;
    float3 cM = S(uv); float lM = L(cM);
    float lN = L(S(uv + float2(0, -px.y))), lS = L(S(uv + float2(0, px.y)));
    float lW = L(S(uv + float2(-px.x, 0))), lE = L(S(uv + float2(px.x, 0)));
    float mx = max(max(lN, lS), max(max(lW, lE), lM)), mn = min(min(lN, lS), min(min(lW, lE), lM));
    float range = mx - mn;
    if (range < max(0.0312, mx * 0.125)) return cM;
    float lNW = L(S(uv + float2(-px.x, -px.y))), lNE = L(S(uv + float2(px.x, -px.y)));
    float lSW = L(S(uv + float2(-px.x, px.y))), lSE = L(S(uv + float2(px.x, px.y)));
    float lNS = lN + lS, lWE = lW + lE;
    float edgeH = abs(-2 * lW + (lNW + lSW)) + abs(-2 * lM + lNS) * 2 + abs(-2 * lE + (lNE + lSE));
    float edgeV = abs(-2 * lN + (lNW + lNE)) + abs(-2 * lM + lWE) * 2 + abs(-2 * lS + (lSW + lSE));
    bool horz = edgeH >= edgeV;
    float l1 = horz ? lN : lW, l2 = horz ? lS : lE;
    float g1 = l1 - lM, g2 = l2 - lM;
    bool is1 = abs(g1) >= abs(g2);
    float grad = 0.25 * max(abs(g1), abs(g2));
    float stepLen = horz ? px.y : px.x;
    float avg;
    if (is1) { stepLen = -stepLen; avg = 0.5 * (l1 + lM); } else avg = 0.5 * (l2 + lM);
    float2 cur = uv; if (horz) cur.y += stepLen * 0.5; else cur.x += stepLen * 0.5;
    float2 off = horz ? float2(px.x, 0) : float2(0, px.y);
    float2 uv1 = cur - off, uv2 = cur + off;
    float e1 = L(S(uv1)) - avg, e2 = L(S(uv2)) - avg;
    bool r1 = abs(e1) >= grad, r2 = abs(e2) >= grad;
    if (!r1) uv1 -= off; if (!r2) uv2 += off;
    [loop] for (int k = 0; k < 14 && !(r1 && r2); k++) {
        float q = k < 4 ? 1.0 : (k < 8 ? 1.5 : 2.5);
        if (!r1) e1 = L(S(uv1)) - avg;
        if (!r2) e2 = L(S(uv2)) - avg;
        r1 = abs(e1) >= grad; r2 = abs(e2) >= grad;
        if (!r1) uv1 -= off * q; if (!r2) uv2 += off * q;
    }
    float d1 = horz ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float d2 = horz ? (uv2.x - uv.x) : (uv2.y - uv.y);
    bool dir1 = d1 < d2;
    float dist = min(d1, d2), thick = d1 + d2;
    float pixOff = -dist / thick + 0.5;
    bool centerSmaller = lM < avg;
    bool correct = ((dir1 ? e1 : e2) < 0) != centerSmaller;
    float fo = correct ? pixOff : 0;
    float lAvg = (1.0 / 12.0) * (2.0 * (lNS + lWE) + (lNW + lSW) + (lNE + lSE));
    float sA = saturate(abs(lAvg - lM) / range);
    float sB = (-2.0 * sA + 3.0) * sA * sA;
    float sub = sB * sB * 0.75;
    fo = max(fo, sub);
    float2 f = uv; if (horz) f.y += fo * stepLen; else f.x += fo * stepLen;
    return S(f);
}

// pass 2: anti-aliasing, deband, sharpen, dither
float4 ps_post(VO i) : SV_Target {
    float2 uv = i.p.xy * dst.zw;
    float3 c = opt.y > 0.5 ? fxaa(uv) : S(uv);
    if (opt.z > 0.5) {   // deband: widen the average while neighbours stay within one 16-bit colour step of the centre
        float3 acc = c; float n = 1;
        [loop] for (int k = 0; k < 8; k++) {
            float r = 3.0 + 3.0 * k;
            float3 a = S(uv + dst.zw * float2(r, 0)), b = S(uv - dst.zw * float2(r, 0)), d = S(uv + dst.zw * float2(0, r)), e = S(uv - dst.zw * float2(0, r));
            float3 m = max(max(abs(a - c), abs(b - c)), max(abs(d - c), abs(e - c)));
            if (max(m.r, max(m.g, m.b)) > 9.5 / 255.0) break;
            acc += a + b + d + e; n += 4;
        }
        c = acc / n;
        c += (hash(i.p.xy) - 0.5) / 255.0;
    }
    if (opt.w > 0) {
        float3 b = (S(uv + dst.zw * float2(1, 0)) + S(uv - dst.zw * float2(1, 0)) + S(uv + dst.zw * float2(0, 1)) + S(uv - dst.zw * float2(0, 1))) * 0.25;
        c = saturate(c + (c - b) * opt.w);
    }
    return float4(c, 1);
}
)HLSL";

struct Cb { float src[4], dst[4], rect[4], opt[4]; };

static struct {
    bool failed = false, ready = false;
    HWND hwnd = 0;
    ID3D11Device *dev = 0; ID3D11DeviceContext *ctx = 0; IDXGISwapChain *sc = 0;
    ID3D11RenderTargetView *bbv = 0;
    ID3D11Texture2D *srcTex = 0; ID3D11ShaderResourceView *srcSrv = 0; int sw = 0, sh = 0;
    ID3D11Texture2D *midTex = 0; ID3D11ShaderResourceView *midSrv = 0; ID3D11RenderTargetView *midRtv = 0; int cw = 0, ch = 0;
    ID3D11VertexShader *vs = 0; ID3D11PixelShader *psScale = 0, *psPost = 0;
    ID3D11SamplerState *lin = 0, *pnt = 0; ID3D11Buffer *cb = 0;
} g;

template <class T> static void rel(T *&p) { if (p) { p->Release(); p = 0; } }

static void drop_size_deps() { rel(g.bbv); rel(g.midRtv); rel(g.midSrv); rel(g.midTex); g.cw = g.ch = 0; }

void gpu_shutdown() {
    drop_size_deps(); rel(g.srcSrv); rel(g.srcTex); rel(g.vs); rel(g.psScale); rel(g.psPost); rel(g.lin); rel(g.pnt); rel(g.cb);
    rel(g.sc); rel(g.ctx); rel(g.dev); g.ready = false; g.hwnd = 0; g.sw = g.sh = 0;
}
bool gpu_failed() { return g.failed; }

static bool compile(const char *entry, const char *target, ID3DBlob **out, GpuLogFn log) {
    static HMODULE m; static pD3DCompile fn;
    if (!m) { m = LoadLibraryA("d3dcompiler_47.dll"); if (!m) m = LoadLibraryA("d3dcompiler_43.dll"); if (m) fn = (pD3DCompile)GetProcAddress(m, "D3DCompile"); }
    if (!fn) { log("GPU: no d3dcompiler"); return false; }
    ID3DBlob *err = 0;
    HRESULT hr = fn(HLSL, strlen(HLSL), "ddraw.hlsl", NULL, NULL, entry, target, 0, 0, out, &err);
    if (FAILED(hr)) { log("GPU: shader %s failed: %s", entry, err ? (const char *)err->GetBufferPointer() : "?"); rel(err); return false; }
    rel(err); return true;
}

static bool init(HWND hwnd, GpuLogFn log) {
    HMODULE d3d = LoadLibraryA("d3d11.dll"); if (!d3d) { log("GPU: no d3d11.dll"); return false; }
    typedef HRESULT(WINAPI * PFN)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    PFN create = (PFN)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain"); if (!create) return false;
    DXGI_SWAP_CHAIN_DESC sd; memset(&sd, 0, sizeof sd);
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    static const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g.sc, &g.dev, &got, &g.ctx);
    if (FAILED(hr)) { log("GPU: hardware device failed (0x%08lx), trying WARP", (unsigned long)hr); hr = create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g.sc, &g.dev, &got, &g.ctx); }
    if (FAILED(hr)) { log("GPU: device creation failed 0x%08lx", (unsigned long)hr); return false; }
    { IDXGIDevice *dd = 0; IDXGIAdapter *ad = 0; IDXGIFactory *fa = 0; DXGI_ADAPTER_DESC desc;
      if (SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dd)) && SUCCEEDED(dd->GetAdapter(&ad))) {
          if (SUCCEEDED(ad->GetDesc(&desc))) { char nm[128]; WideCharToMultiByte(CP_ACP, 0, desc.Description, -1, nm, sizeof nm, 0, 0); log("GPU: Direct3D 11 feature level 0x%x on %s", (unsigned)got, nm); }
          if (SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), (void **)&fa))) { fa->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES); rel(fa); }
      } rel(ad); rel(dd); }
    ID3DBlob *vsb = 0, *psb1 = 0, *psb2 = 0;
    if (!compile("vs", "vs_4_0", &vsb, log) || !compile("ps_scale", "ps_4_0", &psb1, log) || !compile("ps_post", "ps_4_0", &psb2, log)) { rel(vsb); rel(psb1); rel(psb2); return false; }
    bool ok = SUCCEEDED(g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &g.vs)) &&
              SUCCEEDED(g.dev->CreatePixelShader(psb1->GetBufferPointer(), psb1->GetBufferSize(), NULL, &g.psScale)) &&
              SUCCEEDED(g.dev->CreatePixelShader(psb2->GetBufferPointer(), psb2->GetBufferSize(), NULL, &g.psPost));
    rel(vsb); rel(psb1); rel(psb2);
    if (!ok) { log("GPU: creating shaders failed"); return false; }
    D3D11_SAMPLER_DESC sm; memset(&sm, 0, sizeof sm); sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sm.ComparisonFunc = D3D11_COMPARISON_NEVER; sm.MaxLOD = D3D11_FLOAT32_MAX;
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; if (FAILED(g.dev->CreateSamplerState(&sm, &g.lin))) return false;
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; if (FAILED(g.dev->CreateSamplerState(&sm, &g.pnt))) return false;
    D3D11_BUFFER_DESC bd; memset(&bd, 0, sizeof bd); bd.ByteWidth = sizeof(Cb); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g.dev->CreateBuffer(&bd, NULL, &g.cb))) return false;
    g.hwnd = hwnd; g.ready = true; log("GPU: presentation ready"); return true;
}

static bool make_size_deps(int cw, int ch, GpuLogFn log) {
    drop_size_deps();
    HRESULT hr = g.sc->ResizeBuffers(1, cw, ch, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (FAILED(hr)) { log("GPU: ResizeBuffers failed 0x%08lx", (unsigned long)hr); return false; }
    ID3D11Texture2D *bb = 0; if (FAILED(g.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb))) return false;
    hr = g.dev->CreateRenderTargetView(bb, NULL, &g.bbv); rel(bb); if (FAILED(hr)) return false;
    D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof td); td.Width = cw; td.Height = ch; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g.dev->CreateTexture2D(&td, NULL, &g.midTex)) || FAILED(g.dev->CreateShaderResourceView(g.midTex, NULL, &g.midSrv)) || FAILED(g.dev->CreateRenderTargetView(g.midTex, NULL, &g.midRtv))) return false;
    g.cw = cw; g.ch = ch; return true;
}

static bool make_src(int W, int H) {
    rel(g.srcSrv); rel(g.srcTex);
    D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof td); td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8X8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g.dev->CreateTexture2D(&td, NULL, &g.srcTex)) || FAILED(g.dev->CreateShaderResourceView(g.srcTex, NULL, &g.srcSrv))) return false;
    g.sw = W; g.sh = H; return true;
}

static void set_cb(const Cb &c) { g.ctx->UpdateSubresource(g.cb, 0, NULL, &c, 0, 0); }

static void draw_pass(ID3D11RenderTargetView *rtv, ID3D11PixelShader *ps, ID3D11ShaderResourceView *srv, int w, int h) {
    g.ctx->OMSetRenderTargets(1, &rtv, NULL);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 }; g.ctx->RSSetViewports(1, &vp);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g.ctx->IASetInputLayout(NULL);
    g.ctx->VSSetShader(g.vs, NULL, 0); g.ctx->PSSetShader(ps, NULL, 0);
    g.ctx->PSSetConstantBuffers(0, 1, &g.cb); g.ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState *ss[2] = { g.lin, g.pnt }; g.ctx->PSSetSamplers(0, 2, ss);
    g.ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none = 0; g.ctx->PSSetShaderResources(0, 1, &none);
}

static bool present_impl(HWND hwnd, const DWORD *px, int W, int H, int cw, int ch, const RECT &dest, const GpuOptions &o, GpuLogFn log) {
    if (cw < 16 || ch < 16) return false;
    if (g.ready && g.hwnd != hwnd) gpu_shutdown();
    if (!g.ready && !init(hwnd, log)) return false;
    if (cw != g.cw || ch != g.ch) { if (!make_size_deps(cw, ch, log)) return false; }
    if (W != g.sw || H != g.sh) { if (!make_src(W, H)) { log("GPU: creating the frame texture failed"); return false; } }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g.ctx->Map(g.srcTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
    for (int y = 0; y < H; y++) memcpy((BYTE *)ms.pData + (size_t)y * ms.RowPitch, px + (size_t)y * W, (size_t)W * 4);
    g.ctx->Unmap(g.srcTex, 0);
    Cb c;
    c.src[0] = (float)W; c.src[1] = (float)H; c.src[2] = 1.f / W; c.src[3] = 1.f / H;
    c.dst[0] = (float)cw; c.dst[1] = (float)ch; c.dst[2] = 1.f / cw; c.dst[3] = 1.f / ch;
    c.rect[0] = (float)dest.left; c.rect[1] = (float)dest.top; c.rect[2] = (float)(dest.right - dest.left); c.rect[3] = (float)(dest.bottom - dest.top);
    c.opt[0] = (float)o.filter; c.opt[1] = (float)o.aa; c.opt[2] = (float)o.deband; c.opt[3] = o.sharpen / 100.f;
    set_cb(c);
    draw_pass(g.midRtv, g.psScale, g.srcSrv, cw, ch);
    draw_pass(g.bbv, g.psPost, g.midSrv, cw, ch);
    HRESULT hr = g.sc->Present(o.vsync ? 1 : 0, 0);
    if (FAILED(hr)) { log("GPU: Present failed 0x%08lx", (unsigned long)hr); return false; }
    return true;
}

bool gpu_present(HWND hwnd, const DWORD *px, int W, int H, int cw, int ch, const RECT &dest, const GpuOptions &o, GpuLogFn log) {
    if (g.failed) return false;
    if (cw < 16 || ch < 16) return false;   /* minimised: let the caller skip the frame */
    if (present_impl(hwnd, px, W, H, cw, ch, dest, o, log)) return true;
    log("GPU: presentation failed, falling back to GDI for the rest of the session");
    gpu_shutdown(); g.failed = true; return false;
}
