// Outdoor terrain and water. Every terrain triangle carries the ground textures
// of its three corners (bindless index | scale code << 24, constant across the
// triangle) and barycentric weights, so the pixel shader blends the corner
// textures smoothly across the tile.

#include "Frame.hlsli"

struct PushData {
    float4x4 viewProj; // includes the region's offset
    float4 offset;     // the region's offset (xyz), for world positions
    uint64_t frame;    // FrameConstants (0: the built-in defaults)
    uint mode;         // 0 terrain, 1 water, 2 terrain untextured, 3 modern water
    float timeSeconds;
    uint lightmap;     // the region's lightmap (bindless; 0 none)
    uint3 pad;
};
[[vk::push_constant]] PushData pc;


struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float3 barycentric : TEXCOORD0;
    [[vk::location(3)]] uint4 textures : TEXCOORD1;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 local : TEXCOORD0;
    float3 world : TEXCOORD3;
    float3 normal : NORMAL;
    float3 barycentric : TEXCOORD1;
    nointerpolation uint4 textures : TEXCOORD2;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    precise float4 clip = mul(pc.viewProj, float4(input.position, 1.0));
    output.position = clip;
    output.local = input.position;
    output.world = input.position + pc.offset.xyz;
    output.normal = input.normal;
    output.barycentric = input.barycentric;
    output.textures = input.textures;
    return output;
}

// Texture repeat per tile for a tile2d scale code: the base texture spans 4 tiles.
float TileUvScale(uint code) {
    switch (code) {
    case 1: return 0.5;
    case 2: return 0.25;
    case 3: return 2.0;
    case 4: return 4.0;
    default: return 1.0;
    }
}

float4 SampleGround(uint packed, float2 tileCoord) {
    const uint index = packed & 0xFFFFFF;
    const float2 uv = tileCoord * 0.25 * TileUvScale(packed >> 24);
    return gTextures[NonUniformResourceIndex(index)].Sample(gSamplers[0], uv); // LinearWrap
}

// ---- Modern water (its own pass after the scene): the scene behind it bent by the waves,
// coloured by the depth of water it is seen through, light rippling on the ground below, the
// sky and the sun reflected by Fresnel, foam along the shore.

// The surface's slope: a few travelling waves, long and short; each fades out where a pixel
// spans much of its length (`footprint`, world units), so far water does not shimmer.
float2 WaveSlope(float2 p, float t, float footprint) {
    const float2 dirs[4] = {float2(0.8, 0.6), float2(-0.5, 0.86), float2(0.2, -0.98), float2(-0.93, -0.37)};
    const float lengths[4] = {38.0, 23.0, 11.0, 6.5};
    const float heights[4] = {0.35, 0.22, 0.10, 0.05};
    float2 slope = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const float k = 6.2831853 / lengths[i];
        const float phase = dot(dirs[i], p) * k + t * sqrt(9.8 * k) * 1.4;
        const float keep = saturate(1.5 - footprint * 4.0 / lengths[i]);
        slope += dirs[i] * (heights[i] * k * cos(phase)) * keep;
    }
    return slope;
}

// Light focused by the waves onto what lies below: bright crossing lines that drift.
float Caustics(float2 p, float t) {
    const float a = sin(dot(p, float2(0.91, 0.42)) + t * 1.1) + sin(dot(p, float2(-0.37, 0.93)) * 1.3 - t * 0.9);
    const float b = sin(dot(p, float2(0.55, -0.83)) * 1.7 + t * 0.7 + a * 0.6);
    return pow(saturate(1.0 - abs(a + b) * 0.45), 6.0);
}

float LinearDepth(float d, float n, float f) { return n * f / max(f - d * (f - n), 0.0001); }

float4 FrameClip(float3 world) {
    return FrameVec(pc.frame, kFrameViewProj) * world.x + FrameVec(pc.frame, kFrameViewProj + 1) * world.y +
           FrameVec(pc.frame, kFrameViewProj + 2) * world.z + FrameVec(pc.frame, kFrameViewProj + 3);
}

