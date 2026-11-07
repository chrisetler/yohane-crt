// Level harness: renders two frames on WARP the way the game's level path does (a low-res
// target upscaled into a larger one), through the DLL's hooks, and prints which module owns
// key immediate-context vtable slots before and after (the D3D11 runtime swaps some).
// See tests/README.md for how to build and run it.

// Mimics the game: statically imports d3d11!D3D11CreateDevice and our D3DCOMPILER_43, gets the
// factory via device->adapter->parent, renders two frames on WARP.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "D3DCOMPILER_43.lib")  // import lib generated from our .def

#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("FAIL %s %08lx\n", #x, _h); return 1; } } while (0)

static void Dump(const char* tag, ID3D11DeviceContext* c) {
    void** vt = *(void***)c;
    printf("%s vt=%p:", tag, (void*)vt);
    int idx[] = {8, 9, 11, 12, 13, 20, 21, 33, 44, 47, 50};
    for (int i : idx) {
        HMODULE m = 0; char n[MAX_PATH] = "?";
        GetModuleHandleExA(6, (LPCSTR)vt[i], &m); GetModuleFileNameA(m, n, MAX_PATH);
        const char* b = strrchr(n, 92); printf(" %d=%s", i, b ? b + 1 : n);
    }
    printf("\n");
}
int main() {
    HWND w = CreateWindowA("STATIC", "t", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, 0, 0, 0, 0);
    ID3D11Device* dev; ID3D11DeviceContext* ctx;
    CK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    IDXGIDevice* dd; IDXGIAdapter* ad; IDXGIFactory* f;
    CK(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)); CK(dd->GetAdapter(&ad)); CK(ad->GetParent(__uuidof(IDXGIFactory), (void**)&f));
    DXGI_SWAP_CHAIN_DESC sd = {}; sd.BufferDesc = {1280, 720, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 1; sd.OutputWindow = w; sd.Windowed = TRUE;
    IDXGISwapChain* sc; CK(f->CreateSwapChain(dev, &sd, &sc));

    const char* vsrc = "float4 main(uint i:SV_VertexID):SV_Position{return float4(i&1,i>>1,0,1);}";
    const char* psrc = "Texture2D t:register(t0);SamplerState s;float4 main(float4 p:SV_Position):SV_Target{return t.Load(int3(p.xy,0));}";
    ID3DBlob *vb, *pb;
    CK(D3DCompile(vsrc, strlen(vsrc), "v", 0, 0, "main", "vs_5_0", 0, 0, &vb, 0));
    CK(D3DCompile(psrc, strlen(psrc), "p", 0, 0, "main", "ps_5_0", 0, 0, &pb, 0));
    ID3D11VertexShader* vs; ID3D11PixelShader* ps;
    CK(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), 0, &vs));
    CK(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), 0, &ps));

    D3D11_TEXTURE2D_DESC td = {160, 90, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ID3D11Texture2D *low, *bb; ID3D11RenderTargetView *lowRT, *bbRT; ID3D11ShaderResourceView* lowSRV;
    CK(dev->CreateTexture2D(&td, 0, &low)); CK(dev->CreateRenderTargetView(low, 0, &lowRT)); CK(dev->CreateShaderResourceView(low, 0, &lowSRV));
    CK(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)); CK(dev->CreateRenderTargetView(bb, 0, &bbRT));

    Dump("before", ctx);
    for (int frame = 0; frame < 2; frame++) {
        float c[4] = {0, 0, 1, 1};
        ctx->OMSetRenderTargets(1, &lowRT, nullptr);
        D3D11_VIEWPORT vp = {0, 0, 160, 90, 0, 1}; ctx->RSSetViewports(1, &vp);
        ctx->ClearRenderTargetView(lowRT, c);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs, 0, 0); ctx->PSSetShader(ps, 0, 0);
        ID3D11ShaderResourceView* none = nullptr; ctx->PSSetShaderResources(0, 1, &none);
        ctx->Draw(4, 0); ctx->Draw(4, 0); ctx->Draw(4, 0);          // "sprites" x3
        ctx->OMSetRenderTargets(1, &bbRT, nullptr);
        D3D11_VIEWPORT vp2 = {0, 0, 1280, 720, 0, 1}; ctx->RSSetViewports(1, &vp2);
        ctx->PSSetShaderResources(0, 1, &lowSRV);
        ctx->DrawInstanced(4, 1, 0, 0);                              // "upscale"
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->DrawIndexedInstanced(0, 1, 0, 0, 0);                    // "hud"
        ctx->CopyResource(low, low);
        CK(sc->Present(0, 0));
    }
    Dump("after", ctx);
    printf("ok\n");
    return 0;
}
