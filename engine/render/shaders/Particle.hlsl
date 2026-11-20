// Effect particles (.efp): world-space triangles built on the CPU (camera-facing plates,
// meshes), one texture per batch, tinted by the vertex colour. Blending comes from the
// pipeline (alpha or additive); depth is tested, never written.

struct PushData {
    float4x4 viewProj;
    uint textureIndex;
    uint flags; // bit 0: the effect blends its colour with ONE (ignore alpha), bit 16: premultiplied texture
    // Soft particles: the depth pre-pass (0: hard edges), 1 / its size, the camera's near and
    // far planes and the fade's depth.
    uint depthIndex;
    float softness;
    float2 rcpSize;
    float nearPlane;
    float farPlane;
};
[[vk::push_constant]] PushData pc;

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
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
    output.position = mul(pc.viewProj, float4(input.position, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    Texture2D tex = gTextures[NonUniformResourceIndex(pc.textureIndex)];
    float4 c = tex.Sample(gSamplers[0], input.uv);
    if ((pc.flags & 0x10000) != 0 && c.a > 0.0) {
        c.rgb /= c.a;
    }
    c *= input.color;
    if ((pc.flags & 1) != 0) {
        // D3DBLEND_ONE source: the colour adds as it is; alpha plays no part.
        c = float4(c.rgb, 1.0);
    }
    if (pc.depthIndex != 0) {
        // Fade out where the effect meets the scene instead of cutting into it.
        const float d = gTextures[NonUniformResourceIndex(pc.depthIndex)].SampleLevel(gSamplers[3], input.position.xy * pc.rcpSize, 0).r;
        const float n = pc.nearPlane, f = pc.farPlane;
        const float scene = n * f / max(f - d * (f - n), 0.0001);
        const float self = n * f / max(f - input.position.z * (f - n), 0.0001);
        const float fade = saturate((scene - self) / pc.softness);
        if ((pc.flags & 1) != 0) c.rgb *= fade;
        else c.a *= fade;
    }
    return c;
}
