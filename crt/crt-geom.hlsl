// crt-geom (cgwg, Themaister, DOLLS; GPL) for YOHANE THE PARHELION -BLAZE in the DEEPBLUE-.
// Copyright (C) 2010-2012 cgwg, Themaister and DOLLS (crt-geom)
// Copyright (C) 2026 Chris Etler (HLSL port and in-game adapter)
//
// This program is free software; you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version. It is distributed WITHOUT
// ANY WARRANTY; see LICENSE.txt for details.
//
// Ported from RetroArch's crt-geom.slang. Shipped defaults except: curvature off (so the
// level stays aligned with the HUD), and lum / monitorgamma / DOTMASK raised for brightness
// (marked below). Only mask_type 1 (the default) is included.
//
// F7 cycles shaders, F8 reloads after an edit, F9 toggles the filter. Settings are the
// "static const" values below the adapter. With HDR on (crt\hdr.ini) the mod compiles this
// with CRT_HDR and CRT_HDR_BOOST defined.
//
// The body below the adapter matches crt/compare/crt-geom.hlsl in the yohane-crt project.

// ---- in-game adapter ------------------------------------------------------------------------
// The game hands us its 1080p level image at t0. RetroArch shaders expect the native low-res
// image, so "Source" texel (x, y) is the average of the matching 4x4 block of the 1080p frame.

Texture2D Frame : register(t0);

#define VIRTUAL_HEIGHT 270.0

static float4 SourceSize;  // w, h, 1/w, 1/h of the virtual 480x270 image
static float4 OutputSize;  // the 1080p target
static const float FrameCount = 0.0;
static int g_scale;

struct PSIn {
    float4 pos : SV_Position;
    float4 c0  : COLOR0;
    float4 c1  : COLOR1;
    float2 uv  : TEXCOORD0;
};

void InitSizes() {
    float w, h;
    Frame.GetDimensions(w, h);
    g_scale = (int)max(1.0, round(h / VIRTUAL_HEIGHT));
    OutputSize = float4(w, h, 1.0 / w, 1.0 / h);
    float sw = floor(w / g_scale), sh = floor(h / g_scale);
    SourceSize = float4(sw, sh, 1.0 / sw, 1.0 / sh);
}

float4 SampleSource(float2 uv) {
    int2 t = clamp((int2)floor(uv * SourceSize.xy), int2(0, 0), (int2)SourceSize.xy - 1);
    int2 b = t * g_scale;
    float3 sum = 0;
    for (int y = 0; y < g_scale; y++)
        for (int x = 0; x < g_scale; x++)
            sum += Frame.Load(int3(b + int2(x, y), 0)).rgb;
    return float4(sum / (g_scale * g_scale), 1.0);
}

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define mix lerp
#define fract frac
#define inversesqrt rsqrt
#define texture(s, c) SampleSource(c)
float  glsl_mod(float x, float y) { return x - y * floor(x / y); }
float2 glsl_mod(float2 x, float2 y) { return x - y * floor(x / y); }

// ---- crt-geom ---------------------------------------------------------------------------------

static const float CRTgamma = 2.4;
static const float monitorgamma = 2.4;  // shipped 2.2; higher = brighter midtones
static const float cornersize = 0.03;
static const float cornersmooth = 1000.0;
static const float overscan_x = 100.0;
static const float overscan_y = 100.0;
static const float DOTMASK = 0.2;       // shipped 0.3; mask strength. Stronger moires: the mask is on the
                                        // 1080p grid and the game rescales it to the window
static const float DOTMASK_brightboost = 0.0;
static const float SHARPER = 1.0;
#ifdef CRT_HDR
static const float scanline_weight = 0.25;  // HDR: thinner beams, near-black gaps between lines
static const float bright_bloom = 0.10;     // HDR: bright beams widen by this much (in lines), filling the
                                            // gaps on near-white areas the way a real tube blooms
// HDR: each beam is scaled by 0.3 / its width, so every shade keeps the SDR filter's average
// brightness: thin beams get brighter peaks instead of a dimmer picture, and bloomed white beams
// don't get extra. Overall level is crt_boost in hdr.ini.
#else
static const float scanline_weight = 0.3;
static const float bright_bloom = 0.0;
#endif
static const float sdr_scanline_weight = 0.3;  // the reference width the HDR beams are normalised to
static const float lum = 0.3;           // shipped 0.0; scanline beam brightness
static const float interlace_detect = 1.0;

#define PI 3.141592653589
vec4 FIX(vec4 c) { return max(abs(c), 1e-5); }
#define TEX2D(c) pow(max(texture(Source, (c)), 0.0), CRTgamma)

static const vec2 aspect = vec2(1.0, 0.75);

vec4 scanlineWeights(float distance, vec4 color) {
    vec4 wid = 2.0 + 2.0 * pow(color, 4.0);
    vec4 sw = scanline_weight + bright_bloom * color * color;
    vec4 weights = distance / sw;
    return (lum + 1.4) * exp(-pow(weights * inversesqrt(0.5 * wid), wid)) / (0.6 + 0.2 * wid) * (sdr_scanline_weight / sw);
}

