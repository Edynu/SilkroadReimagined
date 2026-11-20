// The original's effects (gfx/EffectPass.h): plates expanded from
// their records (6 vertices each: the plate's corners at +-0.5 of its two axes, upright texture),
// meshes pulled from the geometry pool by index, one record per instance, and ribbons' vertices as they
// are (32 bytes each, their colour running along the strip). The texture's colour times
// the element's; a D3DBLEND_ONE source adds its colour as it is (alpha 1 into the additive blend);
// premultiplied textures are divided back first. Blending comes from the pipeline.

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

static const uint kSourceOne = 4;
static const uint kPremultiplied = 8;
static const uint kColour2x = 32, kColour4x = 64, kAlpha2x = 128, kAlpha4x = 256; // D3DTOP_MODULATE2X / 4X

struct PushData {
    float4x4 viewProj;  // GLM column-major
    uint64_t records;   // sprites (64 bytes each) or this draw's mesh instances (80 bytes each)
    uint64_t vertices;  // a mesh part's vertices in the geometry pool
    uint vertexFlags;   // 1: a lightmap uv follows the uv; 2: bones and weights follow
    float glow;         // ours: the effects' brightness (1 the original's): an added colour (ONE) dimmed, others made fainter
    uint pad1, pad2;
};
[[vk::push_constant]] PushData pc;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float4 colour : COLOR0;
    nointerpolation uint texture : TEXTURE;
    nointerpolation uint flags : FLAGS;
};

float4 Unpack(uint c) {
    return float4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) / 255.0;
}

VSOutput VSSprite(uint vertexId : SV_VertexID) {
    const uint64_t record = pc.records + uint64_t(vertexId / 6) * 64;
    const float4 a = vk::RawBufferLoad<float4>(record, 16);      // centre, colour
    const float4 b = vk::RawBufferLoad<float4>(record + 16, 16); // axis x, texture
    const float4 c = vk::RawBufferLoad<float4>(record + 32, 16); // axis y, flags
    const float4 window = vk::RawBufferLoad<float4>(record + 48, 16);
    // Two triangles: (-x +y) (+x +y) (+x -y), (-x +y) (+x -y) (-x -y).
    const float2 corners[6] = {float2(-0.5, 0.5), float2(0.5, 0.5), float2(0.5, -0.5), float2(-0.5, 0.5), float2(0.5, -0.5), float2(-0.5, -0.5)};
    const float2 corner = corners[vertexId % 6];
    const float3 world = a.xyz + b.xyz * corner.x + c.xyz * corner.y;
    VSOutput output;
    output.position = mul(pc.viewProj, float4(world, 1.0));
    output.uv = float2(corner.x + 0.5, 0.5 - corner.y) * window.zw + window.xy;
    output.colour = Unpack(asuint(a.w));
    output.texture = asuint(b.w);
    output.flags = asuint(c.w);
    return output;
}

VSOutput VSMesh(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    const uint stride = 32 + ((pc.vertexFlags & 1) ? 8 : 0) + ((pc.vertexFlags & 2) ? 12 : 0);
    const uint64_t v = pc.vertices + uint64_t(vertexId) * stride;
    const float3 p = vk::RawBufferLoad<float3>(v, 4);
    const float2 uv = vk::RawBufferLoad<float2>(v + 24, 4);
    const uint64_t record = pc.records + uint64_t(instanceId) * 80;
    const float4 r0 = vk::RawBufferLoad<float4>(record, 16);
    const float4 r1 = vk::RawBufferLoad<float4>(record + 16, 16);
    const float4 r2 = vk::RawBufferLoad<float4>(record + 32, 16);
    const uint4 look = vk::RawBufferLoad<uint4>(record + 48, 16); // colour, texture, flags
    const float4 window = vk::RawBufferLoad<float4>(record + 64, 16);
    const float3 world = float3(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, dot(r2.xyz, p) + r2.w);
    VSOutput output;
    output.position = mul(pc.viewProj, float4(world, 1.0));
    output.uv = uv * window.zw + window.xy;
    output.colour = Unpack(look.x);
    output.texture = look.y;
    output.flags = look.z;
    return output;
}

struct VSRibbonOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 colour : COLOR0;
    nointerpolation uint texture : TEXTURE;
    nointerpolation uint flags : FLAGS;
};

VSRibbonOutput VSRibbon(uint vertexId : SV_VertexID) {
    const uint64_t v = pc.records + uint64_t(vertexId) * 32;
    const float4 a = vk::RawBufferLoad<float4>(v, 16);      // position, colour
    const float4 b = vk::RawBufferLoad<float4>(v + 16, 16); // uv, texture, flags
    VSRibbonOutput output;
    output.position = mul(pc.viewProj, float4(a.xyz, 1.0));
    output.uv = b.xy;
    output.colour = Unpack(asuint(a.w));
    output.texture = asuint(b.z);
    output.flags = asuint(b.w);
    return output;
}

// A ribbon's texel: as PSMain's, its colour the one interpolated along the strip.
float4 PSRibbon(VSRibbonOutput input) : SV_Target {
    float4 c = gTextures[NonUniformResourceIndex(input.texture)].Sample(gSamplers[0], input.uv);
    if ((input.flags & kPremultiplied) != 0 && c.a > 0.0) c.rgb /= c.a;
    c *= input.colour;
    if ((input.flags & (kColour2x | kColour4x)) != 0) c.rgb = saturate(c.rgb * ((input.flags & kColour4x) != 0 ? 4.0 : 2.0));
    if ((input.flags & (kAlpha2x | kAlpha4x)) != 0) c.a = saturate(c.a * ((input.flags & kAlpha4x) != 0 ? 4.0 : 2.0));
    if ((input.flags & kSourceOne) != 0) c.a = 1.0;
    if ((input.flags & kSourceOne) != 0) c.rgb *= pc.glow; else c.a *= pc.glow;
    return c;
}

float4 PSMain(VSOutput input) : SV_Target {
    float4 c = gTextures[NonUniformResourceIndex(input.texture)].Sample(gSamplers[0], input.uv);
    if ((input.flags & kPremultiplied) != 0 && c.a > 0.0) c.rgb /= c.a;
    c *= input.colour;
    // The resource's texture stages: MODULATE2X / 4X brighten the product, clamped as the stage does.
    if ((input.flags & (kColour2x | kColour4x)) != 0) c.rgb = saturate(c.rgb * ((input.flags & kColour4x) != 0 ? 4.0 : 2.0));
    if ((input.flags & (kAlpha2x | kAlpha4x)) != 0) c.a = saturate(c.a * ((input.flags & kAlpha4x) != 0 ? 4.0 : 2.0));
    if ((input.flags & kSourceOne) != 0) c.a = 1.0; // ONE: the colour as it is, alpha plays no part
    if ((input.flags & kSourceOne) != 0) c.rgb *= pc.glow; else c.a *= pc.glow;
    return c;
}
