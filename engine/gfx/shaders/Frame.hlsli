// The frame's constants (gfx/FrameConstants.h), read by address.

static const uint kFrameViewProj = 0;         // float4x4 (GLM column-major)
static const uint kFrameCamera = 64;          // xyz, w time of day
static const uint kFrameTerrainLight = 80;    // towards the light
static const uint kFrameObjectLight = 96;
static const uint kFrameDiffuse = 112;
static const uint kFrameTerrainAmbient = 128;
static const uint kFrameObjectAmbient = 144;
static const uint kFrameLift = 160;           // rgb, w lightmaps on
static const uint kFrameFogColor = 176;       // rgb, w fog on
static const uint kFrameTerrainFogColor = 192;
static const uint kFrameFogRange = 208;       // start, end, 1 / (end - start)
static const uint kFrameTime = 224;           // seconds
static const uint kFrameWater = 240;          // the water colour
static const uint kFrameObjects = 256;        // fog kind (0 linear, 1 squared exponential), density
static const uint kFrameLights = 272;         // x: point lights, 64 bytes each from kFramePointLights; y the material tints' offset, z their count
static const uint kFrameShadowAtlas = 288;    // 4 x float4x4: world -> atlas u, v, depth
static const uint kFrameShadowSplits = 544;   // view depth where cascade i ends
static const uint kFrameShadowTexels = 560;   // world size of a texel of cascade i
static const uint kFrameShadowParams = 576;   // cascades (0: none), atlas, strength, filter
static const uint kFrameShadowLight = 592;    // towards the light; w 1: objects are lit from it
static const uint kFrameSkyZenith = 608;      // rgb; w 1 with a sky
static const uint kFrameSkyHorizon = 624;
static const uint kFrameSun = 640;            // towards the sun; w its strength
static const uint kFrameSunColour = 656;
static const uint kFrameModern = 672;         // x character lighting, y foliage sway, z wet ground
static const uint kFrameWaterCopy = 688;      // x the scene's copy (rgb colour, a view depth), y 1 with one
static const uint kFrameLightTiles = 704;     // tiles across, tile size, the masks' offset, tiles down
static const uint kFrameWeather = 720;        // Modern: x wetness, y snow lying, z rain now (0..1)
static const uint kFrameMetal = 736;          // the metal sheen: x spheremap_gray, y spheremap_highlight, z 0 off, 1 original, 2 modern
static const uint kFrameMetalColour = 752;    // its colour (the environment's diffuse)
static const uint kFrameCameraRight = 768;    // the camera's right
static const uint kFrameCameraUp = 784;       // and up
static const uint kFramePointLights = 800;    // position, diffuse, ambient, attenuation (a0, a1, a2)

float4 FrameValue(uint64_t frame, uint offset) { return vk::RawBufferLoad<float4>(frame + offset, 16); }

// A gather's depth tests (1 lit): x the left bottom texel, y the right bottom, z the right top, w the
// left top.
float4 ShadowTests(Texture2D map, float2 uv, float depth) {
    const float4 d = map.GatherRed(gSamplers[3], uv);
    return float4(depth <= d.x, depth <= d.y, depth <= d.z, depth <= d.w);
}

