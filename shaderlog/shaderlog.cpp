// Stand-in D3DCOMPILER_43.dll for YOHANE THE PARHELION -BLAZE in the DEEPBLUE-.
// Copyright (C) 2026 Chris Etler
//
// This program is free software; you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version. It is distributed WITHOUT
// ANY WARRANTY; see LICENSE.txt for details.
//
// The game statically imports D3DCOMPILER_43.dll, so a copy in the game folder is loaded before
// the game creates its D3D11 device. That gives us a foothold to:
//   - patch game.exe's import of D3D11CreateDevice and hook the device it returns,
//   - dump every shader the game creates (decrypted bytecode + disassembly) to shaderlog\shaders\,
//   - log every render-target texture it creates (size/format) to shaderlog\log.txt,
//   - on F10, record one full frame of draw calls (shader, render target, viewport, input
//     texture per draw) to shaderlog\frame_NN.txt - this shows where level rendering ends and
//     the HUD begins. F11 does the same and also saves each render target as a BMP
//     (snap_NN_SS_*.bmp) at the moment the game switches away from it. Shift+F11 also saves
//     the full-size target after every draw and each input texture the first time it's used.
// D3DCompile / D3DReflect are still forwarded (and D3DCompile logged), though the shipped game
// turned out not to call them.
//
// CRT filter: when the game binds its level copy-back shader (ps_164f...), the one draw that
// copies the finished level back before the glow pass and HUD, one of the shaders compiled from
// <game>\crt\*.hlsl is bound instead. F7 cycles them, F8 recompiles, F9 toggles.
//
// Builds: the default build is the diagnostic one described above. Defining CRT_RELEASE
// compiles out the shader dumps, render-target logging, F10/F11 captures and the extra draw
// hooks, leaving only the CRT swap and its keys. The release build logs to <game>\crt\log.txt,
// rewritten each launch.

#define CRT_VERSION "1.0.0"

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------------------------
// basics

static CRITICAL_SECTION g_lock;
static std::string g_game;     // folder holding game.exe, with trailing slash
static std::string g_root;     // <game>\shaderlog\ (diagnostic output)
static std::string g_logPath;  // shaderlog\log.txt, or crt\log.txt in the release build
static DWORD g_t0;

struct Lock {
    Lock() { EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

static void Log(const char* fmt, ...) {
    FILE* f = fopen(g_logPath.c_str(), "ab");
    if (!f) return;
    fprintf(f, "%9.3fs ", (GetTickCount() - g_t0) / 1000.0);
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputs("\r\n", f);
    fclose(f);
}

static uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    auto b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static std::string Hex(uint64_t h) {
    char s[17];
    sprintf_s(s, "%016llx", (unsigned long long)h);
    return s;
}

static bool Exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static void WriteBytes(const std::string& p, const void* data, size_t n) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return;
    fwrite(data, 1, n, f);
    fclose(f);
}

static void Patch(void** slot, void* fn, void** orig) {
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    if (orig && !*orig) *orig = *slot;
    *slot = fn;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

// ---------------------------------------------------------------------------------------------
// real compilers

typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                     LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* PFN_Reflect)(LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT(WINAPI* PFN_Disasm)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

static PFN_Compile g_compile;
static PFN_Reflect g_reflect;
static PFN_Disasm g_disasm;    // from d3dcompiler_47, which disassembles newer bytecode better
static PFN_Compile g_compile47;  // also _47, for compiling the CRT shader

static void LoadReal() {
    static bool done;
    if (done) return;
    done = true;
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    if (HMODULE m = LoadLibraryA((std::string(sys) + "\\d3dcompiler_43.dll").c_str())) {
        g_compile = (PFN_Compile)GetProcAddress(m, "D3DCompile");
        g_reflect = (PFN_Reflect)GetProcAddress(m, "D3DReflect");
    }
    if (HMODULE m = LoadLibraryA((std::string(sys) + "\\d3dcompiler_47.dll").c_str()))
    {
        g_disasm = (PFN_Disasm)GetProcAddress(m, "D3DDisassemble");
        g_compile47 = (PFN_Compile)GetProcAddress(m, "D3DCompile");
    }
}

// ---------------------------------------------------------------------------------------------
// shader dumping

static std::unordered_map<void*, std::string> g_shaderName;  // shader object -> "ps_<hash>"

static std::string DumpShader(const char* kind, const void* code, SIZE_T size) {
    std::string name = std::string(kind) + "_" + Hex(Fnv(code, size));
#ifdef CRT_RELEASE
    return name;
#endif
    std::string base = g_root + "shaders\\" + name;
    if (!Exists(base + ".dxbc")) {
        WriteBytes(base + ".dxbc", code, size);
        LoadReal();
        ID3DBlob* text = nullptr;
        if (g_disasm && SUCCEEDED(g_disasm(code, size, 0, nullptr, &text)) && text) {
            WriteBytes(base + ".asm", text->GetBufferPointer(), text->GetBufferSize() - 1);
            text->Release();
        }
        Log("shader %s (%zu bytes)", name.c_str(), size);
    }
    return name;
}

// ---------------------------------------------------------------------------------------------
// frame capture state

static FILE* g_cap;       // open while a frame is being recorded
static int g_capCount;
static bool g_capArmed;   // F10 seen; start at next Present
static std::unordered_map<void*, int> g_texId;  // resource -> small id for readable logs
static std::string g_vs, g_ps, g_rt, g_srv0, g_srv1, g_vp;
static std::string g_lastDraw;
static int g_lastDrawN;

static std::string ResDesc(ID3D11Resource* r) {
    if (!r) return "-";
    auto it = g_texId.find(r);
    int id = it != g_texId.end() ? it->second : (g_texId[r] = (int)g_texId.size() + 1);
    char s[96];
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) {
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        t->Release();
        sprintf_s(s, "tex#%d %ux%u f%u", id, d.Width, d.Height, d.Format);
    } else {
        sprintf_s(s, "res#%d", id);
    }
    return s;
}

static std::string ViewDesc(ID3D11View* v) {
    if (!v) return "-";
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    std::string s = ResDesc(r);
    if (r) r->Release();
    return s;
}

static void CapLine(const std::string& line) {
    if (line == g_lastDraw) { g_lastDrawN++; return; }
    if (g_lastDrawN > 1) fprintf(g_cap, "      (x%d)\n", g_lastDrawN);
    g_lastDraw = line;
    g_lastDrawN = 1;
    fprintf(g_cap, "%s\n", line.c_str());
}

static void CapEvent(const char* fmt, ...) {  // non-draw events break the run of draws
    if (g_lastDrawN > 1) fprintf(g_cap, "      (x%d)\n", g_lastDrawN);
    g_lastDraw.clear();
    g_lastDrawN = 0;
    va_list a;
    va_start(a, fmt);
    vfprintf(g_cap, fmt, a);
    va_end(a);
    fputc('\n', g_cap);
}

// ---------------------------------------------------------------------------------------------
// F11 snapshots: when a render target is switched away from, save what's in it as a BMP.

static bool g_snap;     // this capture also takes snapshots
static bool g_snapDraws;  // Shift+F11: also snapshot the target after each draw, and each input texture once
static int g_drawSnaps;
static ID3D11Resource* g_srv0Res;  // resource behind PS slot 0 (the game's binding keeps it alive)
static std::unordered_map<void*, bool> g_dumpedInputs;
static int g_snapSeq;
static bool g_inSnap;   // our own Copy/Map calls shouldn't show up in the trace
static ID3D11DeviceContext* g_ctx;
static ID3D11Resource* g_curRT;  // resource behind RT slot 0

static void Snapshot(ID3D11Resource* res, const char* label) {
    if (!res || !g_ctx) return;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) return;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    bool rgba = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if ((!bgra && !rgba) || d.SampleDesc.Count > 1) { tex->Release(); return; }

