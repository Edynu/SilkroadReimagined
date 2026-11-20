// GPU-driven draws: every draw of a bucket comes from the culling pass. The
// draw's first instance is its record's slot: (instance, mesh, skinned base); the vertex is pulled
// from the geometry pool at the mesh's offset, or from the vertex arena where the skinning pass
// wrote a skinned part, and placed by the instance's world matrix.

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

struct PushData {
    float4x4 viewProj;  // GLM column-major
    uint64_t geometry;  // the geometry pool
    uint64_t instances; // GpuScene records (64 bytes)
    uint64_t meshes;    // MeshTable records (64 bytes)
    uint64_t draws;     // the culling pass's draw records (uint4: instance, mesh, skinned base, -)
    uint64_t arena;     // skinned vertices (32 bytes: position, normal, uv)
    uint64_t frame;     // FrameConstants (Frame.hlsli)
};
[[vk::push_constant]] PushData pc;

#include "Frame.hlsli"

struct VSOutput {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float2 lightmapUv : TEXCOORD1;
    float3 world : WORLDPOS;
    float viewDepth : DEPTH;
    nointerpolation uint texture : TEXTURE;
    nointerpolation uint material : MATERIAL;
    nointerpolation uint lightmap : LIGHTMAP; // bindless index, 0: none
    nointerpolation uint light : LIGHT;       // 1 + the instance's point light, 0: none
    nointerpolation uint fade : FADE;         // 0 whole .. 255 gone (GpuScene.h kInstanceFadeShift)
    nointerpolation uint skinned : SKINNED;   // 1: a character (the Modern character lighting)
    nointerpolation uint tint : TINT;         // 1 + the frame's material tint it takes, 0: none (GpuScene.h kInstanceTintShift)
    // The plus glow (gfx::kMaterialEquipGlow): its texture (bindless, 0 none), its moved coordinates, its colour now.
    float2 glowUv : TEXCOORD2;
    nointerpolation uint glow : GLOW;
    nointerpolation float3 glowColor : GLOWCOLOR;
};

float4 UnpackRgba(uint v) {
    return float4(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24) / 255.0;
}

