// The game UI (sr::ui draw lists) through the bindless RHI.

struct PushData {
    float2 scale;
    float2 translate;
    uint textureIndex;
    // bits 0-3 sampler, bit 16 the texture has premultiplied alpha, bit 17 ignore its alpha
    uint flags;
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
    return c * input.color;
}