    ID3D11Device* dev = nullptr;
    g_ctx->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
        g_inSnap = true;
        g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            char name[160];
            sprintf_s(name, "snap_%02d_%02d_%s.bmp", g_capCount, ++g_snapSeq, label);
            if (FILE* f = fopen((g_root + name).c_str(), "wb")) {
                // 32-bit top-down BMP (negative height); pixels are BGRA
                uint32_t row = d.Width * 4, img = row * d.Height;
                uint8_t h[54] = {'B', 'M'};
                *(uint32_t*)(h + 2) = 54 + img;
                *(uint32_t*)(h + 10) = 54;
                *(uint32_t*)(h + 14) = 40;
                *(int32_t*)(h + 18) = d.Width;
                *(int32_t*)(h + 22) = -(int32_t)d.Height;
                *(uint16_t*)(h + 26) = 1;
                *(uint16_t*)(h + 28) = 32;
                *(uint32_t*)(h + 34) = img;
                fwrite(h, 1, 54, f);
                std::string line(row, '\0');
                for (UINT y = 0; y < d.Height; y++) {
                    memcpy(&line[0], (uint8_t*)m.pData + y * m.RowPitch, row);
                    if (rgba) for (UINT x = 0; x < row; x += 4) std::swap(line[x], line[x + 2]);
                    fwrite(line.data(), 1, row, f);
                }
                fclose(f);
            }
            g_ctx->Unmap(st, 0);
            fprintf(g_cap, "SNAP %s\n", name);
        }
        g_inSnap = false;
        st->Release();
    }
    if (dev) dev->Release();
    tex->Release();
}

static void Snapshot(ID3D11Resource* res, const char* label);
static void SceneAfterDraw();

// Shift+F11: after each draw into a full-size target, save the target, and save each input
// texture the first time it's used. Capped so a busy frame can't write gigabytes.
static void AfterDraw() {
    if (!g_cap || !g_snapDraws || !g_curRT || g_drawSnaps >= 120) return;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(g_curRT->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) return;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    t->Release();
    if (d.Width < 1000) return;  // skip the glow chain and 480x270 layers
    if (g_srv0Res && !g_dumpedInputs[g_srv0Res]) {
        g_dumpedInputs[g_srv0Res] = true;
        std::string label = "input_" + ResDesc(g_srv0Res);
        for (char& ch : label) if (ch == ' ' || ch == '#') ch = '_';
        Snapshot(g_srv0Res, label.c_str());
    }
    char label[64];
    sprintf_s(label, "after_draw_%03d", ++g_drawSnaps);
    Snapshot(g_curRT, label);
}

static void CapDraw(const char* what, UINT count) {
    char s[64];
    sprintf_s(s, "  %s %u", what, count);
    CapLine(std::string(s) + "  " + g_vs + " " + g_ps + "  rt=[" + g_rt + "] vp=" + g_vp + "  in0=[" + g_srv0 +
            "] in1=[" + g_srv1 + "]");
}

// ---------------------------------------------------------------------------------------------
// CRT filter: the game's level copy-back shader is swapped for one of ours whenever the game
// binds it. Every <game>\crt\*.hlsl is an option (crt.hlsl first, then alphabetical).
// F7 cycles to the next, F8 recompiles them all, F9 toggles the filter. The current choice is
// saved to <game>\crt\selected.txt so it survives a restart; a fresh install starts on crt-geom.
//
// Menu-style scenes (the fortune parlor) are drawn entirely by the game's UI shader, so the
// copy-back swap never sees them. For those, kScenes names the texture sheet the scene's pixel art
// comes from (by a fingerprint of its contents) and how many draws from it come before the menu.
// After that many draws, the DLL inserts its own pass: copy the frame so far, run the selected
// filter over it, write it back, then hand the game its state back to draw the menu on top.

