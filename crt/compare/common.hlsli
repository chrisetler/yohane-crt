// Shared header for the RetroArch CRT shader ports. Prepended by crtcompare.exe.
// Maps the slang/GLSL names the original shaders use onto HLSL, so the ported bodies stay
// close to the originals and can be diffed against them.
//
// Bindings: t0 = Source (the game's level image downsampled to 480x270, sRGB-encoded),
// s0 = point sampler (RetroArch presets use filter_linear = false for these passes).

Texture2D Source : register(t0);
SamplerState samp : register(s0);

cbuffer Globals : register(b0) {
    float4 SourceSize;  // w, h, 1/w, 1/h
    float4 OutputSize;
    float  FrameCount;
    float3 _pad;
};

// Matches the game's vertex shader output layout (SV_Position, COLOR0, COLOR1, TEXCOORD0).
struct PSIn {
    float4 pos : SV_Position;
    float4 c0  : COLOR0;
    float4 c1  : COLOR1;
    float2 uv  : TEXCOORD0;
};

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define mix lerp
#define fract frac
#define inversesqrt rsqrt
#define texture(s, c) Source.SampleLevel(samp, (c), 0)

float  glsl_mod(float x, float y) { return x - y * floor(x / y); }
float2 glsl_mod(float2 x, float2 y) { return x - y * floor(x / y); }
