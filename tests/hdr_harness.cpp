// HDR output check, highlight-layer design. Builds one game-like frame on WARP:
//   1. an 8-bit 64x64 "working image" (must stay 8-bit),
//   2. the copy-back draw (swapped for crt\test.hlsl, which outputs above 1.0 with CRT_HDR),
//   3. a "HUD" draw over the left half,
//   4. the final scale to a 128x128 back buffer through an explicit 8-bit view.
// The final pass should add highlights on the right half only. Needs crt\hdr.ini with enabled=1,
// paper_white=200, peak=1000, crt_boost=1.5, and SHADERLOG_CRT_TARGET set to the hash this
// program prints for its copy shader (run it once without to get it).
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "D3DCOMPILER_43.lib")  // the mod, imported the way game.exe imports it
#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("FAIL %s %08lx\n", #x, _h); return 1; } } while (0)

static float h2f(uint16_t h) { uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; float v = e ? (1 + m / 1024.f) * powf(2.f, (float)e - 15) : m / 1024.f * powf(2.f, -14); return s ? -v : v; }
static uint64_t Fnv(const void* p, size_t n) { uint64_t h = 1469598103934665603ull; auto b = (const uint8_t*)p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; } return h; }

static ID3DBlob* Compile(const char* src, const char* target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), "harness", nullptr, nullptr, "main", target, 0, 0, &code, &err))) {
        printf("compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return nullptr;
    }
    return code;
}

