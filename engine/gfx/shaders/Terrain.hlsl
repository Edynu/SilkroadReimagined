// The outdoor terrain (drawn as the original's "Terrain").
// One draw for every visible block: the vertex id names the block, the tile and the corner; height
// and normal are pulled from the region's range of the geometry pool (gfx/Terrain.h). A tile shows
// its textures in the original's order, each blended over the ones before by its alpha, which is
// bilinear between the tile's corners (the original's filtered alpha atlas); a texture is lit as
// the original's MODULATE2X stage (texture x vertex colour x 2), then the region's lightmap plus
// the lift multiplies the result. Fog as the original's: linear by view depth on each of its two
// passes, so far away the base and the lightmap's multiplier both turn to the fog colour.

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4]; // LinearWrap, LinearClamp, PointWrap, PointClamp

struct PushData {
    float4x4 viewProj; // GLM column-major
    uint64_t data;     // the constants (16 bytes), then the visible blocks (32 bytes each)
    uint64_t frame;    // FrameConstants (Frame.hlsli)
    uint lodBase;      // the terrain draw's first block (blocks of one detail level a draw)
    uint lodShift;     // its detail: a tile every 1 << lodShift height samples
};
[[vk::push_constant]] PushData pc;

#include "Frame.hlsli"

// The constants: the pool's address.
static const uint kConstantsBytes = 16;
// A visible block: u64 region offset in the pool, u32 block, u32 lightmap, float3 region offset,
// u32 the detail shifts of its west, east, south and north neighbours (a byte each).
static const uint kBlockBytes = 32;
// A region's range (gfx/Terrain.h).
static const uint kSampleOffset = 256 * 8;
static const uint kTileOffset = kSampleOffset + 36 * 289 * 8;
static const uint kTileStride = 36;

struct VSOutput {
    float4 position : SV_Position;
    float2 sample : TEXCOORD0; // in height samples across the block (0..16)
    float3 lit : COLOR0;       // the vertex's lit colour (fixed-function lighting)
    float viewDepth : DEPTH;   // for the fog
    float3 world : WORLDPOS;   // the sun's shadows (G1)
    float3 normal : NORMAL;
    nointerpolation uint visible : BLOCK;
};

float3 DecodeNormal(uint packed) {
    const float2 e = float2(int(packed << 16) >> 16, int(packed) >> 16) / 32767.0;
    float3 n = float3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
    if (n.y < 0.0) {
        n.xz = float2((1.0 - abs(e.y)) * (e.x >= 0.0 ? 1.0 : -1.0), (1.0 - abs(e.x)) * (e.y >= 0.0 ? 1.0 : -1.0));
    }
    return normalize(n);
}

float SampleHeight(uint64_t samples, uint sx, uint sz) { return vk::RawBufferLoad<float>(samples + uint64_t(sz * 17 + sx) * 8, 4); }