float4 ModernWater(VSOutput input) {
    const float4 waterRow = FrameVec(pc.frame, kFrameWater);
    const float4 screen = FrameVec(pc.frame, kFrameScreen);
    const float n = waterRow.y, f = waterRow.z;
    const float2 uv = input.position.xy * screen.zw;
    const Texture2D depthMap = gTextures[NonUniformResourceIndex(uint(screen.x))];
    const Texture2D behind = gTextures[NonUniformResourceIndex(uint(waterRow.x))];
    const float surface = LinearDepth(input.position.z, n, f);
    const float thickness = max(LinearDepth(depthMap.SampleLevel(gSamplers[3], uv, 0).r, n, f) - surface, 0.0);

    const float t = pc.timeSeconds;
    const float3 camera = FrameVec(pc.frame, kFrameCamera).xyz;
    const float3 toEye = normalize(camera - input.world);
    const float distance = length(camera - input.world);
    // A pixel's length on the water: farther and flatter, longer (the view's grazing angle
    // stretches it along the line of sight).
    const float footprint = distance * 0.0012 / max(toEye.y, 0.08);
    float2 slope = WaveSlope(input.world.xz, t, footprint) + WaveSlope(input.world.xz * 2.7 + 13.0, t * 1.3, footprint * 2.7) * 0.35;
    // Ripples from the original's animated water texture (its mips keep them calm far away).
    const uint ripples = pc.lightmap != 0 ? pc.lightmap : (input.textures.x & 0xFFFFFF);
    const Texture2D rippleMap = gTextures[NonUniformResourceIndex(ripples)];
    const float2 rippleUv = input.local.xz / 80.0;
    const float3 lumaWeights = float3(0.3, 0.59, 0.11);
    const float r0 = dot(rippleMap.Sample(gSamplers[0], rippleUv).rgb, lumaWeights);
    const float rx = dot(rippleMap.Sample(gSamplers[0], rippleUv + float2(1.0 / 128.0, 0)).rgb, lumaWeights);
    const float rz = dot(rippleMap.Sample(gSamplers[0], rippleUv + float2(0, 1.0 / 128.0)).rgb, lumaWeights);
    slope += float2(rx - r0, rz - r0) * 3.0 * saturate(1.2 - footprint * 0.15);
    // Far away the surface reads as one calm sheet: what is left of its detail fades too.
    slope *= saturate(1.3 - footprint * 0.02);
    const float3 normal = normalize(float3(-slope.x, 1.0, -slope.y));

    // Refraction: the scene behind, bent; not where the bent sample lies in front of the water.
    float2 bentUv = uv + normal.xz * 0.035 * saturate(thickness / 25.0);
    float bentDepth = LinearDepth(depthMap.SampleLevel(gSamplers[3], bentUv, 0).r, n, f);
    if (bentDepth < surface) {
        bentUv = uv;
        bentDepth = surface + thickness;
    }
    const float depthBelow = max(bentDepth - surface, 0.0);
    float3 seen = behind.SampleLevel(gSamplers[1], bentUv, 0).rgb;

    const float4 lightDir = FrameVec(pc.frame, kFrameLight);
    const float3 diffuse = FrameVec(pc.frame, kFrameDiffuse).rgb, ambient = FrameVec(pc.frame, kFrameAmbient).rgb;
    const float sunUp = saturate(lightDir.y * 3.0) * FrameShadow(pc.frame, input.world, float3(0, 1, 0));
    // Caustics on the ground under the water (along the view ray to it), fading with depth.
    const float3 ground = input.world - toEye * depthBelow;
    const float caustic = Caustics(ground.xz * 0.11, t) * exp(-depthBelow * 0.05) * saturate(depthBelow * 0.5);
    seen += seen * caustic * diffuse * sunUp * 2.5 * waterRow.w;

    // Absorption: the deeper, the more the water's own colour (the environment's, as the
    // original tints its water).
    const float3 waterTint = FrameVec(pc.frame, kFrameWaterColour).rgb;
    const float3 absorb = exp(-depthBelow * float3(0.060, 0.026, 0.018));
    const float3 deep = float3(0.05, 0.17, 0.21) * (diffuse * 0.6 + ambient) * (0.5 + 0.5 * waterTint);
    float3 color = seen * absorb + deep * (1.0 - absorb);

    // Reflection: what the screen shows along the reflected ray (a few steps out), else the sky.
    const float3 r = reflect(-toEye, normal);
    float3 reflected = lerp(FrameVec(pc.frame, kFrameHorizon).rgb, FrameVec(pc.frame, kFrameZenith).rgb, saturate(r.y * 1.5));
    if (r.y > 0.0) {
        float march = 6.0;
        [loop] for (int k = 0; k < 20; ++k) {
            const float3 p = input.world + r * march;
            const float4 clip = FrameClip(p);
            if (clip.w <= 0.0) break;
            const float2 at = clip.xy / clip.w * 0.5 + 0.5;
            if (any(at < 0.0) || any(at > 1.0)) break;
            const float scene = LinearDepth(depthMap.SampleLevel(gSamplers[3], at, 0).r, n, f);
            if (clip.w > scene && clip.w - scene < max(15.0, march * 0.25)) {
                const float2 edge = saturate(min(at, 1.0 - at) * 10.0);
                reflected = lerp(reflected, behind.SampleLevel(gSamplers[1], at, 0).rgb, edge.x * edge.y);
                break;
            }
            march *= 1.35;
        }
    }
    const float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(normal, toEye)), 5.0);
    color = lerp(color, reflected, fresnel * 0.8);
    // The sun's glint, softer far away.
    color += pow(saturate(dot(r, lightDir.xyz)), 160.0) * diffuse * 1.4 * sunUp * saturate(1.5 - footprint * 0.25);

    // Foam where it is shallow.
    const float shallow = saturate(1.0 - thickness / 5.0);
    color = lerp(color, (diffuse + ambient) * 0.9, saturate(shallow * shallow * (0.5 + r0)) * 0.5);

    const float fog = FrameFog(pc.frame, input.world);
    return float4(lerp(color, FrameFogColor(pc.frame), fog), 1.0);
}