VSOutput VSMain(uint vertexId : SV_VertexID, [[vk::builtin("BaseInstance")]] uint drawIndex : BASEINSTANCE) {
    const uint4 draw = vk::RawBufferLoad<uint4>(pc.draws + uint64_t(drawIndex) * 16, 16);
    const uint64_t mesh = pc.meshes + uint64_t(draw.y) * 64;
    uint64_t v;
    bool lightmapped = false;
    if (draw.z != 0xFFFFFFFFu) {
        v = pc.arena + (uint64_t(draw.z) + vertexId) * 32;
    } else {
        const uint vertexOffset = vk::RawBufferLoad<uint>(mesh + 8, 4);
        const uint vertexFlags = vk::RawBufferLoad<uint>(mesh + 12, 4);
        const uint stride = 32 + ((vertexFlags & 1) ? 8 : 0) + ((vertexFlags & 2) ? 12 : 0);
        v = pc.geometry + vertexOffset + uint64_t(vertexId) * stride;
        lightmapped = (vertexFlags & 1) != 0;
    }
    const float3 position = vk::RawBufferLoad<float3>(v, 4);
    const float3 normal = vk::RawBufferLoad<float3>(v + 12, 4);

    const uint64_t record = pc.instances + uint64_t(draw.x) * 64;
    const float4 r0 = vk::RawBufferLoad<float4>(record, 16);
    const float4 r1 = vk::RawBufferLoad<float4>(record + 16, 16);
    const float4 r2 = vk::RawBufferLoad<float4>(record + 32, 16);
    float3 world = float3(dot(r0.xyz, position) + r0.w, dot(r1.xyz, position) + r1.w, dot(r2.xyz, position) + r2.w);
    // Foliage sway (Modern): plants' parts (gfx::kMaterialFoliage) lean with the wind,
    // more the higher they are above the object's origin; neighbours out of step.
    const uint material = vk::RawBufferLoad<uint>(mesh + 20, 4);
    if (pc.frame != 0 && draw.z == 0xFFFFFFFFu && (material & 8) != 0 && FrameValue(pc.frame, kFrameModern).y > 0.5) {
        const float time = FrameValue(pc.frame, kFrameTime).x;
        const float height = max(world.y - r1.w, 0.0);
        const float phase = time * 1.6 + r0.w * 0.013 + r2.w * 0.011;
        const float sway = (sin(phase) + 0.35 * sin(phase * 2.7 + world.x * 0.05)) * min(height * 0.012, 2.5);
        world.xz += float2(0.8, 0.6) * sway;
    }
    VSOutput output;
    output.position = mul(pc.viewProj, float4(world, 1.0));
    output.viewDepth = output.position.w;
    output.normal = float3(dot(r0.xyz, normal), dot(r1.xyz, normal), dot(r2.xyz, normal));
    output.uv = vk::RawBufferLoad<float2>(v + 24, 4);
    // Scrolling textures (gfx::kMaterialScrolls): (u, v, 1) times a matrix grown from
    // the identity by its rates every second; the offsets wrapped (the texture repeats).
    const bool modernPool = (material & 64) != 0 && pc.frame != 0 && FrameValue(pc.frame, kFrameWaterCopy).w > 0.5;
    if ((material & 32) != 0 && pc.frame != 0 && !modernPool) {
        const uint3 rates = vk::RawBufferLoad<uint3>(mesh + 52, 4);
        const float2 offset = f16tof32(uint2(rates.x & 0xFFFF, rates.x >> 16));
        const float2 row1 = f16tof32(uint2(rates.y & 0xFFFF, rates.y >> 16));
        const float2 row2 = f16tof32(uint2(rates.z & 0xFFFF, rates.z >> 16));
        const float t = FrameValue(pc.frame, kFrameTime).x;
        const float2 uv = output.uv;
        output.uv = float2(uv.x * (1.0 + row1.x * t) + uv.y * row2.x * t, uv.x * row1.y * t + uv.y * (1.0 + row2.y * t)) + frac(offset * t);
    }
    // The plus glow (gfx::kMaterialEquipGlow): the option texture's coordinates moved by the rates
    // every second; the colour from one end to the other over the period and back; doubled for "modulate 2x".
    output.glow = 0;
    output.glowUv = float2(0.0, 0.0);
    output.glowColor = float3(0.0, 0.0, 0.0);
    if ((material & (1u << 24)) != 0 && pc.frame != 0) {
        const uint3 words = vk::RawBufferLoad<uint3>(mesh + 52, 4);
        const float t = FrameValue(pc.frame, kFrameTime).x;
        const float2 rate = f16tof32(uint2(words.x & 0xFFFF, words.x >> 16));
        output.glowUv = output.uv + frac(rate * t);
        const float4 from = UnpackRgba(words.y), to = UnpackRgba(words.z);
        const float period = round(from.w * 255.0) * 0.02;
        float s = 0.0;
        if (period > 0.0) {
            s = frac(t / (2.0 * period)) * 2.0;
            s = s > 1.0 ? 2.0 - s : s;
        }
        output.glowColor = lerp(from.rgb, to.rgb, s) * round(to.w * 255.0);
        output.glow = vk::RawBufferLoad<uint>(mesh + 48, 4);
    }
    output.texture = vk::RawBufferLoad<uint>(mesh + 16, 4);
    output.material = vk::RawBufferLoad<uint>(mesh + 20, 4);
    output.world = world;
    output.lightmapUv = lightmapped ? vk::RawBufferLoad<float2>(v + 32, 4) : float2(0, 0);
    output.lightmap = lightmapped ? vk::RawBufferLoad<uint>(mesh + 48, 4) : 0;
    const uint flags = vk::RawBufferLoad<uint>(record + 52, 4);
    output.light = flags >> 16;
    output.fade = (flags >> 8) & 0xFF;
    output.tint = (flags >> 1) & 0x7F;
    output.skinned = draw.z != 0xFFFFFFFFu ? 1u : 0u;
    return output;
}

