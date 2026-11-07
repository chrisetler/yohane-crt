// HDR output check: creates a legacy 8-bit swap chain the way the game does, clears the back
// buffer through an explicit 8-bit view to known SDR values, presents, and reads the converted
// frame back once it cycles round. Needs crt\hdr.ini with enabled=1, paper_white=200, peak=1000.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "D3DCOMPILER_43.lib")
extern "C" HRESULT WINAPI D3DCompile(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, void**, void**);
#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("FAIL %s %08lx\n", #x, _h); return 1; } } while (0)
static float h2f(uint16_t h) { uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; float v = e ? (1 + m / 1024.f) * powf(2.f, (float)e - 15) : m / 1024.f * powf(2.f, -14); return s ? -v : v; }
int main() {
    volatile HRESULT keep = GetTickCount() == 1 ? D3DCompile(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) : 0;  // import the proxy DLL like the game does
    HWND w = CreateWindowA("STATIC", "t", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 64, 64, 0, 0, 0, 0);
    ID3D11Device* dev; ID3D11DeviceContext* ctx;
    CK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    IDXGIDevice* dd; IDXGIAdapter* ad; IDXGIFactory* fa;
    CK(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)); CK(dd->GetAdapter(&ad)); CK(ad->GetParent(__uuidof(IDXGIFactory), (void**)&fa));
    DXGI_SWAP_CHAIN_DESC sd = {}; sd.BufferDesc = {64, 64, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 1; sd.OutputWindow = w; sd.Windowed = TRUE;
    IDXGISwapChain* sc; CK(fa->CreateSwapChain(dev, &sd, &sc));
    ID3D11Texture2D* bb; CK(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb));
    D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd); printf("backbuffer format %u\n", bd.Format);
    D3D11_RENDER_TARGET_VIEW_DESC rd = {DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D};
    ID3D11RenderTargetView* rtv; CK(dev->CreateRenderTargetView(bb, &rd, &rtv));  // the game's explicit 8-bit view
    // 8-bit 1080p-style render target should come back FP16
    D3D11_TEXTURE2D_DESC td = {1920, 1080, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ID3D11Texture2D* work; CK(dev->CreateTexture2D(&td, nullptr, &work)); D3D11_TEXTURE2D_DESC wd; work->GetDesc(&wd); printf("working image format %u\n", wd.Format);
    FLOAT col[4] = {0.5f, 1.0f, 1.5f, 1};
    ctx->ClearRenderTargetView(rtv, col);
    CK(sc->Present(0, 0));
    D3D11_TEXTURE2D_DESC st = bd; st.Usage = D3D11_USAGE_STAGING; st.BindFlags = 0; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ; st.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D11Texture2D* stg; CK(dev->CreateTexture2D(&st, nullptr, &stg));
    for (int i = 0; i < 3; i++) {  // flip model: the converted frame comes back round as buffer 0 after one more present
        if (i) sc->Present(0, 0);
        ID3D11Texture2D* b; if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&b))) continue;
        ctx->CopyResource(stg, b);
        D3D11_MAPPED_SUBRESOURCE m; CK(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m));
        uint16_t* p = (uint16_t*)m.pData + 32 * m.RowPitch / 2 + 32 * 4;
        printf("buffer %d pixel: %.3f %.3f %.3f %.3f\n", i, h2f(p[0]), h2f(p[1]), h2f(p[2]), h2f(p[3]));
        ctx->Unmap(stg, 0); b->Release();
    }
    return 0;
}