VSOutput VSMain(uint vertexId : SV_VertexID) {
    // Levels of detail (ours): a block of this draw has 16 >> shift tiles a side, each over
    // 1 << shift height samples.
    const uint shift = pc.lodShift;
    const uint side = 16u >> shift;
    const uint perBlock = side * side * 6;
    const uint visible = pc.lodBase + vertexId / perBlock;
    const uint local = vertexId % perBlock;
    const uint tile = local / 6;
    const uint tx = tile % side, tz = tile / side;
    // The original's two triangles a tile: the diagonal from (x, z) to (x+1, z+1) when the column's
    // and the row's parity match, from (x, z+1) to (x+1, z) otherwise. Corners as bits: x, z << 1.
    static const uint kMatch[6] = {0, 2, 3, 0, 3, 1};
    static const uint kOther[6] = {0, 2, 1, 2, 3, 1};
    const uint corner = (tx & 1) == (tz & 1) ? kMatch[local % 6] : kOther[local % 6];
    const uint sx = (tx + (corner & 1)) << shift, sz = (tz + (corner >> 1)) << shift;

    const uint64_t record = pc.data + kConstantsBytes + uint64_t(visible) * kBlockBytes;
    const uint64_t region = vk::RawBufferLoad<uint64_t>(pc.data, 8) + vk::RawBufferLoad<uint64_t>(record, 8);
    const uint block = vk::RawBufferLoad<uint>(record + 8, 4);
    const float3 offset = vk::RawBufferLoad<float3>(record + 16, 4);
    const uint64_t samples = region + kSampleOffset + uint64_t(block * 289) * 8;
    const uint64_t s = samples + uint64_t(sz * 17 + sx) * 8;
    float height = vk::RawBufferLoad<float>(s, 4);
    const float3 normal = DecodeNormal(vk::RawBufferLoad<uint>(s + 4, 4));
    // No cracks: on an edge shared with a coarser neighbour the height follows the neighbour's
    // straight edge between its own samples.
    const uint neighbours = vk::RawBufferLoad<uint>(record + 28, 4);
    uint edge = 0xFFu;
    if (sx == 0) edge = neighbours & 0xFF;
    else if (sx == 16) edge = (neighbours >> 8) & 0xFF;
    if (sz == 0) edge = edge == 0xFFu ? (neighbours >> 16) & 0xFF : max(edge, (neighbours >> 16) & 0xFF);
    else if (sz == 16) edge = edge == 0xFFu ? neighbours >> 24 : max(edge, neighbours >> 24);
    if (edge != 0xFFu && edge > shift) {
        const uint step = 1u << edge;
        if (sx == 0 || sx == 16) {
            const uint z0 = sz / step * step, z1 = min(z0 + step, 16u);
            if (z0 != sz) height = lerp(SampleHeight(samples, sx, z0), SampleHeight(samples, sx, z1), float(sz - z0) / float(step));
        } else {
            const uint x0 = sx / step * step, x1 = min(x0 + step, 16u);
            if (x0 != sx) height = lerp(SampleHeight(samples, x0, sz), SampleHeight(samples, x1, sz), float(sx - x0) / float(step));
        }
    }

    const float3 world = offset + float3(float(block % 6) * 320.0 + float(sx) * 20.0, height, float(block / 6) * 320.0 + float(sz) * 20.0);
    VSOutput output;
    output.position = mul(pc.viewProj, float4(world, 1.0));
    output.sample = float2(sx, sz);
    // The original's lit colour while the terrain draws: the world material is emissive
    // c5 and nothing else; its vertices carry no colour, so D3D9 takes the material's diffuse (0)
    // and the light adds nothing. The light's direction stays in the frame constants (unused here).
    output.lit = saturate(FrameValue(pc.frame, kFrameTerrainAmbient).rgb);
    output.viewDepth = output.position.w;
    output.world = world;
    output.normal = normal;
    output.visible = visible;
    return output;
}

// The texture matrix by scale code: x1, x0.5, x0.25, x2, x4; codes 5-7 a zero matrix.
float ScaleFactor(uint code) {
    switch (code) {
    case 0: return 1.0;
    case 1: return 0.5;
    case 2: return 0.25;
    case 3: return 2.0;
    case 4: return 4.0;
    default: return 0.0;
    }
}