// The sun's shadow at `world` (1 lit, 0 in shadow) from the cascaded maps (G1): the cascade by view
// depth, the point pushed out along the normal by a texel and a half; faded out over the last
// tenth of the reach. Each texel's test is weighted by where the point lies among the texels, so
// an edge is a gradient, not a staircase of texels: one gather, bilinear (Low, and the far
// cascades at Medium and High), or four, a tent three texels wide (the near two at Medium and High,
// all of them at Ultra). Needs gTextures and gSamplers declared before this file.
float FrameShadow(uint64_t frame, float3 world, float3 n, float viewDepth) {
    const float4 params = FrameValue(frame, kFrameShadowParams);
    const uint count = uint(params.x);
    if (count == 0) return 1.0;
    const float4 splits = FrameValue(frame, kFrameShadowSplits);
    uint c = 0;
    [unroll] for (uint i = 0; i < 3; ++i) {
        if (i + 1 < count && viewDepth > splits[i]) c = i + 1;
    }
    const float reach = splits[count - 1];
    if (viewDepth >= reach) return 1.0;
    const float fade = saturate((reach - viewDepth) / (reach * 0.1));
    // Turned away from the light: in its own shadow, no lookup.
    if (dot(n, FrameValue(frame, kFrameShadowLight).xyz) < -0.02) return lerp(1.0, 1.0 - params.z, fade);
    const float3 p = world + n * FrameValue(frame, kFrameShadowTexels)[c] * 1.5;
    const uint m = kFrameShadowAtlas + c * 64;
    const float4x4 atlas = float4x4(FrameValue(frame, m), FrameValue(frame, m + 16), FrameValue(frame, m + 32), FrameValue(frame, m + 48));
    const float4 at = mul(float4(p, 1.0), atlas); // GLM column-major: mul(v, m)
    const float3 uvz = at.xyz / at.w;
    const Texture2D map = gTextures[NonUniformResourceIndex(uint(params.y))];
    uint width, height;
    map.GetDimensions(width, height);
    const float2 texel = 1.0 / float2(width, height);
    // Each cascade's square: the gathers stay inside it.
    const float2 square = float2(c % 2, c / 2) * 0.5;
    const float2 lo = square + texel * 1.5, hi = square + 0.5 - texel * 1.5;
    // Where the point lies among the texels: `base` the texel centre left of and above it, `f` how
    // far toward the next.
    const float2 position = uvz.xy * float2(width, height) - 0.5;
    const float2 base = floor(position), f = position - base;
    float lit;
    if (params.w < 0.5 || (c >= 2 && params.w < 2.5)) {
        // Bilinear: the four texels around it (one gather between base and base + 1).
        const float4 t = ShadowTests(map, clamp((base + 1.0) * texel, lo, hi), uvz.z);
        lit = lerp(lerp(t.w, t.z, f.x), lerp(t.x, t.y, f.x), f.y);
    } else {
        // A tent over the 4 x 4 texels base - 1 .. base + 2: columns (and rows) weighted 1 - f, 1,
        // 1, f; four gathers of 2 x 2 (sums to 9).
        lit = 0.0;
        [unroll] for (uint k = 0; k < 4; ++k) {
            const float2 block = float2(k & 1, k >> 1); // 0: texels -1 and 0; 1: texels 1 and 2
            const float4 t = ShadowTests(map, clamp((base + block * 2.0) * texel, lo, hi), uvz.z);
            const float left = block.x > 0.5 ? 1.0 : 1.0 - f.x, right = block.x > 0.5 ? f.x : 1.0;
            const float top = block.y > 0.5 ? 1.0 : 1.0 - f.y, bottom = block.y > 0.5 ? f.y : 1.0;
            lit += t.w * left * top + t.z * right * top + t.x * left * bottom + t.y * right * bottom;
        }
        lit /= 9.0;
    }
    return lerp(1.0, lerp(1.0, lit, params.z), fade);
}

// The effects' lights (V2, the Modern row): each a sphere of light fading to its radius, softly
// wrapped about the normal; none casts a shadow. They follow the point lights.
float3 FrameEffectLight(uint64_t frame, float3 world, float3 n, float2 pixel) {
    const uint count = uint(FrameValue(frame, kFrameWaterCopy).z);
    if (count == 0) return 0.0;
    const uint first = kFramePointLights + uint(FrameValue(frame, kFrameLights).x) * 64;
    // Only the lights whose reach covers this pixel's screen tile (binned on the CPU).
    const float4 tiles = FrameValue(frame, kFrameLightTiles);
    const uint2 tile = uint2(pixel / tiles.y);
    // Never outside the grid the CPU binned (a pass drawn at another size would read past the masks), nor a light past
    // the count (a stray bit would read past the lights): either would fault the device.
    if (tiles.y < 1.0 || tile.x >= uint(tiles.x) || tile.y >= uint(tiles.w)) return 0.0;
    // 128 lights a tile: four words of bits (SceneRenderer's kLightMaskWords).
    const uint4 mask = vk::RawBufferLoad<uint4>(frame + uint(tiles.z) + (tile.y * uint(tiles.x) + tile.x) * 16, 16);
    // The brightest light at the point (per channel), not their sum: a heap of drops, each with its glow's light, would
    // add up to white over the ground and their names (the user's report 2026-11-21); one light shines as it did.
    float3 sum = 0.0;
    // Bounded, without `continue`. (The Donwhang cave's hang was a `discard` before this loop in the object shaders,
    // World.hlsl's FadeCut note.)
    [loop] for (uint word = 0; word < 4; ++word) {
        uint bits = mask[word];
        [loop] for (uint step = 0; step < 32; ++step) {
            if (bits == 0) break;
            const uint i = word * 32 + firstbitlow(bits);
            bits &= bits - 1;
            if (i < count) {
                const float4 at = FrameValue(frame, first + i * 32);
                const float3 d = at.xyz - world;
                const float dist2 = dot(d, d);
                if (dist2 < at.w * at.w) {
                    const float dist = sqrt(dist2);
                    const float fall = 1.0 - dist / at.w;
                    const float wrap = dist > 1e-3 ? saturate(dot(n, d / dist) * 0.6 + 0.4) : 1.0;
                    sum = max(sum, FrameValue(frame, first + i * 32 + 16).rgb * fall * fall * wrap);
                }
            }
        }
    }
    return sum;
}

