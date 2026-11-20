// The lens flare's sprites ("Glow and lens flare (C3g)"), in pixels; each one's
// alpha is its base times the intensity the visibility pass (Post.hlsl) keeps.

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4]; // LinearWrap, LinearClamp, PointWrap, PointClamp

struct FlarePush {
    float4 rect;        // centre x, y, half size, alpha base (0..255)
    float2 invViewport;
    uint texture;
    uint intensity;
};
[[vk::push_constant]] FlarePush flare;

struct FlareOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

FlareOutput VSFlare(uint vertexId : SV_VertexID) {
    static const float2 kCorners[6] = {float2(-1, -1), float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(1, -1)};
    const float2 c = kCorners[vertexId % 6];
    const float2 pixel = flare.rect.xy + c * flare.rect.z;
    FlareOutput output;
    output.position = float4(pixel * flare.invViewport * 2.0 - 1.0, 0.0, 1.0);
    output.uv = c * 0.5 + 0.5;
    return output;
}

float4 PSFlare(FlareOutput input) : SV_Target {
    const float intensity = gTextures[NonUniformResourceIndex(flare.intensity)].SampleLevel(gSamplers[3], float2(0.5, 0.5), 0).r;
    if (intensity <= 0.1) discard;
    const float4 texel = gTextures[NonUniformResourceIndex(flare.texture)].Sample(gSamplers[1], input.uv);
    // The texture factor: white, alpha = the slot's base x the intensity, truncated.
    return float4(texel.rgb, texel.a * floor(flare.rect.w * intensity) / 255.0);
}
