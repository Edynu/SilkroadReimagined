// The Modern look's full-screen passes over the HDR scene (G0 / G3 /
// G4), by mode: ambient occlusion from the depth (half resolution) and its blur, sun shafts
// (quarter resolution), the bloom's chain down and up, the final step (tone mapping, the
// brightness, grading, what the others made) into the 8-bit picture, and FXAA after it. The
// Classic look runs the final step alone: a clamp and the brightness.

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4]; // LinearWrap, LinearClamp, PointWrap, PointClamp

struct PushData {
    uint64_t constants; // ModernConstants (ModernPass.cpp)
    uint mode;
    uint pad;
};
[[vk::push_constant]] PushData pc;

static const uint kModeOcclusion = 0, kModeOcclusionBlur = 1, kModeShafts = 2, kModeBloomFirst = 3, kModeBloomDown = 4, kModeBloomUp = 5,
                  kModeFinal = 6, kModeFxaa = 7, kModeWaterCopy = 8;

// ModernConstants: uint textures[8]; float4 params[8]; float4x4 invViewProj; float4x4 viewProj.
uint TextureAt(uint i) { return vk::RawBufferLoad<uint>(pc.constants + i * 4, 4); }
float4 Param(uint i) { return vk::RawBufferLoad<float4>(pc.constants + 32 + i * 16, 16); }
float4x4 Matrix(uint offset) {
    // GLM column-major: four columns.
    const float4 c0 = vk::RawBufferLoad<float4>(pc.constants + offset, 16), c1 = vk::RawBufferLoad<float4>(pc.constants + offset + 16, 16);
    const float4 c2 = vk::RawBufferLoad<float4>(pc.constants + offset + 32, 16), c3 = vk::RawBufferLoad<float4>(pc.constants + offset + 48, 16);
    return float4x4(c0, c1, c2, c3); // rows are GLM's columns: mul(v, m) applies it
}
float4x4 InvViewProj() { return Matrix(160); }
float4x4 ViewProj() { return Matrix(224); }

float4 Sample(uint texture, float2 uv) { return gTextures[NonUniformResourceIndex(texture)].SampleLevel(gSamplers[1], uv, 0); }
float Depth(float2 uv) { return gTextures[NonUniformResourceIndex(TextureAt(1))].SampleLevel(gSamplers[3], uv, 0).r; }

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VSOutput output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

float3 WorldAt(float2 uv, float depth) {
    const float4 p = mul(float4(uv * 2.0 - 1.0, depth, 1.0), InvViewProj());
    return p.xyz / p.w;
}