float4 PSMain(VSOutput input) : SV_Target {
    if (pc.mode == 3) return ModernWater(input);
    const float2 tileCoord = input.local.xz / 20.0;
    const float3 n = normalize(input.normal);
    // The original lights terrain with the region's lightmap (baked light and shadows) lifted
    // by the environment's shadow floor; with sun shadows, by the sun like everything else.
    float3 light;
    const float ao = FrameAo(pc.frame, input.position.xy);
    const float sunShadow = pc.mode == 1 ? 1.0 : FrameShadow(pc.frame, input.world, n);
    if (pc.frame != 0 && pc.lightmap != 0 && FrameVec(pc.frame, kFrameTerrainLight).a > 0.5) {
        const float3 baked = gTextures[NonUniformResourceIndex(pc.lightmap)].Sample(gSamplers[1], input.local.xz / 1920.0).rgb;
        light = saturate(baked + FrameVec(pc.frame, kFrameTerrainLight).rgb) * lerp(1.0, ao, 0.7);
    } else {
        light = FrameLight(pc.frame, n, false, sunShadow, ao);
    }
    light += FrameLocalLights(pc.frame, input.world, n, input.position.xy, false);
    const float fog = FrameFog(pc.frame, input.world);
    const float3 fogColor = FrameFogColor(pc.frame);

    if (pc.mode == 1) {
        // The original's water: unlit, the animated frame (pc.lightmap; the type 1 quad its
        // own texture) 4 times a block, times the environment's water colour; as opaque as
        // the vertex says (the depth there).
        const uint texture = input.barycentric.z > 0.5 || pc.lightmap == 0 ? (input.textures.x & 0xFFFFFF) : pc.lightmap;
        const float3 water = gTextures[NonUniformResourceIndex(texture)].Sample(gSamplers[0], input.local.xz / 80.0).rgb;
        const float3 tint = pc.frame != 0 ? FrameVec(pc.frame, kFrameWaterColour).rgb : 1.0;
        return float4(lerp(water * tint, fogColor, fog), input.barycentric.y);
    }

    float3 albedo = float3(0.55, 0.6, 0.5);
    if (pc.mode == 0) {
        albedo = SampleGround(input.textures.x, tileCoord).rgb * input.barycentric.x +
                 SampleGround(input.textures.y, tileCoord).rgb * input.barycentric.y +
                 SampleGround(input.textures.z, tileCoord).rgb * input.barycentric.z;
    }
    return float4(lerp(FrameWet(pc.frame, albedo * light, n, input.world, sunShadow), fogColor, fog), 1.0);
}

// Shadow maps: depth only.
void PSShadow(VSOutput input) {}
