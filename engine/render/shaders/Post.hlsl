// Full-screen passes over a finished image: one triangle covering the screen (no vertex
// buffer), sources read through the bindless array (LinearClamp unless noted). What each
// pass reads from the push constants is written at its entry point.

struct PushData {
    uint4 tex;     // texture indices
    float4 p0;
    float4 p1;
    float4 p2;
    float4 p3;
    uint64_t data; // a buffer address (SSAO: the camera; flare visibility: the points)
    uint2 pad;
};
[[vk::push_constant]] PushData pc;

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

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

float4 Tex(uint index, float2 uv) { return gTextures[NonUniformResourceIndex(index)].SampleLevel(gSamplers[1], uv, 0); }
float4 TexPoint(uint index, float2 uv) { return gTextures[NonUniformResourceIndex(index)].SampleLevel(gSamplers[3], uv, 0); }
float Luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// FXAA: tex.x the image, p0.xy 1 / its size.
float3 Fetch(float2 uv) {
    // An HDR scene is shown clamped (as the original's 8-bit target).
    return saturate(Tex(pc.tex.x, uv).rgb);
}
float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float LumaAt(float2 uv) { return Luma(Fetch(uv)); }

// FXAA (after Lottes' FXAA 3.11, quality 12): skip low-contrast pixels, pick the edge's
// direction, walk both ways along it to its ends, and blend across the edge by how far the
// pixel is from the nearer end; small details also get a sub-pixel blend.
float4 PSFxaa(VSOutput i) : SV_Target {
    const float2 uv = i.uv, r = pc.p0.xy;
    const float3 rgbM = Fetch(uv);
    const float M = Luma(rgbM);
    const float N = LumaAt(uv + float2(0, -r.y)), S = LumaAt(uv + float2(0, r.y));
    const float W = LumaAt(uv + float2(-r.x, 0)), E = LumaAt(uv + float2(r.x, 0));
    const float maxL = max(M, max(max(N, S), max(E, W)));
    const float minL = min(M, min(min(N, S), min(E, W)));
    const float range = maxL - minL;
    if (range < max(0.0312, maxL * 0.125)) return float4(rgbM, 1.0);

    const float NW = LumaAt(uv + float2(-r.x, -r.y)), NE = LumaAt(uv + float2(r.x, -r.y));
    const float SW = LumaAt(uv + float2(-r.x, r.y)), SE = LumaAt(uv + float2(r.x, r.y));
    const float edgeH = abs(-2.0 * W + NW + SW) + abs(-2.0 * M + N + S) * 2.0 + abs(-2.0 * E + NE + SE);
    const float edgeV = abs(-2.0 * N + NW + NE) + abs(-2.0 * M + W + E) * 2.0 + abs(-2.0 * S + SW + SE);
    const bool horizontal = edgeH >= edgeV;

    const float luma1 = horizontal ? N : W, luma2 = horizontal ? S : E;
    const float grad1 = luma1 - M, grad2 = luma2 - M;
    const bool steep1 = abs(grad1) >= abs(grad2);
    const float gradScaled = 0.25 * max(abs(grad1), abs(grad2));
    float stepLength = horizontal ? r.y : r.x;
    float localAverage;
    if (steep1) {
        stepLength = -stepLength;
        localAverage = 0.5 * (luma1 + M);
    } else {
        localAverage = 0.5 * (luma2 + M);
    }
    float2 current = uv;
    if (horizontal) current.y += stepLength * 0.5;
    else current.x += stepLength * 0.5;

    const float2 offset = horizontal ? float2(r.x, 0) : float2(0, r.y);
    float2 uv1 = current - offset, uv2 = current + offset;
    float end1 = LumaAt(uv1) - localAverage, end2 = LumaAt(uv2) - localAverage;
    bool reached1 = abs(end1) >= gradScaled, reached2 = abs(end2) >= gradScaled;
    const float kQuality[12] = {1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0};
    [unroll] for (int k = 0; k < 12; ++k) {
        if (reached1 && reached2) break;
        if (!reached1) {
            uv1 -= offset * kQuality[k];
            end1 = LumaAt(uv1) - localAverage;
            reached1 = abs(end1) >= gradScaled;
        }
        if (!reached2) {
            uv2 += offset * kQuality[k];
            end2 = LumaAt(uv2) - localAverage;
            reached2 = abs(end2) >= gradScaled;
        }
    }
    const float dist1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y;
    const float dist2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
    const bool nearer1 = dist1 < dist2;
    const float distance = min(dist1, dist2);
    const float edgeLength = dist1 + dist2;
    const float pixelOffset = -distance / edgeLength + 0.5;
    const bool centreSmaller = M < localAverage;
    const bool correct = ((nearer1 ? end1 : end2) < 0.0) != centreSmaller;
    float finalOffset = correct ? pixelOffset : 0.0;

    const float average = (2.0 * (N + S + E + W) + NW + NE + SW + SE) / 12.0;
    const float sub1 = saturate(abs(average - M) / range);
    const float sub2 = (-2.0 * sub1 + 3.0) * sub1 * sub1;
    finalOffset = max(finalOffset, sub2 * sub2 * 0.75);

    float2 finalUv = uv;
    if (horizontal) finalUv.y += finalOffset * stepLength;
    else finalUv.x += finalOffset * stepLength;
    return float4(Fetch(finalUv), 1.0);
}