// A cut-out material's reference (gfx::kMaterialAlphaTest): below a half, or with kMaterialAlphaZero (an
// environment map's test, ALPHAREF 1) only where the alpha is 0.
float AlphaReference(uint material) {
    return (material & 128) != 0 ? 1.0 / 255.0 : 0.5;
}

// Fading in or out: a 4 x 4 ordered pattern leaves out that share of the pixels.
bool FadeCut(VSOutput input) {
    if (input.fade == 0) return false;
    const uint2 p = uint2(input.position.xy) & 3;
    const uint bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return (float(bayer[p.y * 4 + p.x]) + 0.5) * 16.0 < float(input.fade);
}
// The pixel shaders below decide a cut first and discard only at their end, after the lighting: a `discard` before
// the effect lights' loop hung the AMD driver (the translucent parts of the Donwhang cave lost the device every
// frame, 2026-11-13; the same shader with its discards moved ran).

// A fountain's pool in the Modern look (gfx::kMaterialWater, the Modern water row): its scrolling
// texture keeps its colour, mostly opaque, lifted a little so it reads as water in the shade; the
// surface is bent by ripples (the texture's brightness taken as a height, and a few small travelling
// waves), reflects the sky by Fresnel and glints in the sun where its shadow does not fall.
float3 ObjectWater(VSOutput input, float3 albedo, float3 color, float3 normal, float shadow) {
    const float time = FrameValue(pc.frame, kFrameTime).x;
    const float3 toEye = normalize(FrameValue(pc.frame, kFrameCamera).xyz - input.world);
    // The surface: the ripples' slope (finer than the sea's: a pool), flatter far away.
    float2 slope = RippleSlope(input.world.xz * 2.2, time * 1.3) * 1.6;
    slope *= 1.0 - saturate(input.viewDepth / 1500.0);
    const float3 up = normal.y >= 0.0 ? normal : -normal;
    const float3 n = normalize(up + float3(-slope.x, 0.0, -slope.y));

    const float4 zenith = FrameValue(pc.frame, kFrameSkyZenith);
    const float3 horizonSky = zenith.w > 0.5 ? FrameValue(pc.frame, kFrameSkyHorizon).rgb : FrameValue(pc.frame, kFrameFogColor).rgb;
    const float3 r = reflect(-toEye, n);
    const float3 sky = lerp(horizonSky, zenith.w > 0.5 ? zenith.rgb : horizonSky, saturate(r.y * 1.6));
    const float fresnel = 0.04 + 0.96 * pow(1.0 - saturate(dot(n, toEye)), 5.0);
    // The water's own colour: the pool texture's hue, darker and deeper (the texture alone reads
    // as glass), dimmer in shadow; a little light of its own.
    const float3 body = albedo * float3(0.42, 0.55, 0.6) * lerp(0.65, 1.0, shadow);
    // Through it: the basin under it (the scene's copy), bent by the ripples, sinking into the body.
    const float4 below = FrameWaterBelow(pc.frame, input.position.xy, n.xz * 0.02, input.viewDepth);
    // Caustics on the basin where the sun reaches it through the shallow water.
    const float4 sunUp = FrameValue(pc.frame, kFrameSun);
    const float caustics = WaterCaustics(input.world.xz * 2.5, time) * shadow * sunUp.w;
    const float3 bottom = below.w > 0.0 ? lerp(below.rgb * (1.0 + caustics * 0.9) * float3(0.7, 0.9, 0.95), body, 0.6) : body;
    // Over it: what it mirrors (the fountain, the buildings, the sky).
    const float3 mirrored = FrameWaterReflect(pc.frame, input.world, r, sky);
    float3 water = lerp(bottom, mirrored, saturate(0.08 + fresnel * 0.9)) + albedo * 0.05;
    const float4 sun = FrameValue(pc.frame, kFrameSun);
    if (sun.w > 0.0) {
        const float3 halfway = normalize(sun.xyz + toEye);
        const float glint = pow(saturate(dot(n, halfway)), 300.0) * 4.0 + pow(saturate(dot(n, halfway)), 48.0) * 0.12;
        water += FrameValue(pc.frame, kFrameSunColour).rgb * glint * sun.w * shadow;
    }
    return water;
}

