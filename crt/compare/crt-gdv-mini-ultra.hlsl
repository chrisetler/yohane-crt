// Port of crt-gdv-mini-ultra.slang (guest(r), with DariusG; GPL v2+) from RetroArch's slang
// shaders: guest's single-pass condensation of crt-guest-advanced. Defaults as shipped, except
// warpY = 0 (no curvature). GDV_MASK can be overridden at compile time to try other masks.
// Original: Copyright (C) 2017-2018 guest(r) - guest.r@gmail.com, GPL v2 or later.

#ifndef GDV_MASK
#define GDV_MASK 11.0
#endif

static const float scanline = 10.0;
static const float beam_min = 1.5;
static const float beam_max = 2.0;
static const float h_sharp = 2.5;
static const float shadowMask = GDV_MASK;  // 0 CGWG, 1-4 Lottes, 5-11 guest variants
static const float thres = 0.4;
static const float masksize = 1.0;
static const float mcut = 0.2;
static const float maskDark = 0.0;
static const float maskLight = 1.5;
static const float CGWG = 1.0;
static const float warpX = 0.0;
static const float warpY = 0.0;    // shipped 0.05
static const float vignette = 1.0;
static const float gamma_out_red = 2.2;
static const float gamma_out_green = 2.2;
static const float gamma_out_blue = 2.2;
static const float brightboost = 1.2;
static const float sat = 1.2;
static const float glow = 0.35;

float sw(float x, float l) {
    float b = mix(beam_min, beam_max, l);
    return exp2(-scanline * pow(x, b));
}