static std::string g_copyBackPS = "ps_164fc6f26724bc56";  // copies the finished level back before glow + HUD
static ID3D11Device* g_dev;
static ID3D11PixelShader* g_copyPS;  // the game's shader object
static ID3D11PixelShader* g_crtPS;   // the selected one of ours (null = none loaded)
static bool g_crtOn = true;
static const char* kDefaultCrt = "crt-geom.hlsl";  // used until the player picks one with F7

struct CrtShader { std::string name; ID3D11PixelShader* ps; };
static std::vector<CrtShader> g_crt;
static size_t g_crtIdx;

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreatePS)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
static PFN_CreatePS o_CreatePS;

static std::string CrtDir() { return g_game + "crt\\"; }

static bool ReadText(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static ID3D11PixelShader* CompileCrt(const std::string& name) {
    std::string src;
    if (!ReadText(CrtDir() + name, src)) return nullptr;
    PFN_Compile compile = g_compile47 ? g_compile47 : g_compile;
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile ? compile(src.data(), src.size(), name.c_str(), nullptr, nullptr, "main", "ps_5_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)
                         : E_FAIL;
    ID3D11PixelShader* ps = nullptr;
    if (FAILED(hr)) {
        Log("CRT: %s compile FAILED:\r\n%.*s", name.c_str(), err ? (int)err->GetBufferSize() : 0,
            err ? (const char*)err->GetBufferPointer() : "");
    } else if (FAILED(o_CreatePS(g_dev, code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps))) {
        Log("CRT: %s CreatePixelShader failed", name.c_str());
        ps = nullptr;
    }
    if (code) code->Release();
    if (err) err->Release();
    return ps;
}

static void SelectCrt(size_t idx, bool save) {
    if (g_crt.empty()) { g_crtPS = nullptr; return; }
    g_crtIdx = idx % g_crt.size();
    g_crtPS = g_crt[g_crtIdx].ps;
    Log("CRT: %s (%u of %u)%s", g_crt[g_crtIdx].name.c_str(), (unsigned)g_crtIdx + 1, (unsigned)g_crt.size(),
        g_crtOn ? "" : " - filter is off, F9 to enable");
    if (save)
        if (FILE* f = fopen((CrtDir() + "selected.txt").c_str(), "wb")) {
            fputs(g_crt[g_crtIdx].name.c_str(), f);
            fclose(f);
        }
}

// (Re)loads every shader in the crt folder. A file that fails to compile keeps its previously
// loaded version, so a typo during tuning doesn't drop you back to the unfiltered image.
static void LoadCrt() {
    if (!g_dev || !o_CreatePS) return;
    LoadReal();
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((CrtDir() + "*.hlsl").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do names.push_back(fd.cFileName); while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        bool ab = _stricmp(a.c_str(), "crt.hlsl") == 0, bb = _stricmp(b.c_str(), "crt.hlsl") == 0;
        return ab != bb ? ab : _stricmp(a.c_str(), b.c_str()) < 0;
    });

    std::string current = g_crt.empty() ? "" : g_crt[g_crtIdx].name;
    if (current.empty()) ReadText(CrtDir() + "selected.txt", current);

    std::vector<CrtShader> next;
    for (auto& n : names) {
        ID3D11PixelShader* ps = CompileCrt(n);
        if (!ps)
            for (auto& old : g_crt)
                if (old.name == n && old.ps) { ps = old.ps; old.ps = nullptr; Log("CRT: keeping previous %s", n.c_str()); }
        if (ps) next.push_back({n, ps});
    }
    for (auto& old : g_crt)
        if (old.ps) old.ps->Release();
    g_crt = next;

    if (g_crt.empty()) {
        g_crtPS = nullptr;
        Log("CRT: no shaders loaded from %s, filter off", CrtDir().c_str());
        return;
    }
    std::string list;
    for (auto& c : g_crt) list += (list.empty() ? "" : ", ") + c.name;
    Log("CRT: loaded %s", list.c_str());
    // the saved choice wins; with none (a fresh install), start on the default filter
    if (current.empty()) current = kDefaultCrt;
    size_t idx = 0;
    for (size_t i = 0; i < g_crt.size(); i++)
        if (_stricmp(g_crt[i].name.c_str(), current.c_str()) == 0) idx = i;
    SelectCrt(idx, false);
}

// ---------------------------------------------------------------------------------------------
// ID3D11DeviceContext hooks (vtable slots)

typedef void(STDMETHODCALLTYPE* PFN_PSSetSRV)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
typedef void(STDMETHODCALLTYPE* PFN_SetPS)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_SetVS)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawIndexedInst)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_DrawInst)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* PFN_OMSetRT)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* PFN_SetVP)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void(STDMETHODCALLTYPE* PFN_CopySub)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void(STDMETHODCALLTYPE* PFN_Copy)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* PFN_ClearRTV)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE* PFN_Resolve)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);

static PFN_PSSetSRV o_PSSetSRV;
static PFN_SetPS o_SetPS;
static PFN_SetVS o_SetVS;
static PFN_DrawIndexed o_DrawIndexed;
static PFN_Draw o_Draw;
static PFN_DrawIndexedInst o_DrawIndexedInst;
static PFN_DrawInst o_DrawInst;
static PFN_OMSetRT o_OMSetRT;
static PFN_SetVP o_SetVP;
static PFN_CopySub o_CopySub;
static PFN_Copy o_Copy;
static PFN_ClearRTV o_ClearRTV;
static PFN_Resolve o_Resolve;