// The effects' lights mirrored along `r` (the Modern metal sheen's glints): each light of this pixel's tile that reaches
// the point, a sharp highlight where the reflection points at it; the brightest, as FrameEffectLight.
float3 FrameEffectGlint(uint64_t frame, float3 world, float3 r, float2 pixel) {
    const uint count = uint(FrameValue(frame, kFrameWaterCopy).z);
    if (count == 0) return 0.0;
    const uint first = kFramePointLights + uint(FrameValue(frame, kFrameLights).x) * 64;
    const float4 tiles = FrameValue(frame, kFrameLightTiles);
    const uint2 tile = uint2(pixel / tiles.y);
    if (tiles.y < 1.0 || tile.x >= uint(tiles.x) || tile.y >= uint(tiles.w)) return 0.0;
    const uint4 mask = vk::RawBufferLoad<uint4>(frame + uint(tiles.z) + (tile.y * uint(tiles.x) + tile.x) * 16, 16);
    float3 glint = 0.0;
    [loop] for (uint word = 0; word < 4; ++word) {
        uint bits = mask[word];
        [loop] for (uint step = 0; step < 32; ++step) {
            if (bits == 0) break;
            const uint i = word * 32 + firstbitlow(bits);
            bits &= bits - 1;
            if (i < count) {
                const float4 at = FrameValue(frame, first + i * 32);
                const float3 d = at.xyz - world;
                const float dist2 = dot(d, d);
                if (dist2 < at.w * at.w && dist2 > 1e-6) {
                    const float dist = sqrt(dist2);
                    const float fall = 1.0 - dist / at.w;
                    const float facing = saturate(dot(r, d / dist));
                    glint = max(glint, FrameValue(frame, first + i * 32 + 16).rgb * fall * (pow(facing, 40.0) * 1.5 + pow(facing, 6.0) * 0.12));
                }
            }
        }
    }
    return glint;
}

// Towards the light objects are lit from: the shadows' when they are on (so the lit side and the
// shadow agree), else the original's fixed direction.
float3 FrameObjectLight(uint64_t frame) {
    const float4 shadow = FrameValue(frame, kFrameShadowLight);
    return shadow.w > 0.5 ? shadow.xyz : FrameValue(frame, kFrameObjectLight).xyz;
}

// The original's linear fog by view depth: 1 = no fog, 0 = all fog.
float FrameFogKeep(uint64_t frame, float viewDepth) {
    if (FrameValue(frame, kFrameFogColor).w < 0.5) return 1.0;
    const float4 range = FrameValue(frame, kFrameFogRange);
    return saturate((range.y - viewDepth) * range.z);
}

// Fogged in the fog colour of everything but the near terrain.
float3 FrameFog(uint64_t frame, float3 color, float viewDepth) {
    return lerp(FrameValue(frame, kFrameFogColor).rgb, color, FrameFogKeep(frame, viewDepth));
}

// The fog lit objects take: the linear one outdoors; in dungeons the original's vertex shader
// fog 2^-(depth x density)^2 (its "exp" instruction is a power of two).
float3 FrameObjectFog(uint64_t frame, float3 color, float viewDepth) {
    const float4 objects = FrameValue(frame, kFrameObjects);
    if (objects.x < 0.5) return FrameFog(frame, color, viewDepth);
    const float scaled = viewDepth * objects.y;
    return lerp(FrameValue(frame, kFrameFogColor).rgb, color, saturate(exp2(-scaled * scaled)));
}

