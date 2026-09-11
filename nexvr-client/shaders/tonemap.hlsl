Texture2D<float4> InputTex : register(t0);
RWTexture2D<float4> OutputTex : register(u0);

#pragma warning(disable: 3577)

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    float4 color = InputTex.Load(int3(DTid.xy, 0));
    float3 c = color.rgb;
    if (isnan(c.r) || isnan(c.g) || isnan(c.b) || isinf(c.r) || isinf(c.g) || isinf(c.b)) {
        c = float3(0.0, 0.0, 0.0);
    }
    c = clamp(c, 0.0f, 1.0f);
    OutputTex[DTid.xy] = float4(c, 1.0f);
}
