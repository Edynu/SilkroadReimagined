// Dear ImGui through the bindless RHI. Push constants select the texture,
// sampler and how to display it (the texture viewer uses the extra modes).

struct PushData {
    float2 scale;
    float2 translate;
    uint textureIndex;
    // bits 0-3 sampler, 4-7 channel view, 8-15 mip level + 1 (0 = automatic),
    // bit 16 texture has premultiplied alpha
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
    const uint samplerIndex = pc.flags & 0xF;
    const uint channel = (pc.flags >> 4) & 0xF;
    const uint mipPlusOne = (pc.flags >> 8) & 0xFF;
    const bool premultiplied = (pc.flags & 0x10000) != 0;

    Texture2D tex = gTextures[NonUniformResourceIndex(pc.textureIndex)];
    SamplerState smp = gSamplers[samplerIndex];
    float4 c = mipPlusOne != 0 ? tex.SampleLevel(smp, input.uv, float(mipPlusOne - 1)) : tex.Sample(smp, input.uv);

    if (premultiplied && c.a > 0.0) {
        c.rgb /= c.a;
    }
    switch (channel) {
    case 1: c = float4(c.rgb, 1.0); break; // RGB, ignore alpha
    case 2: c = float4(c.aaa, 1.0); break; // alpha as grey
    case 3: c = float4(c.rrr, 1.0); break;
    case 4: c = float4(c.ggg, 1.0); break;
    case 5: c = float4(c.bbb, 1.0); break;
    default: break;                        // RGBA
    }
    return c * input.color;
}
