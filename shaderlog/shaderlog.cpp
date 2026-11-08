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
// HDR output (opt-in through <game>\crt\hdr.ini): see "HDR output" below. F6 toggles highlights.
//
// Builds: the default build is the diagnostic one described above. Defining CRT_RELEASE
// compiles out the shader dumps, render-target logging, F10/F11 captures and the extra draw
// hooks, leaving only the CRT swap and its keys. The release build logs to <game>\crt\log.txt,
// rewritten each launch.

#define CRT_VERSION "1.1.0-dev"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
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

// ---------------------------------------------------------------------------------------------
// HDR output, opt-in through <game>\crt\hdr.ini ("enabled" is read at launch; F8 rereads the rest).
// The swap chain becomes scRGB (FP16, linear, 1.0 = 80 nits). The game's own render targets stay
// 8-bit, so every game draw (glow, HUD, menus, fades) clips at each step exactly as in SDR.
// The filter is drawn a second time into a private FP16 "highlight layer" that keeps its output
// unclipped. A last pass before Present turns the game's gamma-encoded SDR values into light
// (1.0 = paper white) and, where the screen still shows what the filter drew, adds back the part
// of the layer above 1.0, mapped through the game's final scale and rolled off toward the peak.
// F6 switches highlights off: filters built as for SDR and the output clipped at paper white.

static bool g_hdr;                    // hdr.ini says enabled
static bool g_hdrActive;              // the swap chain really is scRGB
static bool g_hdrHighlights = true;   // F6
static float g_paperWhite = 200.0f;   // nits for SDR white (HUD, menus, cutscenes)
static float g_peak;                  // nits; 0 = what the display reports
static float g_crtBoost = 1.5f;       // linear gain on the filter's output
static float g_displayPeak;           // reported by the display; 0 = unknown
// Set when the CRT filter draws (level copy-back or a scene pass); cleared each Present.
static bool g_crtThisFrame;
static bool g_crtBound;  // the game's copy-back shader is bound and swapped for the filter

// Highlight layer: the filter's unclipped output, redrawn into our own FP16 target.
static bool g_hlValid;                     // the layer was drawn this frame
static ID3D11Resource* g_hlSource;         // the game target the filter drew into (the 1080p working image)
static bool g_hlScaled;                    // the game's final scale (g_hlSource -> FP16 back buffer) was seen
static ID3D11Resource* g_hlScaleRT;        // where that final scale drew
static D3D11_VIEWPORT g_hlVP;              // its viewport
static bool g_hlLinear = true;             // its sampler magnifies bilinearly (else point)
static bool HlBegin(ID3D11DeviceContext* c);
static void HlEnd(ID3D11DeviceContext* c);
static void HlNoteDraw(ID3D11DeviceContext* c);
static bool HlEnsure(UINT w, UINT h);
static ID3D11RenderTargetView* g_hlRTV;

static float HdrPeak() { return g_peak > 0 ? g_peak : g_displayPeak > 0 ? g_displayPeak : 1000.0f; }

static void LoadHdrConfig(bool atLaunch) {
    std::string s;
    if (!ReadText(CrtDir() + "hdr.ini", s)) return;
    auto trim = [](const std::string& t) {
        size_t a = t.find_first_not_of(" \t\r"), b = t.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    };
    for (size_t p = 0; p < s.size();) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(p, e - p);
        p = e + 1;
        line = line.substr(0, line.find(';'));
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "enabled" && atLaunch) g_hdr = v == "1" || _stricmp(v.c_str(), "true") == 0;
        else if (k == "paper_white") g_paperWhite = (float)atof(v.c_str());
        else if (k == "peak") g_peak = _stricmp(v.c_str(), "auto") == 0 ? 0.0f : (float)atof(v.c_str());
        else if (k == "crt_boost") g_crtBoost = (float)atof(v.c_str());
    }
    g_paperWhite = std::min(std::max(g_paperWhite, 80.0f), 1000.0f);
    g_crtBoost = std::min(std::max(g_crtBoost, 1.0f), 10.0f);
    if (g_peak > 0) g_peak = std::max(g_peak, g_paperWhite);
    if (g_hdr) {
        char peak[32];
        sprintf_s(peak, g_peak > 0 ? "%.0f nits" : "auto", g_peak);
        Log("HDR: on, paper white %.0f nits, peak %s, filter boost %.2f", g_paperWhite, peak, g_crtBoost);
    }
}