// ---- Ambient occlusion -------------------------------------------------------------------
// The camera at pc.data: view-projection and its inverse (GLM column-major), the camera's
// position and forward direction.
float4 MulCols(uint64_t m, float4 v) {
    return vk::RawBufferLoad<float4>(m, 16) * v.x + vk::RawBufferLoad<float4>(m + 16, 16) * v.y +
           vk::RawBufferLoad<float4>(m + 32, 16) * v.z + vk::RawBufferLoad<float4>(m + 48, 16) * v.w;
}
float3 WorldAt(float2 uv, float depth) {
    const float4 p = MulCols(pc.data + 64, float4(uv * 2.0 - 1.0, depth, 1.0));
    return p.xyz / p.w;
}
float ViewDepth(float3 world) {
    return dot(world - vk::RawBufferLoad<float4>(pc.data + 128, 16).xyz, vk::RawBufferLoad<float4>(pc.data + 144, 16).xyz);
}
float Noise(float2 pixel) { return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))); }

// SSAO in world space: tex.x the depth pre-pass; p0 radius, strength, samples, fade-out
// distance; p1.xy 1 / the depth's size. The normal comes from the neighbours' positions (the
// nearer side across each axis, so edges keep theirs).
float4 PSSsao(VSOutput i) : SV_Target {
    const float depth = TexPoint(pc.tex.x, i.uv).r;
    if (depth >= 1.0) return 1.0;
    const float3 p = WorldAt(i.uv, depth);
    const float2 texel = pc.p1.xy;
    const float3 pr = WorldAt(i.uv + float2(texel.x, 0), TexPoint(pc.tex.x, i.uv + float2(texel.x, 0)).r);
    const float3 pl = WorldAt(i.uv - float2(texel.x, 0), TexPoint(pc.tex.x, i.uv - float2(texel.x, 0)).r);
    const float3 pd = WorldAt(i.uv + float2(0, texel.y), TexPoint(pc.tex.x, i.uv + float2(0, texel.y)).r);
    const float3 pu = WorldAt(i.uv - float2(0, texel.y), TexPoint(pc.tex.x, i.uv - float2(0, texel.y)).r);
    const float3 dx = abs(ViewDepth(pr) - ViewDepth(p)) < abs(ViewDepth(pl) - ViewDepth(p)) ? pr - p : p - pl;
    const float3 dy = abs(ViewDepth(pd) - ViewDepth(p)) < abs(ViewDepth(pu) - ViewDepth(p)) ? pd - p : p - pu;
    float3 n = normalize(cross(dy, dx));
    const float3 camera = vk::RawBufferLoad<float4>(pc.data + 128, 16).xyz;
    if (dot(n, camera - p) < 0.0) n = -n;
    const float viewP = ViewDepth(p);
    if (viewP > pc.p0.w) return 1.0;
    // A turned basis around the normal, a different turn each pixel.
    const float angle = Noise(i.position.xy) * 6.2831853;
    const float3 helper = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(helper, n));
    float3 b = cross(n, t);
    const float c = cos(angle), s = sin(angle);
    const float3 t2 = t * c + b * s;
    b = cross(n, t2);
    t = t2;
    const uint samples = uint(pc.p0.z);
    const float radius = pc.p0.x;
    float occlusion = 0.0;
    for (uint k = 0; k < samples; ++k) {
        // Points spread over the hemisphere (a golden-angle spiral), denser near the centre.
        const float f = (float(k) + 0.5) / float(samples);
        const float phi = float(k) * 2.3999632;
        const float r = sqrt(1.0 - f);
        const float3 dir = t * (cos(phi) * r) + b * (sin(phi) * r) + n * sqrt(f);
        const float scale = lerp(0.15, 1.0, f * f);
        const float3 q = p + dir * radius * scale;
        const float4 clip = MulCols(pc.data, float4(q, 1.0));
        if (clip.w <= 0.0) continue;
        const float2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(uv < 0.0) || any(uv > 1.0)) continue;
        const float sceneDepth = TexPoint(pc.tex.x, uv).r;
        if (sceneDepth >= 1.0) continue;
        const float sceneView = ViewDepth(WorldAt(uv, sceneDepth));
        const float range = smoothstep(0.0, 1.0, radius / max(abs(viewP - sceneView), 0.001));
        occlusion += (sceneView < clip.w - radius * 0.03 ? 1.0 : 0.0) * range;
    }
    const float fade = saturate((pc.p0.w - viewP) / (pc.p0.w * 0.25));
    const float ao = 1.0 - saturate(occlusion / float(samples) * pc.p0.y) * fade;
    return float4(ao, ao, ao, 1.0);
}

