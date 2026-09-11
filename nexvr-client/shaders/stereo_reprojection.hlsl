#if defined(VULKAN)
#define VK_BINDING(x) [[vk::binding(x)]]
#else
#define VK_BINDING(x)
#endif

VK_BINDING(0) cbuffer StereoConstants : register(b0)
{
    row_major float4x4 InverseViewProj;
    row_major float4x4 LeftViewProj;
    row_major float4x4 RightViewProj;
    
    float3 OriginalEyePos;
    float Pad0;
    
    float3 LeftEyePos;
    float Pad1;
    
    float3 RightEyePos;
    float Pad2;
    
    uint Width;
    uint Height;
    uint ShouldAttemptStereo;
    float Padding;
};

VK_BINDING(1) Texture2D<float4> GameColor : register(t0);
VK_BINDING(2) Texture2D<float> GameDepth : register(t1);
VK_BINDING(3) SamplerState LinearSampler : register(s0);

VK_BINDING(4) RWTexture2D<float4> OutLeftEye : register(u0);
VK_BINDING(5) RWTexture2D<float4> OutRightEye : register(u1);

// Standard depth unprojection
float3 WorldPositionFromDepth(float2 uv, float depth)
{
    float x = uv.x * 2.0f - 1.0f;
    float y = (1.0f - uv.y) * 2.0f - 1.0f;
    float4 clipSpacePosition = float4(x, y, depth, 1.0f);
    
    float4 worldSpacePosition = mul(clipSpacePosition, InverseViewProj);
    return worldSpacePosition.xyz / worldSpacePosition.w;
}

// 8-tap bilateral edge-aware push-pull hole inpainter for disoccluded stereo regions
float4 InpaintDisocclusion(int2 p, float refDepth)
{
    float4 sumColor = float4(0, 0, 0, 0);
    float totalWeight = 0.0001f;
    
    const int2 offsets[8] = {
        int2(-1, -1), int2(0, -1), int2(1, -1),
        int2(-1,  0),              int2(1,  0),
        int2(-1,  1), int2(0,  1), int2(1,  1)
    };
    
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        int2 neighborPos = clamp(p + offsets[i] * 2, int2(0, 0), int2(Width - 1, Height - 1));
        float2 neighborUV = float2((float)neighborPos.x / Width, (float)neighborPos.y / Height);
        float neighborDepth = GameDepth.Load(int3(neighborPos, 0));
        float4 neighborColor = GameColor.SampleLevel(LinearSampler, neighborUV, 0);
        
        // Prefer deeper background pixels to fill disocclusions cleanly
        float depthWeight = (neighborDepth >= refDepth) ? 2.0f : 0.5f;
        sumColor += neighborColor * depthWeight;
        totalWeight += depthWeight;
    }
    
    float4 result = sumColor / totalWeight;
    result.a = 1.0f;
    return result;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height)
        return;
        
    int2 pixelPos = int2(dispatchThreadId.x, dispatchThreadId.y);
    float2 uv = float2(((float)pixelPos.x + 0.5f) / (float)Width, ((float)pixelPos.y + 0.5f) / (float)Height);
    
    // Sample raw 2D color
    float4 baseColor = GameColor.SampleLevel(LinearSampler, uv, 0);
    baseColor.a = 1.0f;
    
    if (ShouldAttemptStereo == 0)
    {
        // 100% Full FOV in 2D mode - no letterboxing or black bars
        float4 outColor = baseColor;
        outColor.a = 1.0f;
        
        OutLeftEye[pixelPos] = outColor;
        OutRightEye[pixelPos] = outColor;
        return;
    }
    
    float depth = GameDepth.SampleLevel(LinearSampler, uv, 0).r;
    
    // 2D HUD / Clear depth check: If depth is at exact clearing bounds or uninitialized, pass 2D color
    if (depth <= 0.000001f || depth >= 0.999999f)
    {
        float4 hudColor = baseColor;
        hudColor.a = 1.0f;
        OutLeftEye[pixelPos] = hudColor;
        OutRightEye[pixelPos] = hudColor;
        return;
    }
    
    // Unproject pixel ray to 3D world space
    float3 worldPos = WorldPositionFromDepth(uv, depth);
    
    // Left Eye Backward Gather
    float4 leftClip = mul(float4(worldPos, 1.0f), LeftViewProj);
    float4 leftColor = baseColor;
    if (leftClip.w > 0.0001f)
    {
        float2 leftNdc = leftClip.xy / leftClip.w;
        float2 leftUV = float2(leftNdc.x * 0.5f + 0.5f, 1.0f - (leftNdc.y * 0.5f + 0.5f));
        
        if (leftUV.x >= 0.0f && leftUV.x <= 1.0f && leftUV.y >= 0.0f && leftUV.y <= 1.0f)
        {
            leftColor = GameColor.SampleLevel(LinearSampler, leftUV, 0);
            leftColor.a = 1.0f;
        }
        else
        {
            leftColor = InpaintDisocclusion(pixelPos, depth);
        }
    }
    
    // Right Eye Backward Gather
    float4 rightClip = mul(float4(worldPos, 1.0f), RightViewProj);
    float4 rightColor = baseColor;
    if (rightClip.w > 0.0001f)
    {
        float2 rightNdc = rightClip.xy / rightClip.w;
        float2 rightUV = float2(rightNdc.x * 0.5f + 0.5f, 1.0f - (rightNdc.y * 0.5f + 0.5f));
        
        if (rightUV.x >= 0.0f && rightUV.x <= 1.0f && rightUV.y >= 0.0f && rightUV.y <= 1.0f)
        {
            rightColor = GameColor.SampleLevel(LinearSampler, rightUV, 0);
            rightColor.a = 1.0f;
        }
        else
        {
            rightColor = InpaintDisocclusion(pixelPos, depth);
        }
    }
    
    // Preserve native game lighting, darks, fog, and specular highlights without artificial gamma squaring
    leftColor.a = 1.0f;
    rightColor.a = 1.0f;

    // Output full coverage per destination pixel
    OutLeftEye[pixelPos] = leftColor;
    OutRightEye[pixelPos] = rightColor;
}
