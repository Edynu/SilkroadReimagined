// The game UI (sr::ui draw lists, gfx::UiPass) through the bindless RHI.

struct PushData {
    float2 scale;
    float2 translate;
    uint textureIndex;
    // bits 0-3 sampler, bit 16 the texture has premultiplied alpha, bit 17 ignore its alpha
    uint flags;
    // A mask (0: none): its alpha multiplies the texture's, stretched over maskRect (pixels).
    uint maskIndex;
    // The scene's depth (0: no test) and the command's world depth: names over heads are hidden
    // where the scene is nearer (behind walls), as the original draws them with the head's depth.
    uint depthIndex;
    float4 maskRect;
    float depth;
    float3 pad;
};
[[vk::push_constant]] PushData pc;

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

struct VSInput {
    [[vk::location(0)]] float2 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 color : COLOR0;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position * pc.scale + pc.translate, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    Texture2D tex = gTextures[NonUniformResourceIndex(pc.textureIndex)];
    float4 c = tex.Sample(gSamplers[pc.flags & 0xF], input.uv);
    if ((pc.flags & 0x10000) != 0 && c.a > 0.0) {
        c.rgb /= c.a;
    }
    if ((pc.flags & 0x20000) != 0) {
        c.a = 1.0;
    }
    if (pc.maskIndex != 0) {
        // The original's minimap: a second texture stage whose alpha cuts the map to a disc.
        float2 maskUv = (input.position.xy - pc.maskRect.xy) / pc.maskRect.zw;
        c.a *= gTextures[NonUniformResourceIndex(pc.maskIndex)].Sample(gSamplers[pc.flags & 0xF], maskUv).a;
    }
    if (pc.depthIndex != 0) {
        float2 screenUv = input.position.xy * pc.scale * 0.5;
        float scene = gTextures[NonUniformResourceIndex(pc.depthIndex)].SampleLevel(gSamplers[3], screenUv, 0).r;
        if (pc.depth > scene + 0.00002) discard;
    }
    return c * input.color;
}