vec3 Mask(vec2 pos, vec3 c) {
    pos = floor(pos / masksize);
    vec3 mask = vec3(maskDark, maskDark, maskDark);
    if (shadowMask == -1.0) {
        mask = vec3(1, 1, 1);
    } else if (shadowMask == 0.0) {
        pos.x = fract(pos.x * 0.5);
        float mc = 1.0 - CGWG;
        if (pos.x < 0.5) { mask.r = 1.1; mask.g = mc; mask.b = 1.1; }
        else { mask.r = mc; mask.g = 1.1; mask.b = mc; }
    } else if (shadowMask == 1.0) {
        float lineMask = maskLight, odd = 0.0;
        if (fract(pos.x / 6.0) < 0.5) odd = 1.0;
        if (fract((pos.y + odd) / 2.0) < 0.5) lineMask = maskDark;
        pos.x = fract(pos.x / 3.0);
        if (pos.x < 0.333) mask.b = maskLight; else if (pos.x < 0.666) mask.g = maskLight; else mask.r = maskLight;
        mask *= lineMask;
    } else if (shadowMask == 2.0) {
        pos.x = fract(pos.x / 3.0);
        if (pos.x < 0.333) mask.b = maskLight; else if (pos.x < 0.666) mask.g = maskLight; else mask.r = maskLight;
    } else if (shadowMask == 3.0) {
        pos.x += pos.y * 3.0;
        pos.x = fract(pos.x / 6.0);
        if (pos.x < 0.333) mask.b = maskLight; else if (pos.x < 0.666) mask.g = maskLight; else mask.r = maskLight;
    } else if (shadowMask == 4.0) {
        pos.xy = floor(pos.xy * vec2(1.0, 0.5));
        pos.x += pos.y * 3.0;
        pos.x = fract(pos.x / 6.0);
        if (pos.x < 0.333) mask.b = maskLight; else if (pos.x < 0.666) mask.g = maskLight; else mask.r = maskLight;
    } else if (shadowMask == 5.0) {
        float mx = max(max(c.r, c.g), c.b);
        vec3 maskTmp = min(1.25 * max(mx - mcut, 0.0) / (1.0 - mcut), maskDark + 0.2 * (1.0 - maskDark) * mx);
        float adj = 0.80 * maskLight - 0.5 * (0.80 * maskLight - 1.0) * mx + 0.75 * (1.0 - mx);
        mask = maskTmp;
        pos.x = fract(pos.x / 2.0);
        if (pos.x < 0.5) { mask.r = adj; mask.b = adj; } else mask.g = adj;
    } else if (shadowMask == 6.0) {
        float mx = max(max(c.r, c.g), c.b);
        vec3 maskTmp = min(1.33 * max(mx - mcut, 0.0) / (1.0 - mcut), maskDark + 0.225 * (1.0 - maskDark) * mx);
        float adj = 0.80 * maskLight - 0.5 * (0.80 * maskLight - 1.0) * mx + 0.75 * (1.0 - mx);
        mask = maskTmp;
        pos.x = fract(pos.x / 3.0);
        if (pos.x < 0.333) mask.r = adj; else if (pos.x < 0.666) mask.g = adj; else mask.b = adj;
    } else if (shadowMask == 7.0) {
        float mc = 1.0 - CGWG;
        float mx = max(max(c.r, c.g), c.b);
        float maskTmp = min(1.6 * max(mx - mcut, 0.0) / (1.0 - mcut), mc);
        mask = maskTmp;
        pos.x = fract(pos.x / 2.0);
        if (pos.x < 0.5) mask = 1.0 + 0.6 * (1.0 - mx);
    } else if (shadowMask == 8.0) {
        float lineMask = maskLight, odd = 0.0;
        if (fract(pos.x / 4.0) < 0.5) odd = 1.0;
        if (fract((pos.y + odd) / 2.0) < 0.5) lineMask = maskDark;
        pos.x = fract(pos.x / 2.0);
        if (pos.x < 0.5) { mask.r = maskLight; mask.b = maskLight; } else mask.g = maskLight;
        mask *= lineMask;
    } else if (shadowMask == 9.0) {
        vec3 M = vec3(maskDark, maskDark, maskDark);
        float left = 0.0;
        if (fract(pos.x / 6.0) < 0.5) left = 1.0;
        float m = fract(pos.x / 3.0);
        if (m < 0.3333) M.b = 0.9; else if (m < 0.6666) M.g = 0.9; else M.r = 0.9;
        if ((glsl_mod(pos.y, 2.0) == 1.0 && left == 1.0) || (glsl_mod(pos.y, 2.0) == 0.0 && left == 0.0)) M *= maskLight;
        return M;
    } else if (shadowMask == 10.0) {
        vec3 M = vec3(maskDark, maskDark, maskDark);
        float lineMask = maskLight, odd = 0.0;
        if (fract(pos.x / 6.0) < 0.5) odd = 1.0;
        if (fract((pos.y + odd) / 2.0) < 0.5) lineMask = 1.0;
        float m = fract(pos.x / 3.0);
        if (m > 0.3333) { M.r = 1.0; M.b = 1.0; } else if (m > 0.6666) M.g = 1.0; else M = mcut;
        if (m > 0.333) M *= lineMask;
        return M;
    } else if (shadowMask == 11.0) {
        vec3 M = vec3(maskDark, maskDark, maskDark);
        pos.x = fract(pos.x / 3.0);
        if (pos.x > 0.333) M = vec3(1, 1, 1);
        return M;
    }
    return mask;
}

float vign(vec2 vTexCoord) {
    vec2 vpos = vTexCoord;
    vpos *= 1.0 - vpos.xy;
    float vig = vpos.x * vpos.y * 45.0;
    vig = min(pow(max(vig, 0.0), 0.15), 1.0);
    return vignette == 0.0 ? 1.0 : vig;
}

vec2 Warp(vec2 pos) {
    pos = pos * 2.0 - 1.0;
    pos *= vec2(1.0 + (pos.y * pos.y) * warpX, 1.0 + (pos.x * pos.x) * warpY);
    return pos * 0.5 + 0.5;
}

vec3 saturation(vec3 textureColor) {
    float lum = length(textureColor.rgb) * 0.5775;
    vec3 lw = vec3(0.3, 0.6, 0.1);
    if (lum < 0.5) lw = (lw * lw) + (lw * lw);
    float luminance = dot(textureColor.rgb, lw);
    return mix(vec3(luminance, luminance, luminance), textureColor.rgb, sat);
}

