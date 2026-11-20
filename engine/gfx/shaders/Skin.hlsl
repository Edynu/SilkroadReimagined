// Compute skinning: one group per job the culling pass wrote (instance,
// mesh, arena base); its threads walk the part's vertices, blend each by its two bones from the
// frame's palette and write position, normal and uv (32 bytes, model space) into the vertex arena,
// where the world pass's draw reads them like a static part's.

struct PushData {
    uint64_t constants; // SkinConstants, below
};
[[vk::push_constant]] PushData pc;

// SkinConstants (in transient memory):
//   uint64 jobs       0   uint4 (instance, mesh, arena base, -)
//   uint64 meshes     8   MeshTable records (64 bytes)
//   uint64 instances  16  GpuScene records (64 bytes)
//   uint64 geometry   24  the geometry pool
//   uint64 palette    32  the frame's skinning matrices (64 bytes, GLM column-major)
//   uint64 arena      40  skinned vertices (32 bytes)
//   uint maxJobs      48

// p transformed by the column-major matrix at `address` (w = 1 for points, 0 for directions).
float3 Transform(uint64_t address, float3 p, float w) {
    const float4 c0 = vk::RawBufferLoad<float4>(address, 16);
    const float4 c1 = vk::RawBufferLoad<float4>(address + 16, 16);
    const float4 c2 = vk::RawBufferLoad<float4>(address + 32, 16);
    const float4 c3 = vk::RawBufferLoad<float4>(address + 48, 16);
    return (c0 * p.x + c1 * p.y + c2 * p.z + c3 * w).xyz;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint64_t c = pc.constants;
    if (group.x >= vk::RawBufferLoad<uint>(c + 48, 4)) return; // more jobs were counted than fit
    const uint4 job = vk::RawBufferLoad<uint4>(vk::RawBufferLoad<uint64_t>(c, 8) + uint64_t(group.x) * 16, 16);
    const uint64_t m = vk::RawBufferLoad<uint64_t>(c + 8, 8) + uint64_t(job.y) * 64;
    const uint vertexOffset = vk::RawBufferLoad<uint>(m + 8, 4);
    const uint vertexFlags = vk::RawBufferLoad<uint>(m + 12, 4);
    const uint vertexCount = vk::RawBufferLoad<uint>(m + 28, 4);
    const uint stride = 32 + ((vertexFlags & 1) ? 8 : 0) + ((vertexFlags & 2) ? 12 : 0);
    const uint skinOffset = 32 + ((vertexFlags & 1) ? 8 : 0);
    const uint paletteOffset = vk::RawBufferLoad<uint>(vk::RawBufferLoad<uint64_t>(c + 16, 8) + uint64_t(job.x) * 64 + 48, 4);
    const uint64_t palette = vk::RawBufferLoad<uint64_t>(c + 32, 8) + uint64_t(paletteOffset) * 64;
    const uint64_t source = vk::RawBufferLoad<uint64_t>(c + 24, 8) + vertexOffset;
    const uint64_t target = vk::RawBufferLoad<uint64_t>(c + 40, 8) + uint64_t(job.z) * 32;

    for (uint i = thread.x; i < vertexCount; i += 64) {
        const uint64_t v = source + uint64_t(i) * stride;
        const float3 position = vk::RawBufferLoad<float3>(v, 4);
        const float3 normal = vk::RawBufferLoad<float3>(v + 12, 4);
        const float2 uv = vk::RawBufferLoad<float2>(v + 24, 4);
        const uint bones = vk::RawBufferLoad<uint>(v + skinOffset, 4);
        const float2 weights = vk::RawBufferLoad<float2>(v + skinOffset + 4, 4);
        const uint64_t b0 = palette + uint64_t(bones & 0xFF) * 64;
        const uint64_t b1 = palette + uint64_t((bones >> 8) & 0xFF) * 64;
        const uint64_t dst = target + uint64_t(i) * 32;
        vk::RawBufferStore<float3>(dst, Transform(b0, position, 1.0) * weights.x + Transform(b1, position, 1.0) * weights.y, 4);
        vk::RawBufferStore<float3>(dst + 12, Transform(b0, normal, 0.0) * weights.x + Transform(b1, normal, 0.0) * weights.y, 4);
        vk::RawBufferStore<float2>(dst + 24, uv, 4);
    }
}