// Fogged in the near terrain's fog colour.
float3 FrameTerrainFog(uint64_t frame, float3 color, float viewDepth) {
    return lerp(FrameValue(frame, kFrameTerrainFogColor).rgb, color, FrameFogKeep(frame, viewDepth));
}

// Ripples (the modern water's small waves; procedural: sampling the water's frame texture here
// lost the device under heavy streaming). A soft ridged noise over space and time: gradient noise in four
// octaves (each turned and twice as fine), then its distance from the middle, so ripples change
// shape where they are instead of sliding past; octaves smaller than a few pixels are left out
// (no shimmer, no grid far away). The slope (height per unit) comes from the height by differences.
uint RippleHash(uint3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16;
    return v.x + v.y + v.z;
}
// Gradient noise over space and time (-1..1 roughly): no square cells as a value noise has.
float3 RippleGradient(uint3 c) {
    const uint h = RippleHash(c);
    // Twelve directions on the cube's edges.
    static const float3 kGradients[12] = {float3(1, 1, 0), float3(-1, 1, 0), float3(1, -1, 0), float3(-1, -1, 0),
                                          float3(1, 0, 1), float3(-1, 0, 1), float3(1, 0, -1), float3(-1, 0, -1),
                                          float3(0, 1, 1), float3(0, -1, 1), float3(0, 1, -1), float3(0, -1, -1)};
    return kGradients[h % 12];
}
float GradientNoise(float3 p) {
    const float3 i = floor(p), f = frac(p);
    const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    const uint3 c = uint3(int3(i) + 32768);
    float corner[8];
    [unroll] for (uint k = 0; k < 8; ++k) {
        const float3 o = float3(k & 1, (k >> 1) & 1, k >> 2);
        corner[k] = dot(RippleGradient(c + uint3(o)), f - o);
    }
    const float x00 = lerp(corner[0], corner[1], u.x), x10 = lerp(corner[2], corner[3], u.x);
    const float x01 = lerp(corner[4], corner[5], u.x), x11 = lerp(corner[6], corner[7], u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}
// Value noise and its slope: (d/dx, d/dy, value 0..1).
float3 NoiseSlopeValue(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 t = f * f * (3.0 - 2.0 * f), dt = 6.0 * f * (1.0 - f);
    const uint3 c = uint3(int3(int2(i), 0) + 32768);
    const float a = float(RippleHash(c) & 0xFFFF) / 65535.0, b = float(RippleHash(c + uint3(1, 0, 0)) & 0xFFFF) / 65535.0;
    const float d = float(RippleHash(c + uint3(0, 1, 0)) & 0xFFFF) / 65535.0, e = float(RippleHash(c + uint3(1, 1, 0)) & 0xFFFF) / 65535.0;
    const float k = a - b - d + e;
    return float3(((b - a) + k * t.y) * dt.x, ((d - a) + k * t.x) * dt.y, a + (b - a) * t.x + (d - a) * t.y + k * t.x * t.y);
}

// The water's detail: four octaves of noise, each twice as fine and each pushed along by the slope
// of the ones before (organic shapes, no grid), drifting slowly in turning directions (still water,
// no current). Returns the slope (x, z) and the height; `scale` (0..1) how much of it a pixel can
// hold (1 near, 0 far: a pixel larger than the ripples sees a flat surface).
float3 WaterDetail(float2 p, float time, out float scale) {
    const float2 q0 = p / 7.0;
    const float2 width = max(abs(ddx(q0)), abs(ddy(q0)));
    const float w = max(width.x, width.y);
    scale = 1.0 / (1.0 + w * w * 60.0);
    float2 q = q0 + float2(1.7, 9.2);
    float2 drift = float2(0.11, 0.07) * time;
    float3 sum = 0.0;
    float total = 0.0, weight = 1.0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        q += drift;
        drift = float2(-drift.y, drift.x) * 0.8; // each octave drifts another way
        const float3 v = NoiseSlopeValue(q);
        sum += v * weight;
        q += v.xy * 0.3; // the next octave bent by this one's slope
        q = float2(q.x * 1.6 - q.y * 1.2, q.x * 1.2 + q.y * 1.6); // twice as fine, turned
        total += weight;
        weight *= 0.7;
    }
    return sum / total;
}

