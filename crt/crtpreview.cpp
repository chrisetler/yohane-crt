// Offline preview: runs crt.hlsl over a captured 1920x1080 BGRA BMP on WARP and writes the result.
// usage: crtpreview <crt.hlsl> <in.bmp> <out.bmp>
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("FAIL %s %08lx\n", #x, _h); return 1; } } while (0)

static std::string ReadAll(const char* p) {
    std::string s; FILE* f = fopen(p, "rb"); if (!f) return s;
    char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s;
}

// Mirrors the game's vs_0fb4 output layout: SV_Position, COLOR0, COLOR1, TEXCOORD0
static const char* kVS =
    "struct O{float4 pos:SV_Position;float4 c0:COLOR0;float4 c1:COLOR1;float2 uv:TEXCOORD0;};"
    "O main(uint i:SV_VertexID){O o;float2 t=float2((i<<1)&2,i&2);o.uv=t;"
    "o.pos=float4(t*float2(2,-2)+float2(-1,1),0,1);o.c0=1;o.c1=0;return o;}";

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    std::string bmp = ReadAll(argv[2]);
    if (bmp.size() < 54) { printf("bad bmp\n"); return 1; }
    int w = *(int*)&bmp[18], h = -*(int*)&bmp[22];
    printf("input %dx%d\n", w, h);

    ID3D11Device* dev; ID3D11DeviceContext* ctx;
    CK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    D3D11_TEXTURE2D_DESC td = {(UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET};
    D3D11_SUBRESOURCE_DATA init = {&bmp[54], (UINT)w * 4, 0};
    ID3D11Texture2D *src, *dst, *st; ID3D11ShaderResourceView* srv; ID3D11RenderTargetView* rtv;
    CK(dev->CreateTexture2D(&td, &init, &src)); CK(dev->CreateShaderResourceView(src, nullptr, &srv));
    CK(dev->CreateTexture2D(&td, nullptr, &dst)); CK(dev->CreateRenderTargetView(dst, nullptr, &rtv));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CK(dev->CreateTexture2D(&td, nullptr, &st));

    std::string ps = ReadAll(argv[1]);
    ID3DBlob *vb, *pb, *err = nullptr;
    CK(D3DCompile(kVS, strlen(kVS), "vs", 0, 0, "main", "vs_5_0", 0, 0, &vb, 0));
    if (FAILED(D3DCompile(ps.data(), ps.size(), "crt.hlsl", 0, 0, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pb, &err))) {
        printf("compile failed:\n%s\n", err ? (char*)err->GetBufferPointer() : "?"); return 1;
    }
    if (err) printf("warnings:\n%s\n", (char*)err->GetBufferPointer());
    ID3D11VertexShader* vs; ID3D11PixelShader* p;
    CK(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), 0, &vs));
    CK(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), 0, &p));

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1}; ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, 0, 0); ctx->PSSetShader(p, 0, 0); ctx->PSSetShaderResources(0, 1, &srv);
    ctx->Draw(3, 0);
    ctx->CopyResource(st, dst);
    D3D11_MAPPED_SUBRESOURCE m; CK(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
    FILE* f = fopen(argv[3], "wb");
    fwrite(bmp.data(), 1, 54, f);
    for (int y = 0; y < h; y++) fwrite((char*)m.pData + y * m.RowPitch, 1, w * 4, f);
    fclose(f);
    ctx->Unmap(st, 0);
    printf("wrote %s\n", argv[3]);
    return 0;
}