// The D3D11 runtime keeps the immediate context's vtable in writable heap memory and swaps
// Draw/Copy/Clear entries for different implementations as pipeline state changes, which wipes
// out our hooks. So while capturing, every hook re-asserts all of them after calling through, and
// adopts whatever the runtime last put in a slot as the "original" to call.
struct Slot { int index; void* hook; void** orig; };
static void** g_ctxVt;
static Slot g_slots[13];
static int g_slotCount;

static void Repatch() {
    if (!g_ctxVt) return;
    for (int i = 0; i < g_slotCount; i++) {
        Slot& s = g_slots[i];
        if (g_ctxVt[s.index] != s.hook) { *s.orig = g_ctxVt[s.index]; g_ctxVt[s.index] = s.hook; }
    }
}

static void STDMETHODCALLTYPE H_PSSetSRV(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11ShaderResourceView* const* v) {
    o_PSSetSRV(c, start, n, v);
    Lock l;
    if (start == 0 && n > 0) {  // slot 0 is tracked always: scene draw counting needs it
        g_srv0Res = nullptr;
        if (v && v[0]) { v[0]->GetResource(&g_srv0Res); g_srv0Res->Release(); }  // the game's binding keeps it alive
    }
    if (g_cap)
        for (UINT i = 0; i < n; i++) {
            if (start + i == 0) g_srv0 = ViewDesc(v ? v[i] : nullptr);
            if (start + i == 1) g_srv1 = ViewDesc(v ? v[i] : nullptr);
        }
    Repatch();
}
static void STDMETHODCALLTYPE H_SetPS(ID3D11DeviceContext* c, ID3D11PixelShader* s, ID3D11ClassInstance* const* ci, UINT n) {
    bool crt = s && s == g_copyPS && g_crtOn && g_crtPS;
    o_SetPS(c, crt ? g_crtPS : s, ci, n);
    Lock l;
    Repatch();
    if (!g_cap) return;
    auto it = g_shaderName.find(s);
    g_ps = s ? (it != g_shaderName.end() ? it->second : "ps_?") : "ps_none";
    if (crt) g_ps += "->CRT";
    Repatch();
}
static void STDMETHODCALLTYPE H_SetVS(ID3D11DeviceContext* c, ID3D11VertexShader* s, ID3D11ClassInstance* const* ci, UINT n) {
    o_SetVS(c, s, ci, n);
    Lock l;
    Repatch();
    if (!g_cap) return;
    auto it = g_shaderName.find(s);
    g_vs = s ? (it != g_shaderName.end() ? it->second : "vs_?") : "vs_none";
    Repatch();
}
static void STDMETHODCALLTYPE H_DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT a, INT b) {
    if (g_cap) { Lock l; CapDraw("DrawIndexed", n); }
    o_DrawIndexed(c, n, a, b);
    { Lock l; SceneAfterDraw(); if (g_cap) AfterDraw(); Repatch(); }
}
static void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, UINT n, UINT a) {
    if (g_cap) { Lock l; CapDraw("Draw", n); }
    o_Draw(c, n, a);
    { Lock l; SceneAfterDraw(); if (g_cap) AfterDraw(); Repatch(); }
}
static void STDMETHODCALLTYPE H_DrawIndexedInst(ID3D11DeviceContext* c, UINT n, UINT inst, UINT a, INT b, UINT d) {
    if (g_cap) { Lock l; CapDraw("DrawIndexedInstanced", n * inst); }
    o_DrawIndexedInst(c, n, inst, a, b, d);
    { Lock l; SceneAfterDraw(); if (g_cap) AfterDraw(); Repatch(); }
}
static void STDMETHODCALLTYPE H_DrawInst(ID3D11DeviceContext* c, UINT n, UINT inst, UINT a, UINT b) {
    if (g_cap) { Lock l; CapDraw("DrawInstanced", n * inst); }
    o_DrawInst(c, n, inst, a, b);
    { Lock l; SceneAfterDraw(); if (g_cap) AfterDraw(); Repatch(); }
}
static void STDMETHODCALLTYPE H_OMSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v, ID3D11DepthStencilView* d) {
    o_OMSetRT(c, n, v, d);
    Lock l;
    ID3D11Resource* rt0 = nullptr;
    if (n && v && v[0]) { v[0]->GetResource(&rt0); rt0->Release(); }  // the game's binding keeps it alive
    if (!g_cap) { g_curRT = rt0; Repatch(); return; }
    if (g_snap && g_curRT && g_curRT != rt0) {
        // snapshot what the previous target ended up holding, now it's unbound
        std::string label = ResDesc(g_curRT);
        for (char& ch : label) if (ch == ' ' || ch == '#') ch = '_';
        if (g_lastDrawN > 1) fprintf(g_cap, "      (x%d)\n", g_lastDrawN);
        g_lastDraw.clear();
        g_lastDrawN = 0;
        Snapshot(g_curRT, label.c_str());
    }
    g_curRT = rt0;
    std::string s;
    for (UINT i = 0; i < n; i++) s += (i ? " + " : "") + ViewDesc(v ? v[i] : nullptr);
    if (d) s += " depth";
    if (s != g_rt) CapEvent("RT -> %s", s.c_str());
    g_rt = s;
    Repatch();
}
static void STDMETHODCALLTYPE H_SetVP(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
    o_SetVP(c, n, v);
    if (!g_cap) return;
    Lock l;
    if (n && v) {
        char s[64];
        sprintf_s(s, "%gx%g@%g,%g", v[0].Width, v[0].Height, v[0].TopLeftX, v[0].TopLeftY);
        g_vp = s;
    }
    Repatch();
}
static void STDMETHODCALLTYPE H_CopySub(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT a, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT b, const D3D11_BOX* box) {
    if (g_cap && !g_inSnap) { Lock l; CapEvent("COPY region %s -> %s", ResDesc(src).c_str(), ResDesc(dst).c_str()); }
    o_CopySub(c, dst, a, x, y, z, src, b, box);
    if (g_cap) { Lock l; Repatch(); }
}
static void STDMETHODCALLTYPE H_Copy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (g_cap) { Lock l; CapEvent("COPY %s -> %s", ResDesc(src).c_str(), ResDesc(dst).c_str()); }
    o_Copy(c, dst, src);
    if (g_cap) { Lock l; Repatch(); }
}
static void STDMETHODCALLTYPE H_ClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT col[4]) {
    if (g_cap) { Lock l; CapEvent("CLEAR %s (%.2f %.2f %.2f %.2f)", ViewDesc(v).c_str(), col[0], col[1], col[2], col[3]); }
    o_ClearRTV(c, v, col);
    if (g_cap) { Lock l; Repatch(); }
}
static void STDMETHODCALLTYPE H_Resolve(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT a, ID3D11Resource* src, UINT b, DXGI_FORMAT f) {
    if (g_cap) { Lock l; CapEvent("RESOLVE %s -> %s", ResDesc(src).c_str(), ResDesc(dst).c_str()); }
    o_Resolve(c, dst, a, src, b, f);
    if (g_cap) { Lock l; Repatch(); }
}

