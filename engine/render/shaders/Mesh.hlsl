// Mesh shading for the world, characters and tools: one texture, the frame's light and fog
// (Frame.hlsli), alpha test. Matrices are column-major (GLM layout).
// VSMain draws static meshes instanced: each instance's world matrix (float4x4) is read
// through a buffer device address. VSSkinned draws two-bone skinned meshes one at a time:
// the address holds the model's world matrix, then its bone palette.

#include "Frame.hlsli"

struct PushData {
    float4x4 viewProj;
    uint64_t frame;       // FrameConstants (0: the built-in defaults)
    float swayHeight;     // foliage: the model's height (0: it does not sway)
    float pad;
    uint textureIndex;
    uint mode;            // 0 textured, 1 normals, 2 untextured, 3 UVs; | 0x100: additive (fog fades it out)
    float alphaCutoff;    // < 0 disables the alpha test
    uint tint;            // RGBA8 (r low): a material modifier's colour; its alpha for blended parts
    uint64_t dataAddress; // static: the instances' world matrices; skinned: world, then the bone palette
    float4 uvTransform;   // uv * xy + zw: flip-books and scrolling textures
};
[[vk::push_constant]] PushData pc;


struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 world : TEXCOORD1;
    nointerpolation uint skinned : TEXCOORD2;
};

// Foliage in the wind (frame shading: y the strength, z the seconds): the higher up the
// model, the further it moves, in slow gusts that differ from tree to tree.
float3 Sway(float3 local, float3 origin) {
    if (pc.swayHeight <= 0.0 || pc.frame == 0) return 0.0;
    const float4 shading = FrameVec(pc.frame, kFrameShading);
    if (shading.y <= 0.0) return 0.0;
    const float h = saturate(local.y / pc.swayHeight);
    const float t = shading.z;
    const float phase = dot(origin.xz, float2(0.013, 0.017));
    const float gust = 0.6 + 0.4 * sin(t * 0.31 + phase * 0.5);
    const float reach = min(pc.swayHeight * 0.03, 3.0) * shading.y * gust * h * h;
    float3 offset = float3(sin(t * 1.7 + phase + local.x * 0.05), 0.0, cos(t * 1.3 + phase * 1.3 + local.z * 0.05) * 0.6) * reach;
    // Grass and bushes (not trees) bend away from the player walking through them.
    const float4 player = FrameVec(pc.frame, kFramePlayer);
    if (player.w > 0.0 && pc.swayHeight < 60.0) {
        const float2 away = origin.xz + local.xz - player.xz;
        const float distance = length(away);
        const float push = saturate(1.0 - distance / player.w) * h;
        offset.xz += (away / max(distance, 0.001)) * push * min(pc.swayHeight * 0.5, 10.0);
        offset.y -= push * min(pc.swayHeight * 0.2, 4.0);
    }
    return offset;
}

// GLM column-major matrices: four float4 columns. float4x4(c0..c3) takes rows, so the
// loaded matrix is the transpose and is applied as mul(rowVector, m).
float4x4 LoadMatrix(uint index) {
    const uint64_t base = pc.dataAddress + uint64_t(index) * 64;
    return float4x4(vk::RawBufferLoad<float4>(base, 16), vk::RawBufferLoad<float4>(base + 16, 16),
                    vk::RawBufferLoad<float4>(base + 32, 16), vk::RawBufferLoad<float4>(base + 48, 16));
}

VSOutput VSMain(VSInput input, uint instance : SV_InstanceID) {
    const float4x4 world = LoadMatrix(instance);
    precise float4 position = mul(float4(input.position, 1.0), world);
    position.xyz += Sway(input.position, world[3].xyz);
    VSOutput output;
    output.position = mul(pc.viewProj, position);
    output.normal = mul(float4(input.normal, 0.0), world).xyz;
    output.uv = input.uv * pc.uvTransform.xy + pc.uvTransform.zw;
    output.world = position.xyz;
    output.skinned = 0;
    return output;
}

struct VSSkinnedInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
    [[vk::location(3)]] uint4 bones : BLENDINDICES;
    [[vk::location(4)]] float2 weights : BLENDWEIGHT;
};

VSOutput VSSkinned(VSSkinnedInput input) {
    const float4x4 world = LoadMatrix(0);
    const float4x4 b0 = LoadMatrix(input.bones.x + 1);
    const float4x4 b1 = LoadMatrix(input.bones.y + 1);
    const float4 p = float4(input.position, 1.0);
    const float4 n = float4(input.normal, 0.0);
    precise const float4 position = mul(float4((mul(p, b0) * input.weights.x + mul(p, b1) * input.weights.y).xyz, 1.0), world);
    const float3 normal = (mul(n, b0) * input.weights.x + mul(n, b1) * input.weights.y).xyz;

    VSOutput output;
    output.position = mul(pc.viewProj, position);
    output.normal = mul(float4(normal, 0.0), world).xyz;
    output.uv = input.uv * pc.uvTransform.xy + pc.uvTransform.zw;
    output.world = position.xyz;
    output.skinned = 1;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    float3 n = normalize(input.normal);
    const uint mode = pc.mode & 0xFF;
    if (mode == 1) {
        return float4(n * 0.5 + 0.5, 1.0);
    }
    if (mode == 3) {
        return float4(frac(input.uv), 0.0, 1.0);
    }

    float4 albedo = float4(0.8, 0.8, 0.8, 1.0);
    if (mode == 0) {
        // LinearWrap: the original UVs tile.
        albedo = gTextures[NonUniformResourceIndex(pc.textureIndex)].Sample(gSamplers[0], input.uv);
        if (pc.alphaCutoff >= 0.0 && albedo.a < pc.alphaCutoff) {
            discard;
        }
    }
    // Before environments, both sides are lit: many original materials (leaves, cloth) are
    // drawn from both sides.
    const float shadow = FrameShadow(pc.frame, input.world, n);
    const float ao = FrameAo(pc.frame, input.position.xy);
    const float3 sun = input.skinned != 0 && FrameModernCharacters(pc.frame) ? FrameCharacterLight(pc.frame, n, input.world, shadow, ao)
                                                                            : FrameLight(pc.frame, n, true, shadow, ao);
    const float3 light = sun + FrameLocalLights(pc.frame, input.world, n, input.position.xy, true);
    const float4 tint = float4(pc.tint & 0xFF, (pc.tint >> 8) & 0xFF, (pc.tint >> 16) & 0xFF, pc.tint >> 24) / 255.0;
    float3 color = albedo.rgb * light * tint.rgb;
    if (input.skinned == 0) color = FrameWet(pc.frame, color, n, input.world, shadow);
    const float fog = FrameFog(pc.frame, input.world);
    // An additive part fades into the fog; everything else takes the fog's colour.
    color = (pc.mode & 0x100) != 0 ? color * (1.0 - fog) : lerp(color, FrameFogColor(pc.frame), fog);
    // Opaque parts keep the target opaque (tint alpha 255); blended ones carry their alpha.
    return float4(color, (pc.tint >> 24) == 255 ? 1.0 : albedo.a * tint.a);
}

// Shadow maps: depth only; alpha-tested parts (leaves, fences) cut their holes.
void PSShadow(VSOutput input) {
    if ((pc.mode & 0xFF) == 0 && pc.alphaCutoff >= 0.0) {
        const float alpha = gTextures[NonUniformResourceIndex(pc.textureIndex)].Sample(gSamplers[0], input.uv).a;
        if (alpha < pc.alphaCutoff) discard;
    }
}
