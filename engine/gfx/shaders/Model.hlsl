// Streamed models: vertices pulled by address from the geometry pool, the
// instance's world matrix from the GPU scene, bones from the frame's palette. The pass C2's viewer
// uses until C3's renderer replaces it.

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

struct PushData {
    float4x4 viewProj;   // GLM column-major
    uint64_t geometry;   // the geometry pool
    uint64_t instances;  // GpuScene records, 64 bytes each
    uint64_t palette;    // skinning matrices, 64 bytes each (GLM column-major)
    uint vertexOffset;   // the part's vertices, in bytes into the pool
    uint vertexFlags;    // 1: a lightmap uv follows the uv; 2: bones and weights follow; 4: do not skin
    uint instance;
    uint texture;        // bindless index
    uint material;       // 1: alpha test
    uint pad;
};
[[vk::push_constant]] PushData pc;

struct VSOutput {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

// p transformed by the column-major matrix at `address` (w = 1 for points, 0 for directions).
float3 Transform(uint64_t address, float3 p, float w) {
    const float4 c0 = vk::RawBufferLoad<float4>(address, 16);
    const float4 c1 = vk::RawBufferLoad<float4>(address + 16, 16);
    const float4 c2 = vk::RawBufferLoad<float4>(address + 32, 16);
    const float4 c3 = vk::RawBufferLoad<float4>(address + 48, 16);
    return (c0 * p.x + c1 * p.y + c2 * p.z + c3 * w).xyz;
}

VSOutput VSMain(uint vertexId : SV_VertexID) {
    const uint stride = 32 + ((pc.vertexFlags & 1) ? 8 : 0) + ((pc.vertexFlags & 2) ? 12 : 0);
    const uint64_t v = pc.geometry + pc.vertexOffset + uint64_t(vertexId) * stride;
    float3 position = vk::RawBufferLoad<float3>(v, 4);
    float3 normal = vk::RawBufferLoad<float3>(v + 12, 4);
    const float2 uv = vk::RawBufferLoad<float2>(v + 24, 4);

    const uint64_t record = pc.instances + uint64_t(pc.instance) * 64;
    if ((pc.vertexFlags & 6) == 2) {
        const uint64_t skin = v + 32 + ((pc.vertexFlags & 1) ? 8 : 0);
        const uint bones = vk::RawBufferLoad<uint>(skin, 4);
        const float2 weights = vk::RawBufferLoad<float2>(skin + 4, 4);
        const uint paletteOffset = vk::RawBufferLoad<uint>(record + 48, 4);
        const uint64_t b0 = pc.palette + uint64_t(paletteOffset + (bones & 0xFF)) * 64;
        const uint64_t b1 = pc.palette + uint64_t(paletteOffset + ((bones >> 8) & 0xFF)) * 64;
        position = Transform(b0, position, 1.0) * weights.x + Transform(b1, position, 1.0) * weights.y;
        normal = Transform(b0, normal, 0.0) * weights.x + Transform(b1, normal, 0.0) * weights.y;
    }

    // gfx::kMaterialOverSkin: a worn piece over the skin, pushed out along its normals by kOverSkinLift.
    if ((pc.material & (1u << 25)) != 0 && dot(normal, normal) > 0.0) position += normalize(normal) * 0.06;

    const float4 r0 = vk::RawBufferLoad<float4>(record, 16);
    const float4 r1 = vk::RawBufferLoad<float4>(record + 16, 16);
    const float4 r2 = vk::RawBufferLoad<float4>(record + 32, 16);
    const float3 world = float3(dot(r0.xyz, position) + r0.w, dot(r1.xyz, position) + r1.w, dot(r2.xyz, position) + r2.w);
    VSOutput output;
    output.position = mul(pc.viewProj, float4(world, 1.0));
    output.normal = float3(dot(r0.xyz, normal), dot(r1.xyz, normal), dot(r2.xyz, normal));
    output.uv = uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    const float4 albedo = gTextures[NonUniformResourceIndex(pc.texture)].Sample(gSamplers[0], input.uv);
    if ((pc.material & 1) != 0 && albedo.a < ((pc.material & 128) != 0 ? 1.0 / 255.0 : 0.5)) discard; // kMaterialAlphaZero: alpha 0 only
    const float3 light = normalize(float3(0.45, 0.8, -0.35));
    const float lambert = saturate(dot(normalize(input.normal), light));
    return float4(albedo.rgb * (0.45 + 0.55 * lambert), 1.0);
}