static void HookContext(ID3D11DeviceContext* ctx) {
    Slot s[] = {
        {9, H_SetPS, (void**)&o_SetPS},  // first: the only one the release build needs
        {8, H_PSSetSRV, (void**)&o_PSSetSRV},
        {11, H_SetVS, (void**)&o_SetVS},              {12, H_DrawIndexed, (void**)&o_DrawIndexed},
        {13, H_Draw, (void**)&o_Draw},                {20, H_DrawIndexedInst, (void**)&o_DrawIndexedInst},
        {21, H_DrawInst, (void**)&o_DrawInst},        {33, H_OMSetRT, (void**)&o_OMSetRT},
        {44, H_SetVP, (void**)&o_SetVP},              {46, H_CopySub, (void**)&o_CopySub},
        {47, H_Copy, (void**)&o_Copy},                {50, H_ClearRTV, (void**)&o_ClearRTV},
        {57, H_Resolve, (void**)&o_Resolve},
    };
    memcpy(g_slots, s, sizeof s);
    g_slotCount = (int)(sizeof s / sizeof s[0]);
    void** vt = *(void***)ctx;
    for (int i = 0; i < g_slotCount; i++) Patch(&vt[g_slots[i].index], g_slots[i].hook, g_slots[i].orig);
    g_ctxVt = vt;
    g_ctx = ctx;
}

// ---------------------------------------------------------------------------------------------
// Menu-style scenes: insert the CRT filter partway through the UI stage.

struct Scene { const char* name; UINT width, height; uint64_t fingerprint; int drawsBeforeMenu; };
static const Scene kScenes[] = {
    // Fortune parlor: room background, Yohane, Lailaps, then the menu panel and text.
    {"fortune parlor", 2048, 2048, 0xe5dab5b2e840e42dull, 3},
};
static const int kSceneCount = (int)(sizeof kScenes / sizeof kScenes[0]);

static std::unordered_map<void*, int> g_texScene;  // texture -> index into kScenes, or -1
static int g_sceneDraws[kSceneCount];               // draws from each scene's sheet this frame
static bool g_sceneLogged[kSceneCount];
static bool g_inScenePass;
static ID3D11VertexShader* g_fullscreenVS;
static ID3D11DepthStencilState* g_noDepth;
static ID3D11Texture2D* g_sceneCopy;
static ID3D11ShaderResourceView* g_sceneCopySRV;

// Fingerprint: FNV-1a over a 256x64 patch of mip 0 at (w/4, h/8), read back once per texture.
static int SceneFor(ID3D11Resource* res) {
    auto it = g_texScene.find(res);
    if (it != g_texScene.end()) return it->second;
    int found = -1;
    ID3D11Texture2D* tex = nullptr;
    if (g_ctx && g_dev && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        bool sized = false;
        for (auto& sc : kScenes) sized |= d.Width == sc.width && d.Height == sc.height;
        bool fmt = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
        if (sized && fmt && d.SampleDesc.Count == 1) {
            D3D11_TEXTURE2D_DESC sd = {256, 64, 1, 1, d.Format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
            ID3D11Texture2D* st = nullptr;
            if (SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &st))) {
                UINT x0 = d.Width / 4, y0 = d.Height / 8;
                D3D11_BOX box = {x0, y0, 0, x0 + 256, y0 + 64, 1};
                g_inSnap = true;
                g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, &box);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                    uint64_t h = 1469598103934665603ull;
                    for (UINT y = 0; y < 64; y++) h = Fnv((uint8_t*)m.pData + y * m.RowPitch, 256 * 4, h);
                    g_ctx->Unmap(st, 0);
                    for (int i = 0; i < kSceneCount; i++)
                        if (kScenes[i].fingerprint == h) found = i;
#ifndef CRT_RELEASE
                    Log("texture %ux%u fingerprint %016llx%s", d.Width, d.Height, (unsigned long long)h,
                        found >= 0 ? (std::string(" = ") + kScenes[found].name).c_str() : "");
#endif
                }
                g_inSnap = false;
                st->Release();
            }
        }
        tex->Release();
    }
    g_texScene[res] = found;
    return found;
}