// The original computes c*c for each tap but discards it (statements like "c20*c20;"), so the
// glow sums the raw samples. Kept that way for fidelity.
vec3 glow0(vec2 tc) {
    vec2 b = SourceSize.zw;
    vec3 c11 = texture(Source, tc).rgb;
    vec3 ring1 = texture(Source, tc + vec2(-b.x, 0)).rgb + texture(Source, tc + vec2(b.x, 0)).rgb +
                 texture(Source, tc + vec2(-b.x, -b.y)).rgb + texture(Source, tc + vec2(b.x, b.y)).rgb +
                 texture(Source, tc + vec2(-b.x, b.y)).rgb + texture(Source, tc + vec2(b.x, -b.y)).rgb +
                 texture(Source, tc + vec2(0, -b.y)).rgb + texture(Source, tc + vec2(0, b.y)).rgb;
    vec3 ring2 = texture(Source, tc + vec2(-2 * b.x, 0)).rgb + texture(Source, tc + vec2(2 * b.x, 0)).rgb +
                 texture(Source, tc + vec2(-2 * b.x, -b.y)).rgb + texture(Source, tc + vec2(-b.x, -2 * b.y)).rgb +
                 texture(Source, tc + vec2(b.x, 2 * b.y)).rgb + texture(Source, tc + vec2(2 * b.x, b.y)).rgb +
                 texture(Source, tc + vec2(-2 * b.x, b.y)).rgb + texture(Source, tc + vec2(-b.x, 2 * b.y)).rgb +
                 texture(Source, tc + vec2(b.x, -2 * b.y)).rgb + texture(Source, tc + vec2(2 * b.x, -b.y)).rgb +
                 texture(Source, tc + vec2(0, -2 * b.y)).rgb + texture(Source, tc + vec2(0, 2 * b.y)).rgb;
    return (3.0 * c11 + 2.5 * ring1 + 1.5 * ring2) / 45.0 * glow;
}

float4 main(PSIn i) : SV_Target {
    vec2 vTexCoord = i.uv * 1.0001;
    vec2 pos = Warp(vTexCoord);
    vec2 ps = SourceSize.zw;
    vec2 OGL2Pos = pos * SourceSize.xy;
    vec2 fp = fract(OGL2Pos);
    vec2 dx = vec2(ps.x, 0.0), dy = vec2(0.0, ps.y);
    vec2 pC4 = floor(OGL2Pos) * ps + 0.5 * ps;

    vec3 ul = texture(Source, pC4).xyz, ur = texture(Source, pC4 + dx).xyz;
    vec3 dl = texture(Source, pC4 + dy).xyz, dr = texture(Source, pC4 + ps).xyz;

    float lx = pow(fp.x, h_sharp);
    float rx = pow(1.0 - fp.x, h_sharp);
    vec3 color1 = (ur * lx + ul * rx) / (lx + rx);
    vec3 color2 = (dr * lx + dl * rx) / (lx + rx);

    float f = fp.y;
    float luma1 = color1.r * 0.3 + color1.g * 0.6 + color1.b * 0.1;
    float luma2 = color2.r * 0.3 + color2.g * 0.6 + color2.b * 0.1;

    color1 = (2.0 * pow(color1, 2.8)) - pow(color1, 3.6);
    color2 = (2.0 * pow(color2, 2.8)) - pow(color2, 3.6);

    color1 = color1 * mix(Mask(vTexCoord * OutputSize.xy, color1), vec3(1, 1, 1), luma1 * thres);
    color2 = color2 * mix(Mask(vTexCoord * OutputSize.xy, color2), vec3(1, 1, 1), luma2 * thres);

    vec3 color = color1 * sw(f, luma1) + color2 * sw(1.0 - f, luma2);
    color = min(color, 1.0);
    float lum = color.r * 0.3 + color.g * 0.6 + color.b * 0.1;

    color = pow(max(color, 0.0), vec3(1.0 / gamma_out_red, 1.0 / gamma_out_green, 1.0 / gamma_out_blue));
    color += glow0(pC4);
    color *= mix(1.0, brightboost, lum);
    color = saturation(color);
    color *= vign(vTexCoord);
    return float4(color, 1.0);
}
