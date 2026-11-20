// Cheap volumetrics at half resolution, read back by the compose pass:
//  - PSFog: height fog marched from the camera to the scene (the depth pre-pass), lit by the
//    sun through the cascaded shadow maps (shafts through gaps) and by the ambient light;
//    out: in-scattered light (rgb) and what is left of the scene behind it (a).
//  - PSClouds: a cloud layer 1800..3200 above the camera, marched where the sky shows, its
//    density from drifting value-noise and its light from one step toward the sun;
//    out: premultiplied colour (rgb) and coverage (a).

#include "Frame.hlsli"

struct PushData {
    uint64_t frame;   // FrameConstants
    uint64_t camera;  // viewProj, then its inverse (GLM column-major)
    uint depthIndex;  // the depth pre-pass
    float steps;
    float density;    // fog: the base density; clouds: the coverage
    float time;       // seconds (wind)
    float4 params;    // fog: x the mist's floor (world y), y its height; clouds: x base, y top (world y)
};
[[vk::push_constant]] PushData pc;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float4 MulCols(uint64_t m, float4 v) {
    return vk::RawBufferLoad<float4>(m, 16) * v.x + vk::RawBufferLoad<float4>(m + 16, 16) * v.y +
           vk::RawBufferLoad<float4>(m + 32, 16) * v.z + vk::RawBufferLoad<float4>(m + 48, 16) * v.w;
}
float3 WorldAt(float2 uv, float depth) {
    const float4 p = MulCols(pc.camera + 64, float4(uv * 2.0 - 1.0, depth, 1.0));
    return p.xyz / p.w;
}
float Jitter(float2 pixel) { return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))); }

// Henyey-Greenstein: how much light turns toward the eye.
float Phase(float cosine, float g) {
    const float g2 = g * g;
    return (1.0 - g2) / (12.566371 * pow(max(1.0 + g2 - 2.0 * g * cosine, 1e-4), 1.5));
}

// One bilinear compare in the sun's cascades (1 lit), cheap enough to take at every step.
float SunVisible(float3 world) {
    const float4 params = FrameVec(pc.frame, kFrameShadowParams);
    const uint cascades = uint(params.y);
    if (cascades == 0) return 1.0;
    const float depth = dot(world - FrameVec(pc.frame, kFrameCamera).xyz, FrameVec(pc.frame, kFrameForward).xyz);
    const float4 splits = FrameVec(pc.frame, kFrameShadowSplits);
    uint c = 0;
    while (c < cascades && depth > splits[c]) ++c;
    if (c >= cascades) return 1.0;
    const uint m = kFrameShadowAtlas + c * 4;
    const float4x4 atlas = float4x4(FrameVec(pc.frame, m), FrameVec(pc.frame, m + 1), FrameVec(pc.frame, m + 2), FrameVec(pc.frame, m + 3));
    const float3 s = mul(float4(world, 1.0), atlas).xyz;
    if (s.z >= 1.0) return 1.0;
    const float2 low = float2(c % 2, c / 2) * 0.5, high = low + 0.5;
    if (any(s.xy < low) || any(s.xy > high)) return 1.0;
    return s.z <= gTextures[NonUniformResourceIndex(uint(params.x))].SampleLevel(gSamplers[3], s.xy, 0).r ? 1.0 : 0.0;
}

float4 PSFog(VSOutput i) : SV_Target {
    const float d = gTextures[NonUniformResourceIndex(pc.depthIndex)].SampleLevel(gSamplers[3], i.uv, 0).r;
    const float3 camera = FrameVec(pc.frame, kFrameCamera).xyz;
    const float3 end = WorldAt(i.uv, min(d, 0.99999));
    float3 dir = end - camera;
    const float full = length(dir);
    dir /= max(full, 1e-4);
    const float reach = min(full, 1600.0);
    const uint steps = uint(pc.steps);
    const float stepLength = reach / float(steps);
    const float4 light = FrameVec(pc.frame, kFrameLight);
    const float sun = FrameVec(pc.frame, kFrameShadowStrength).x * saturate(light.y * 4.0);
    const float3 sunColour = FrameVec(pc.frame, kFrameDiffuse).rgb * sun;
    const float3 ambient = FrameVec(pc.frame, kFrameAmbient).rgb;
    const float phase = Phase(dot(dir, light.xyz), 0.55) * 12.566371; // 1 for light going sideways
    float transmittance = 1.0;
    float3 scattered = 0.0;
    float t = stepLength * Jitter(i.position.xy);
    for (uint k = 0; k < steps; ++k) {
        const float3 p = camera + dir * t;
        // None right by the camera: the fog starts 150 out and is whole by 500.
        const float near = smoothstep(150.0, 500.0, t);
        const float density = pc.density * near * exp(-max(p.y - pc.params.x, 0.0) / pc.params.y);
        const float extinction = density * stepLength;
        const float3 lit = sunColour * SunVisible(p) * phase + ambient * 0.6;
        scattered += transmittance * lit * extinction;
        transmittance *= exp(-extinction);
        t += stepLength;
    }
    return float4(scattered, transmittance);
}