static void LoadScenePass() {
    if (g_fullscreenVS || !g_dev) return;
    // Same output layout as the game's vertex shader, so the filters' PSIn matches.
    static const char* vs =
        "struct O{float4 pos:SV_Position;float4 c0:COLOR0;float4 c1:COLOR1;float2 uv:TEXCOORD0;};"
        "O main(uint i:SV_VertexID){O o;float2 t=float2((i<<1)&2,i&2);o.uv=t;"
        "o.pos=float4(t*float2(2,-2)+float2(-1,1),0,1);o.c0=1;o.c1=0;return o;}";
    PFN_Compile compile = g_compile47 ? g_compile47 : g_compile;
    ID3DBlob* code = nullptr;
    if (compile && SUCCEEDED(compile(vs, strlen(vs), "scene_vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, nullptr))) {
        g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_fullscreenVS);
        code->Release();
    }
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    g_dev->CreateDepthStencilState(&dd, &g_noDepth);
    if (!g_fullscreenVS || !g_noDepth) Log("CRT: scene pass setup failed; menu scenes won't be filtered");
}

// Runs the selected filter over the current render target, in place.
static void RunScenePass(ID3D11Resource* target) {
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(target->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) return;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    tex->Release();
    if (g_sceneCopy) {
        D3D11_TEXTURE2D_DESC cd;
        g_sceneCopy->GetDesc(&cd);
        if (cd.Width != d.Width || cd.Height != d.Height || cd.Format != d.Format) {
            g_sceneCopySRV->Release(); g_sceneCopySRV = nullptr;
            g_sceneCopy->Release(); g_sceneCopy = nullptr;
        }
    }
    if (!g_sceneCopy) {
        D3D11_TEXTURE2D_DESC cd = {d.Width, d.Height, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(g_dev->CreateTexture2D(&cd, nullptr, &g_sceneCopy))) return;
        if (FAILED(g_dev->CreateShaderResourceView(g_sceneCopy, nullptr, &g_sceneCopySRV))) {
            g_sceneCopy->Release(); g_sceneCopy = nullptr;
            return;
        }
    }
    ID3D11DeviceContext* c = g_ctx;
    g_inScenePass = g_inSnap = true;
    c->CopySubresourceRegion(g_sceneCopy, 0, 0, 0, 0, target, 0, nullptr);

    // save the state we touch
    ID3D11VertexShader* vs = nullptr; ID3D11ClassInstance* vsCI[256]; UINT vsN = 256;
    ID3D11PixelShader* ps = nullptr; ID3D11ClassInstance* psCI[256]; UINT psN = 256;
    c->VSGetShader(&vs, vsCI, &vsN);
    c->PSGetShader(&ps, psCI, &psN);
    ID3D11InputLayout* il = nullptr; c->IAGetInputLayout(&il);
    D3D11_PRIMITIVE_TOPOLOGY topo; c->IAGetPrimitiveTopology(&topo);
    ID3D11ShaderResourceView* srv0 = nullptr; c->PSGetShaderResources(0, 1, &srv0);
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT bm; c->OMGetBlendState(&bs, bf, &bm);
    ID3D11DepthStencilState* ds = nullptr; UINT sref; c->OMGetDepthStencilState(&ds, &sref);
    ID3D11RasterizerState* rs = nullptr; c->RSGetState(&rs);

    // draw the filter over the whole target
    c->VSSetShader(g_fullscreenVS, nullptr, 0);
    c->PSSetShader(g_crtPS, nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->PSSetShaderResources(0, 1, &g_sceneCopySRV);
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(g_noDepth, 0);
    c->RSSetState(nullptr);
    c->Draw(3, 0);

    // put the game's state back
    c->VSSetShader(vs, vsCI, vsN);
    c->PSSetShader(ps, psCI, psN);
    c->IASetInputLayout(il);
    c->IASetPrimitiveTopology(topo);
    c->PSSetShaderResources(0, 1, &srv0);
    c->OMSetBlendState(bs, bf, bm);
    c->OMSetDepthStencilState(ds, sref);
    c->RSSetState(rs);
    if (vs) vs->Release();
    for (UINT i = 0; i < vsN; i++) if (vsCI[i]) vsCI[i]->Release();
    if (ps) ps->Release();
    for (UINT i = 0; i < psN; i++) if (psCI[i]) psCI[i]->Release();
    if (il) il->Release();
    if (srv0) srv0->Release();
    if (bs) bs->Release();
    if (ds) ds->Release();
    if (rs) rs->Release();
    g_inScenePass = g_inSnap = false;
}

// Called after every game draw. Counts draws from a scene's sheet into the full-size image and
// runs the filter once the scene's art is down.
static void SceneAfterDraw() {
    if (g_inScenePass || !g_crtOn || !g_crtPS || !g_srv0Res || !g_curRT) return;
    int sc = SceneFor(g_srv0Res);
    if (sc < 0) return;
    if (++g_sceneDraws[sc] != kScenes[sc].drawsBeforeMenu) return;
    LoadScenePass();
    if (!g_fullscreenVS || !g_noDepth) return;
    RunScenePass(g_curRT);
    if (!g_sceneLogged[sc]) {
        g_sceneLogged[sc] = true;
        Log("CRT: filtering scene \"%s\"", kScenes[sc].name);
    }
#ifndef CRT_RELEASE
    if (g_cap) CapEvent("SCENE PASS (%s)", kScenes[sc].name);
#endif
}

// ---------------------------------------------------------------------------------------------
// IDXGISwapChain::Present - frame boundary and F10 trigger

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
static PFN_Present o_Present;

static HRESULT STDMETHODCALLTYPE H_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    {
        Lock l;
        for (int& n : g_sceneDraws) n = 0;
        static unsigned frames;
        if (++frames == 600 && !g_copyPS)  // ~10 s in; the game creates it at startup
            Log("CRT: the level copy-back shader (%s) never appeared, so the filter can't attach. "
                "The game has probably been updated; check for a newer version of this mod.", g_copyBackPS.c_str());
#ifndef CRT_RELEASE
        if (g_cap) {
            if (g_snap && g_curRT) {
                std::string label = ResDesc(g_curRT);
                for (char& ch : label) if (ch == ' ' || ch == '#') ch = '_';
                CapEvent("(final target before present)");
                Snapshot(g_curRT, label.c_str());
            }
            CapEvent("PRESENT");
            fclose(g_cap);
            g_cap = nullptr;
            g_snap = g_snapDraws = false;
            Log("frame_%02d.txt written", g_capCount);
        }
#endif
        Repatch();  // the runtime can swap context vtable entries at any time; keep PSSetShader ours
        bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0, f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0,
             f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        static bool f7was, f8was, f9was;
        if (f7 && !f7was) {
            if (!g_crtOn) { g_crtOn = true; SelectCrt(g_crtIdx, true); }  // first press from off just turns it on
            else SelectCrt(g_crtIdx + 1, true);
        }
        if (f8 && !f8was) LoadCrt();
        if (f9 && !f9was) { g_crtOn = !g_crtOn; Log("CRT: %s", g_crtOn ? "on" : "off"); }
        f7was = f7;
        f8was = f8;
        f9was = f9;
#ifndef CRT_RELEASE
        bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0, f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        static bool f10was, f11was;
        if (f10 && !f10was) g_capArmed = true;
        if (f11 && !f11was) {
            g_capArmed = g_snap = true;
            g_snapDraws = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        }
        f10was = f10;
        f11was = f11;
        if (g_capArmed) {
            g_capArmed = false;
            char p[32];
            sprintf_s(p, "frame_%02d.txt", ++g_capCount);
            g_cap = fopen((g_root + p).c_str(), "w");
            if (g_cap) {
                ID3D11Texture2D* bb = nullptr;
                if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
                    fprintf(g_cap, "backbuffer = %s\n\n", ResDesc(bb).c_str());
                    bb->Release();
                }
                g_vs = g_ps = g_rt = g_srv0 = g_srv1 = g_vp = "?";  // state left over from before capture
                g_lastDraw.clear();
                g_lastDrawN = 0;
                g_curRT = nullptr;
                g_srv0Res = nullptr;
                g_snapSeq = g_drawSnaps = 0;
                g_dumpedInputs.clear();
                Repatch();
            }
        }
#endif
    }
    return o_Present(sc, sync, flags);
}