static ID3D11PixelShader* CompileCrt(const std::string& name) {
    std::string src;
    if (!ReadText(CrtDir() + name, src)) return nullptr;
    PFN_Compile compile = g_compile47 ? g_compile47 : g_compile;
    ID3DBlob *code = nullptr, *err = nullptr;
    char boost[32];
    sprintf_s(boost, "%f", g_crtBoost);
    D3D_SHADER_MACRO hdr[] = {{"CRT_HDR", "1"}, {"CRT_HDR_BOOST", boost}, {nullptr, nullptr}};
    bool hl = g_hdrActive && g_hdrHighlights;
    if (hl) {
        // HDR: the filter's main becomes a helper, and a wrapper writes its result to two targets
        // in one draw: the game's 8-bit image (clips) and the FP16 highlight layer (doesn't).
        // Needs the filter's input struct to be called PSIn, as in the shipped filters.
        src = "#define main crt_filter_main\n#line 1\n" + src +
              "\n#undef main\n"
              "struct CrtHdrOut { float4 sdr : SV_Target0; float4 hl : SV_Target1; };\n"
              "CrtHdrOut main(PSIn i) { float4 c = crt_filter_main(i); CrtHdrOut o; o.sdr = c; o.hl = c; return o; }\n";
    }
    HRESULT hr = compile ? compile(src.data(), src.size(), name.c_str(), hl ? hdr : nullptr, nullptr, "main", "ps_5_0",
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

#ifndef CRT_RELEASE
static void DumpOnFirstUse(ID3D11Resource* res, UINT slot);
#endif

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
static Slot g_slots[40];
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
#ifndef CRT_RELEASE
    for (UINT i = 0; i < n && v; i++)
        if (v[i]) {
            ID3D11Resource* r = nullptr;
            v[i]->GetResource(&r);
            DumpOnFirstUse(r, start + i);
            r->Release();
        }
#endif
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
    if (crt) g_crtThisFrame = true;
    g_crtBound = crt;
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
// Around each game draw: if it's the filter, HlBegin adds the highlight layer as a second render
// target so the one draw fills both (HlEnd puts the game's target back). After the draw, note a
// possible final scale and count scene draws.
static void STDMETHODCALLTYPE H_DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT a, INT b) {
    if (g_cap) { Lock l; CapDraw("DrawIndexed", n); }
    bool hl; { Lock l; hl = HlBegin(c); }
    o_DrawIndexed(c, n, a, b);
    { Lock l; if (hl) HlEnd(c); HlNoteDraw(c); SceneAfterDraw(); if (g_cap) AfterDraw(); Repatch(); }
}
static void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, UINT n, UINT a) {
    if (g_cap) { Lock l; CapDraw("Draw", n); }
    bool hl; { Lock l; hl = HlBegin(c); }
    o_Draw(c, n, a);
    { Lock l; if (hl) HlEnd(c); HlNoteDraw(c); SceneAfterDraw(); if (g_cap) AfterDraw(); Repatch(); }
}
static void STDMETHODCALLTYPE H_DrawIndexedInst(ID3D11DeviceContext* c, UINT n, UINT inst, UINT a, INT b, UINT d) {
    if (g_cap) { Lock l; CapDraw("DrawIndexedInstanced", n * inst); }
    bool hl; { Lock l; hl = HlBegin(c); }
    o_DrawIndexedInst(c, n, inst, a, b, d);
    { Lock l; if (hl) HlEnd(c); HlNoteDraw(c); SceneAfterDraw(); if (g_cap) AfterDraw(); Repatch(); }
}
static void STDMETHODCALLTYPE H_DrawInst(ID3D11DeviceContext* c, UINT n, UINT inst, UINT a, UINT b) {
    if (g_cap) { Lock l; CapDraw("DrawInstanced", n * inst); }
    bool hl; { Lock l; hl = HlBegin(c); }
    o_DrawInst(c, n, inst, a, b);
    { Lock l; if (hl) HlEnd(c); HlNoteDraw(c); SceneAfterDraw(); if (g_cap) AfterDraw(); Repatch(); }
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
    Lock l;
    if (!g_cap) { Repatch(); return; }
    if (n && v) {
        char s[64];
        sprintf_s(s, "%gx%g@%g,%g", v[0].Width, v[0].Height, v[0].TopLeftX, v[0].TopLeftY);
        g_vp = s;
    }
    Repatch();
}
static bool HdrCopyMismatch(ID3D11Resource* dst, ID3D11Resource* src);
static bool BlitCopy(ID3D11Resource* dst, ID3D11Resource* src);
static void LogCopyMismatch(const char* what, ID3D11Resource* dst, ID3D11Resource* src, bool fixed);

static void STDMETHODCALLTYPE H_CopySub(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT a, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT b, const D3D11_BOX* box) {
    if (g_cap && !g_inSnap) { Lock l; CapEvent("COPY region %s -> %s", ResDesc(src).c_str(), ResDesc(dst).c_str()); }
    if (g_hdr && !g_inSnap) { Lock l; if (HdrCopyMismatch(dst, src)) LogCopyMismatch("copied region", dst, src, false); }
    o_CopySub(c, dst, a, x, y, z, src, b, box);
    { Lock l; Repatch(); }
}
static void STDMETHODCALLTYPE H_Copy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (g_cap) { Lock l; CapEvent("COPY %s -> %s", ResDesc(src).c_str(), ResDesc(dst).c_str()); }
    if (g_hdr && !g_inSnap) {
        Lock l;
        if (HdrCopyMismatch(dst, src)) {
            bool ok = BlitCopy(dst, src);
            LogCopyMismatch("copied", dst, src, ok);
            if (ok) { Repatch(); return; }
        }
    }
    o_Copy(c, dst, src);
    { Lock l; Repatch(); }
}
static void STDMETHODCALLTYPE H_ClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT col[4]) {
    if (g_cap) { Lock l; CapEvent("CLEAR %s (%.2f %.2f %.2f %.2f)", ViewDesc(v).c_str(), col[0], col[1], col[2], col[3]); }
    o_ClearRTV(c, v, col);
    { Lock l; Repatch(); }
}
static void STDMETHODCALLTYPE H_Resolve(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT a, ID3D11Resource* src, UINT b, DXGI_FORMAT f) {
    if (g_cap) { Lock l; CapEvent("RESOLVE %s -> %s", ResDesc(src).c_str(), ResDesc(dst).c_str()); }
    o_Resolve(c, dst, a, src, b, f);
    { Lock l; Repatch(); }
}

