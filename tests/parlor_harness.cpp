// Parlor harness: loads the fortune parlor's 2048x2048 texture sheet from atlas.bmp (an
// input_*_2048x2048 snapshot from a Shift+F11 capture in the parlor; not included, it's game
// art) and draws from it four times into a 1920x1080 target. The DLL's scene pass should run
// after draw 3. See tests/README.md.

// Parlor mimic: loads the captured 2048x2048 parlor sheet (atlas.bmp) as a texture, then draws
// from it 4 times into a 1920x1080 target. The DLL should insert its scene pass after draw 3.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "D3DCOMPILER_43.lib")

#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("FAIL %s %08lx\n", #x, _h); return 1; } } while (0)

int main() {
    FILE* f = fopen("atlas.bmp", "rb");
    if (!f) { printf("need atlas.bmp\n"); return 1; }
    std::vector<uint8_t> bmp(54 + 2048 * 2048 * 4);
    fread(bmp.data(), 1, bmp.size(), f); fclose(f);

    HWND w = CreateWindowA("STATIC", "t", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, 0, 0, 0, 0);
    ID3D11Device* dev; ID3D11DeviceContext* ctx;
    CK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    IDXGIDevice* dd; IDXGIAdapter* ad; IDXGIFactory* fa;
    CK(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)); CK(dd->GetAdapter(&ad)); CK(ad->GetParent(__uuidof(IDXGIFactory), (void**)&fa));
    DXGI_SWAP_CHAIN_DESC sd = {}; sd.BufferDesc = {320, 180, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 1; sd.OutputWindow = w; sd.Windowed = TRUE;
    IDXGISwapChain* sc; CK(fa->CreateSwapChain(dev, &sd, &sc));

    // VS mirrors the game's layout; PS samples the sheet like the UI shader (sample * tint)
    const char* vsrc =
        "struct O{float4 pos:SV_Position;float4 c0:COLOR0;float4 c1:COLOR1;float2 uv:TEXCOORD0;};"
        "O main(uint i:SV_VertexID){O o;float2 t=float2((i<<1)&2,i&2);o.uv=t*float2(1,0.5273);"
        "o.pos=float4(t*float2(2,-2)+float2(-1,1),0,1);o.c0=1;o.c1=0;return o;}";
    const char* psrc =
        "Texture2D t:register(t0);SamplerState s:register(s0);"
        "struct I{float4 pos:SV_Position;float4 c0:COLOR0;float4 c1:COLOR1;float2 uv:TEXCOORD0;};"
        "float4 main(I i):SV_Target{return t.Sample(s,i.uv)*i.c0;}";
    ID3DBlob *vb, *pb;
    CK(D3DCompile(vsrc, strlen(vsrc), "v", 0, 0, "main", "vs_5_0", 0, 0, &vb, 0));
    CK(D3DCompile(psrc, strlen(psrc), "p", 0, 0, "main", "ps_5_0", 0, 0, &pb, 0));
    ID3D11VertexShader* vs; ID3D11PixelShader* ps;
    CK(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), 0, &vs));
    CK(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), 0, &ps));
    // never bound; only here so SHADERLOG_CRT_TARGET has something to name
    const char* dsrc = "float4 main():SV_Target{return 0.25;}";
    ID3DBlob* db; ID3D11PixelShader* dummy;
    CK(D3DCompile(dsrc, strlen(dsrc), "d", 0, 0, "main", "ps_5_0", 0, 0, &db, 0));
    CK(dev->CreatePixelShader(db->GetBufferPointer(), db->GetBufferSize(), 0, &dummy));

    D3D11_TEXTURE2D_DESC td = {2048, 2048, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA init = {&bmp[54], 2048 * 4, 0};
    ID3D11Texture2D* atlas; ID3D11ShaderResourceView* atlasSRV;
    CK(dev->CreateTexture2D(&td, &init, &atlas)); CK(dev->CreateShaderResourceView(atlas, nullptr, &atlasSRV));
    D3D11_TEXTURE2D_DESC rd = {1920, 1080, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    ID3D11Texture2D* rt; ID3D11RenderTargetView* rtv;
    CK(dev->CreateTexture2D(&rd, nullptr, &rt)); CK(dev->CreateRenderTargetView(rt, nullptr, &rtv));
    ID3D11Texture2D* bb; ID3D11RenderTargetView* bbRT;
    CK(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)); CK(dev->CreateRenderTargetView(bb, 0, &bbRT));
    D3D11_SAMPLER_DESC smd = {}; smd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; smd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* smp; CK(dev->CreateSamplerState(&smd, &smp));

    for (int frame = 0; frame < 2; frame++) {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, 1920, 1080, 0, 1}; ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, 0, 0); ctx->PSSetShader(ps, 0, 0); ctx->PSSetSamplers(0, 1, &smp);
        ctx->PSSetShaderResources(0, 1, &atlasSRV);
        for (int d = 0; d < 4; d++) ctx->Draw(3, 0);  // room, Yohane, Lailaps, then "menu"
        ctx->OMSetRenderTargets(1, &bbRT, nullptr);
        CK(sc->Present(0, 0));
    }
    printf("ok\n");
    return 0;
}