// ---- Clouds ----
// A cloud layer at pc.params.x above the camera, seen where the sky shows: billows from
// warped value-noise (pc.density the cover), lit by how much cloud lies toward the sun
// (four steps) with bright edges against it, fading toward the horizon.
float Hash(float2 p) {
    float3 q = frac(float3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}
float ValueNoise(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
}
float Fbm(float2 p, int octaves) {
    float sum = 0.0, amplitude = 0.5, total = 0.0;
    for (int o = 0; o < octaves; ++o) {
        sum += ValueNoise(p) * amplitude;
        total += amplitude;
        p = float2(p.x * 1.6 - p.y * 1.2, p.x * 1.2 + p.y * 1.6) + float2(17.0, 31.0); // turned each octave
        amplitude *= 0.5;
    }
    return sum / total;
}
float CloudShape(float2 xz) {
    const float2 wind = float2(pc.time * 8.0, pc.time * 3.0);
    const float2 p = (xz + wind) * 0.00022;
    const float2 warp = float2(Fbm(p * 0.7 + 5.2, 3), Fbm(p * 0.7 + 1.3, 3)) - 0.5;
    const float shape = Fbm(p + warp * 0.9, 5);
    return smoothstep(1.0 - pc.density, 1.0 - pc.density + 0.3, shape);
}

float4 PSClouds(VSOutput i) : SV_Target {
    const float d = gTextures[NonUniformResourceIndex(pc.depthIndex)].SampleLevel(gSamplers[3], i.uv, 0).r;
    if (d < 1.0) return float4(0, 0, 0, 0); // only where the sky shows
    const float3 camera = FrameVec(pc.frame, kFrameCamera).xyz;
    const float3 dir = normalize(WorldAt(i.uv, 1.0) - camera);
    if (dir.y <= 0.01) return float4(0, 0, 0, 0);
    const float t = (pc.params.x - camera.y) / dir.y;
    const float2 xz = camera.xz + dir.xz * t;
    const float density = CloudShape(xz);
    if (density <= 0.001) return float4(0, 0, 0, 0);
    const float4 light = FrameVec(pc.frame, kFrameLight);
    const float3 toLight = light.y > -0.2 ? light.xyz : float3(0, 1, 0);
    // How much cloud lies between here and the sun.
    float toward = 0.0;
    const float2 stepXz = normalize(toLight.xz + 1e-4) * 260.0;
    [unroll] for (int k = 1; k <= 4; ++k) toward += CloudShape(xz + stepXz * float(k));
    const float lit = exp(-toward * 0.55);
    const float3 sun = FrameVec(pc.frame, kFrameDiffuse).rgb + FrameVec(pc.frame, kFrameAmbient).rgb * 0.4;
    const float3 sky = lerp(FrameVec(pc.frame, kFrameHorizon).rgb, FrameVec(pc.frame, kFrameZenith).rgb, 0.6);
    const float edge = pow(saturate(dot(dir, toLight)), 6.0) * (1.0 - density) * 2.0; // light through thin edges
    const float3 shadowSide = sky * 0.55 + FrameVec(pc.frame, kFrameAmbient).rgb * 0.35;
    float3 colour = lerp(shadowSide, sun * 1.05, lit) + sun * edge * lit;
    // Thinner and hazier toward the horizon.
    const float fade = saturate((dir.y - 0.01) * 6.0);
    colour = lerp(FrameVec(pc.frame, kFrameHorizon).rgb, colour, saturate(dir.y * 4.0));
    const float alpha = saturate(density * 1.15) * fade;
    return float4(colour * alpha, alpha);
}