#ifndef CRT_RELEASE
typedef void(STDMETHODCALLTYPE* PFN_UpdateSub)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
static PFN_UpdateSub o_UpdateSub;
static void STDMETHODCALLTYPE H_UpdateSub(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
#endif

// Pass-through hooks on the other state setters the game calls before a draw. They only re-assert
// our slots: a setter the runtime handles by swapping the Draw entries would otherwise leave the
// next draw un-hooked (the highlight-layer redraw and scene counting both need every draw).
#define PASS_HOOK(name, PARAMS, ARGS)                                         \
    typedef void(STDMETHODCALLTYPE* PFN_##name) PARAMS;                       \
    static PFN_##name o_##name;                                               \
    static void STDMETHODCALLTYPE H_##name PARAMS { o_##name ARGS; Lock lk; Repatch(); }
PASS_HOOK(VSSetCB, (ID3D11DeviceContext* c, UINT a, UINT n, ID3D11Buffer* const* b), (c, a, n, b))
PASS_HOOK(PSSetSamp, (ID3D11DeviceContext* c, UINT a, UINT n, ID3D11SamplerState* const* s), (c, a, n, s))
PASS_HOOK(Unmap, (ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub), (c, r, sub))
PASS_HOOK(PSSetCB, (ID3D11DeviceContext* c, UINT a, UINT n, ID3D11Buffer* const* b), (c, a, n, b))
PASS_HOOK(IASetLayout, (ID3D11DeviceContext* c, ID3D11InputLayout* l), (c, l))
PASS_HOOK(IASetVB, (ID3D11DeviceContext* c, UINT a, UINT n, ID3D11Buffer* const* b, const UINT* s, const UINT* o), (c, a, n, b, s, o))
PASS_HOOK(IASetIB, (ID3D11DeviceContext* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT o), (c, b, f, o))
PASS_HOOK(IASetTopo, (ID3D11DeviceContext* c, D3D11_PRIMITIVE_TOPOLOGY t), (c, t))
PASS_HOOK(VSSetSRV, (ID3D11DeviceContext* c, UINT a, UINT n, ID3D11ShaderResourceView* const* v), (c, a, n, v))
PASS_HOOK(VSSetSamp, (ID3D11DeviceContext* c, UINT a, UINT n, ID3D11SamplerState* const* s), (c, a, n, s))
PASS_HOOK(OMSetBlend, (ID3D11DeviceContext* c, ID3D11BlendState* b, const FLOAT f[4], UINT m), (c, b, f, m))
PASS_HOOK(OMSetDepth, (ID3D11DeviceContext* c, ID3D11DepthStencilState* d, UINT r), (c, d, r))
PASS_HOOK(RSSetState, (ID3D11DeviceContext* c, ID3D11RasterizerState* r), (c, r))
PASS_HOOK(RSSetScissor, (ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r), (c, n, r))
#undef PASS_HOOK

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
        {7, H_VSSetCB, (void**)&o_VSSetCB},           {10, H_PSSetSamp, (void**)&o_PSSetSamp},
        {15, H_Unmap, (void**)&o_Unmap},              {16, H_PSSetCB, (void**)&o_PSSetCB},
        {17, H_IASetLayout, (void**)&o_IASetLayout},  {18, H_IASetVB, (void**)&o_IASetVB},
        {19, H_IASetIB, (void**)&o_IASetIB},          {24, H_IASetTopo, (void**)&o_IASetTopo},
        {25, H_VSSetSRV, (void**)&o_VSSetSRV},        {26, H_VSSetSamp, (void**)&o_VSSetSamp},
        {35, H_OMSetBlend, (void**)&o_OMSetBlend},    {36, H_OMSetDepth, (void**)&o_OMSetDepth},
        {43, H_RSSetState, (void**)&o_RSSetState},    {45, H_RSSetScissor, (void**)&o_RSSetScissor},
    };
    memcpy(g_slots, s, sizeof s);
    g_slotCount = (int)(sizeof s / sizeof s[0]);
#ifndef CRT_RELEASE
    g_slots[g_slotCount++] = {48, H_UpdateSub, (void**)&o_UpdateSub};  // texture dumps
#endif
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

// Everything our own full-screen passes change, saved before and put back after.
struct GameState {
    ID3D11VertexShader* vs = nullptr; ID3D11ClassInstance* vsCI[256]; UINT vsN = 256;
    ID3D11PixelShader* ps = nullptr; ID3D11ClassInstance* psCI[256]; UINT psN = 256;
    ID3D11InputLayout* il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11ShaderResourceView* srv0 = nullptr;
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT bm;
    ID3D11DepthStencilState* ds = nullptr; UINT sref;
    ID3D11RasterizerState* rs = nullptr;
    bool targets;  // also the render targets and viewports
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT vpN = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

    GameState(ID3D11DeviceContext* c, bool withTargets) : targets(withTargets) {
        c->VSGetShader(&vs, vsCI, &vsN);
        c->PSGetShader(&ps, psCI, &psN);
        c->IAGetInputLayout(&il);
        c->IAGetPrimitiveTopology(&topo);
        c->PSGetShaderResources(0, 1, &srv0);
        c->OMGetBlendState(&bs, bf, &bm);
        c->OMGetDepthStencilState(&ds, &sref);
        c->RSGetState(&rs);
        if (targets) {
            c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
            c->RSGetViewports(&vpN, vp);
        }
    }
    void Restore(ID3D11DeviceContext* c) {
        c->VSSetShader(vs, vsCI, vsN);
        c->PSSetShader(ps, psCI, psN);
        c->IASetInputLayout(il);
        c->IASetPrimitiveTopology(topo);
        c->PSSetShaderResources(0, 1, &srv0);
        c->OMSetBlendState(bs, bf, bm);
        c->OMSetDepthStencilState(ds, sref);
        c->RSSetState(rs);
        if (targets) {
            c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
            c->RSSetViewports(vpN, vp);
        }
        if (vs) vs->Release();
        for (UINT i = 0; i < vsN; i++) if (vsCI[i]) vsCI[i]->Release();
        if (ps) ps->Release();
        for (UINT i = 0; i < psN; i++) if (psCI[i]) psCI[i]->Release();
        if (il) il->Release();
        if (srv0) srv0->Release();
        if (bs) bs->Release();
        if (ds) ds->Release();
        if (rs) rs->Release();
        for (auto* v : rtv) if (v) v->Release();
        if (dsv) dsv->Release();
    }
};

static void DrawFullscreen(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ShaderResourceView* srv) {
    c->VSSetShader(g_fullscreenVS, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->IASetInputLayout(nullptr);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->PSSetShaderResources(0, 1, &srv);
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    c->OMSetDepthStencilState(g_noDepth, 0);
    c->RSSetState(nullptr);
    c->Draw(3, 0);
}

static void LoadScenePass() {
    if (g_fullscreenVS || !g_dev) return;
    LoadReal();
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

    GameState st(c, false);
    // the filter over the whole target, and with HDR also into the highlight layer (same draw)
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    bool hl = g_hdrActive && g_hdrHighlights && HlEnsure(d.Width, d.Height);
    if (hl) {
        c->OMGetRenderTargets(1, &rtv, &dsv);
        ID3D11RenderTargetView* both[2] = {rtv, g_hlRTV};
        c->OMSetRenderTargets(2, both, dsv);
    }
    DrawFullscreen(c, g_crtPS, g_sceneCopySRV);
    g_crtThisFrame = true;
    if (hl) {
        c->OMSetRenderTargets(1, &rtv, dsv);
        if (rtv) rtv->Release();
        if (dsv) dsv->Release();
        g_hlSource = target;
        g_hlValid = true;
    }
    st.Restore(c);
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
// HDR output: the pass before Present, and copies the FP16 working image no longer matches.

// t0: the finished SDR frame. With HIGHLIGHTS, t1 is the highlight layer (the filter's unclipped
// output at the working image's size), mapped onto the picture's rect inside the final-scale
// viewport (b0.vp: x, y, 1/w, 1/h; centred aspect fit) so it lines up with what the game drew.
// The game resampled its *clipped* 8-bit image, so the layer is filtered by hand (b0.opt.x: 1 =
// bilinear like the game, 0 = point) to get both that clipped blend, which must match the screen,
// and the unclipped one, which is the light.
static const char* kHdrPS = R"(
Texture2D Img : register(t0);
#if HIGHLIGHTS
Texture2D Hl : register(t1);
cbuffer V : register(b0) { float4 vp; float4 opt; };
void Fetch(float2 uv, out float3 E, out float3 C) {
    float2 sz;
    Hl.GetDimensions(sz.x, sz.y);
    int2 hi = int2(sz) - 1;
    if (opt.x > 0.5) {
        float2 t = uv * sz - 0.5, f = frac(t);
        int2 i = int2(floor(t));
        E = 0; C = 0;
        [unroll] for (int k = 0; k < 4; k++) {
            int2 o = int2(k & 1, k >> 1);
            float w = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
            float3 s = max(Hl.Load(int3(clamp(i + o, int2(0, 0), hi), 0)).rgb, 0.0);
            E += w * s;
            C += w * min(s, 1.0);
        }
    } else {
        E = max(Hl.Load(int3(min(int2(uv * sz), hi), 0)).rgb, 0.0);
        C = min(E, 1.0);
    }
}
#endif
float4 main(float4 pos : SV_Position) : SV_Target {
    // the game's values are gamma-encoded for an SDR screen; 1.0 = paper white
    float3 x = pow(max(Img.Load(int3(pos.xy, 0)).rgb, 0.0), 2.2);
#if HIGHLIGHTS
    float2 uv = (pos.xy - vp.xy) * vp.zw;
    if (all(uv > 0.0) && all(uv < 1.0)) {
        float3 E, C;
        Fetch(uv, E, C);
        float3 e = pow(E, 2.2), c = pow(C, 2.2);  // unclipped filter output, and what the game kept
        float3 extra = e - c;
        if (any(extra > 0.0)) {
            // t: how much of the filter's light still reaches the screen. Whatever the game drew
            // on top (a fade, fog, a dark panel) can only have dimmed each channel by at least
            // that much, so the smallest screen/filter ratio over the channels the filter lit is a
            // safe estimate; a pure glow leaves it at 1.
            float3 r = c > 0.05 ? x / max(c, 1e-4) : 1.0;
            float t = saturate(min(r.r, min(r.g, r.b)));
            // what was added on top that the surviving filter light doesn't explain: a little is
            // glow or fog colour; a lot means something bright and opaque (text) covers the beam,
            // so fade the highlight out smoothly (kept up to +0.35, gone at +1.0)
            float3 over = max(x - t * c, 0.0);
            float vis = saturate((1.0 - max(over.r, max(over.g, over.b))) / 0.65);
            x += t * vis * extra;
        }
    }
    float h = PEAK_REL - 1.0;  // headroom above paper white; roll off smoothly into it
    x = h > 0.0 ? (x > 1.0 ? 1.0 + h * (1.0 - exp(-(x - 1.0) / h)) : x) : min(x, 1.0);
#else
    x = min(x, 1.0);
#endif
    return float4(x * PAPER, 1.0);  // scRGB: 1.0 = 80 nits
})";
static const char* kBlitPS = "Texture2D t:register(t0);float4 main(float4 p:SV_Position):SV_Target{return t.Load(int3(p.xy,0));}";

static ID3D11PixelShader* g_hdrPS;
static ID3D11PixelShader* g_hdrPSClip;  // HIGHLIGHTS 0, for frames without highlights
static ID3D11PixelShader* g_blitPS;
static ID3D11Texture2D* g_hdrCopy;
static ID3D11ShaderResourceView* g_hdrCopySRV;

static ID3D11Texture2D* g_hlTex;
static ID3D11ShaderResourceView* g_hlSRV;
static ID3D11Buffer* g_hlCB;  // final-scale viewport for kHdrPS

// (Re)creates the highlight layer at w x h, FP16, plus the constant buffer the final pass uses.
static bool HlEnsure(UINT w, UINT h) {
    if (!g_dev) return false;
    if (g_hlTex) {
        D3D11_TEXTURE2D_DESC cd;
        g_hlTex->GetDesc(&cd);
        if (cd.Width == w && cd.Height == h) return true;
        g_hlSRV->Release(); g_hlSRV = nullptr;
        g_hlRTV->Release(); g_hlRTV = nullptr;
        g_hlTex->Release(); g_hlTex = nullptr;
    }
    D3D11_TEXTURE2D_DESC cd = {w, h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    if (FAILED(g_dev->CreateTexture2D(&cd, nullptr, &g_hlTex))) { g_hlTex = nullptr; Log("HDR: highlight layer %ux%u failed", w, h); return false; }
    if (FAILED(g_dev->CreateRenderTargetView(g_hlTex, nullptr, &g_hlRTV)) ||
        FAILED(g_dev->CreateShaderResourceView(g_hlTex, nullptr, &g_hlSRV))) {
        if (g_hlRTV) { g_hlRTV->Release(); g_hlRTV = nullptr; }
        if (g_hlSRV) { g_hlSRV->Release(); g_hlSRV = nullptr; }
        g_hlTex->Release(); g_hlTex = nullptr;
        Log("HDR: highlight layer views failed");
        return false;
    }
    if (!g_hlCB) {
        D3D11_BUFFER_DESC bd = {32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_hlCB))) g_hlCB = nullptr;
    }
    Log("HDR: highlight layer %ux%u", w, h);
    return true;
}

// The layer and the final scale's viewport and sampler carry over between frames, so a frame where
// a hook is missed reuses the last ones instead of blinking the highlights off. Reset on resize.
static void HlReset() {
    g_hlValid = g_hlScaled = false;
    g_hlSource = g_hlScaleRT = nullptr;
    g_hlLinear = true;
}

static ID3D11PixelShader* CompilePS(const char* src, const D3D_SHADER_MACRO* defs, const char* name) {
    LoadReal();
    PFN_Compile compile = g_compile47 ? g_compile47 : g_compile;
    ID3DBlob *code = nullptr, *err = nullptr;
    ID3D11PixelShader* ps = nullptr;
    if (compile && SUCCEEDED(compile(src, strlen(src), name, defs, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        o_CreatePS(g_dev, code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps);
    else
        Log("HDR: %s compile FAILED: %.*s", name, err ? (int)err->GetBufferSize() : 0, err ? (const char*)err->GetBufferPointer() : "");
    if (code) code->Release();
    if (err) err->Release();
    return ps;
}

static void BuildHdrShader() {
    if (!g_dev) return;
    if (g_hdrPS) { g_hdrPS->Release(); g_hdrPS = nullptr; }
    if (g_hdrPSClip) { g_hdrPSClip->Release(); g_hdrPSClip = nullptr; }
    char paper[32], peak[32];
    sprintf_s(paper, "%f", g_paperWhite / 80.0f);
    sprintf_s(peak, "%f", HdrPeak() / g_paperWhite);
    D3D_SHADER_MACRO d[] = {{"PAPER", paper}, {"PEAK_REL", peak}, {"HIGHLIGHTS", "1"}, {nullptr, nullptr}};
    g_hdrPS = CompilePS(kHdrPS, d, "hdr_output");
    d[2].Definition = "0";
    g_hdrPSClip = CompilePS(kHdrPS, d, "hdr_output_clip");
}

static void QueryDisplay(IDXGISwapChain* sc) {
    IDXGIOutput* o = nullptr;
    IDXGIOutput6* o6 = nullptr;
    if (SUCCEEDED(sc->GetContainingOutput(&o)) && SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput6), (void**)&o6))) {
        DXGI_OUTPUT_DESC1 d;
        if (SUCCEEDED(o6->GetDesc1(&d))) {
            bool on = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            g_displayPeak = on ? d.MaxLuminance : 0.0f;
            Log("HDR: display %ls: Windows HDR %s, reports peak %.0f nits, full-frame %.0f nits; using peak %.0f",
                d.DeviceName, on ? "on" : "OFF (highlights will clip; turn on HDR in Windows display settings)",
                d.MaxLuminance, d.MaxFullFrameLuminance, HdrPeak());
        }
    } else {
        Log("HDR: couldn't query the display");
    }
    if (o6) o6->Release();
    if (o) o->Release();
}

// Turns the finished SDR frame in the (FP16) back buffer into scRGB, in place.
static void HdrPass(IDXGISwapChain* sc) {
    LoadScenePass();
    static bool queried;
    if (!queried) {
        queried = true;
        QueryDisplay(sc);
        BuildHdrShader();
    }
    bool filtered = g_crtThisFrame;
    g_crtThisFrame = false;
    if (!g_fullscreenVS || !g_noDepth || !g_hdrPS || !g_hdrPSClip) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);

    // Highlights only where this frame's filter output can be traced to the screen.
    bool hl = filtered && g_hdrHighlights && g_hlValid && g_hlScaled && g_hlScaleRT == bb && g_hlSRV && g_hlCB;
    const char* why = !filtered            ? "no filter this frame, clipped at paper white"
                      : !g_hdrHighlights   ? "highlights off (F6), clipped at paper white"
                      : !g_hlValid         ? "filter drew but the highlight layer didn't; clipped"
                      : !g_hlScaled        ? "couldn't find the game's final scale to the screen; clipped"
                      : g_hlScaleRT != bb  ? "the final scale didn't draw to the back buffer; clipped"
                      : hl                 ? "highlights on"
                                           : "highlight resources missing; clipped";
    static const char* lastWhy;
    if (why != lastWhy) {
        lastWhy = why;
        if (hl)
            Log("HDR: highlights on (final scale viewport %gx%g at %g,%g into %ux%u; picture fitted inside it)",
                g_hlVP.Width, g_hlVP.Height, g_hlVP.TopLeftX, g_hlVP.TopLeftY, d.Width, d.Height);
        else
            Log("HDR: %s", why);
    }
    ID3D11PixelShader* ps = hl ? g_hdrPS : g_hdrPSClip;
    if (g_hdrCopy) {
        D3D11_TEXTURE2D_DESC cd;
        g_hdrCopy->GetDesc(&cd);
        if (cd.Width != d.Width || cd.Height != d.Height || cd.Format != d.Format) {
            g_hdrCopySRV->Release(); g_hdrCopySRV = nullptr;
            g_hdrCopy->Release(); g_hdrCopy = nullptr;
        }
    }
    if (!g_hdrCopy) {
        D3D11_TEXTURE2D_DESC cd = {d.Width, d.Height, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (SUCCEEDED(g_dev->CreateTexture2D(&cd, nullptr, &g_hdrCopy)) &&
            FAILED(g_dev->CreateShaderResourceView(g_hdrCopy, nullptr, &g_hdrCopySRV))) {
            g_hdrCopy->Release(); g_hdrCopy = nullptr;
        }
    }
    ID3D11RenderTargetView* rtv = nullptr;
    if (g_hdrCopy && SUCCEEDED(g_dev->CreateRenderTargetView(bb, nullptr, &rtv))) {
        ID3D11DeviceContext* c = g_ctx;
        g_inScenePass = g_inSnap = true;
        c->CopyResource(g_hdrCopy, bb);
        GameState st(c, true);
        c->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)d.Width, (float)d.Height, 0, 1};
        c->RSSetViewports(1, &vp);
        ID3D11ShaderResourceView* oldT1 = nullptr;
        ID3D11Buffer* oldB0 = nullptr;
        if (hl) {
            c->PSGetShaderResources(1, 1, &oldT1);
            c->PSGetConstantBuffers(0, 1, &oldB0);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(c->Map(g_hlCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                // The game letterboxes: its 16:9 image is fitted, centred, inside the final-scale
                // viewport (2560x1440 at y 80 in a 2560x1600 one), so map the layer onto that rect.
                D3D11_TEXTURE2D_DESC ld;
                g_hlTex->GetDesc(&ld);
                float fit = std::min(g_hlVP.Width / ld.Width, g_hlVP.Height / ld.Height);
                float rw = ld.Width * fit, rh = ld.Height * fit;
                float* f = (float*)m.pData;
                f[0] = g_hlVP.TopLeftX + (g_hlVP.Width - rw) * 0.5f;
                f[1] = g_hlVP.TopLeftY + (g_hlVP.Height - rh) * 0.5f;
                f[2] = rw > 0 ? 1.0f / rw : 0.0f;
                f[3] = rh > 0 ? 1.0f / rh : 0.0f;
                f[4] = g_hlLinear ? 1.0f : 0.0f;
                f[5] = f[6] = f[7] = 0.0f;
                c->Unmap(g_hlCB, 0);
            }
            c->PSSetShaderResources(1, 1, &g_hlSRV);
            c->PSSetConstantBuffers(0, 1, &g_hlCB);
        }
        DrawFullscreen(c, ps, g_hdrCopySRV);
        if (hl) {
            c->PSSetShaderResources(1, 1, &oldT1);
            c->PSSetConstantBuffers(0, 1, &oldB0);
            if (oldT1) oldT1->Release();
            if (oldB0) oldB0->Release();
        }
        st.Restore(c);
        g_inScenePass = g_inSnap = false;
        rtv->Release();
    }
    bb->Release();
}