// The lit colour of an object's pixel before its fog: the original's light 0 while objects draw, the shadows, the
// effects' and the instance's point lights, the night lights,
// lightmaps and the Modern rows.
float3 ObjectColor(VSOutput input, float3 albedo, float3 normal, out float shadow) {
    // The environment's object ambient and diffuse from a fixed direction. Materials are white
    // until the object renderer's are read.
    const float3 light = FrameObjectLight(pc.frame);
    shadow = FrameShadow(pc.frame, input.world, normal, input.viewDepth);
    // With the sun's cascaded shadows the shadow darkens the finished colour (below): taken out of
    // the light, it would be lost wherever the original's light and lightmap clip to white.
    const bool cascaded = FrameValue(pc.frame, kFrameShadowParams).x > 0.5;
    const float lightShadow = cascaded ? 1.0 : shadow;
    const float4 modern = FrameValue(pc.frame, kFrameModern);
    const float3 diffuse = FrameValue(pc.frame, kFrameDiffuse).rgb;
    float3 lit;
    if (input.skinned != 0 && modern.x > 0.5) {
        // Characters (Modern): the light wraps past the terminator, and a rim of the sky's light
        // outlines them against the scene.
        const float3 toEye = normalize(FrameValue(pc.frame, kFrameCamera).xyz - input.world);
        const float wrap = saturate((dot(normal, light) + 0.4) / 1.4);
        const float rim = pow(1.0 - saturate(dot(normal, toEye)), 3.0) * (0.35 + 0.65 * lightShadow);
        const float4 zenith = FrameValue(pc.frame, kFrameSkyZenith);
        const float3 sky = zenith.w > 0.5 ? FrameValue(pc.frame, kFrameSkyHorizon).rgb : FrameValue(pc.frame, kFrameObjectAmbient).rgb;
        lit = FrameValue(pc.frame, kFrameObjectAmbient).rgb + diffuse * wrap * lightShadow + rim * (sky * 0.35 + diffuse * 0.25);
    } else {
        lit = FrameValue(pc.frame, kFrameObjectAmbient).rgb + diffuse * saturate(dot(normal, light)) * lightShadow;
    }
    // The effects' light (Modern): added to the finished colour below, after the light's clamp and the lightmap, where
    // a torch in a lit dungeon or a glow by day would otherwise be cut off at white (the user's report, 2026-11-13).
    const float3 effectLight = FrameEffectLight(pc.frame, input.world, normal, input.position.xy);
    // The instance's point light (dungeons): its ambient, and its diffuse attenuated by distance.
    const uint lights = uint(FrameValue(pc.frame, kFrameLights).x);
    if (input.light != 0 && input.light <= lights) {
        const uint at = kFramePointLights + (input.light - 1) * 64;
        const float3 toLight = FrameValue(pc.frame, at).xyz - input.world;
        const float distance = max(length(toLight), 1e-3);
        const float3 attenuation = FrameValue(pc.frame, at + 48).xyz;
        const float falloff = 1.0 / max(dot(attenuation, float3(1.0, distance, distance * distance)), 1e-6);
        lit += FrameValue(pc.frame, at + 32).rgb + FrameValue(pc.frame, at + 16).rgb * saturate(dot(normal, toLight / distance)) * falloff;
    }
    // At night (before 0.25, after 0.75) night-light materials (the windows) are unlit: full bright.
    const float timeOfDay = FrameValue(pc.frame, kFrameCamera).w;
    const bool glowing = (input.material & 2) != 0 && (timeOfDay < 0.25 || timeOfDay > 0.75);
    if (glowing) lit = float3(1.0, 1.0, 1.0);
    // A material tint (a skill's SCT_MAT): the 1.188 model's material diffuse and ambient, so every
    // light on it times the colour.
    if (input.tint != 0 && input.tint <= uint(FrameValue(pc.frame, kFrameLights).z)) {
        lit *= FrameValue(pc.frame, uint(FrameValue(pc.frame, kFrameLights).y) + (input.tint - 1) * 16).rgb;
    }
    float3 color = albedo * saturate(lit);
    // Lightmapped meshes: lit like the rest, then times the baked light, doubled.
    if (input.lightmap != 0) {
        const float3 baked = gTextures[NonUniformResourceIndex(input.lightmap)].Sample(gSamplers[0], input.lightmapUv).rgb;
        color = saturate(color * baked * 2.0);
    }
    if (!glowing) color += albedo * effectLight;
    // The cascaded shadow over the finished colour: half the light in full shadow, the sun's side
    // only (faces turned away are dark already).
    if (cascaded && !glowing) color *= lerp(0.5, 1.0, shadow);
    // Modern: the lit windows above 1, so the bloom spills their light.
    if (glowing && modern.w > 0.5) color *= 2.2;
    // Wet ground (Modern weather): what faces up darkens under the rain.
    if (modern.z > 0.0 && input.skinned == 0) color *= lerp(1.0, 0.7, modern.z * saturate(normal.y * 2.0 - 0.6));
    return color;
}

