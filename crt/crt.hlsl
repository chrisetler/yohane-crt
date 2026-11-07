// CRT filter for YOHANE THE PARHELION -BLAZE in the DEEPBLUE-.
// Copyright (C) 2026 Chris Etler
//
// This program is free software; you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version. It is distributed WITHOUT
// ANY WARRANTY; see LICENSE.txt for details.
//
// Replaces the game's level copy-back draw (ps_164fc6f26724bc56), which runs after the whole
// level is drawn into the 1920x1080 working image and before the glow pass and the HUD. So
// this filters the play area only; HUD, dialogue and cutscene art are drawn afterwards.
//
// The level is pixel art at 4x (480x270 "virtual" pixels in a 1080p image). The filter rebuilds
// each virtual pixel, then draws it as a Gaussian-profile scanline, with an optional aperture-
// grille mask and screen curvature.
//
// Edit the settings below, then press F8 in-game to reload. F9 toggles the filter on and off.
// If this file fails to compile, the error goes to shaderlog\log.txt and the game keeps using
// the previous version (or the unfiltered original).

// ---- settings -------------------------------------------------------------------------------

#define VIRTUAL_HEIGHT    270.0  // native line count of the pixel art
#define SCANLINE_STRENGTH 0.70   // 0 = no scanlines, 1 = full gaps between lines
#define BEAM_DARK         0.30   // beam thickness for dark pixels (in lines)
#define BEAM_BRIGHT       0.50   // beam thickness for bright pixels (bright lines bloom wider)
#define SHARPNESS         0.4   // horizontal softness, in virtual pixels (lower = sharper)
#define BRIGHTNESS        1.30   // compensates for the light lost in the scanline gaps
#define MASK_STRENGTH     0.10   // aperture-grille RGB stripes; 0 = off. Keep low: the image
                                 // is rescaled 1.33x to a 1440p window, so strong masks moire
#define CURVATURE         0.00   // barrel distortion; try 0.03-0.06. Corners go black.
#define GAMMA             2.2

// ---- implementation -------------------------------------------------------------------------

Texture2D tex_sampler_WorkRTT : register(t0);

// Must match the game's vertex shader outputs (vs_0fb4...) register for register.
struct PSIn {
    float4 pos : SV_Position;
    float4 c0  : COLOR0;
    float4 c1  : COLOR1;
    float2 uv  : TEXCOORD0;
};

static float2 g_dims;
static float  g_scale;

// One virtual pixel's colour, in linear light. Averages the middle 2x2 of its 4x4 block so
// sprites that sit off the 4-pixel grid don't shimmer.
float3 Virt(int2 v) {
    int2 maxv = int2(g_dims / g_scale) - 1;
    v = clamp(v, int2(0, 0), maxv);
    int s = (int)g_scale;
    int2 b = v * s + (int)((uint)s / 2u) - 1;
    float3 c = tex_sampler_WorkRTT.Load(int3(b, 0)).rgb
             + tex_sampler_WorkRTT.Load(int3(b + int2(1, 0), 0)).rgb
             + tex_sampler_WorkRTT.Load(int3(b + int2(0, 1), 0)).rgb
             + tex_sampler_WorkRTT.Load(int3(b + int2(1, 1), 0)).rgb;
    return pow(saturate(c * 0.25), GAMMA);
}

// A horizontal run of virtual pixels on line y, resampled at fractional x with a Gaussian.
float3 Line(int y, int x0, float fx) {
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int dx = -1; dx <= 2; dx++) {
        float d = (dx - fx) / SHARPNESS;
        float w = exp(-d * d);
        sum += Virt(int2(x0 + dx, y)) * w;
        wsum += w;
    }
    return sum / wsum;
}

float Beam(float3 c, float dist) {
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    float width = lerp(BEAM_DARK, BEAM_BRIGHT, saturate(lum));
    float d = dist / width;
    return exp(-0.5 * d * d);
}

float2 Warp(float2 uv) {
    float2 c = uv * 2.0 - 1.0;
    c *= 1.0 + CURVATURE * dot(c, c) * float2(0.75, 1.0);
    return c * 0.5 + 0.5;
}

float4 main(PSIn i) : SV_Target {
    tex_sampler_WorkRTT.GetDimensions(g_dims.x, g_dims.y);
    g_scale = max(1.0, round(g_dims.y / VIRTUAL_HEIGHT));

    float2 uv = i.uv;
    if (CURVATURE > 0) {
        uv = Warp(uv);
        if (any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);
    }

    float2 v = uv * g_dims / g_scale - 0.5;  // virtual pixel space, centres on integers
    int2 base = (int2)floor(v);
    float2 f = v - base;

    float3 a = Line(base.y, base.x, f.x);      // line above
    float3 b = Line(base.y + 1, base.x, f.x);  // line below

    float3 scan = a * Beam(a, f.y) + b * Beam(b, 1.0 - f.y);
    float3 flat = lerp(a, b, f.y);
    float3 col = lerp(flat, scan * BRIGHTNESS, SCANLINE_STRENGTH);

    if (MASK_STRENGTH > 0) {
        uint m = (uint)i.pos.x % 3u;
        float3 stripe = float3(m == 0u, m == 1u, m == 2u);
        col *= 1.0 - MASK_STRENGTH + 3.0 * MASK_STRENGTH * stripe;  // averages to 1
    }

    float alpha = tex_sampler_WorkRTT.Load(int3(i.pos.xy, 0)).a;
    return float4(pow(saturate(col), 1.0 / GAMMA), alpha);
}