static DXGI_FORMAT TexFormat(ID3D11Resource* r, D3D11_TEXTURE2D_DESC* out = nullptr) {
    ID3D11Texture2D* t = nullptr;
    if (!r || FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) return DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC d;
    t->GetDesc(&d);
    t->Release();
    if (out) *out = d;
    return d.Format;
}

// The game copying between the FP16 back buffer and an 8-bit texture would fail silently
// (copies need matching formats), so same-size copies into a render target are redone as a draw.
// Anything else is logged; the handoff notes say what to look for.
static bool HdrCopyMismatch(ID3D11Resource* dst, ID3D11Resource* src) {
    DXGI_FORMAT a = TexFormat(dst), b = TexFormat(src);
    return a != b && a != DXGI_FORMAT_UNKNOWN && b != DXGI_FORMAT_UNKNOWN &&
           (a == DXGI_FORMAT_R16G16B16A16_FLOAT || b == DXGI_FORMAT_R16G16B16A16_FLOAT);
}

static void LogCopyMismatch(const char* what, ID3D11Resource* dst, ID3D11Resource* src, bool fixed) {
    static int n;
    if (++n > 20) return;
    D3D11_TEXTURE2D_DESC a, b;
    TexFormat(dst, &a);
    TexFormat(src, &b);
    Log("HDR: game %s %ux%u fmt %u -> %ux%u fmt %u (bind %x, usage %u): %s", what, b.Width, b.Height, b.Format, a.Width,
        a.Height, a.Format, a.BindFlags, a.Usage, fixed ? "redone as a draw" : "NOT handled, copy is lost");
}

