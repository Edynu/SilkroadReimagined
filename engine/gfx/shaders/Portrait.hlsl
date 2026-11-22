// A character's portrait: one posed model drawn into a small target of its
// own under the original's portrait light. Vertices pulled by address from the geometry pool, the
// instance record and the palette from the frame's transient memory (gfx::PortraitPass).

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

struct PushData {
    float4x4 viewProj;   // GLM column-major
    uint64_t geometry;   // the geometry pool
    uint64_t instance;   // one 64-byte instance record (rows of the world matrix, then the palette offset)
    uint64_t palette;    // skinning matrices, 64 bytes each (GLM column-major)
    uint vertexOffset;   // the part's vertices, in bytes into the pool
    uint vertexFlags;    // 1: a lightmap uv follows the uv; 2: bones and weights follow; 4: do not skin
    uint texture;        // bindless index
    uint material;       // gfx::kMaterial* (1 alpha test, 16 blended)
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

    if ((pc.vertexFlags & 6) == 2) {
        const uint64_t skin = v + 32 + ((pc.vertexFlags & 1) ? 8 : 0);
        const uint bones = vk::RawBufferLoad<uint>(skin, 4);
        const float2 weights = vk::RawBufferLoad<float2>(skin + 4, 4);
        const uint paletteOffset = vk::RawBufferLoad<uint>(pc.instance + 48, 4);
        const uint64_t b0 = pc.palette + uint64_t(paletteOffset + (bones & 0xFF)) * 64;
        const uint64_t b1 = pc.palette + uint64_t(paletteOffset + ((bones >> 8) & 0xFF)) * 64;
        position = Transform(b0, position, 1.0) * weights.x + Transform(b1, position, 1.0) * weights.y;
        normal = Transform(b0, normal, 0.0) * weights.x + Transform(b1, normal, 0.0) * weights.y;
    }

    // gfx::kMaterialOverSkin: a worn piece over the skin, pushed out along its normals by kOverSkinLift.
    if ((pc.material & (1u << 25)) != 0 && dot(normal, normal) > 0.0) position += normalize(normal) * 0.06;

    const float4 r0 = vk::RawBufferLoad<float4>(pc.instance, 16);
    const float4 r1 = vk::RawBufferLoad<float4>(pc.instance + 16, 16);
    const float4 r2 = vk::RawBufferLoad<float4>(pc.instance + 32, 16);
    const float3 world = float3(dot(r0.xyz, position) + r0.w, dot(r1.xyz, position) + r1.w, dot(r2.xyz, position) + r2.w);
    VSOutput output;
    output.position = mul(pc.viewProj, float4(world, 1.0));
    output.normal = float3(dot(r0.xyz, normal), dot(r1.xyz, normal), dot(r2.xyz, normal));
    output.uv = uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    const float4 albedo = gTextures[NonUniformResourceIndex(pc.texture)].Sample(gSamplers[0], input.uv);
    // The original draws with blending off: translucent parts are cut as the
    // alpha-tested ones are.
    if ((pc.material & 17) != 0 && albedo.a < ((pc.material & 128) != 0 ? 1.0 / 255.0 : 0.5)) discard; // kMaterialAlphaZero: alpha 0 only
    // The portrait's light: one directional light shining along +z (from the camera's
    // side onto the face), diffuse 0.7 and ambient 0.3; the material white, as the world's objects
    // are until the object renderer's materials are read.
    const float3 towardLight = float3(0.0, 0.0, -1.0);
    const float lambert = saturate(dot(normalize(input.normal), towardLight));
    return float4(saturate(albedo.rgb * (0.3 + 0.7 * lambert)), 1.0);
}
