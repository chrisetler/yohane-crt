// Port of crt-easymode.slang (EasyMode, GPL) from RetroArch's slang shaders. Defaults as shipped.
// Original: CRT Shader by EasyMode, licensed GPL. Port distributed under the same terms.

static const float SHARPNESS_H = 0.5;
static const float SHARPNESS_V = 1.0;
static const float MASK_STRENGTH = 0.3;
static const float MASK_DOT_WIDTH = 1.0;
static const float MASK_DOT_HEIGHT = 1.0;
static const float MASK_STAGGER = 0.0;
static const float MASK_SIZE = 1.0;
static const float SCANLINE_STRENGTH = 1.0;
static const float SCANLINE_BEAM_WIDTH_MIN = 1.5;
static const float SCANLINE_BEAM_WIDTH_MAX = 1.5;
static const float SCANLINE_BRIGHT_MIN = 0.35;
static const float SCANLINE_BRIGHT_MAX = 0.65;
static const float SCANLINE_CUTOFF = 400.0;
static const float GAMMA_INPUT = 2.0;
static const float GAMMA_OUTPUT = 1.8;
static const float BRIGHT_BOOST = 1.2;
static const float DILATION = 1.0;

#define PI 3.141592653589
vec4 FIX(vec4 c) { return max(abs(c), 1e-5); }

vec4 dilate(vec4 col) { vec4 x = mix(vec4(1, 1, 1, 1), col, DILATION); return col * x; }
#define TEX2D(c) dilate(texture(Source, c))

float curve_distance(float x, float sharp) {
    float x_step = step(0.5, x);
    float curve = 0.5 - sqrt(0.25 - (x - x_step) * (x - x_step)) * sign(0.5 - x);
    return mix(x, curve, sharp);
}

// GLSL mat4(a,b,c,d) * v = a*v.x + b*v.y + c*v.z + d*v.w; columns kept for the min/max clamp.
vec3 filter_lanczos(vec4 coeffs, vec4 c0, vec4 c1, vec4 c2, vec4 c3) {
    vec4 col = c0 * coeffs.x + c1 * coeffs.y + c2 * coeffs.z + c3 * coeffs.w;
    col = clamp(col, min(c1, c2), max(c1, c2));
    return col.rgb;
}

float4 main(PSIn i) : SV_Target {
    vec2 vTexCoord = i.uv;
    vec2 dx = vec2(SourceSize.z, 0.0);
    vec2 dy = vec2(0.0, SourceSize.w);
    vec2 pix_co = vTexCoord * SourceSize.xy - vec2(0.5, 0.5);
    vec2 tex_co = (floor(pix_co) + vec2(0.5, 0.5)) * SourceSize.zw;
    vec2 dist = fract(pix_co);

    float curve_x = curve_distance(dist.x, SHARPNESS_H * SHARPNESS_H);
    vec4 coeffs = PI * vec4(1.0 + curve_x, curve_x, 1.0 - curve_x, 2.0 - curve_x);
    coeffs = FIX(coeffs);
    coeffs = 2.0 * sin(coeffs) * sin(coeffs * 0.5) / (coeffs * coeffs);
    coeffs /= dot(coeffs, vec4(1, 1, 1, 1));

    vec3 col = filter_lanczos(coeffs, TEX2D(tex_co - dx), TEX2D(tex_co), TEX2D(tex_co + dx), TEX2D(tex_co + 2.0 * dx));
    vec2 t2 = tex_co + dy;
    vec3 col2 = filter_lanczos(coeffs, TEX2D(t2 - dx), TEX2D(t2), TEX2D(t2 + dx), TEX2D(t2 + 2.0 * dx));

    col = mix(col, col2, curve_distance(dist.y, SHARPNESS_V));
    col = pow(max(col, 0.0), GAMMA_INPUT / (DILATION + 1.0));

    float luma = dot(vec3(0.2126, 0.7152, 0.0722), col);
    float bright = (max(col.r, max(col.g, col.b)) + luma) * 0.5;
    float scan_bright = clamp(bright, SCANLINE_BRIGHT_MIN, SCANLINE_BRIGHT_MAX);
    float scan_beam = clamp(bright * SCANLINE_BEAM_WIDTH_MAX, SCANLINE_BEAM_WIDTH_MIN, SCANLINE_BEAM_WIDTH_MAX);
    float scan_weight = 1.0 - pow(cos(vTexCoord.y * 2.0 * PI * SourceSize.y) * 0.5 + 0.5, scan_beam) * SCANLINE_STRENGTH;

    float mask = 1.0 - MASK_STRENGTH;
    vec2 mod_fac = floor(vTexCoord * OutputSize.xy * SourceSize.xy / (SourceSize.xy * vec2(MASK_SIZE, MASK_DOT_HEIGHT * MASK_SIZE)));
    int dot_no = int(glsl_mod((mod_fac.x + glsl_mod(mod_fac.y, 2.0) * MASK_STAGGER) / MASK_DOT_WIDTH, 3.0));
    vec3 mask_weight;
    if (dot_no == 0) mask_weight = vec3(1.0, mask, mask);
    else if (dot_no == 1) mask_weight = vec3(mask, 1.0, mask);
    else mask_weight = vec3(mask, mask, 1.0);

    if (SourceSize.y >= SCANLINE_CUTOFF) scan_weight = 1.0;

    col2 = col.rgb;
    col *= scan_weight;
    col = mix(col, col2, scan_bright);
    col *= mask_weight;
    col = pow(max(col, 0.0), 1.0 / GAMMA_OUTPUT);
    return float4(col * BRIGHT_BOOST, 1.0);
}
