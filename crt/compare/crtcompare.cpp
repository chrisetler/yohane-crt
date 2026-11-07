// Renders a CRT shader port over a captured image on WARP, for side-by-side comparisons.
// usage: crtcompare <header.hlsli|-> <shader.hlsl> <in.bmp> <out.bmp> <outW> <outH> [NAME=VALUE ...]
//   header   prepended to the shader source (common.hlsli for the RetroArch ports, - for none)
//   in.bmp   32-bit BMP, top-down or bottom-up (an F11 snapshot, or an ffmpeg-downsampled one)
//   NAME=VALUE  extra #defines, e.g. GDV_MASK=1.0
// Build: cl /EHsc /D_CRT_SECURE_NO_WARNINGS crtcompare.cpp
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("FAIL %s %08lx\n", #x, _h); return 1; } } while (0)

static std::string ReadAll(const char* p) {
    std::string s; FILE* f = fopen(p, "rb"); if (!f) return s;
    char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s;
}

static const char* kVS =
    "struct O{float4 pos:SV_Position;float4 c0:COLOR0;float4 c1:COLOR1;float2 uv:TEXCOORD0;};"
    "O main(uint i:SV_VertexID){O o;float2 t=float2((i<<1)&2,i&2);o.uv=t;"
    "o.pos=float4(t*float2(2,-2)+float2(-1,1),0,1);o.c0=1;o.c1=0;return o;}";

struct Globals { float src[4], out[4], frame, pad[3]; };

int main(int argc, char** argv) {
    if (argc < 7) { printf("usage: crtcompare header shader in.bmp out.bmp outW outH [DEF=VAL...]\n"); return 2; }
    std::string bmp = ReadAll(argv[3]);
    if (bmp.size() < 54) { printf("can't read %s\n", argv[3]); return 1; }
    int w = *(int32_t*)&bmp[18], hRaw = *(int32_t*)&bmp[22], h = abs(hRaw);
    uint32_t off = *(uint32_t*)&bmp[10];
    if (*(uint16_t*)&bmp[28] != 32) { printf("need a 32-bit BMP\n"); return 1; }
    std::vector<uint8_t> px((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {  // normalise to top-down
        int sy = hRaw < 0 ? y : h - 1 - y;
        memcpy(&px[(size_t)y * w * 4], &bmp[off + (size_t)sy * w * 4], (size_t)w * 4);
    }
    int ow = atoi(argv[5]), oh = atoi(argv[6]);

    ID3D11Device* dev; ID3D11DeviceContext* ctx;
    CK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    D3D11_TEXTURE2D_DESC td = {(UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA init = {px.data(), (UINT)w * 4, 0};
    ID3D11Texture2D *src, *dst, *st; ID3D11ShaderResourceView* srv; ID3D11RenderTargetView* rtv;
    CK(dev->CreateTexture2D(&td, &init, &src)); CK(dev->CreateShaderResourceView(src, nullptr, &srv));
    td.Width = ow; td.Height = oh; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    CK(dev->CreateTexture2D(&td, nullptr, &dst)); CK(dev->CreateRenderTargetView(dst, nullptr, &rtv));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CK(dev->CreateTexture2D(&td, nullptr, &st));

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* samp; CK(dev->CreateSamplerState(&sd, &samp));

    Globals g = {{(float)w, (float)h, 1.0f / w, 1.0f / h}, {(float)ow, (float)oh, 1.0f / ow, 1.0f / oh}, 0};
    D3D11_BUFFER_DESC bd = {sizeof g, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    D3D11_SUBRESOURCE_DATA bi = {&g};
    ID3D11Buffer* cb; CK(dev->CreateBuffer(&bd, &bi, &cb));

    std::string ps = (strcmp(argv[1], "-") ? ReadAll(argv[1]) + "\n" : "") + ReadAll(argv[2]);
    std::vector<std::string> defStore;
    std::vector<D3D_SHADER_MACRO> defs;
    for (int a = 7; a < argc; a++) defStore.push_back(argv[a]);
    for (auto& d : defStore) {
        size_t eq = d.find('=');
        d[eq] = '\0';
        defs.push_back({d.c_str(), d.c_str() + eq + 1});
    }
    defs.push_back({nullptr, nullptr});

    ID3DBlob *vb, *pb, *err = nullptr;
    CK(D3DCompile(kVS, strlen(kVS), "vs", 0, 0, "main", "vs_5_0", 0, 0, &vb, 0));
    if (FAILED(D3DCompile(ps.data(), ps.size(), argv[2], defs.data(), 0, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pb, &err))) {
        printf("compile failed:\n%s\n", err ? (char*)err->GetBufferPointer() : "?");
        return 1;
    }
    ID3D11VertexShader* vs; ID3D11PixelShader* p;
    CK(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), 0, &vs));
    CK(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), 0, &p));

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)ow, (float)oh, 0, 1}; ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, 0, 0); ctx->PSSetShader(p, 0, 0);
    ctx->PSSetShaderResources(0, 1, &srv); ctx->PSSetSamplers(0, 1, &samp); ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->Draw(3, 0);
    ctx->CopyResource(st, dst);
    D3D11_MAPPED_SUBRESOURCE m; CK(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));

    FILE* f = fopen(argv[4], "wb");
    uint32_t row = ow * 4, img = row * oh;
    uint8_t hd[54] = {'B', 'M'};
    *(uint32_t*)(hd + 2) = 54 + img; *(uint32_t*)(hd + 10) = 54; *(uint32_t*)(hd + 14) = 40;
    *(int32_t*)(hd + 18) = ow; *(int32_t*)(hd + 22) = -oh; *(uint16_t*)(hd + 26) = 1; *(uint16_t*)(hd + 28) = 32;
    *(uint32_t*)(hd + 34) = img;
    fwrite(hd, 1, 54, f);
    for (int y = 0; y < oh; y++) fwrite((char*)m.pData + y * m.RowPitch, 1, row, f);
    fclose(f);
    ctx->Unmap(st, 0);
    printf("%s -> %s (%dx%d)\n", argv[2], argv[4], ow, oh);
    return 0;
}
