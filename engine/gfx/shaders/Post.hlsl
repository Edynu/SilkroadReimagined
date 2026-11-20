// The original's glow and the lens flare's visibility ("Glow and lens flare (C3g)"):
// full-screen passes, by mode. The flare's sprites are Flare.hlsl.

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4]; // LinearWrap, LinearClamp, PointWrap, PointClamp

struct PushData {
    uint64_t constants; // PostConstants: uint textures[4], float4 params, float4 points[5]
    uint mode;
    uint pad;
};
[[vk::push_constant]] PushData pc;

static const uint kModeDown = 0, kModeBlurX = 1, kModeBlurY = 2, kModeComposite = 3, kModeVisibility = 4;

uint TextureAt(uint i) { return vk::RawBufferLoad<uint>(pc.constants + i * 4, 4); }
float4 Params() { return vk::RawBufferLoad<float4>(pc.constants + 16, 16); }
float4 Point(uint i) { return vk::RawBufferLoad<float4>(pc.constants + 32 + i * 16, 16); }
float4 Sample(uint texture, float2 uv) { return gTextures[NonUniformResourceIndex(texture)].SampleLevel(gSamplers[1], uv, 0); }

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
    // One triangle over the screen.
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VSOutput output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

// The blur's taps: 0, +-2.6 and +-5.2 texels of the 512 target (spread 13: 2 x 13 x i / 10),
// weighted 0x50, 0x46, 0x32 out of 255. A tap whose shifted image does not cover the pixel adds
// nothing (the original moves the quad).
static const float kTaps[5] = {0.0, 2.6, -2.6, 5.2, -5.2};
static const float kWeights[5] = {80.0 / 255.0, 70.0 / 255.0, 70.0 / 255.0, 50.0 / 255.0, 50.0 / 255.0};

float3 Blur(uint texture, float2 uv, float2 axis, float threshold) {
    float3 sum = 0.0;
    for (uint k = 0; k < 5; ++k) {
        const float2 at = uv - axis * kTaps[k] / 512.0;
        if (any(at < 0.0) || any(at > 1.0)) continue;
        sum += kWeights[k] * max(Sample(texture, at).rgb - threshold, 0.0);
    }
    return saturate(sum);
}

float4 PSMain(VSOutput input) : SV_Target {
    if (pc.mode == kModeDown) return float4(Sample(TextureAt(0), input.uv).rgb * (128.0 / 255.0), 1.0);
    if (pc.mode == kModeBlurX) return float4(Blur(TextureAt(0), input.uv, float2(1, 0), 40.0 / 255.0), 1.0);
    if (pc.mode == kModeBlurY) return float4(Blur(TextureAt(0), input.uv, float2(0, 1), 0.0), 1.0);
    if (pc.mode == kModeComposite) {
        const float3 scene = Sample(TextureAt(0), input.uv).rgb;
        if (Params().x < 0.5) return float4(scene, 1.0);
        // 0xC6 of the scene plus 0xC0 of the bloom stretched over it.
        return float4(saturate(scene * (198.0 / 255.0) + Sample(TextureAt(1), input.uv).rgb * (192.0 / 255.0)), 1.0);
    }
    // Visibility: the sun's point counts 0.3, each neighbour 0.175, when nothing nearer was drawn
    // there; the intensity follows by min(10 dt, 1) a frame.
    float score = 0.0;
    for (uint i = 0; i < 5; ++i) {
        const float4 p = Point(i);
        if (p.w < 0.5 || any(p.xy < 0.0) || any(p.xy > 1.0)) continue;
        const float depth = gTextures[NonUniformResourceIndex(TextureAt(0))].SampleLevel(gSamplers[3], p.xy, 0).r;
        if (p.z <= depth) score += i == 0 ? 0.3 : 0.175;
    }
    const float previous = Sample(TextureAt(1), float2(0.5, 0.5)).r;
    const float next = previous + (score - previous) * min(10.0 * Params().x, 1.0);
    return float4(next, 0.0, 0.0, 1.0);
}