float4 PSMain(VSOutput input) : SV_Target {
    const float4 albedo = gTextures[NonUniformResourceIndex(input.texture)].Sample(gSamplers[0], input.uv);
    const bool cut = ((input.material & 1) != 0 && albedo.a < AlphaReference(input.material)) || FadeCut(input);
    const float3 normal = normalize(input.normal);
    float shadow;
    float3 color = ObjectColor(input, albedo.rgb, normal, shadow);
    if (cut) discard;
    if ((input.material & 64) != 0 && FrameValue(pc.frame, kFrameWaterCopy).w > 0.5) {
        color = ObjectWater(input, albedo.rgb, color, normal, shadow);
    }
    // The plus glow: stage 0 the option texture times its colour, stage 1 the item's texture added;
    // neither takes the vertex light, so the glowing item is its texture, unlit, with the glow on it. Each fixed-function
    // stage's result is clamped to 0..1, as Direct3D 9's: unclamped, a +9 weapon went far past white and the bloom
    // whitened it (the user's report, 2026-11-13).
    if (input.glow != 0) {
        const float3 stage0 = saturate(gTextures[NonUniformResourceIndex(input.glow)].Sample(gSamplers[0], input.glowUv).rgb * input.glowColor);
        color = saturate(albedo.rgb + stage0);
    }
    return float4(FrameObjectFog(pc.frame, color, input.viewDepth), 1.0);
}

// How much of an object's colour its fog keeps (FrameObjectFog's blend).
float ObjectFogKeep(float viewDepth) {
    const float4 objects = FrameValue(pc.frame, kFrameObjects);
    if (objects.x < 0.5) return FrameFogKeep(pc.frame, viewDepth);
    const float scaled = viewDepth * objects.y;
    return saturate(exp2(-scaled * scaled));
}