static bool BlitCopy(ID3D11Resource* dst, ID3D11Resource* src) {
    D3D11_TEXTURE2D_DESC a, b;
    TexFormat(dst, &a);
    TexFormat(src, &b);
    if (a.Width != b.Width || a.Height != b.Height || !(a.BindFlags & D3D11_BIND_RENDER_TARGET) ||
        !(b.BindFlags & D3D11_BIND_SHADER_RESOURCE))
        return false;
    LoadScenePass();
    if (!g_blitPS) g_blitPS = CompilePS(kBlitPS, nullptr, "hdr_blit");
    if (!g_fullscreenVS || !g_noDepth || !g_blitPS) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    bool ok = SUCCEEDED(g_dev->CreateRenderTargetView(dst, nullptr, &rtv)) && SUCCEEDED(g_dev->CreateShaderResourceView(src, nullptr, &srv));
    if (ok) {
        ID3D11DeviceContext* c = g_ctx;
        g_inScenePass = g_inSnap = true;
        GameState st(c, true);
        c->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)a.Width, (float)a.Height, 0, 1};
        c->RSSetViewports(1, &vp);
        DrawFullscreen(c, g_blitPS, srv);
        st.Restore(c);
        g_inScenePass = g_inSnap = false;
    }
    if (rtv) rtv->Release();
    if (srv) srv->Release();
    return ok;
}

