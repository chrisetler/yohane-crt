// harness filter: red above white with CRT_HDR, green mid grey, blue at white
struct PSIn { float4 pos : SV_Position; };  // filters name their input struct PSIn (the HDR wrapper uses it)
float4 main(PSIn i) : SV_Target {
#ifdef CRT_HDR
    return float4(1.5, 0.5, 1.0, 1.0);
#else
    return float4(1.0, 0.5, 1.0, 1.0);
#endif
}
