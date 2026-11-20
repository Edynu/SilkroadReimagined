// The original's sky, around the camera (positions are camera-relative, 20000 units out),
// drawn first and without depth: dome (horizon -> zenith by height, the glow around the
// sun), stars, sun, moon, clouds, then a fog-coloured skirt below the horizon. Colours
// come from the frame's environment (Frame.hlsli).

#include "Frame.hlsli"

struct PushData {
    float4x4 viewProj;   // the camera's rotation and projection (no translation)
    uint64_t frame;
    uint kind;           // 1 dome, 2 stars, 3 sun, 4 moon, 5 clouds, 6 skirt
    uint textureIndex;
    float2 pixel;        // 1 / viewport size
    float starAlpha;     // the environment's star alpha (-1..1)
    float cloudAlpha;    // 0..1
    uint4 flicker;       // ten twinkle bytes (batches of 100 stars), packed
    float4 disc;         // sun / moon: centre direction angle, half size, cloud scroll, 0
};
[[vk::push_constant]] PushData pc;


struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 color : COLOR0;   // RGBA8
    [[vk::location(3)]] uint4 extra : TEXCOORD1; // stars: twinkle batch (0..9, 10 steady), size in pixels
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float depth : TEXCOORD1;
};

static const float kPi = 3.14159265;

uint FlickerByte(uint batch) {
    const uint word = batch / 4 == 0 ? pc.flicker.x : batch / 4 == 1 ? pc.flicker.y : pc.flicker.z;
    return (word >> ((batch % 4) * 8)) & 0xFF;
}

VSOutput VSMain(VSInput input) {
    VSOutput o;
    float3 p = input.position;
    o.uv = input.uv;
    o.color = input.color;
    if (pc.kind == 1) {
        // The dome: the gradient by height, then the glow by distance to the sun.
        const float4 zenith = FrameVec(pc.frame, kFrameZenith);
        const float4 scatter = FrameVec(pc.frame, kFrameScatter);
        const float3 base = lerp(FrameVec(pc.frame, kFrameHorizon).rgb, zenith.rgb, saturate(p.y / 5000.0 * zenith.w));
        const float angle = (FrameVec(pc.frame, kFrameFog).z - 0.25) * 2.0 * kPi;
        const float3 sun = float3(cos(angle), sin(angle), 0.0) * 20000.0;
        o.color = float4(lerp(scatter.rgb, base, min(1.0, distance(p, sun) / max(1.0, scatter.w))), 1.0);
    } else if (pc.kind == 6) {
        o.color = float4(FrameFogColor(pc.frame), input.color.a);
    } else if (pc.kind == 3 || pc.kind == 4) {
        // A quad facing the centre of the sky, on the arc at angle disc.x.
        const float a = pc.disc.x;
        const float3 centre = float3(cos(a), sin(a), 0.0) * 20000.0;
        p = centre + float3(-p.y * sin(a), p.y * cos(a), p.z) * pc.disc.y;
    } else if (pc.kind == 5) {
        o.uv = input.uv + pc.disc.z;
    }
    o.position = mul(pc.viewProj, float4(p, 1.0));
    o.depth = o.position.w;
    if (pc.kind == 2) {
        // Stars: a point of 1 (steady) or 2 (twinkling) pixels.
        const uint batch = input.extra.x;
        const float size = float(input.extra.y);
        o.position.xy += input.uv * size * pc.pixel * o.position.w;
        const float level = batch < 10 ? float(FlickerByte(batch)) : 255.0;
        o.color.a = float(uint(max(0.0, level * pc.starAlpha)) & 255u) / 255.0;
    }
    // Behind everything; the projection's depth range does not matter (no depth test).
    o.position.z = 0.5 * o.position.w;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target {
    if (pc.kind == 1 || pc.kind == 2 || pc.kind == 6) return input.color;
    const float4 tex = gTextures[NonUniformResourceIndex(pc.textureIndex)].Sample(gSamplers[0], input.uv);
    if (pc.kind == 3) return float4(saturate(FrameVec(pc.frame, kFrameSunTint).rgb), tex.a); // flat tint, the texture's alpha
    if (pc.kind == 4) return tex;
    // Clouds: lit by the scatter colour, into the fog far away.
    const float far = saturate((input.depth - 90000.0) / 110000.0);
    return float4(lerp(saturate(FrameVec(pc.frame, kFrameScatter).rgb) * tex.rgb, FrameFogColor(pc.frame), far),
                  tex.a * saturate(pc.cloudAlpha));
}