float4 PSMain(VSOutput input) : SV_Target {
    const uint64_t record = pc.data + kConstantsBytes + uint64_t(input.visible) * kBlockBytes;
    const uint64_t region = vk::RawBufferLoad<uint64_t>(pc.data, 8) + vk::RawBufferLoad<uint64_t>(record, 8);
    const uint block = vk::RawBufferLoad<uint>(record + 8, 4);
    const uint lightmap = vk::RawBufferLoad<uint>(record + 12, 4);

    const float2 s = input.sample;
    const uint2 t = uint2(clamp(floor(s), 0.0, 15.0));
    const float2 f = saturate(s - float2(t));
    const uint64_t tileRecord = region + kTileOffset + uint64_t(block * 256 + t.y * 16 + t.x) * kTileStride;
    const uint count = vk::RawBufferLoad<uint>(tileRecord, 4);

    // Tile coordinates: a sample index x 0.25 (a texture spans 4 tiles), times the scale. The
    // gradients are taken here, outside the per-pixel loop.
    const float2 uv = s * 0.25;
    const float2 dx = ddx(uv), dy = ddy(uv);
    float3 color = 0.0;
    for (uint i = 0; i < count; ++i) {
        const uint layer = vk::RawBufferLoad<uint>(tileRecord + 4 + i * 4, 4);
        const uint mask = layer >> 8;
        const float alpha = lerp(lerp(float(mask & 1), float((mask >> 1) & 1), f.x), lerp(float((mask >> 2) & 1), float((mask >> 3) & 1), f.x), f.y);
        const uint2 entry = vk::RawBufferLoad<uint2>(region + (layer & 0xFF) * 8, 4);
        const float scale = ScaleFactor(entry.y);
        const float3 texel = gTextures[NonUniformResourceIndex(entry.x)].SampleGrad(gSamplers[0], uv * scale, dx * scale, dy * scale).rgb;
        color = lerp(color, saturate(texel * input.lit * 2.0), alpha);
    }

    // The effects' lights (V2): over the lit ground.
    color += color * FrameEffectLight(pc.frame, input.world, normalize(input.normal), input.position.xy) * 1.5;
    // The Modern weather (0 with the Original's): snow lying on what faces up, as a near-white texel would be lit; wet
    // ground darker, the sky mirrored at a glancing look and the light's glint on what is flat (no normal maps: the
    // ground's own normal).
    const float4 weather = FrameValue(pc.frame, kFrameWeather);
    if (weather.x > 0.0 || weather.y > 0.0) {
        const float3 n = normalize(input.normal);
        const float flat = saturate(n.y * 2.0 - 1.0);
        if (weather.y > 0.0) {
            // At most two thirds, the ground's own pattern kept under it (its brightness carried into the snow).
            const float lying = weather.y * saturate(n.y * 2.5 - 1.4) * 0.65;
            const float pattern = dot(color, float3(0.299, 0.587, 0.114)) / max(dot(input.lit * 2.0, float3(0.299, 0.587, 0.114)), 1e-3);
            color = lerp(color, saturate(float3(0.92, 0.94, 0.98) * input.lit * 2.0 * lerp(0.7, 1.1, saturate(pattern * 1.5))), lying);
        }
        if (weather.x > 0.0) {
            color *= lerp(1.0, 0.68, weather.x);
            const float3 v = normalize(FrameValue(pc.frame, kFrameCamera).xyz - input.world);
            const float glancing = pow(1.0 - saturate(dot(n, v)), 4.0);
            const float4 horizon = FrameValue(pc.frame, kFrameSkyHorizon);
            const float3 sky = FrameValue(pc.frame, kFrameSkyZenith).w > 0.5 ? horizon.rgb : FrameValue(pc.frame, kFrameFogColor).rgb;
            color = lerp(color, sky * 0.8, weather.x * flat * glancing * 0.45);
            const float4 sun = FrameValue(pc.frame, kFrameSun);
            const float3 h = normalize(normalize(FrameValue(pc.frame, kFrameShadowLight).xyz + float3(0.0, 1e-4, 0.0)) + v);
            color += FrameValue(pc.frame, kFrameSunColour).rgb * pow(saturate(dot(n, h)), 90.0) * weather.x * flat * 0.5 * max(sun.w, 0.25);
        }
    }
    // The sun's shadows (G1): what the lightmap does not already darken, darkened by 45 %.
    if (FrameValue(pc.frame, kFrameShadowParams).x > 0.5) {
        color *= lerp(0.55, 1.0, FrameShadow(pc.frame, input.world, normalize(input.normal), input.viewDepth));
    }
    color = FrameTerrainFog(pc.frame, color, input.viewDepth);

    // The lightmap: the region's 96 tiles across, half a texel in (a 512 map); its pass is fogged too.
    const float4 lift = FrameValue(pc.frame, kFrameLift);
    const float2 at = (float2(block % 6, block / 6) * 16.0 + s) / 96.0 * 0.998 + 1.0 / 1024.0;
    const float2 atDx = ddx(at), atDy = ddy(at);
    if (lightmap != 0 && lift.w > 0.5) {
        // With the sun's cascaded shadows the lightmap's own shadows would fall twice: only its broad
        // light is kept (a low mip, about 60 units a texel), the sharp baked shadows left out.
        const Texture2D map = gTextures[NonUniformResourceIndex(lightmap)];
        const float3 baked = FrameValue(pc.frame, kFrameShadowParams).x > 0.5 ? map.SampleLevel(gSamplers[1], at, 4.0).rgb
                                                                               : map.SampleGrad(gSamplers[1], at, atDx, atDy).rgb;
        color *= FrameTerrainFog(pc.frame, saturate(baked + lift.rgb), input.viewDepth);
    }
    return float4(color, 1.0);
}

// ---- Blob shadows ("Blob shadows"): per shadow an 8 x 8 grid of tiles from the
// square's south-west tile, those inside the square drawn again at the terrain's own vertices
// (the same arithmetic as VSMain, so they lie on it) with the shadow texture across the square.
// The frame's data: u64 pool, u32 count, u32 texture, f32 alpha (32 bytes); the region table
// (32 x 32 u64: offset in the pool + 1, 0 none; (x + 16) + (z + 16) * 32 in regions from the
// origin); the shadows (float4: centre, side).
static const uint kShadowConstantsBytes = 32;
static const uint kRegionTableSide = 32;