// Translucent parts (gfx::kMaterialBlended: waterfalls, glows), drawn after the solid ones without
// depth writes, into a premultiplied blend (one, one minus source alpha): the part's own source
// factor is applied here and its destination factor becomes the alpha written. The texture's alpha
// is the source alpha (the original's alpha stage takes the texture's); the lit texture is tinted by
// the material's key colour and its colour stage (gfx::kMaterialTintShift). One-sided parts drop
// their back faces (the pipeline draws both).
float4 PSBlended(VSOutput input, bool front : SV_IsFrontFace) : SV_Target {
    const bool backCut = !front && (input.material & 4) == 0;
    // The modern water's pools (culled into this list when it is on, after the scene's copy): opaque.
    if ((input.material & 64) != 0 && FrameValue(pc.frame, kFrameWaterCopy).w > 0.5) {
        const float4 albedo = gTextures[NonUniformResourceIndex(input.texture)].Sample(gSamplers[0], input.uv);
        const float3 normal = normalize(input.normal);
        float shadow;
        const float3 lit = ObjectColor(input, albedo.rgb, normal, shadow);
        const float3 water = ObjectWater(input, albedo.rgb, lit, normal, shadow);
        if (backCut) return float4(0.0, 0.0, 0.0, 0.0); // nothing added, what is under it kept (one, one minus source alpha)
        return float4(FrameObjectFog(pc.frame, water, input.viewDepth), 1.0);
    }
    const float4 albedo = gTextures[NonUniformResourceIndex(input.texture)].Sample(gSamplers[0], input.uv);
    const bool cut = backCut || ((input.material & 1) != 0 && albedo.a < AlphaReference(input.material)) || FadeCut(input);
    const float3 normal = normalize(front ? input.normal : -input.normal);
    float shadow;
    float3 color = ObjectColor(input, albedo.rgb, normal, shadow);
    // No discard in this shader: the blend is one, one minus source alpha, so a cut pixel (colour 0, alpha 0) adds
    // nothing and keeps what is under it (it writes no depth either).
    if (cut) return float4(0.0, 0.0, 0.0, 0.0);
    // A glowing item's translucent part: the glow as on its solid ones (PSMain).
    if (input.glow != 0) {
        color = albedo.rgb + gTextures[NonUniformResourceIndex(input.glow)].Sample(gSamplers[0], input.glowUv).rgb * input.glowColor;
    }
    color *= float((input.material >> 16) & 0xFF) / 100.0;
    const uint source = (input.material >> 8) & 15, destination = (input.material >> 12) & 15;
    const float a = albedo.a;
    // D3DBLEND: 1 zero, 2 one, 3 source colour, 4 its inverse, 5 source alpha, 6 its inverse;
    // 14 / 15 the blend factor (white: one / zero).
    const bool additive = destination == 2 || destination == 14;
    if (additive) {
        color *= ObjectFogKeep(input.viewDepth); // fog fades what is added
    } else {
        color = FrameObjectFog(pc.frame, color, input.viewDepth);
    }
    float3 rgb;
    if (source == 1) rgb = 0.0;
    else if (source == 2 || source == 14) rgb = color;
    else if (source == 3) rgb = color * color;
    else if (source == 4) rgb = color * (1.0 - color);
    else if (source == 6) rgb = color * (1.0 - a);
    else rgb = color * a;
    float alpha;
    if (additive) alpha = 0.0;
    else if (destination == 1 || destination == 15) alpha = 1.0;
    else if (destination == 5) alpha = 1.0 - a;
    else alpha = a;
    return float4(rgb, alpha);
}

// The sun's shadow maps (G1): depth only; cut-out materials and fading instances leave their holes.
struct DepthOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint texture : TEXTURE;
    nointerpolation uint material : MATERIAL;
    nointerpolation uint fade : FADE;
};

DepthOutput VSDepth(uint vertexId : SV_VertexID, [[vk::builtin("BaseInstance")]] uint drawIndex : BASEINSTANCE) {
    const VSOutput full = VSMain(vertexId, drawIndex);
    DepthOutput output;
    output.position = full.position;
    output.uv = full.uv;
    output.texture = full.texture;
    output.material = full.material;
    output.fade = full.fade;
    return output;
}

void PSDepth(DepthOutput input) {
    if ((input.material & 1) != 0) {
        const float alpha = gTextures[NonUniformResourceIndex(input.texture)].Sample(gSamplers[0], input.uv).a;
        if (alpha < AlphaReference(input.material)) discard;
    }
    if (input.fade != 0) {
        const uint2 p = uint2(input.position.xy) & 3;
        const uint bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        if ((float(bayer[p.y * 4 + p.x]) + 0.5) * 16.0 < float(input.fade)) discard;
    }
}

// Solid shadow casters: the position only, no fragment shader (the GPU's early depth test kept).
float4 VSDepthOnly(uint vertexId : SV_VertexID, [[vk::builtin("BaseInstance")]] uint drawIndex : BASEINSTANCE) : SV_Position {
    return VSMain(vertexId, drawIndex).position;
}