// --- Ambient occlusion: eight taps in a disc about the point's normal, those in front of the
// depth there occlude, fading with distance (params[0]: radius, strength, size of a texel x, y).
float Occlusion(float2 uv) {
    const float depth = Depth(uv);
    if (depth >= 0.99999) return 1.0;
    const float4 p0 = Param(0);
    const float3 p = WorldAt(uv, depth);
    const float3 px = WorldAt(uv + float2(p0.z, 0), Depth(uv + float2(p0.z, 0)));
    const float3 py = WorldAt(uv + float2(0, p0.w), Depth(uv + float2(0, p0.w)));
    float3 n = normalize(cross(px - p, py - p));
    const float3 eye = Param(5).xyz;
    if (dot(n, eye - p) < 0.0) n = -n;
    // A rotation per pixel (interleaved), eight directions in the tangent plane at two radii.
    const float angle = frac(sin(dot(floor(uv / p0.zw), float2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
    const float3 t = normalize(abs(n.y) < 0.99 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
    const float3 b = cross(n, t);
    float occluded = 0.0;
    [unroll] for (uint i = 0; i < 8; ++i) {
        const float a = angle + i * 0.78539816;
        const float r = p0.x * (i & 1 ? 1.0 : 0.45);
        const float3 s = p + (t * cos(a) + b * sin(a)) * r + n * (0.25 * r);
        const float4 clip = mul(float4(s, 1.0), ViewProj());
        if (clip.w <= 0.0) continue;
        const float2 suv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(suv < 0.0) || any(suv > 1.0)) continue;
        const float sceneDepth = Depth(suv);
        const float3 hit = WorldAt(suv, sceneDepth);
        const float dist = length(hit - p);
        if (clip.z / clip.w > sceneDepth + 0.00002 && dist < p0.x * 2.0) occluded += 1.0 - dist / (p0.x * 2.0);
    }
    return saturate(1.0 - occluded / 8.0 * p0.y);
}

float OcclusionBlur(float2 uv) {
    // A 4 x 4 box that skips taps across a depth edge.
    const float4 p0 = Param(0);
    const float d0 = Depth(uv);
    float sum = 0.0, weight = 0.0;
    [unroll] for (int y = -1; y <= 2; ++y) {
        [unroll] for (int x = -1; x <= 2; ++x) {
            const float2 at = uv + float2(x, y) * p0.zw * 2.0;
            const float w = abs(Depth(at) - d0) < 0.0005 ? 1.0 : 0.05;
            sum += Sample(TextureAt(2), at).r * w;
            weight += w;
        }
    }
    return sum / max(weight, 1e-4);
}

// --- Sun shafts: the sky's light along the line to the sun (params[1]: sun uv, its strength, the
// sun's colour in params[2]).
float3 Shafts(float2 uv) {
    // March from the pixel towards the sun over the sky (the depth's far plane): each sky sample
    // adds the sun's glare there (bright near the sun, nothing far from it), fading along the way;
    // what stands in front of the sun cuts the rays. params[1]: the sun's uv, strength, aspect.
    const float4 p1 = Param(1);
    const uint kSteps = 48;
    const float2 delta = (p1.xy - uv) * (0.95 / float(kSteps));
    float2 at = uv;
    float decay = 1.0, light = 0.0;
    [loop] for (uint i = 0; i < kSteps; ++i) {
        at += delta;
        if (any(at < 0.0) || any(at > 1.0)) break;
        if (Depth(at) >= 0.99999) {
            const float2 d = (at - p1.xy) * float2(p1.w, 1.0);
            const float glare = pow(saturate(1.0 - length(d) * 1.8), 3.0);
            light += glare * decay;
        }
        decay *= 0.975;
    }
    return light * (2.2 / float(kSteps)) * p1.z * Param(2).rgb;
}

// --- Bloom: down with a 13-tap filter (the first with a soft threshold), up with a tent.
float3 Down13(uint texture, float2 uv, float2 texel) {
    float3 c = Sample(texture, uv).rgb * 0.125;
    c += (Sample(texture, uv + texel * float2(-1, -1)).rgb + Sample(texture, uv + texel * float2(1, -1)).rgb +
          Sample(texture, uv + texel * float2(-1, 1)).rgb + Sample(texture, uv + texel * float2(1, 1)).rgb) * 0.125;
    c += (Sample(texture, uv + texel * float2(-2, 0)).rgb + Sample(texture, uv + texel * float2(2, 0)).rgb +
          Sample(texture, uv + texel * float2(0, -2)).rgb + Sample(texture, uv + texel * float2(0, 2)).rgb) * 0.0625;
    c += (Sample(texture, uv + texel * float2(-2, -2)).rgb + Sample(texture, uv + texel * float2(2, -2)).rgb +
          Sample(texture, uv + texel * float2(-2, 2)).rgb + Sample(texture, uv + texel * float2(2, 2)).rgb) * 0.03125;
    return c;
}
float3 SoftThreshold(float3 c, float threshold) {
    const float bright = max(c.r, max(c.g, c.b));
    const float knee = threshold * 0.5;
    float soft = clamp(bright - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    return c * max(soft, bright - threshold) / max(bright, 1e-4);
}
float3 UpTent(uint texture, float2 uv, float2 texel) {
    float3 c = Sample(texture, uv).rgb * 4.0;
    c += (Sample(texture, uv + texel * float2(-1, 0)).rgb + Sample(texture, uv + texel * float2(1, 0)).rgb +
          Sample(texture, uv + texel * float2(0, -1)).rgb + Sample(texture, uv + texel * float2(0, 1)).rgb) * 2.0;
    c += Sample(texture, uv + texel * float2(-1, -1)).rgb + Sample(texture, uv + texel * float2(1, -1)).rgb +
         Sample(texture, uv + texel * float2(-1, 1)).rgb + Sample(texture, uv + texel * float2(1, 1)).rgb;
    return c / 16.0;
}

// --- Tone mapping: ACES (Narkowicz's fit) and an AgX approximation.
float3 Aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float3 AgxContrast(float3 x) {
    const float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float3 Agx(float3 c) {
    const float3x3 into = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051, 0.0784335999999992, 0.878468636469772,
                                   0.0784336, 0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 back = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438, -0.0980208811401368, 1.15190312990417,
                                   -0.0980434501171241, -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    c = mul(c, into);
    c = clamp(log2(max(c, 1e-10)), -12.47393, 4.026069);
    c = (c + 12.47393) / 16.5;
    c = AgxContrast(c);
    return saturate(mul(c, back));
}

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

// params[3]: tone mapping (0 clamp, 1 ACES, 2 AgX), 1 / gamma, grading strength, the vignette;
// params[4]: the time of day (0..1), AO on (> 0), bloom strength, shafts on.
float3 Final(float2 uv) {
    // The original's shading writes display values (gamma 2.2); the Modern steps work on light, so
    // they take the colour back to linear first (Classic keeps its values as they are).
    const float4 p3 = Param(3), p4 = Param(4);
    const bool toLight = p3.x > 0.5;
    float3 c = max(Sample(TextureAt(0), uv).rgb, 0.0);
    if (toLight) c = pow(c, 2.2);
    if (p4.y > 0.0) c *= lerp(0.3, 1.0, Sample(TextureAt(2), uv).r);
    if (p4.z > 0.0) {
        const float3 b = max(Sample(TextureAt(3), uv).rgb, 0.0);
        c += (toLight ? pow(b, 2.2) : b) * p4.z;
    }
    if (p4.w > 0.0) {
        // The rays show in the air around what stands before the sun (over the sky), only faintly
        // over the objects themselves: light streaming through the gaps, not glare over walls.
        const float overSky = Depth(uv) >= 0.99999 ? 1.0 : 0.22;
        const float3 s = max(Sample(TextureAt(4), uv).rgb, 0.0) * overSky;
        c += toLight ? s * 0.7 : s; // light already, added as it is
    }
    // Height fog (Modern, params[6] colour and density, [7] towards the sun and the ground's
    // height): thicker low down, brighter towards the sun; the sky is left alone.
    const float4 fog = Param(6);
    if (fog.w > 0.0) {
        const float d = Depth(uv);
        if (d < 0.99999) {
            const float3 eye = Param(5).xyz, p = WorldAt(uv, d);
            const float4 p7 = Param(7);
            const float3 ray = p - eye;
            const float len = length(ray);
            // Halves over about 200 units; no thicker than 150 below the ground's height.
            const float falloff = 1.0 / 300.0;
            const float dy = (max(p.y, p7.w - 150.0) - eye.y) * falloff;
            const float along = abs(dy) > 1e-3 ? (1.0 - exp(-dy)) / dy : 1.0;
            // Nothing within 250 units: the fog lies over the distance, not around the character.
            const float amount = 1.0 - exp(-fog.w * exp(-(eye.y - p7.w) * falloff) * along * max(len - 250.0, 0.0));
            const float toward = pow(saturate(dot(ray / max(len, 1e-3), p7.xyz)), 8.0);
            float3 colour = fog.rgb + Param(2).rgb * toward * 0.6;
            if (toLight) colour = pow(max(colour, 0.0), 2.2);
            c = lerp(c, colour, saturate(amount) * 0.85);
        }
    }
    // AgX ends in display values; ACES in light (then back to gamma 2.2). The exposures keep the
    // original's middle grey where it was and roll its whites off.
    if (p3.x > 1.5) c = Agx(c * 1.2);
    else if (toLight) c = pow(Aces(c * 0.8), 1.0 / 2.2);
    else c = saturate(c);
    // Grading by the time of day: warm at sunrise and sunset, cool and a little paler at night.
    if (p3.z > 0.0) {
        const float t = p4.x;
        const float dawn = saturate(1.0 - abs(t - 0.25) * 10.0) + saturate(1.0 - abs(t - 0.75) * 10.0);
        const float night = saturate((abs(t - 0.5) - 0.27) * 8.0);
        float3 tint = lerp(float3(1, 1, 1), float3(1.08, 0.97, 0.88), dawn);
        tint = lerp(tint, float3(0.88, 0.94, 1.08), night);
        c = lerp(c, lerp(Luma(c).xxx, c, 1.0 - 0.2 * night) * tint, p3.z);
    }
    if (p3.w > 0.0) {
        const float2 d = uv - 0.5;
        c *= 1.0 - p3.w * dot(d, d) * 1.6;
    }
    return pow(saturate(c), p3.y);
}

// FXAA (a short version of Lottes' 3.11 console): along the edge, by luma.
float3 Fxaa(float2 uv) {
    const float2 texel = Param(0).zw;
    const uint t = TextureAt(0);
    const float3 m = Sample(t, uv).rgb;
    const float lm = Luma(m);
    const float ln = Luma(Sample(t, uv + float2(0, -texel.y)).rgb), ls = Luma(Sample(t, uv + float2(0, texel.y)).rgb);
    const float le = Luma(Sample(t, uv + float2(texel.x, 0)).rgb), lw = Luma(Sample(t, uv + float2(-texel.x, 0)).rgb);
    const float lo = min(lm, min(min(ln, ls), min(le, lw))), hi = max(lm, max(max(ln, ls), max(le, lw)));
    if (hi - lo < max(0.0312, hi * 0.125)) return m;
    const float lnw = Luma(Sample(t, uv + texel * float2(-1, -1)).rgb), lne = Luma(Sample(t, uv + texel * float2(1, -1)).rgb);
    const float lsw = Luma(Sample(t, uv + texel * float2(-1, 1)).rgb), lse = Luma(Sample(t, uv + texel * float2(1, 1)).rgb);
    float2 dir = float2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    const float reduce = max((lnw + lne + lsw + lse) * 0.03125, 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * texel;
    const float3 a = 0.5 * (Sample(t, uv + dir * (1.0 / 3.0 - 0.5)).rgb + Sample(t, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    const float3 b = a * 0.5 + 0.25 * (Sample(t, uv - dir * 0.5).rgb + Sample(t, uv + dir * 0.5).rgb);
    const float lb = Luma(b);
    return lb < lo || lb > hi ? a : b;
}

float4 PSMain(VSOutput input) : SV_Target {
    const float2 uv = input.uv;
    if (pc.mode == kModeOcclusion) return float4(Occlusion(uv), 0, 0, 1);
    if (pc.mode == kModeOcclusionBlur) return float4(OcclusionBlur(uv), 0, 0, 1);
    if (pc.mode == kModeShafts) return float4(Shafts(uv), 1);
    if (pc.mode == kModeBloomFirst) return float4(SoftThreshold(Down13(TextureAt(0), uv, Param(0).zw), Param(1).w), 1);
    if (pc.mode == kModeBloomDown) return float4(Down13(TextureAt(0), uv, Param(0).zw), 1);
    if (pc.mode == kModeBloomUp) return float4(UpTent(TextureAt(0), uv, Param(0).zw), 1);
    if (pc.mode == kModeFxaa) return float4(Fxaa(uv), 1);
    if (pc.mode == kModeWaterCopy) {
        // The scene's colour, and in alpha its view depth (the water's reflections march over it).
        const float d = Depth(uv);
        const float4 clip = mul(float4(WorldAt(uv, d), 1.0), ViewProj());
        return float4(Sample(TextureAt(0), uv).rgb, d >= 0.99999 ? 60000.0 : clip.w);
    }
    return float4(Final(uv), 1);
}

// A depth texture copied into the depth target (the static shadows into the live atlas).
float PSDepthCopy(VSOutput input) : SV_Depth {
    return gTextures[NonUniformResourceIndex(TextureAt(0))].SampleLevel(gSamplers[3], input.uv, 0).r;
}