struct ShadowOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

ShadowOutput VSShadow(uint vertexId : SV_VertexID) {
    const uint shadow = vertexId / 384;
    const uint local = vertexId % 384;
    const uint tile = local / 6;
    const uint64_t shadows = pc.data + kShadowConstantsBytes + kRegionTableSide * kRegionTableSide * 8;
    const float4 s = vk::RawBufferLoad<float4>(shadows + uint64_t(shadow) * 16, 16);
    const float halfSide = s.w * 0.5;
    const int2 first = int2(floor((s.xz - halfSide) / 20.0));
    const int2 last = int2(floor((s.xz + halfSide) / 20.0));
    const int2 t = first + int2(tile % 8, tile / 8);

    ShadowOutput output;
    output.position = float4(0.0, 0.0, 0.0, 1.0); // a tile outside the square: degenerate
    output.uv = 0.0;
    if (any(t > last)) return output;
    static const uint kMatch[6] = {0, 2, 3, 0, 3, 1};
    static const uint kOther[6] = {0, 2, 1, 2, 3, 1};
    const uint corner = (t.x & 1) == (t.y & 1) ? kMatch[local % 6] : kOther[local % 6];
    const int2 g = t + int2(corner & 1, corner >> 1); // the at, counted from the origin
    const int2 r = int2(floor(float2(g) / 96.0));      // its region
    const int2 inRegion = g - r * 96;                  // 0..95
    const int2 cell = r + int2(kRegionTableSide / 2, kRegionTableSide / 2);
    if (any(cell < 0) || any(cell >= int(kRegionTableSide))) return output;
    const uint64_t entry = vk::RawBufferLoad<uint64_t>(pc.data + kShadowConstantsBytes + uint64_t(cell.y * kRegionTableSide + cell.x) * 8, 8);
    if (entry == 0) return output;
    const uint64_t region = vk::RawBufferLoad<uint64_t>(pc.data, 8) + entry - 1; // (ShadowConstants.pool)
    const uint2 b = uint2(inRegion) / 16;
    const uint2 at = uint2(inRegion) - b * 16;
    const uint block = b.y * 6 + b.x;
    const float height = vk::RawBufferLoad<float>(region + kSampleOffset + uint64_t(block * 289 + at.y * 17 + at.x) * 8, 4);

    const float3 offset = float3(float(r.x) * 1920.0, 0.0, float(r.y) * 1920.0);
    const float3 world = offset + float3(float(b.x) * 320.0 + float(at.x) * 20.0, height, float(b.y) * 320.0 + float(at.y) * 20.0);
    output.position = mul(pc.viewProj, float4(world, 1.0));
    output.uv = (world.xz - (s.xz - halfSide)) / s.w;
    return output;
}

float4 PSShadow(ShadowOutput input) : SV_Target {
    const uint texture = vk::RawBufferLoad<uint>(pc.data + 12, 4);
    const float alpha = vk::RawBufferLoad<float>(pc.data + 16, 4);
    // The original: texture x texture factor (black, alpha 0xA8), blended by its alpha.
    const float a = gTextures[NonUniformResourceIndex(texture)].Sample(gSamplers[1], input.uv).a * alpha;
    return float4(0.0, 0.0, 0.0, a);
}

// ---- Water ("Water"): a quad per visible block with water, at its height. The data:
// u64 pool, u32 water frame, u32 ice (16 bytes); the blocks (32 bytes: u64 region offset, u32
// block, u32 type, float3 region offset, float height).
struct WaterOutput {
    float4 position : SV_Position;
    float2 local : TEXCOORD0; // in height samples across the block (0..16)
    float viewDepth : DEPTH;
    nointerpolation uint visible : BLOCK;
};

WaterOutput VSWater(uint vertexId : SV_VertexID) {
    const uint visible = vertexId / 6;
    static const float2 kCorners[6] = {float2(0, 0), float2(0, 1), float2(1, 1), float2(0, 0), float2(1, 1), float2(1, 0)};
    const float2 c = kCorners[vertexId % 6];
    const uint64_t record = pc.data + 16 + uint64_t(visible) * 32;
    const uint block = vk::RawBufferLoad<uint>(record + 8, 4);
    const float3 offset = vk::RawBufferLoad<float3>(record + 16, 4);
    const float height = vk::RawBufferLoad<float>(record + 28, 4);
    const float3 world = offset + float3((float(block % 6) + c.x) * 320.0, height, (float(block / 6) + c.y) * 320.0);
    WaterOutput output;
    output.position = mul(pc.viewProj, float4(world, 1.0));
    output.local = c * 16.0;
    output.viewDepth = output.position.w;
    output.visible = visible;
    return output;
}