// The ripples' slope (the pools use it): the detail at full strength where a pixel holds it.
float2 RippleSlope(float2 p, float time) {
    float scale;
    const float3 d = WaterDetail(p, time, scale);
    return d.xy * 0.35 * scale;
}

// Caustics on what lies under shallow water: bright lines where the waves focus the sun, from two
// layers of noise's zero crossings drifting apart (0 none .. about 2).
float WaterCaustics(float2 p, float time) {
    float c = 0.0;
    [unroll] for (uint i = 0; i < 2; ++i) {
        const float s = 1.0 / (7.0 + float(i) * 4.0);
        const float n = GradientNoise(float3(p.x * s + float(i) * 3.1, p.y * s - float(i) * 1.7, time * (0.35 + 0.15 * float(i))));
        c += pow(saturate(1.0 - abs(n) * 5.0), 5.0);
    }
    return c;
}

// The modern water's reflection (pixel shaders): the reflected ray from `world` along `r` marched
// over the scene's copy made before the water (rgb colour, a view depth) in growing steps; the
// first point behind what the copy saw (within a thickness) is what the water mirrors, faded
// towards the screen's edges where the copy runs out. `sky` where nothing is met or no copy.
// `steps` and `growth`: how many steps and how much longer each is than the last (24 and 1.2 reach
// about 1,200 units; fewer, longer steps reach as far, coarser).
float3 FrameWaterReflect(uint64_t frame, float3 world, float3 r, float3 sky, uint steps, float growth) {
    const float4 info = FrameValue(frame, kFrameWaterCopy);
    if (info.y < 0.5) return sky;
    const Texture2D copy = gTextures[NonUniformResourceIndex(uint(info.x))];
    const float4x4 viewProj = float4x4(FrameValue(frame, kFrameViewProj), FrameValue(frame, kFrameViewProj + 16),
                                       FrameValue(frame, kFrameViewProj + 32), FrameValue(frame, kFrameViewProj + 48));
    float t = 3.0, before = 0.0;
    [loop] for (uint i = 0; i < steps; ++i) {
        before = t;
        t = t * growth + 3.0;
        const float4 clip = mul(float4(world + r * t, 1.0), viewProj); // GLM column-major: mul(v, m)
        if (clip.w <= 0.0) break;
        float2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(uv < 0.0) || any(uv > 1.0)) break;
        const float4 seen = copy.SampleLevel(gSamplers[3], uv, 0);
        // Behind what the copy saw, by less than the step (a thickness): the ray passed through it
        // in this step. Halved back to where it went behind (no stairs from long steps).
        if (clip.w > seen.a + 0.5 && clip.w - seen.a < (t - before) * 1.2 + 12.0) {
            float lo = before, hi = t;
            [unroll] for (uint k = 0; k < 4; ++k) {
                const float mid = 0.5 * (lo + hi);
                const float4 c = mul(float4(world + r * mid, 1.0), viewProj);
                const float2 at = c.xy / c.w * 0.5 + 0.5;
                if (c.w > copy.SampleLevel(gSamplers[3], at, 0).a + 0.5) {
                    hi = mid;
                    uv = at;
                } else {
                    lo = mid;
                }
            }
            const float2 edge = saturate(min(uv, 1.0 - uv) * 8.0);
            return lerp(sky, copy.SampleLevel(gSamplers[1], uv, 0).rgb, edge.x * edge.y);
        }
    }
    return sky;
}

float3 FrameWaterReflect(uint64_t frame, float3 world, float3 r, float3 sky) { return FrameWaterReflect(frame, world, r, sky, 24, 1.2); }

// What lies under the modern water at `pixel` seen through a surface bent by `bend` (screen uv),
// unbent where something stands in front of the water; w 0 when there is no copy.
float4 FrameWaterBelow(uint64_t frame, float2 pixel, float2 bend, float viewDepth) {
    const float4 info = FrameValue(frame, kFrameWaterCopy);
    if (info.y < 0.5) return 0.0;
    const Texture2D copy = gTextures[NonUniformResourceIndex(uint(info.x))];
    uint width, height;
    copy.GetDimensions(width, height);
    const float2 screen = pixel / float2(width, height);
    float4 under = copy.SampleLevel(gSamplers[1], screen + bend, 0);
    if (under.a < viewDepth) under = copy.SampleLevel(gSamplers[1], screen, 0);
    return float4(under.rgb, 1.0);
}