int main() {
    HWND w = CreateWindowA("STATIC", "t", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 160, 160, 0, 0, 0, 0);
    ID3D11Device* dev; ID3D11DeviceContext* ctx;
    CK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    IDXGIDevice* dd; IDXGIAdapter* ad; IDXGIFactory* fa;
    CK(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)); CK(dd->GetAdapter(&ad)); CK(ad->GetParent(__uuidof(IDXGIFactory), (void**)&fa));
    DXGI_SWAP_CHAIN_DESC sd = {}; sd.BufferDesc = {128, 128, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 1; sd.OutputWindow = w; sd.Windowed = TRUE;
    IDXGISwapChain* sc; CK(fa->CreateSwapChain(dev, &sd, &sc));

    // shaders: full-screen triangle, the copy-back (the swap target), a flat HUD colour, the final scale
    ID3DBlob* vsb = Compile("float4 main(uint i:SV_VertexID):SV_Position{float2 t=float2((i<<1)&2,i&2);return float4(t*float2(2,-2)+float2(-1,1),0,1);}", "vs_5_0");
    ID3DBlob* cpb = Compile("Texture2D t:register(t0);float4 main(float4 p:SV_Position):SV_Target{return t.Load(int3(p.xy,0)); /* harness copy-back */}", "ps_5_0");
    ID3DBlob* hub = Compile("float4 main(float4 p:SV_Position):SV_Target{return float4(0,1,0,1);}", "ps_5_0");
    ID3DBlob* scb = Compile("Texture2D t:register(t0);SamplerState s:register(s0);float4 main(float4 p:SV_Position):SV_Target{return t.Sample(s,p.xy/128.0);}", "ps_5_0");
    if (!vsb || !cpb || !hub || !scb) return 1;
    char hash[32];
    sprintf_s(hash, "ps_%016llx", (unsigned long long)Fnv(cpb->GetBufferPointer(), cpb->GetBufferSize()));
    const char* want = getenv("SHADERLOG_CRT_TARGET");
    printf("copy shader %s%s\n", hash, want && !strcmp(want, hash) ? " (targeted)" : "  <- set SHADERLOG_CRT_TARGET to this and rerun");
    ID3D11VertexShader* vs; ID3D11PixelShader *cp, *hud, *scale;
    CK(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs));
    CK(dev->CreatePixelShader(cpb->GetBufferPointer(), cpb->GetBufferSize(), nullptr, &cp));
    CK(dev->CreatePixelShader(hub->GetBufferPointer(), hub->GetBufferSize(), nullptr, &hud));
    CK(dev->CreatePixelShader(scb->GetBufferPointer(), scb->GetBufferSize(), nullptr, &scale));

    ID3D11Texture2D* bb; CK(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb));
    D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd); printf("backbuffer format %u (10 = FP16)\n", bd.Format);
    D3D11_RENDER_TARGET_VIEW_DESC rd = {DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D};
    ID3D11RenderTargetView* bbRTV; CK(dev->CreateRenderTargetView(bb, &rd, &bbRTV));  // the game's explicit 8-bit view

    D3D11_TEXTURE2D_DESC td = {64, 64, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ID3D11Texture2D *work, *scratch; CK(dev->CreateTexture2D(&td, nullptr, &work)); CK(dev->CreateTexture2D(&td, nullptr, &scratch));
    D3D11_TEXTURE2D_DESC wd; work->GetDesc(&wd); printf("working image format %u (87 = 8-bit, must stay)\n", wd.Format);
    ID3D11RenderTargetView *workRTV, *scratchRTV; ID3D11ShaderResourceView *workSRV, *scratchSRV;
    CK(dev->CreateRenderTargetView(work, nullptr, &workRTV)); CK(dev->CreateRenderTargetView(scratch, nullptr, &scratchRTV));
    CK(dev->CreateShaderResourceView(work, nullptr, &workSRV)); CK(dev->CreateShaderResourceView(scratch, nullptr, &scratchSRV));
    D3D11_SAMPLER_DESC smd = {D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP};
    smd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* point; CK(dev->CreateSamplerState(&smd, &point));

    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0);
    D3D11_VIEWPORT vp64 = {0, 0, 64, 64, 0, 1}, vpHud = {0, 0, 32, 64, 0, 1}, vp128 = {0, 0, 128, 128, 0, 1};
    for (int frame = 0; frame < 3; frame++) {
        FLOAT grey[4] = {0.5f, 0.5f, 0.5f, 1};
        ctx->ClearRenderTargetView(scratchRTV, grey);
        // copy-back: scratch -> work, with the filter swapped in
        ctx->OMSetRenderTargets(1, &workRTV, nullptr);
        ctx->RSSetViewports(1, &vp64);
        ctx->PSSetShaderResources(0, 1, &scratchSRV);
        ctx->PSSetShader(cp, nullptr, 0);
        ctx->Draw(3, 0);
        // HUD over the left half
        ctx->PSSetShader(hud, nullptr, 0);
        ctx->RSSetViewports(1, &vpHud);
        ctx->Draw(3, 0);
        // final scale: work -> back buffer
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->OMSetRenderTargets(1, &bbRTV, nullptr);
        ctx->RSSetViewports(1, &vp128);
        ctx->PSSetShaderResources(0, 1, &workSRV);
        ctx->PSSetSamplers(0, 1, &point);
        ctx->PSSetShader(scale, nullptr, 0);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &none);
        CK(sc->Present(0, 0));
    }

    // what the game's working image holds (should be clipped at 1.0)
    D3D11_TEXTURE2D_DESC ws = wd; ws.Usage = D3D11_USAGE_STAGING; ws.BindFlags = 0; ws.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* wstg; CK(dev->CreateTexture2D(&ws, nullptr, &wstg));
    ctx->CopyResource(wstg, work);
    { D3D11_MAPPED_SUBRESOURCE m; CK(ctx->Map(wstg, 0, D3D11_MAP_READ, 0, &m)); uint8_t* p = (uint8_t*)m.pData + 32 * m.RowPitch + 48 * 4;
      printf("working image right half (BGRA): %u %u %u %u\n", p[0], p[1], p[2], p[3]); ctx->Unmap(wstg, 0); }

    // the presented frame: after an odd number of presents the converted one is back as buffer 0
    D3D11_TEXTURE2D_DESC st = bd; st.Usage = D3D11_USAGE_STAGING; st.BindFlags = 0; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ; st.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D11Texture2D* stg; CK(dev->CreateTexture2D(&st, nullptr, &stg));
    for (int i = 0; i < 2; i++) {
        if (i) sc->Present(0, 0);
        ID3D11Texture2D* b; if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&b))) continue;
        ctx->CopyResource(stg, b);
        D3D11_MAPPED_SUBRESOURCE m; CK(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m));
        uint16_t* l = (uint16_t*)m.pData + 64 * m.RowPitch / 2 + 16 * 4;
        uint16_t* r = (uint16_t*)m.pData + 64 * m.RowPitch / 2 + 96 * 4;
        printf("buffer %d  HUD side: %.3f %.3f %.3f   filter side: %.3f %.3f %.3f\n", i, h2f(l[0]), h2f(l[1]), h2f(l[2]), h2f(r[0]), h2f(r[1]), h2f(r[2]));
        ctx->Unmap(stg, 0); b->Release();
    }
    return 0;
}