// A depth-aware blur of the occlusion along one axis: tex.x the occlusion, tex.y the depth;
// p0.xy one texel of the occlusion along the axis; p0.z near, p0.w far (to compare depths).
float LinearDepth(float d) { return pc.p0.z * pc.p0.w / max(pc.p0.w - d * (pc.p0.w - pc.p0.z), 0.0001); }
float4 PSAoBlur(VSOutput i) : SV_Target {
    const float centre = LinearDepth(TexPoint(pc.tex.y, i.uv).r);
    const float weights[4] = {0.324, 0.232, 0.0855, 0.0205};
    float sum = Tex(pc.tex.x, i.uv).r * weights[0], total = weights[0];
    for (int k = 1; k < 4; ++k) {
        for (int side = -1; side <= 1; side += 2) {
            const float2 uv = i.uv + pc.p0.xy * float(k * side);
            const float d = LinearDepth(TexPoint(pc.tex.y, uv).r);
            const float w = weights[k] * saturate(1.0 - abs(d - centre) / (centre * 0.05 + 1.0));
            sum += Tex(pc.tex.x, uv).r * w;
            total += w;
        }
    }
    const float ao = sum / total;
    return float4(ao, ao, ao, 1.0);
}

// ---- The original's bloom (into two 512 x 512 8-bit targets) -----------------------------
// Down: tex.x the scene, scaled by 128/255.
float4 PSBloomDown(VSOutput i) : SV_Target { return float4(saturate(Tex(pc.tex.x, i.uv).rgb) * (128.0 / 255.0), 1.0); }

// Five taps 2.6 texels apart along p0.xy (one texel), weights 80, 70, 50 (/255); p0.z the
// bright pass subtracted from each tap (40/255 across, 0 down). Taps off the image add nothing.
float4 PSBloomBlur(VSOutput i) : SV_Target {
    const float weights[3] = {80.0 / 255.0, 70.0 / 255.0, 50.0 / 255.0};
    float3 sum = 0.0;
    for (int k = -2; k <= 2; ++k) {
        const float2 uv = i.uv + pc.p0.xy * (2.6 * float(k));
        if (any(uv < 0.0) || any(uv > 1.0)) continue;
        sum = saturate(sum + max(Tex(pc.tex.x, uv).rgb - pc.p0.z, 0.0) * weights[abs(k)]);
    }
    return float4(sum, 1.0);
}