// The shore alpha at a local: the depth below the surface in 2-unit steps, 0..15.
float WaterLevel(uint64_t samples, uint2 at, float surface) {
    const float h = vk::RawBufferLoad<float>(samples + uint64_t(at.y * 17 + at.x) * 8, 4);
    return clamp(trunc((surface - h) * 0.5), 0.0, 15.0);
}

float4 PSWater(WaterOutput input) : SV_Target {
    const uint64_t record = pc.data + 16 + uint64_t(input.visible) * 32;
    const uint block = vk::RawBufferLoad<uint>(record + 8, 4);
    const uint type = vk::RawBufferLoad<uint>(record + 12, 4);
    const float surface = vk::RawBufferLoad<float>(record + 28, 4);
    // The texture repeats every 80 units (4 tiles), aligned to the block.
    const float2 uv = input.local * 0.25;
    const float2 dx = ddx(uv), dy = ddy(uv);
    if (type == 1) {
        // Ice: untinted, a constant 180 / 255.
        const uint ice = vk::RawBufferLoad<uint>(pc.data + 12, 4);
        const float3 color = gTextures[NonUniformResourceIndex(ice)].SampleGrad(gSamplers[0], uv, dx, dy).rgb;
        return float4(FrameFog(pc.frame, color, input.viewDepth), 180.0 / 255.0);
    }
    const uint64_t region = vk::RawBufferLoad<uint64_t>(pc.data, 8) + vk::RawBufferLoad<uint64_t>(record, 8);
    const uint64_t samples = region + kSampleOffset + uint64_t(block) * 289 * 8;
    const float2 s = clamp(input.local, 0.0, 16.0);
    const uint2 t = uint2(min(floor(s), 15.0));
    const float2 f = s - float2(t);
    // The original's 17 x 17 mask, filtered: the levels bilinear over the block (its 4-bit form,
    // alpha = level / 15).
    const float level = lerp(lerp(WaterLevel(samples, t, surface), WaterLevel(samples, t + uint2(1, 0), surface), f.x),
                             lerp(WaterLevel(samples, t + uint2(0, 1), surface), WaterLevel(samples, t + uint2(1, 1), surface), f.x), f.y);
    const uint frame = vk::RawBufferLoad<uint>(pc.data + 8, 4);
    const float3 texel = gTextures[NonUniformResourceIndex(frame)].SampleGrad(gSamplers[0], uv, dx, dy).rgb;
    const float3 color = texel * FrameValue(pc.frame, kFrameWater).rgb;
    return float4(FrameFog(pc.frame, color, input.viewDepth), level / 15.0);
}

// ---- The Modern water (G2): on the same quads, over the terrain already drawn under it. Its depth
// below the surface (from the region's heights) colours and hides the bottom; waves from a few
// travelling directions bend the normal; the sky is reflected by Fresnel and the sun glints where
// its shadow does not fall; the shore foams where the water turns shallow.
float WaterDepthAt(uint64_t samples, float2 s, float surface) {
    const float2 c = clamp(s, 0.0, 16.0);
    const uint2 t = uint2(min(floor(c), 15.0));
    const float2 f = c - float2(t);
    const float h00 = vk::RawBufferLoad<float>(samples + uint64_t(t.y * 17 + t.x) * 8, 4);
    const float h10 = vk::RawBufferLoad<float>(samples + uint64_t(t.y * 17 + t.x + 1) * 8, 4);
    const float h01 = vk::RawBufferLoad<float>(samples + uint64_t((t.y + 1) * 17 + t.x) * 8, 4);
    const float h11 = vk::RawBufferLoad<float>(samples + uint64_t((t.y + 1) * 17 + t.x + 1) * 8, 4);
    return surface - lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
}

float3 WaveNormal(float2 p, float time) {
    // Gerstner-like slopes from five directions (world units; the longest waves 140 units).
    static const float4 kWaves[5] = {float4(0.8, 0.6, 140.0, 0.9), float4(-0.5, 0.86, 90.0, 0.7), float4(0.2, -0.98, 55.0, 0.5),
                                     float4(-0.9, -0.3, 33.0, 0.35), float4(0.6, -0.8, 19.0, 0.25)};
    float2 slope = 0.0;
    [unroll] for (uint i = 0; i < 5; ++i) {
        const float2 d = kWaves[i].xy;
        const float k = 6.2831853 / kWaves[i].z;
        const float phase = dot(d, p) * k - time * sqrt(9.8 * k * 8.0);
        slope += d * cos(phase) * kWaves[i].w * 0.09;
    }
    return normalize(float3(-slope.x, 1.0, -slope.y));
}

