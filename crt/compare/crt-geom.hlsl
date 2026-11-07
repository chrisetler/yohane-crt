// Port of crt-geom.slang (cgwg, Themaister, DOLLS; GPL) from RetroArch's slang shaders.
// Defaults as shipped, except CURVATURE = 0 (flat, so the level lines up with the HUD). The
// vertex-stage precomputation is done per pixel here; only mask_type 1 (the default) is ported.
// Original: Copyright (C) 2010-2012 cgwg, Themaister and DOLLS, GPL v2 or later.

static const float CRTgamma = 2.4;
static const float monitorgamma = 2.2;
static const float cornersize = 0.03;
static const float cornersmooth = 1000.0;
static const float overscan_x = 100.0;
static const float overscan_y = 100.0;
static const float DOTMASK = 0.3;
static const float DOTMASK_brightboost = 0.0;
static const float SHARPER = 1.0;
static const float scanline_weight = 0.3;
static const float lum = 0.0;
static const float interlace_detect = 1.0;

#define PI 3.141592653589
vec4 FIX(vec4 c) { return max(abs(c), 1e-5); }
#define TEX2D(c) pow(max(texture(Source, (c)), 0.0), CRTgamma)

static const vec2 aspect = vec2(1.0, 0.75);

vec4 scanlineWeights(float distance, vec4 color) {
    vec4 wid = 2.0 + 2.0 * pow(color, 4.0);
    vec4 weights = distance / scanline_weight;
    return (lum + 1.4) * exp(-pow(weights * inversesqrt(0.5 * wid), wid)) / (0.6 + 0.2 * wid);
}

float corner(vec2 coord) {
    coord = (coord - 0.5) * vec2(overscan_x / 100.0, overscan_y / 100.0) + 0.5;
    coord = min(coord, 1.0 - coord) * aspect;
    vec2 cdist = cornersize;
    coord = (cdist - min(coord, cdist));
    float dist = sqrt(dot(coord, coord));
    return clamp((cdist.x - dist) * cornersmooth, 0.0, 1.0);
}

float4 main(PSIn i) : SV_Target {
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

    return float4(pow(max(cout, 0.0), 1.0 / monitorgamma), 1.0);
}