// ---- Modern bloom: a dual-filter chain on the HDR scene ----------------------------------
// Down: tex.x the larger level, p0.xy its texel; p0.z > 0: the first step's soft threshold
// (p0.z threshold, p0.w knee).
float3 Threshold(float3 c) {
    if (pc.p0.z <= 0.0) return c;
    const float bright = max(c.r, max(c.g, c.b));
    float soft = clamp(bright - pc.p0.z + pc.p0.w, 0.0, 2.0 * pc.p0.w);
    soft = soft * soft / (4.0 * pc.p0.w + 0.0001);
    return c * max(soft, bright - pc.p0.z) / max(bright, 0.0001);
}
float4 PSDualDown(VSOutput i) : SV_Target {
    const float2 h = pc.p0.xy * 0.5;
    float3 c = Threshold(Tex(pc.tex.x, i.uv).rgb) * 4.0;
    c += Threshold(Tex(pc.tex.x, i.uv + float2(-h.x, -h.y)).rgb);
    c += Threshold(Tex(pc.tex.x, i.uv + float2(h.x, -h.y)).rgb);
    c += Threshold(Tex(pc.tex.x, i.uv + float2(-h.x, h.y)).rgb);
    c += Threshold(Tex(pc.tex.x, i.uv + float2(h.x, h.y)).rgb);
    return float4(c / 8.0, 1.0);
}
// Up: tex.x the smaller level, p0.xy its texel; added onto the larger one.
float4 PSDualUp(VSOutput i) : SV_Target {
    const float2 h = pc.p0.xy * 0.5;
    float3 c = Tex(pc.tex.x, i.uv + float2(-h.x * 2.0, 0)).rgb + Tex(pc.tex.x, i.uv + float2(h.x * 2.0, 0)).rgb;
    c += Tex(pc.tex.x, i.uv + float2(0, -h.y * 2.0)).rgb + Tex(pc.tex.x, i.uv + float2(0, h.y * 2.0)).rgb;
    c += (Tex(pc.tex.x, i.uv + float2(-h.x, -h.y)).rgb + Tex(pc.tex.x, i.uv + float2(h.x, -h.y)).rgb) * 2.0;
    c += (Tex(pc.tex.x, i.uv + float2(-h.x, h.y)).rgb + Tex(pc.tex.x, i.uv + float2(h.x, h.y)).rgb) * 2.0;
    return float4(c / 12.0, 1.0);
}

// ---- Light shafts: the bright sky smeared toward the sun ----------------------------------
// tex.x the scene, tex.y the depth pre-pass (sky = 1); p0.xy the sun on screen (uv), p0.z
// the samples, p0.w the step's decay.
float4 PSShafts(VSOutput i) : SV_Target {
    const uint samples = uint(pc.p0.z);
    const float2 step = (pc.p0.xy - i.uv) / float(samples) * 0.9;
    float2 uv = i.uv;
    float decay = 1.0;
    float3 sum = 0.0;
    for (uint k = 0; k < samples; ++k) {
        uv += step;
        if (all(uv >= 0.0) && all(uv <= 1.0) && TexPoint(pc.tex.y, uv).r >= 1.0) {
            const float3 c = Tex(pc.tex.x, uv).rgb;
            sum += c * saturate(Luminance(c) - 0.3) * decay;
        }
        decay *= pc.p0.w;
    }
    return float4(sum / float(samples), 1.0);
}

// ---- Compose: bloom and shafts in, tone mapping, colour grading --------------------------
// tex.x the scene, tex.y bloom (0 none), tex.z shafts (0 none); p0 tone mapping (0 the
// original's clamp, 1 ACES, 2 AgX), bloom kind (1 the original's 512 glow, 2 modern), exposure,
// shafts strength; p1 grading gain (rgb) and saturation; p2 lift (rgb) and contrast; p3
// vignette, modern bloom strength, 1 to grade, the volumetric clouds (bindless index as a
// float, 0 none); tex.w the volumetric fog (0 none).
float3 Aces(float3 x) { return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14)); }
float3 AgxContrast(float3 x) {
    const float3 x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}