float corner(vec2 coord) {
    coord = (coord - 0.5) * vec2(overscan_x / 100.0, overscan_y / 100.0) + 0.5;
    coord = min(coord, 1.0 - coord) * aspect;
    vec2 cdist = cornersize;
    coord = (cdist - min(coord, cdist));
    float dist = sqrt(dot(coord, coord));
    return clamp((cdist.x - dist) * cornersmooth, 0.0, 1.0);
}

float4 geom_main(PSIn i) {
    vec2 vTexCoord = i.uv * 1.00001;
    vec2 TextureSize = vec2(SHARPER * SourceSize.x, SourceSize.y);
    vec2 ilfac = vec2(1.0, clamp(floor(SourceSize.y / (interlace_detect > 0.5 ? 200.0 : 1000.0)), 1.0, 2.0));
    vec2 one = ilfac / TextureSize;

    vec2 xy = vTexCoord;  // CURVATURE off
    float cval = corner(xy);

    vec2 ilvec = vec2(0.0, ilfac.y * interlace_detect > 1.5 ? glsl_mod(FrameCount, 2.0) : 0.0);
    vec2 ratio_scale = (xy * TextureSize - 0.5 + ilvec) / ilfac;
    vec2 uv_ratio = fract(ratio_scale);
    xy = (floor(ratio_scale) * ilfac + 0.5 - ilvec) / TextureSize;

    vec4 coeffs = PI * vec4(1.0 + uv_ratio.x, uv_ratio.x, 1.0 - uv_ratio.x, 2.0 - uv_ratio.x);
    coeffs = FIX(coeffs);
    coeffs = 2.0 * sin(coeffs) * sin(coeffs / 2.0) / (coeffs * coeffs);
    coeffs /= dot(coeffs, vec4(1, 1, 1, 1));

    vec4 col = clamp(TEX2D(xy + vec2(-one.x, 0.0)) * coeffs.x + TEX2D(xy) * coeffs.y +
                     TEX2D(xy + vec2(one.x, 0.0)) * coeffs.z + TEX2D(xy + vec2(2.0 * one.x, 0.0)) * coeffs.w, 0.0, 1.0);
    vec4 col2 = clamp(TEX2D(xy + vec2(-one.x, one.y)) * coeffs.x + TEX2D(xy + vec2(0.0, one.y)) * coeffs.y +
                      TEX2D(xy + one) * coeffs.z + TEX2D(xy + vec2(2.0 * one.x, one.y)) * coeffs.w, 0.0, 1.0);

    vec4 weights = scanlineWeights(uv_ratio.y, col);
    vec4 weights2 = scanlineWeights(1.0 - uv_ratio.y, col2);
    // OVERSAMPLE
    float filter_ = fwidth(ratio_scale.y);
    uv_ratio.y = uv_ratio.y + 1.0 / 3.0 * filter_;
    weights = (weights + scanlineWeights(uv_ratio.y, col)) / 3.0;
    weights2 = (weights2 + scanlineWeights(abs(1.0 - uv_ratio.y), col2)) / 3.0;
    uv_ratio.y = uv_ratio.y - 2.0 / 3.0 * filter_;
    weights = weights + scanlineWeights(abs(uv_ratio.y), col) / 3.0;
    weights2 = weights2 + scanlineWeights(abs(1.0 - uv_ratio.y), col2) / 3.0;

    vec3 mul_res = (col * weights + col2 * weights2).rgb * cval;

    // mask_type 1: magenta/green aperture, alpha 3/6
    float on = 1.0, off = 0.0;  // mask_weights_alpha is called with mask_intensity 1
    vec3 magenta = vec3(on, off, on), green = vec3(off, on, off);
    vec2 coord = vTexCoord * OutputSize.xy;
    vec3 maskw = mix(magenta, green, floor(glsl_mod(coord.x, 2.0)));
    float alpha = 3.0 / 6.0;

    vec2 u_tex_size1 = OutputSize.xy * SourceSize.zw;
    float nbright = 255.0 - 255.0 * alpha;
    float fbright = nbright / (u_tex_size1.x * u_tex_size1.y);
    float aperture_average = mix(1.0 - DOTMASK * (1.0 - DOTMASK_brightboost), 1.0, fbright);
    vec3 clow = (1.0 - DOTMASK) * mul_res + (DOTMASK * DOTMASK_brightboost) * mul_res * mul_res;
    float ifbright = 1.0 / fbright;
    vec3 chi = (ifbright * aperture_average) * mul_res - (ifbright - 1.0) * clow;
    vec3 cout = mix(clow, chi, maskw);

#ifdef CRT_HDR
    cout *= CRT_HDR_BOOST;  // HDR: beams can go past paper white (hdr.ini crt_boost)
#endif
    return float4(pow(max(cout, 0.0), 1.0 / monitorgamma), 1.0);
}

float4 main(PSIn i) : SV_Target {
    InitSizes();
    float4 c = geom_main(i);
    c.a = Frame.Load(int3(i.pos.xy, 0)).a;  // keep the game's alpha, as the copy it replaces does
    return c;
}
