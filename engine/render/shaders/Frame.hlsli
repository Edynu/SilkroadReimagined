// The frame's constants (render/FrameConstants.h), read through a buffer address; an
// address of 0 (tools without a scene) uses the defaults built in here.

static const uint kFrameLight = 0, kFrameDiffuse = 1, kFrameAmbient = 2, kFrameFogColor = 3, kFrameFog = 4,
                  kFrameCamera = 5, kFrameForward = 6, kFrameZenith = 7, kFrameHorizon = 8, kFrameScatter = 9,
                  kFrameSunTint = 10, kFrameTerrainLight = 11, kFrameShadowAtlas = 12, kFrameShadowSplits = 28,
                  kFrameShadowTexels = 29, kFrameShadowParams = 30, kFrameLightGrid = 31, kFrameLightSlices = 32,
                  kFrameLightBuffers = 33, kFrameLightIndices = 34, kFrameShadowStrength = 35, kFrameScreen = 36,
                  kFrameShading = 37, kFrameWater = 38, kFrameWeather = 39,
                  kFrameWaterColour = 40, kFramePlayer = 41, kFrameViewProj = 42;

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

float4 FrameVec(uint64_t frame, uint index) {
    return vk::RawBufferLoad<float4>(frame + uint64_t(index) * 16, 16);
}

// How much of the sun reaches `world` (1 lit, 0 in shadow) from the cascaded shadow maps;
// 1 without them. `n` (unit) pushes the lookup off the surface by a texel and a half.
float FrameShadow(uint64_t frame, float3 world, float3 n) {
    if (frame == 0) return 1.0;
    const float4 params = FrameVec(frame, kFrameShadowParams);
    const uint cascades = uint(params.y);
    if (cascades == 0) return 1.0;
    const float depth = dot(world - FrameVec(frame, kFrameCamera).xyz, FrameVec(frame, kFrameForward).xyz);
    const float4 splits = FrameVec(frame, kFrameShadowSplits);
    uint c = 0;
    while (c < cascades && depth > splits[c]) ++c;
    if (c >= cascades) return 1.0;
    const float3 p = world + n * FrameVec(frame, kFrameShadowTexels)[c] * 1.5;
    const uint m = kFrameShadowAtlas + c * 4;
    const float4x4 atlas = float4x4(FrameVec(frame, m), FrameVec(frame, m + 1), FrameVec(frame, m + 2), FrameVec(frame, m + 3));
    const float3 s = mul(float4(p, 1.0), atlas).xyz; // rows are GLM's columns: row-vector order
    if (s.z >= 1.0) return 1.0;
    const float texel = params.z;
    const int radius = int(params.w);
    // Stay inside this cascade's quarter of the atlas.
    const float2 low = float2(c % 2, c / 2) * 0.5 + texel, high = low + 0.5 - 3.0 * texel;
    const Texture2D map = gTextures[NonUniformResourceIndex(uint(params.x))];
    float lit = 0.0, taps = 0.0;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            // A bilinear compare of the four texels around the tap.
            const float2 uv = clamp(s.xy + float2(x, y) * texel, low, high);
            const float2 f = frac(uv / texel - 0.5);
            const float4 d = map.GatherRed(gSamplers[3], uv); // (-,+) (+,+) (+,-) (-,-)
            const float4 inLight = step(s.z, d);
            lit += lerp(lerp(inLight.w, inLight.z, f.x), lerp(inLight.x, inLight.y, f.x), f.y);
            taps += 1.0;
        }
    }
    // Fade out over the last tenth of the shadows' reach.
    const float end = splits[cascades - 1];
    const float strength = FrameVec(frame, kFrameShadowStrength).x;
    return lerp(1.0, lit / taps, saturate((end - depth) / (end * 0.1)) * strength);
}

// Ambient occlusion at a pixel (SV_Position.xy): 1 without it.
float FrameAo(uint64_t frame, float2 pixel) {
    if (frame == 0) return 1.0;
    const float4 screen = FrameVec(frame, kFrameScreen);
    if (screen.y < 0.5) return 1.0;
    return gTextures[NonUniformResourceIndex(uint(screen.y))].SampleLevel(gSamplers[1], pixel * screen.zw, 0).r;
}

// The light on a surface: diffuse * N.L * shadow + ambient * occlusion, clamped (D3D
// fixed-function lighting; the shadow only with cascaded shadows). `twoSidedDefault`: the
// look before environments, for meshes lit from both sides.
float3 FrameLight(uint64_t frame, float3 n, bool twoSidedDefault, float shadow, float ao) {
    float4 light = float4(normalize(float3(0.4, 1.0, -0.3)), 0.0);
    float3 diffuse = 0.55, ambient = 0.45;
    if (frame != 0) {
        light = FrameVec(frame, kFrameLight);
        diffuse = FrameVec(frame, kFrameDiffuse).rgb;
        ambient = FrameVec(frame, kFrameAmbient).rgb;
    }
    const float d = dot(n, light.xyz);
    const float ndl = light.w > 0.5 || !twoSidedDefault ? max(0.0, d) : abs(d);
    return saturate(diffuse * ndl * shadow + ambient * ao);
}