// Beyond this view depth the water skips its reflections and refraction (a sheen of the sky and
// its own colour) and ice is the original's: far water is small on screen, and the cost and the
// risk stay near the camera.
static const float kModernWaterReach = 3000.0;

// The long waves' height (units, about +-0.4): the shore line laps with them.
float WaveHeight(float2 p, float time) {
    static const float4 kWaves[5] = {float4(0.8, 0.6, 140.0, 0.9), float4(-0.5, 0.86, 90.0, 0.7), float4(0.2, -0.98, 55.0, 0.5),
                                     float4(-0.9, -0.3, 33.0, 0.35), float4(0.6, -0.8, 19.0, 0.25)};
    float h = 0.0;
    [unroll] for (uint i = 0; i < 3; ++i) {
        const float k = 6.2831853 / kWaves[i].z;
        h += sin(dot(kWaves[i].xy, p) * k - time * sqrt(9.8 * k * 8.0)) * kWaves[i].w;
    }
    return h * 0.2;
}



struct WaterModernOutput {
    float4 position : SV_Position;
    float2 local : TEXCOORD0;
    float viewDepth : DEPTH;
    float3 world : WORLDPOS;
    nointerpolation uint visible : BLOCK;
};

WaterModernOutput VSWaterModern(uint vertexId : SV_VertexID) {
    const WaterOutput w = VSWater(vertexId);
    const uint64_t record = pc.data + 16 + uint64_t(w.visible) * 32;
    const uint block = vk::RawBufferLoad<uint>(record + 8, 4);
    const float3 offset = vk::RawBufferLoad<float3>(record + 16, 4);
    const float height = vk::RawBufferLoad<float>(record + 28, 4);
    WaterModernOutput output;
    output.position = w.position;
    output.local = w.local;
    output.viewDepth = w.viewDepth;
    output.world = offset + float3(float(block % 6) * 320.0 + w.local.x * 20.0, height, float(block / 6) * 320.0 + w.local.y * 20.0);
    output.visible = w.visible;
    return output;
}

// The modern ice (frozen water): a still, glossy sheet over deep blue ice; what lies under it
// shows faintly through, the old frame's frost on its surface, the sky and the scene mirrored.
float4 ModernIce(WaterModernOutput input, float depth) {
    const float3 eye = FrameValue(pc.frame, kFrameCamera).xyz;
    const float3 toEye = normalize(eye - input.world);
    const uint ice = vk::RawBufferLoad<uint>(pc.data + 12, 4);
    const float2 uv = input.local * 0.25;
    const float3 frost = gTextures[NonUniformResourceIndex(ice)].SampleGrad(gSamplers[0], uv, ddx(uv), ddy(uv)).rgb;
    // The sheet: almost flat, slow frozen swells, no motion.
    float scale;
    const float3 swell = WaterDetail(input.world.xz * 0.15, 0.0, scale);
    const float3 n = normalize(float3(-swell.x * 0.06 * scale, 1.0, -swell.y * 0.06 * scale));
    const float4 sun = FrameValue(pc.frame, kFrameSun);
    const float3 sunColour = FrameValue(pc.frame, kFrameSunColour).rgb;
    const float shadow = FrameShadow(pc.frame, input.world, float3(0, 1, 0), input.viewDepth);
    const float3 r = reflect(-toEye, n);
    const float4 zenith = FrameValue(pc.frame, kFrameSkyZenith);
    const float3 horizonSky = zenith.w > 0.5 ? FrameValue(pc.frame, kFrameSkyHorizon).rgb : FrameValue(pc.frame, kFrameFogColor).rgb;
    const float3 sky = lerp(horizonSky, zenith.w > 0.5 ? zenith.rgb : horizonSky, saturate(r.y * 1.6));
    const float fresnel = 0.03 + 0.97 * pow(1.0 - saturate(dot(n, toEye)), 5.0);
    // The ice's own colour: light lost through it, red first (deep ice is blue); the sky's light
    // scattered in it.
    const float thickness = max(depth, 2.0);
    const float3 extinction = exp2(-thickness * float3(0.030, 0.011, 0.006));
    const float3 scattered = (horizonSky * 0.45 + sunColour * 0.12 * sun.w * lerp(0.4, 1.0, shadow)) * float3(0.30, 0.55, 0.75);
    float3 below = scattered;
    float3 mirrored = sky;
    // Near: what lies under it through the ice and the scene mirrored, fading out toward
    // kModernWaterReach (no line where they stop); far: its own colour and the sky.
    const float near = saturate((kModernWaterReach - input.viewDepth) / 600.0);
    if (near > 0.0) {
        const float4 under = FrameWaterBelow(pc.frame, input.position.xy, n.xz * 0.01, input.viewDepth);
        if (under.w > 0.0) {
            below = lerp(scattered, under.rgb * extinction + scattered * (1.0 - extinction), near);
            // Frost blurs the mirror: a coarse march (12 steps) is enough, and none where the
            // Fresnel term leaves next to nothing of it.
            if (fresnel * near > 0.03) mirrored = lerp(sky, FrameWaterReflect(pc.frame, input.world, r, sky, 12, 1.45), near);
        }
    }
    // The frost: the original's ice texture as white veins and haze on the surface.
    const float haze = saturate(dot(frost, 0.333) * 1.4 - 0.25);
    float3 color = lerp(below, frost * lerp(0.7, 1.0, shadow) + scattered * 0.3, haze * 0.3);
    color = lerp(color, mirrored, fresnel * 0.7);
    if (sun.w > 0.0) {
        const float3 h = normalize(sun.xyz + toEye);
        color += sunColour * pow(saturate(dot(n, h)), 400.0) * 3.0 * sun.w * shadow;
    }
    return float4(FrameFog(pc.frame, color, input.viewDepth), 1.0);
}

