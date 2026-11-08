// harness filter: red above white with CRT_HDR, green mid grey, blue at white
float4 main(float4 p : SV_Position) : SV_Target {
#ifdef CRT_HDR
    return float4(1.5, 0.5, 1.0, 1.0);
#else
    return float4(1.0, 0.5, 1.0, 1.0);
#endif
}
