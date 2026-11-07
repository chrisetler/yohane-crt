// Port of crt-lottes.slang (Timothy Lottes, public domain) from RetroArch's slang shaders.
// Defaults as shipped, except warpX/warpY = 0 (no curvature, so the level lines up with the HUD).
// Original: "PUBLIC DOMAIN CRT STYLED SCAN-LINE SHADER" by Timothy Lottes.

static const float hardScan = -8.0;
static const float hardPix = -3.0;
static const float warpX = 0.0;     // shipped 0.031
static const float warpY = 0.0;     // shipped 0.041
static const float maskDark = 0.5;
static const float maskLight = 1.5;
static const float shadowMask = 3.0;  // 1 TV, 2 aperture grille, 3 stretched VGA, 4 VGA
static const float brightBoost = 1.0;
static const float hardBloomPix = -1.5;
static const float hardBloomScan = -2.0;
static const float bloomAmount = 0.15;
static const float shape = 2.0;

float ToLinear1(float c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
vec3 ToLinear(vec3 c) { return vec3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b)); }
float ToSrgb1(float c) { return (c < 0.0031308 ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055); }
vec3 ToSrgb(vec3 c) { return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b)); }

vec3 Fetch(vec2 pos, vec2 off) {
    pos = (floor(pos * SourceSize.xy + off) + vec2(0.5, 0.5)) / SourceSize.xy;
    return ToLinear(brightBoost * texture(Source, pos.xy).rgb);
}
vec2 Dist(vec2 pos) { pos = pos * SourceSize.xy; return -((pos - floor(pos)) - vec2(0.5, 0.5)); }
float Gaus(float pos, float scale) { return exp2(scale * pow(abs(pos), shape)); }

vec3 Horz3(vec2 pos, float off) {
    vec3 b = Fetch(pos, vec2(-1.0, off)), c = Fetch(pos, vec2(0.0, off)), d = Fetch(pos, vec2(1.0, off));
    float dst = Dist(pos).x;
    float wb = Gaus(dst - 1.0, hardPix), wc = Gaus(dst, hardPix), wd = Gaus(dst + 1.0, hardPix);
    return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}
vec3 Horz5(vec2 pos, float off) {
    vec3 a = Fetch(pos, vec2(-2.0, off)), b = Fetch(pos, vec2(-1.0, off)), c = Fetch(pos, vec2(0.0, off));
    vec3 d = Fetch(pos, vec2(1.0, off)), e = Fetch(pos, vec2(2.0, off));
    float dst = Dist(pos).x;
    float wa = Gaus(dst - 2.0, hardPix), wb = Gaus(dst - 1.0, hardPix), wc = Gaus(dst, hardPix);
    float wd = Gaus(dst + 1.0, hardPix), we = Gaus(dst + 2.0, hardPix);
    return (a * wa + b * wb + c * wc + d * wd + e * we) / (wa + wb + wc + wd + we);
}
vec3 Horz7(vec2 pos, float off) {
    vec3 a = Fetch(pos, vec2(-3.0, off)), b = Fetch(pos, vec2(-2.0, off)), c = Fetch(pos, vec2(-1.0, off));
    vec3 d = Fetch(pos, vec2(0.0, off)), e = Fetch(pos, vec2(1.0, off)), f = Fetch(pos, vec2(2.0, off));
    vec3 g = Fetch(pos, vec2(3.0, off));
    float dst = Dist(pos).x;
    float s = hardBloomPix;
    float wa = Gaus(dst - 3.0, s), wb = Gaus(dst - 2.0, s), wc = Gaus(dst - 1.0, s), wd = Gaus(dst, s);
    float we = Gaus(dst + 1.0, s), wf = Gaus(dst + 2.0, s), wg = Gaus(dst + 3.0, s);
    return (a * wa + b * wb + c * wc + d * wd + e * we + f * wf + g * wg) / (wa + wb + wc + wd + we + wf + wg);
}
float Scan(vec2 pos, float off) { return Gaus(Dist(pos).y + off, hardScan); }
float BloomScan(vec2 pos, float off) { return Gaus(Dist(pos).y + off, hardBloomScan); }

vec3 Tri(vec2 pos) {
    vec3 a = Horz3(pos, -1.0), b = Horz5(pos, 0.0), c = Horz3(pos, 1.0);
    return a * Scan(pos, -1.0) + b * Scan(pos, 0.0) + c * Scan(pos, 1.0);
}
vec3 Bloom(vec2 pos) {
    vec3 a = Horz5(pos, -2.0), b = Horz7(pos, -1.0), c = Horz7(pos, 0.0), d = Horz7(pos, 1.0), e = Horz5(pos, 2.0);
    return a * BloomScan(pos, -2.0) + b * BloomScan(pos, -1.0) + c * BloomScan(pos, 0.0) +
           d * BloomScan(pos, 1.0) + e * BloomScan(pos, 2.0);
}
vec2 Warp(vec2 pos) {
    pos = pos * 2.0 - 1.0;
    pos *= vec2(1.0 + (pos.y * pos.y) * warpX, 1.0 + (pos.x * pos.x) * warpY);
    return pos * 0.5 + 0.5;
}
vec3 Mask(vec2 pos) {
    vec3 mask = vec3(maskDark, maskDark, maskDark);
    if (shadowMask == 1.0) {
        float lineMask = maskLight, odd = 0.0;
        if (fract(pos.x * 0.166666666) < 0.5) odd = 1.0;
        if (fract((pos.y + odd) * 0.5) < 0.5) lineMask = maskDark;
        pos.x = fract(pos.x * 0.333333333);
        if (pos.x < 0.333) mask.r = maskLight; else if (pos.x < 0.666) mask.g = maskLight; else mask.b = maskLight;
        mask *= lineMask;
    } else if (shadowMask == 2.0) {
        pos.x = fract(pos.x * 0.333333333);
        if (pos.x < 0.333) mask.r = maskLight; else if (pos.x < 0.666) mask.g = maskLight; else mask.b = maskLight;
    } else if (shadowMask == 3.0) {
        pos.x += pos.y * 3.0;
        pos.x = fract(pos.x * 0.166666666);
        if (pos.x < 0.333) mask.r = maskLight; else if (pos.x < 0.666) mask.g = maskLight; else mask.b = maskLight;
    } else if (shadowMask == 4.0) {
        pos.xy = floor(pos.xy * vec2(1.0, 0.5));
        pos.x += pos.y * 3.0;
        pos.x = fract(pos.x * 0.166666666);
        if (pos.x < 0.333) mask.r = maskLight; else if (pos.x < 0.666) mask.g = maskLight; else mask.b = maskLight;
    }
    return mask;
}

float4 main(PSIn i) : SV_Target {
    vec2 pos = Warp(i.uv);
    vec3 outColor = Tri(pos);
    outColor += Bloom(pos) * bloomAmount;
    if (shadowMask > 0.0) outColor *= Mask(i.uv / OutputSize.zw * 1.000001);
    return float4(ToSrgb(outColor), 1.0);
}