// The game's copy-back draw with the filter bound: add the highlight layer (same size) as render
// target 1, which the HDR build of the filter writes. HlEnd puts the game's single target back.
static ID3D11RenderTargetView* g_hlSavedRTV;
static ID3D11DepthStencilView* g_hlSavedDSV;

static bool HlBegin(ID3D11DeviceContext* c) {
    if (!g_crtBound || !g_hdrActive || !g_hdrHighlights || !g_curRT || g_inScenePass) return false;
    D3D11_TEXTURE2D_DESC d;
    if (TexFormat(g_curRT, &d) == DXGI_FORMAT_UNKNOWN || !HlEnsure(d.Width, d.Height)) return false;
    g_hlSource = g_curRT;
    c->OMGetRenderTargets(1, &g_hlSavedRTV, &g_hlSavedDSV);
    ID3D11RenderTargetView* both[2] = {g_hlSavedRTV, g_hlRTV};
    c->OMSetRenderTargets(2, both, g_hlSavedDSV);
    return true;
}

static void HlEnd(ID3D11DeviceContext* c) {
    c->OMSetRenderTargets(1, &g_hlSavedRTV, g_hlSavedDSV);
    if (g_hlSavedRTV) g_hlSavedRTV->Release();
    if (g_hlSavedDSV) g_hlSavedDSV->Release();
    g_hlSavedRTV = nullptr; g_hlSavedDSV = nullptr;
    g_hlValid = true;
    static bool logged;
    if (!logged) { logged = true; Log("HDR: filter writing the highlight layer"); }
}

// After any game draw: one that reads the filtered image and writes the FP16 back buffer (the
// only FP16 target the game has now) is the final scale. Keep its viewport and filtering.
static void HlNoteDraw(ID3D11DeviceContext* c) {
    if (!g_hlValid || g_inScenePass || !g_srv0Res || g_srv0Res != g_hlSource || !g_curRT || g_curRT == g_hlSource)
        return;
    if (TexFormat(g_curRT) != DXGI_FORMAT_R16G16B16A16_FLOAT) return;
    UINT n = 1;
    c->RSGetViewports(&n, &g_hlVP);
    if (!n) return;
    ID3D11SamplerState* smp = nullptr;
    c->PSGetSamplers(0, 1, &smp);
    bool linear = true;  // no sampler bound = the default, which is linear
    if (smp) {
        D3D11_SAMPLER_DESC sd;
        smp->GetDesc(&sd);
        linear = D3D11_DECODE_MAG_FILTER(sd.Filter) == D3D11_FILTER_TYPE_LINEAR;
        smp->Release();
    }
    static int lastLinear = -1;
    if ((int)linear != lastLinear) { lastLinear = linear; Log("HDR: final scale filters %s", linear ? "bilinear" : "point"); }
    g_hlLinear = linear;
    g_hlScaleRT = g_curRT;
    g_hlScaled = true;
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
        if (f8 && !f8was) {
            if (g_hdr) LoadHdrConfig(false);
            LoadCrt();
            if (g_hdrActive) BuildHdrShader();
        }
        bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        static bool f6was;
        if (f6 && !f6was && g_hdrActive) {
            g_hdrHighlights = !g_hdrHighlights;
            Log("HDR: highlights %s", g_hdrHighlights ? "on" : "off (SDR filters, clipped at paper white)");
            LoadCrt();
            BuildHdrShader();
        }
        f6was = f6;
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
        if (g_hdrActive) HdrPass(sc);
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

typedef HRESULT(STDMETHODCALLTYPE* PFN_Resize)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_Resize o_Resize;

static void SetColorSpace(IDXGISwapChain* sc) {
    IDXGISwapChain3* sc3 = nullptr;
    UINT support = 0;
    g_hdrActive = SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) &&
                  SUCCEEDED(sc3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
                  (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
                  SUCCEEDED(sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709));
    if (sc3) sc3->Release();
    Log(g_hdrActive ? "HDR: swap chain is scRGB (FP16, flip model)" : "HDR: scRGB colour space refused; output stays SDR");
}

static HRESULT STDMETHODCALLTYPE H_Resize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (g_hdrActive) {
        if (fmt != DXGI_FORMAT_UNKNOWN) fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (n == 1) n = 2;
        flags &= ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    }
    HRESULT hr = o_Resize(sc, n, w, h, fmt, flags);
    Lock l;
    HlReset();  // the final scale's viewport changes with the window
    if (FAILED(hr)) Log("HDR: ResizeBuffers %ux%u failed (%08lx)", w, h, hr);
    else if (g_hdrActive) { SetColorSpace(sc); QueryDisplay(sc); BuildHdrShader(); }
    return hr;
}