float3 Agx(float3 c) {
    const float3x3 inset = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 outset = float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                                     -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                                     -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    float3 x = mul(c, inset);
    x = clamp(log2(max(x, 1e-10)), -12.47393, 4.026069);
    x = (x + 12.47393) / (4.026069 + 12.47393);
    x = AgxContrast(x);
    x = mul(x, outset);
    return saturate(pow(max(x, 0.0), 2.2)); // back to linear
}
float4 PSCompose(VSOutput i) : SV_Target {
    float3 c = Tex(pc.tex.x, i.uv).rgb;
    // Volumetric clouds over the sky, then the fog between the eye and everything.
    const uint clouds = uint(pc.p3.w + 0.5);
    if (clouds != 0) {
        const float4 cloud = Tex(clouds, i.uv);
        c = c * (1.0 - cloud.a) + cloud.rgb;
    }
    if (pc.tex.w != 0) {
        const float4 fog = Tex(pc.tex.w, i.uv);
        c = c * fog.a + fog.rgb;
    }
    const uint tone = uint(pc.p0.x), bloom = uint(pc.p0.y);
    if (bloom == 1 && pc.tex.y != 0) {
        // The original: the scene darkened to 198/255 under the glow at 192/255.
        c = saturate(c) * (198.0 / 255.0) + Tex(pc.tex.y, i.uv).rgb * (192.0 / 255.0);
    } else if (bloom == 2 && pc.tex.y != 0) {
        c += Tex(pc.tex.y, i.uv).rgb * pc.p3.y;
    }
    if (pc.tex.z != 0) c += Tex(pc.tex.z, i.uv).rgb * pc.p0.w;
    if (tone == 0) {
        c = saturate(c);
    } else {
        // The original's colours are display values: to light, map, and back.
        const float3 lin = pow(max(c, 0.0), 2.2) * pc.p0.z;
        c = pow(tone == 1 ? Aces(lin) : Agx(lin), 1.0 / 2.2);
    }
    if (pc.p3.z > 0.5) {
        c = saturate(pc.p2.rgb + c * pc.p1.rgb);
        c = saturate((c - 0.5) * pc.p2.a + 0.5);
        c = lerp(Luminance(c).xxx, c, pc.p1.a);
        const float2 d = i.uv - 0.5;
        c *= 1.0 - pc.p3.x * dot(d, d) * 1.6;
    }
    return float4(saturate(c), 1.0);
}

// ---- The lens flare's visibility, kept from frame to frame -------------------------------
// Into a 1 x 1 target: tex.x the depth pre-pass, tex.y last frame's visibility (0 none);
// pc.data five float4 (u, v, depth, weight; weight 0 off screen); p0.x how far to move toward
// this frame's (min(dt * 10, 1)).
float4 PSFlareVisibility(VSOutput i) : SV_Target {
    float raw = 0.0;
    for (uint k = 0; k < 5; ++k) {
        const float4 probe = vk::RawBufferLoad<float4>(pc.data + uint64_t(k) * 16, 16);
        if (probe.w <= 0.0) continue;
        if (probe.z <= TexPoint(pc.tex.x, probe.xy).r) raw += probe.w;
    }
    const float previous = pc.tex.y != 0 ? TexPoint(pc.tex.y, float2(0.5, 0.5)).r : 0.0;
    const float v = previous + pc.p0.x * (raw - previous);
    return float4(v, v, v, 1.0);
}

// ---- Copies ---------------------------------------------------------------------------------
// tex.x the source, read texel for texel.
float4 PSCopy(VSOutput i) : SV_Target { return TexPoint(pc.tex.x, i.uv); }
// A depth-only pass: tex.x a sampled depth, written as this target's depth.
float PSDepthCopy(VSOutput i) : SV_Depth { return TexPoint(pc.tex.x, i.uv).r; }