// Characters in the modern look: light wrapping around the body (softer terminator) and a
// rim of sky light along the silhouette, on top of the original's terms.
float3 FrameCharacterLight(uint64_t frame, float3 n, float3 world, float shadow, float ao) {
    const float4 light = FrameVec(frame, kFrameLight);
    const float3 diffuse = FrameVec(frame, kFrameDiffuse).rgb, ambient = FrameVec(frame, kFrameAmbient).rgb;
    const float wrap = saturate((dot(n, light.xyz) + 0.35) / 1.35);
    const float3 toEye = normalize(FrameVec(frame, kFrameCamera).xyz - world);
    const float rim = pow(1.0 - saturate(dot(n, toEye)), 3.0) * (0.35 + 0.65 * shadow);
    const float3 sky = FrameVec(frame, kFrameHorizon).rgb;
    return saturate(diffuse * wrap * shadow + ambient * ao) + rim * (sky * 0.35 + diffuse * 0.25);
}

// Rain-soaked ground (the modern weather): upward faces darker, with a glossy glint of the
// sun. `color` is the lit surface, `shadow` the sun's.
float3 FrameWet(uint64_t frame, float3 color, float3 n, float3 world, float shadow) {
    if (frame == 0) return color;
    const float wet = FrameVec(frame, kFrameWeather).x * saturate(n.y * 1.5 - 0.3);
    if (wet <= 0.0) return color;
    const float3 toEye = normalize(FrameVec(frame, kFrameCamera).xyz - world);
    const float3 l = FrameVec(frame, kFrameLight).xyz;
    const float glint = pow(saturate(dot(reflect(-toEye, n), l)), 64.0) * shadow;
    return color * (1.0 - 0.35 * wet) + FrameVec(frame, kFrameDiffuse).rgb * glint * wet * 0.6;
}

bool FrameModernCharacters(uint64_t frame) { return frame != 0 && FrameVec(frame, kFrameShading).x > 0.5; }

// Linear fog by view depth between the frame's start and end (0 when off).
float FrameFog(uint64_t frame, float3 world) {
    if (frame == 0) return 0.0;
    if (FrameVec(frame, kFrameFogColor).a < 0.5) return 0.0;
    const float4 fog = FrameVec(frame, kFrameFog);
    const float depth = dot(world - FrameVec(frame, kFrameCamera).xyz, FrameVec(frame, kFrameForward).xyz);
    return saturate((depth - fog.x) / max(0.001, fog.y - fog.x));
}

float3 FrameFogColor(uint64_t frame) { return frame != 0 ? FrameVec(frame, kFrameFogColor).rgb : 0.0; }

uint64_t FrameAddress(uint2 halves) { return uint64_t(halves.x) | (uint64_t(halves.y) << 32); }

// The local lights (effects, torches) on a surface: those of the pixel's cluster, each
// fading to nothing at its radius; `pixel` is SV_Position.xy. 0 without local lights.
float3 FrameLocalLights(uint64_t frame, float3 world, float3 n, float2 pixel, bool twoSided) {
    if (frame == 0) return 0.0;
    const float4 grid = FrameVec(frame, kFrameLightGrid);
    if (grid.w < 0.5) return 0.0;
    const float4 slices = FrameVec(frame, kFrameLightSlices);
    const uint4 buffers = asuint(FrameVec(frame, kFrameLightBuffers));
    const uint4 indices = asuint(FrameVec(frame, kFrameLightIndices));
    const float3 camera = FrameVec(frame, kFrameCamera).xyz;
    const float depth = dot(world - camera, FrameVec(frame, kFrameForward).xyz);
    const uint slice = depth <= slices.z ? 0u : min(uint(log(depth / slices.z) * slices.w), uint(grid.z) - 1u);
    const uint tx = min(uint(pixel.x / slices.x), uint(grid.x) - 1u);
    const uint ty = min(uint(pixel.y / slices.y), uint(grid.y) - 1u);
    const uint cluster = (slice * uint(grid.y) + ty) * uint(grid.x) + tx;
    const uint2 entry = vk::RawBufferLoad<uint2>(FrameAddress(buffers.zw) + uint64_t(cluster) * 8, 8);
    const uint64_t lights = FrameAddress(buffers.xy), list = FrameAddress(indices.xy);
    const float3 toEye = normalize(camera - world);
    float3 sum = 0.0;
    for (uint i = 0; i < entry.y; ++i) {
        const uint index = vk::RawBufferLoad<uint>(list + uint64_t(entry.x + i) * 4, 4);
        const float4 a = vk::RawBufferLoad<float4>(lights + uint64_t(index) * 32, 16);
        const float4 b = vk::RawBufferLoad<float4>(lights + uint64_t(index) * 32 + 16, 16);
        float3 l = a.xyz - world;
        const float d2 = dot(l, l);
        if (d2 >= a.w * a.w) continue;
        const float d = sqrt(d2);
        l /= max(d, 0.001);
        float fade = 1.0 - d / a.w;
        fade *= fade;
        const float facing = dot(n, l);
        const float ndl = twoSided ? abs(facing) : max(facing, 0.0);
        float3 light = b.rgb * b.w * fade * ndl;
        if (indices.z != 0 && facing > 0.0) {
            light += b.rgb * b.w * fade * pow(saturate(dot(n, normalize(l + toEye))), 24.0) * 0.4;
        }
        sum += light;
    }
    return sum;
}