static void HookSwapChain(IDXGISwapChain* sc) {
    Lock l;
    void** vt = *(void***)sc;
    if (vt[8] != (void*)H_Present) Patch(&vt[8], H_Present, (void**)&o_Present);
    if (g_hdr && vt[13] != (void*)H_Resize) Patch(&vt[13], H_Resize, (void**)&o_Resize);
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(sc->GetDesc(&d)))
        Log("swapchain %ux%u fmt=%u buffers=%u windowed=%d swapeffect=%u", d.BufferDesc.Width, d.BufferDesc.Height,
            d.BufferDesc.Format, d.BufferCount, d.Windowed, d.SwapEffect);
}

// With HDR on, the swap chain is created as FP16 flip-model instead of what the game asked for.
static HRESULT STDMETHODCALLTYPE H_CreateSC(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out) {
    HRESULT hr = E_FAIL;
    if (g_hdr && desc) {
        DXGI_SWAP_CHAIN_DESC hd = *desc;
        hd.BufferDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hd.BufferCount = std::max(2u, hd.BufferCount);
        hd.SampleDesc = {1, 0};
        hd.Flags &= ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
        hr = o_CreateSC(f, dev, &hd, out);
        if (SUCCEEDED(hr) && out && *out) { Lock l; SetColorSpace(*out); }
        else Log("HDR: couldn't create an HDR swap chain (%08lx); output stays SDR", hr);
    }
    if (FAILED(hr)) hr = o_CreateSC(f, dev, desc, out);
    if (SUCCEEDED(hr) && out && *out) HookSwapChain(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateSCHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = E_FAIL;
    if (g_hdr && d) {
        DXGI_SWAP_CHAIN_DESC1 hd = *d;
        hd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hd.BufferCount = std::max(2u, hd.BufferCount);
        hd.SampleDesc = {1, 0};
        hd.Flags &= ~DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
        hr = o_CreateSCHwnd(f, dev, w, &hd, fs, o, out);
        if (SUCCEEDED(hr) && out && *out) { Lock l; SetColorSpace(*out); }
        else Log("HDR: couldn't create an HDR swap chain (%08lx); output stays SDR", hr);
    }
    if (FAILED(hr)) hr = o_CreateSCHwnd(f, dev, w, d, fs, o, out);
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

#ifndef CRT_RELEASE
// ---------------------------------------------------------------------------------------------
// Texture dumps: every texture the game uploads with pixel data goes to shaderlog\textures\,
// named by a hash of its contents so repeats are skipped. 32-bit formats are written as BMP,
// anything else as raw bytes with the format in the name. textures\index.txt lists each upload.

static std::unordered_map<uint64_t, bool> g_dumpedTex;
static std::unordered_map<void*, int> g_texUpdates;  // per-texture UpdateSubresource dumps, capped (video)

static UINT BitsPerPixel(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_UINT: return 32;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM: return 16;
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_A8_UNORM: return 8;
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_UNORM: return 4;  // per pixel, 4x4 blocks
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return 8;
    default: return 0;
    }
}
static bool IsBC(DXGI_FORMAT f) { return f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM || f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB; }

// Writes w x h pixels starting at `p` (rows `pitch` apart). `how` says where it came from.
static void DumpPixels(const void* p, UINT pitch, UINT w, UINT h, DXGI_FORMAT fmt, const char* how) {
    UINT bpp = BitsPerPixel(fmt);
    if (!p || !bpp || w < 8 || h < 8) return;
    bool bc = IsBC(fmt);
    UINT rows = bc ? (h + 3) / 4 : h, rowBytes = bc ? (w + 3) / 4 * bpp * 2 : w * bpp / 8;
    uint64_t hash = Fnv(&fmt, sizeof fmt);
    for (UINT y = 0; y < rows; y++) hash = Fnv((const uint8_t*)p + (size_t)y * pitch, rowBytes, hash);
    if (g_dumpedTex[hash]) return;
    g_dumpedTex[hash] = true;
    std::string dir = g_root + "textures\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    char name[96];
    bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || fmt == DXGI_FORMAT_B8G8R8X8_UNORM;
    bool rgba = fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || fmt == DXGI_FORMAT_R8G8B8A8_UINT;
    sprintf_s(name, "%s_%ux%u_f%u.%s", Hex(hash).c_str(), w, h, fmt, bgra || rgba ? "bmp" : "raw");
    if (FILE* f = fopen((dir + name).c_str(), "wb")) {
        if (bgra || rgba) {  // 32-bit top-down BMP, BGRA
            uint8_t hd[54] = {'B', 'M'};
            *(uint32_t*)(hd + 2) = 54 + w * 4 * h;
            *(uint32_t*)(hd + 10) = 54;
            *(uint32_t*)(hd + 14) = 40;
            *(int32_t*)(hd + 18) = w;
            *(int32_t*)(hd + 22) = -(int32_t)h;
            *(uint16_t*)(hd + 26) = 1;
            *(uint16_t*)(hd + 28) = 32;
            *(uint32_t*)(hd + 34) = w * 4 * h;
            fwrite(hd, 1, 54, f);
        }
        std::string line(rowBytes, '\0');
        for (UINT y = 0; y < rows; y++) {
            memcpy(&line[0], (const uint8_t*)p + (size_t)y * pitch, rowBytes);
            if (rgba) for (UINT x = 0; x < rowBytes; x += 4) std::swap(line[x], line[x + 2]);
            fwrite(line.data(), 1, rowBytes, f);
        }
        fclose(f);
    }
    if (FILE* f = fopen((dir + "index.txt").c_str(), "ab")) {
        fprintf(f, "%9.3fs %-44s %s\r\n", (GetTickCount() - g_t0) / 1000.0, name, how);
        fclose(f);
    }
}

static void STDMETHODCALLTYPE H_UpdateSub(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data, UINT pitch, UINT depthPitch) {
    o_UpdateSub(c, dst, sub, box, data, pitch, depthPitch);
    Lock l;
    ID3D11Texture2D* t = nullptr;
    if (sub == 0 && dst && SUCCEEDED(dst->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) {
        D3D11_TEXTURE2D_DESC d;
        t->GetDesc(&d);
        t->Release();
        UINT w = box ? box->right - box->left : d.Width, h = box ? box->bottom - box->top : d.Height;
        if (w >= 32 && h >= 32 && g_texUpdates[dst]++ < 4) {
            char how[96];
            sprintf_s(how, "UpdateSubresource into %ux%u at %u,%u", d.Width, d.Height, box ? box->left : 0, box ? box->top : 0);
            DumpPixels(data, pitch, w, h, d.Format, how);
        }
    }
    Repatch();
}

// Sheets the game fills some other way (staging copies, Map) are read back the first time a
// pixel shader samples them.
static std::unordered_map<void*, bool> g_readTex;
static void DumpOnFirstUse(ID3D11Resource* res, UINT slot) {
    if (!res || !g_ctx || g_readTex[res]) return;
    g_readTex[res] = true;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) return;
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    if (d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL) || d.SampleDesc.Count > 1 || !BitsPerPixel(d.Format)) {
        tex->Release();
        return;
    }
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
            char how[96];
            sprintf_s(how, "first use, PS slot %u, usage %u, %u mips, %u slices", slot, d.Usage, d.MipLevels, d.ArraySize);
            DumpPixels(m.pData, m.RowPitch, d.Width, d.Height, d.Format, how);
            g_ctx->Unmap(st, 0);
        }
        g_inSnap = false;
        st->Release();
    }
    if (dev) dev->Release();
    tex->Release();
}
#endif

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
// HDR: the game's own render targets stay 8-bit (see "HDR output"); only the swap chain is FP16,
// so its views below may need their format corrected.
static bool Is8BitRGBA(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

static HRESULT STDMETHODCALLTYPE H_CreateTex2D(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    HRESULT hr = o_CreateTex2D(d, desc, init, out);
    if (SUCCEEDED(hr) && out && *out) { Lock l; g_texScene.erase(*out); }  // address may be reused
#ifndef CRT_RELEASE
    if (SUCCEEDED(hr) && out && *out) { Lock l; g_readTex.erase(*out); }
#endif
#ifndef CRT_RELEASE
    if (SUCCEEDED(hr) && desc && init && !(desc->BindFlags & D3D11_BIND_RENDER_TARGET)) {
        Lock l;
        UINT mips = desc->MipLevels ? desc->MipLevels : 1;
        for (UINT a = 0; a < desc->ArraySize; a++) {
            char how[96];
            sprintf_s(how, "created, slice %u/%u, %u mips, usage %u", a, desc->ArraySize, mips, desc->Usage);
            DumpPixels(init[a * mips].pSysMem, init[a * mips].SysMemPitch, desc->Width, desc->Height, desc->Format, how);
        }
    }
    if (SUCCEEDED(hr) && desc && (desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) {
        Lock l;
        Log("rendertarget %s %ux%u fmt=%u bind=%x msaa=%u", out && *out ? ResDesc(*out).c_str() : "?", desc->Width,
            desc->Height, desc->Format, desc->BindFlags, desc->SampleDesc.Count);
    }
#endif
    return hr;
}

// Views the game creates with an explicit 8-bit format would fail on an FP16 texture.
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSRV)(ID3D11Device*, ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateRTV)(ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**);
static PFN_CreateSRV o_CreateSRV;
static PFN_CreateRTV o_CreateRTV;

static HRESULT STDMETHODCALLTYPE H_CreateSRV(ID3D11Device* d, ID3D11Resource* r, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) {
    D3D11_SHADER_RESOURCE_VIEW_DESC fixed;
    if (desc && Is8BitRGBA(desc->Format) && TexFormat(r) == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        fixed = *desc;
        fixed.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc = &fixed;
    }
    return o_CreateSRV(d, r, desc, out);
}
static HRESULT STDMETHODCALLTYPE H_CreateRTV(ID3D11Device* d, ID3D11Resource* r, const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** out) {
    D3D11_RENDER_TARGET_VIEW_DESC fixed;
    if (desc && Is8BitRGBA(desc->Format) && TexFormat(r) == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        fixed = *desc;
        fixed.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc = &fixed;
    }
    return o_CreateRTV(d, r, desc, out);
}

static void HookDevice(ID3D11Device* dev) {
    static bool done;
    if (done) return;
    done = true;
    g_dev = dev;
    void** vt = *(void***)dev;
    Patch(&vt[5], H_CreateTex2D, (void**)&o_CreateTex2D);
    if (g_hdr) {
        Patch(&vt[7], H_CreateSRV, (void**)&o_CreateSRV);
        Patch(&vt[9], H_CreateRTV, (void**)&o_CreateRTV);
    }
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
    if (SUCCEEDED(hr) && dev && *dev) {
        IDXGIDevice* dd = nullptr;
        IDXGIAdapter* ad = nullptr;
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED((*dev)->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&ad)) &&
            SUCCEEDED(ad->GetDesc(&desc)))
            Log("GPU: %ls", desc.Description);  // hybrid laptops may hand the game the integrated one
        if (ad) ad->Release();
        if (dd) dd->Release();
        HookDevice(*dev);
    }
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
    LoadHdrConfig(true);
    if (const char* t = getenv("SHADERLOG_CRT_TARGET")) g_copyBackPS = t;  // test hook: swap a different shader
    // game.exe's imports are already bound by the time our DllMain runs, so these patches stick.
    PatchImport("d3d11.dll", "D3D11CreateDevice", H_CreateDevice, (void**)&o_CreateDevice);
    PatchImport("d3d9.dll", "Direct3DCreate9", H_Create9, (void**)&o_Create9);
    return TRUE;
}