float4 PSWaterModern(WaterModernOutput input) : SV_Target {
    const uint64_t record = pc.data + 16 + uint64_t(input.visible) * 32;
    const uint block = vk::RawBufferLoad<uint>(record + 8, 4);
    const uint type = vk::RawBufferLoad<uint>(record + 12, 4);
    const float surface = vk::RawBufferLoad<float>(record + 28, 4);
    const uint64_t region = vk::RawBufferLoad<uint64_t>(pc.data, 8) + vk::RawBufferLoad<uint64_t>(record, 8);
    const uint64_t samples = region + kSampleOffset + uint64_t(block) * 289 * 8;
    const float time = FrameValue(pc.frame, kFrameTime).x;
    // The shore laps a little with the long waves.
    const float rawDepth = WaterDepthAt(samples, input.local, surface) + WaveHeight(input.world.xz, time);
    const float depth = max(rawDepth, 0.0); // dry where <= 0: discarded at the end
    const float3 eye = FrameValue(pc.frame, kFrameCamera).xyz;
    const float3 toEye = normalize(eye - input.world);
    // The surface: the long waves (soft swells) and the detail on them; the detail fades where a
    // pixel is larger than its ripples, and the surface turns less glossy with it (far water a
    // soft sheen, not a shimmer).
    const float calm = saturate(input.viewDepth / 1500.0);
    const float3 waves = WaveNormal(input.world.xz, time);
    float detailScale;
    const float3 detail = WaterDetail(input.world.xz, time, detailScale);
    const float3 rippled = normalize(float3(-detail.x * 0.35, 1.0, -detail.y * 0.35));
    const float3 up = normalize(lerp(waves, float3(0, 1, 0), calm * 0.7));
    const float3 n = normalize(lerp(up, normalize(up + rippled - float3(0, 1, 0)), detailScale));
    const float gloss = 0.985 * lerp(0.75, 1.0, detailScale);

    // The light: the sun (and its shadow), the sky.
    const float4 sun = FrameValue(pc.frame, kFrameSun);
    const float3 sunColour = FrameValue(pc.frame, kFrameSunColour).rgb;
    const float shadow = FrameShadow(pc.frame, input.world, float3(0, 1, 0), input.viewDepth);
    const float3 r = reflect(-toEye, n);
    const float4 zenith = FrameValue(pc.frame, kFrameSkyZenith);
    const float3 horizonSky = zenith.w > 0.5 ? FrameValue(pc.frame, kFrameSkyHorizon).rgb : FrameValue(pc.frame, kFrameFogColor).rgb;
    const float3 sky = lerp(horizonSky, zenith.w > 0.5 ? zenith.rgb : horizonSky, saturate(r.y * 1.6));
    const float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(n, toEye)), 5.0);

    // In the water: light lost with depth, red first (clear shallows, blue-green depths), and the
    // water's own light scattered back in from the sky and the sun; the environment's water colour
    // tints it (night, weather).
    const float3 tint = FrameValue(pc.frame, kFrameWater).rgb;
    const float3 extinction = exp2(-depth * 2.0 * float3(0.040, 0.016, 0.009));
    const float murk = 1.0 - extinction.g; // how little of the bottom shows
    const float3 scattered = (horizonSky * 0.35 + sunColour * 0.12 * sun.w * lerp(0.4, 1.0, shadow)) * float3(0.10, 0.32, 0.38) * tint * 1.4;
    const float3 body = scattered;
    float3 reflected = sky;
    float3 bottom = body;
    bool opaque = false;
    const float4 copyInfo = FrameValue(pc.frame, kFrameWaterCopy);
    if (copyInfo.y > 0.5 && input.viewDepth <= kModernWaterReach) {
        // The scene under and around the water, copied before it (rgb, a the view depth).
        const Texture2D copy = gTextures[NonUniformResourceIndex(uint(copyInfo.x))];
        uint width, height;
        copy.GetDimensions(width, height);
        const float2 screen = input.position.xy / float2(width, height);
        // Refraction: the bottom through the bent surface (unbent where something stands in front).
        const float2 bent = screen + (n.xz - up.xz) * 0.04 * saturate(depth / 8.0);
        float4 under = copy.SampleLevel(gSamplers[1], bent, 0);
        if (under.a < input.viewDepth) under = copy.SampleLevel(gSamplers[1], screen, 0);
        // The bottom, lit by caustics where the sun reaches it through shallow water, then dimmed
        // by the water over it and filled with its scattered light.
        const float caustics = WaterCaustics(input.world.xz, time) * shadow * sun.w * saturate(1.0 - depth / 30.0);
        const float3 ground = under.rgb * (1.0 + caustics * 0.9);
        bottom = ground * extinction + scattered * (1.0 - extinction);
        opaque = true;
        // Looking down the water mirrors little: the sky's colour is enough there.
        reflected = fresnel > 0.05 ? FrameWaterReflect(pc.frame, input.world, r, sky) : sky;
    }
    float3 color = lerp(bottom, reflected, fresnel);
    // The sun's highlight: glossy near, broader and softer far away.
    if (sun.w > 0.0) {
        const float3 h = normalize(sun.xyz + toEye);
        const float ndh = saturate(dot(n, h));
        const float alpha = max(1.0 - gloss, 0.02);
        const float a2 = alpha * alpha;
        const float denominator = ndh * ndh * (a2 - 1.0) + 1.0;
        const float highlight = a2 / (3.14159 * denominator * denominator) * 0.25;
        color += sunColour * min(highlight, 40.0) * sun.w * shadow * fresnel * 4.0;
    }
    // Foam along the shore: a band where the water is shallow, broken by the waves.
    const float band = saturate(1.0 - depth / 5.0);
    const float breakup = 0.5 + 0.5 * sin(dot(input.world.xz, float2(0.21, 0.17)) + time * 1.7) * sin(dot(input.world.xz, float2(-0.13, 0.29)) - time * 1.3);
    const float foam = band * band * smoothstep(0.25, 0.75, breakup + band * 0.5);
    color = lerp(color, float3(0.85, 0.9, 0.92) * lerp(0.6, 1.0, shadow), foam * 0.8);

    // How much of the bottom shows: the shallows clear, deep water opaque; the edge fades in.
    const float alpha = opaque ? 1.0 : saturate(max(lerp(0.35, 1.0, murk), fresnel) + foam) * saturate(depth / 1.5);
    if (rawDepth <= 0.0) discard;
    return float4(FrameFog(pc.frame, color, input.viewDepth), alpha);
}

// The modern ice blocks (drawn on their own, after the water blocks), its thickness from the
// heights; far away the same ice without what lies under it and the mirror (ModernIce).
float4 PSIceModern(WaterModernOutput input) : SV_Target {
    const uint64_t record = pc.data + 16 + uint64_t(input.visible) * 32;
    const uint block = vk::RawBufferLoad<uint>(record + 8, 4);
    const float surface = vk::RawBufferLoad<float>(record + 28, 4);
    const uint64_t region = vk::RawBufferLoad<uint64_t>(pc.data, 8) + vk::RawBufferLoad<uint64_t>(record, 8);
    const uint64_t samples = region + kSampleOffset + uint64_t(block) * 289 * 8;
    const float depth = WaterDepthAt(samples, input.local, surface);
    const float4 colour = ModernIce(input, max(depth, 0.0));
    if (depth <= 0.0) discard;
    return colour;
}