// ---------------------------------------------------------------------------------------------
// IDXGIFactory::CreateSwapChain / IDXGIFactory2::CreateSwapChainForHwnd

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSC)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSCHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
static PFN_CreateSC o_CreateSC;
static PFN_CreateSCHwnd o_CreateSCHwnd;

static void HookSwapChain(IDXGISwapChain* sc) {
    Lock l;
    void** vt = *(void***)sc;
    if (vt[8] != (void*)H_Present) Patch(&vt[8], H_Present, (void**)&o_Present);
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(sc->GetDesc(&d)))
        Log("swapchain %ux%u fmt=%u buffers=%u windowed=%d swapeffect=%u", d.BufferDesc.Width, d.BufferDesc.Height,
            d.BufferDesc.Format, d.BufferCount, d.Windowed, d.SwapEffect);
}

static HRESULT STDMETHODCALLTYPE H_CreateSC(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    HRESULT hr = o_CreateSC(f, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateSCHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_CreateSCHwnd(f, dev, w, d, fs, o, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}

static void HookFactory(ID3D11Device* dev) {
    IDXGIDevice* dd = nullptr;
    IDXGIAdapter* ad = nullptr;
    IDXGIFactory* f = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&ad)) &&
        SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), (void**)&f))) {
        Patch(&(*(void***)f)[10], H_CreateSC, (void**)&o_CreateSC);
        IDXGIFactory2* f2 = nullptr;
        if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
            Patch(&(*(void***)f2)[15], H_CreateSCHwnd, (void**)&o_CreateSCHwnd);
            f2->Release();
        }
        Log("factory hooked");
    } else {
        Log("factory hook FAILED");
    }
    if (f) f->Release();
    if (ad) ad->Release();
    if (dd) dd->Release();
}

// ---------------------------------------------------------------------------------------------
// ID3D11Device hooks

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateVS)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTex2D)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static PFN_CreateVS o_CreateVS;
static PFN_CreateTex2D o_CreateTex2D;

static HRESULT STDMETHODCALLTYPE H_CreateVS(ID3D11Device* d, const void* code, SIZE_T n, ID3D11ClassLinkage* cl, ID3D11VertexShader** out) {
    HRESULT hr = o_CreateVS(d, code, n, cl, out);
    if (SUCCEEDED(hr) && out && *out) { Lock l; g_shaderName[*out] = DumpShader("vs", code, n); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreatePS(ID3D11Device* d, const void* code, SIZE_T n, ID3D11ClassLinkage* cl, ID3D11PixelShader** out) {
    HRESULT hr = o_CreatePS(d, code, n, cl, out);
    if (SUCCEEDED(hr) && out && *out) {
        Lock l;
        std::string name = DumpShader("ps", code, n);
        g_shaderName[*out] = name;
        if (name == g_copyBackPS) {
            g_copyPS = *out;
            g_dev = d;
            Log("CRT: found the level copy-back shader");
            LoadCrt();
        }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateTex2D(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    HRESULT hr = o_CreateTex2D(d, desc, init, out);
    if (SUCCEEDED(hr) && out && *out) { Lock l; g_texScene.erase(*out); }  // address may be reused
#ifndef CRT_RELEASE
    if (SUCCEEDED(hr) && desc && (desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
        Lock l;
        Log("rendertarget %s %ux%u fmt=%u bind=%x msaa=%u", out && *out ? ResDesc(*out).c_str() : "?", desc->Width,
            desc->Height, desc->Format, desc->BindFlags, desc->SampleDesc.Count);
    }
#endif
    return hr;
}

static void HookDevice(ID3D11Device* dev) {
    static bool done;
    if (done) return;
    done = true;
    void** vt = *(void***)dev;
    Patch(&vt[5], H_CreateTex2D, (void**)&o_CreateTex2D);
#ifndef CRT_RELEASE
    Patch(&vt[12], H_CreateVS, (void**)&o_CreateVS);
#endif
    Patch(&vt[15], H_CreatePS, (void**)&o_CreatePS);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (ctx) { HookContext(ctx); ctx->Release(); }
    HookFactory(dev);
    Log("device hooked (feature level %x)", dev->GetFeatureLevel());
}

// ---------------------------------------------------------------------------------------------
// game.exe import hooks

typedef HRESULT(WINAPI* PFN_CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                          ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef void*(WINAPI* PFN_Create9)(UINT);
static PFN_CreateDevice o_CreateDevice;
static PFN_Create9 o_Create9;

static HRESULT WINAPI H_CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags, const D3D_FEATURE_LEVEL* fl,
                                     UINT nfl, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    Log("D3D11CreateDevice flags=%x", flags);
    HRESULT hr = o_CreateDevice(a, t, sw, flags, fl, nfl, sdk, dev, got, ctx);
    if (SUCCEEDED(hr) && dev && *dev) HookDevice(*dev);
    return hr;
}
static void* WINAPI H_Create9(UINT sdk) {
    Log("Direct3DCreate9 called - game is on the DX9 path, hooks won't see it");
    return o_Create9(sdk);
}

static void PatchImport(const char* dll, const char* fn, void* hook, void** orig) {
    auto base = (BYTE*)GetModuleHandleA(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll)) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (strcmp((char*)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, fn)) continue;
            Patch((void**)&iat->u1.Function, hook, orig);
            Log("patched %s!%s", dll, fn);
            return;
        }
    }
    Log("import %s!%s NOT FOUND", dll, fn);
}

// ---------------------------------------------------------------------------------------------
// D3DCompiler exports (forwarded)

extern "C" HRESULT WINAPI Proxy_D3DCompile(LPCVOID src, SIZE_T srcSize, LPCSTR srcName, const D3D_SHADER_MACRO* defines,
                                           ID3DInclude* include, LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                           ID3DBlob** code, ID3DBlob** errors) {
    {
        Lock l;
        LoadReal();
#ifndef CRT_RELEASE
        std::string name = "src_" + Hex(Fnv(src, srcSize)) + ".hlsl";
        if (!Exists(g_root + "shaders\\" + name)) WriteBytes(g_root + "shaders\\" + name, src, srcSize);
        Log("D3DCompile %s entry=%s target=%s", name.c_str(), entry ? entry : "?", target ? target : "?");
#endif
    }
    return g_compile ? g_compile(src, srcSize, srcName, defines, include, entry, target, flags1, flags2, code, errors) : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_D3DReflect(LPCVOID data, SIZE_T size, REFIID iid, void** out) {
    {
        Lock l;
        LoadReal();
    }
    return g_reflect ? g_reflect(data, size, iid, out) : E_FAIL;
}

// ---------------------------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    InitializeCriticalSection(&g_lock);
    g_t0 = GetTickCount();
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_game = exe;
    g_game.resize(g_game.find_last_of("\\/") + 1);
    g_root = g_game + "shaderlog\\";
#ifdef CRT_RELEASE
    g_logPath = g_game + "crt\\log.txt";
    if (FILE* f = fopen(g_logPath.c_str(), "wb")) fclose(f);  // fresh log each launch
    Log("Yohane CRT %s loaded into %s", CRT_VERSION, exe);
#else
    g_logPath = g_root + "log.txt";
    CreateDirectoryA(g_root.c_str(), nullptr);
    CreateDirectoryA((g_root + "shaders").c_str(), nullptr);
    Log("---- Yohane CRT %s (diagnostic build) loaded into %s", CRT_VERSION, exe);
    if (const char* arm = getenv("SHADERLOG_ARM")) {  // test hook: capture the first frame without a key
        g_capArmed = g_snap = true;
        g_snapDraws = strcmp(arm, "draws") == 0;  // SHADERLOG_ARM=draws behaves like Shift+F11
    }
#endif
    if (const char* t = getenv("SHADERLOG_CRT_TARGET")) g_copyBackPS = t;  // test hook: swap a different shader
    // game.exe's imports are already bound by the time our DllMain runs, so these patches stick.
    PatchImport("d3d11.dll", "D3D11CreateDevice", H_CreateDevice, (void**)&o_CreateDevice);
    PatchImport("d3d9.dll", "Direct3DCreate9", H_Create9, (void**)&o_Create9);
    return TRUE;
}
