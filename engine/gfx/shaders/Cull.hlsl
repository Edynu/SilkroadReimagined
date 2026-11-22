// GPU culling: one thread per instance. Each part of a visible instance whose
// bounding sphere touches the frustum is appended to its bucket's draw list: an indirect command
// (VkDrawIndexedIndirectCommand, 20 bytes) and a draw record (instance, mesh, skinned base), at
// the same slot; the command's first instance is that slot, so the vertex shader finds the record.
// A skinned instance's part with bones also gets room in the frame's vertex arena and a skinning
// job (instance, mesh, arena base); the skinning pass (Skin.hlsl) runs one group per job, and the
// draw reads the part's vertices from the arena.
// The lists come in two phases, as the original draws ("Water"): what stands below
// the water level, and every skinned instance (the entities), draws before the water, the rest
// after it: a character wading shows under the surface through the water, tinted by it (the
// original's, the user's screenshot 2026-11-21). A list is bucket + phase * 3 (buckets: culled,
// two-sided, translucent).
// Several views (the sun's shadow cascades) are culled in one pass: a part goes to each view's
// lists it touches (list + view * 6), and a skinned part is skinned once for all of them. The
// counts of view 0 are at 0, those of views 1..3 at 64 + ((view - 1) * 6 + list) * 4.

[[vk::binding(2, 0)]] RWByteAddressBuffer gBuffers[];

struct PushData {
    uint64_t constants; // CullConstants, below
};
[[vk::push_constant]] PushData pc;

// CullConstants (in transient memory):
//   float4 planes[6]       0   world-space frustum planes (inside: dot(n, p) + d >= 0)
//   uint instanceCount     96  instances [0, count) are walked
//   uint maxDraws          100 slots per bucket
//   uint counts            104 storage index of the counts: one uint per list at 0, the arena's
//                              vertices at 32, the skinning dispatch (jobs, 1, 1) at 48
//   uint maxJobs           108 skinning jobs (0: no palettes this frame, skinned parts draw unposed)
//   uint64 instances       112 GpuScene records (64 bytes)
//   uint64 meshes          120 MeshTable records (64 bytes)
//   uint64 commands        128 buckets x maxDraws commands
//   uint64 draws           136 buckets x maxDraws records (uint4)
//   uint64 jobs            144 maxJobs skinning jobs (uint4)
//   uint arenaVertices     152 the arena's capacity in vertices
//   float waterLevel       156 instances whose origin is below it draw before the water
//   uint views             160 the views culled (1..4); the planes of views 1..3 from 176 (96 bytes each)
//   float minRadius[4]     464 a part smaller than this leaves the view (a shadow cascade's specks)
//   uint skinnedViews      480 skinned instances go to views below this only (the near cascades)
//   float3 eye, minAngle   496 static parts whose radius / distance from the eye is below minAngle
//                              are left out (a few pixels on screen: ours, levels of detail)
//   uint staticViews       492 static instances go to views below this only (shadow casters)
//   uint foliageViews      512 a bit per view a static instance's foliage parts (kMaterialFoliage: they sway) go to,
//                              in place of staticViews: the sun's shadows draw them every frame into the near
//                              cascades and keep them out of the cached ones there (a cached sway would stand still)
//   uint cutoutBucket      484 1: the bucket is 1 for cut-out or fading parts, 0 for solid ones
//                              (shadow casters: the solid ones draw without a fragment shader);
//                              translucent parts (bucket 2) are left out: they cast no shadow
static const uint kNotSkinned = 0xFFFFFFFFu;
static const uint kBuckets = 3; // MeshTable's kBucketCount
static const uint kBucketBlended = 2;
static const uint kInstanceSkinned = 1;
static const uint kVertexSkinned = 2;
static const uint kMaterialFoliage = 8;
static const uint kListsPerView = kBuckets * 2;

uint CountOffset(uint view, uint list) { return view == 0 ? list * 4 : 64 + ((view - 1) * kListsPerView + list) * 4; }

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const uint64_t c = pc.constants;
    if (id.x >= vk::RawBufferLoad<uint>(c + 96, 4)) return;
    const uint64_t record = vk::RawBufferLoad<uint64_t>(c + 112, 8) + uint64_t(id.x) * 64;
    const uint meshCount = vk::RawBufferLoad<uint>(record + 60, 4);
    if (meshCount == 0) return; // a free slot, or a model not resident yet
    const uint meshBase = vk::RawBufferLoad<uint>(record + 56, 4);
    const uint maxJobs = vk::RawBufferLoad<uint>(c + 108, 4);
    const bool skinned = (vk::RawBufferLoad<uint>(record + 52, 4) & kInstanceSkinned) != 0 && maxJobs != 0;
    const float4 r0 = vk::RawBufferLoad<float4>(record, 16);
    const float4 r1 = vk::RawBufferLoad<float4>(record + 16, 16);
    const float4 r2 = vk::RawBufferLoad<float4>(record + 32, 16);
    // The largest axis scale, for the radius; animation reaches beyond the bind pose's sphere.
    float scale = max(length(float3(r0.x, r1.x, r2.x)), max(length(float3(r0.y, r1.y, r2.y)), length(float3(r0.z, r1.z, r2.z))));
    if (skinned) scale *= 1.5;

    const uint views = clamp(vk::RawBufferLoad<uint>(c + 160, 4), 1u, 4u);
    const uint maxDraws = vk::RawBufferLoad<uint>(c + 100, 4);
    const uint counts = vk::RawBufferLoad<uint>(c + 104, 4);
    const uint64_t meshes = vk::RawBufferLoad<uint64_t>(c + 120, 8);
    const uint64_t commands = vk::RawBufferLoad<uint64_t>(c + 128, 8);
    const uint64_t draws = vk::RawBufferLoad<uint64_t>(c + 136, 8);
    const uint64_t jobs = vk::RawBufferLoad<uint64_t>(c + 144, 8);
    const uint arenaVertices = vk::RawBufferLoad<uint>(c + 152, 4);
    const bool entity = skinned || (vk::RawBufferLoad<uint>(record + 52, 4) & kInstanceSkinned) != 0;
    const uint phase = (!entity && r1.w >= vk::RawBufferLoad<float>(c + 156, 4)) ? 1 : 0;

    const bool shadowCasters = vk::RawBufferLoad<uint>(c + 484, 4) != 0;
    for (uint i = 0; i < meshCount; ++i) {
        const uint mesh = meshBase + i;
        const uint64_t m = meshes + uint64_t(mesh) * 64;
        if (shadowCasters && vk::RawBufferLoad<uint>(m + 24, 4) == kBucketBlended) continue;
        const float4 sphere = vk::RawBufferLoad<float4>(m + 32, 16);
        const float3 centre = float3(dot(r0.xyz, sphere.xyz) + r0.w, dot(r1.xyz, sphere.xyz) + r1.w, dot(r2.xyz, sphere.xyz) + r2.w);
        const float radius = sphere.w * scale;
        const float minAngle = vk::RawBufferLoad<float>(c + 508, 4);
        if (minAngle > 0.0 && !skinned && (vk::RawBufferLoad<uint>(record + 52, 4) & kInstanceSkinned) == 0 &&
            radius < minAngle * length(centre - vk::RawBufferLoad<float3>(c + 496, 4))) continue;
        uint seen = 0; // a bit per view whose frustum the part touches
        const uint skinnedViews = vk::RawBufferLoad<uint>(c + 480, 4);
        const bool anySkinned = (vk::RawBufferLoad<uint>(record + 52, 4) & kInstanceSkinned) != 0;
        const bool foliage = !anySkinned && (vk::RawBufferLoad<uint>(m + 20, 4) & kMaterialFoliage) != 0;
        const uint foliageViews = vk::RawBufferLoad<uint>(c + 512, 4);
        for (uint v = 0; v < views; ++v) {
            if (radius < vk::RawBufferLoad<float>(c + 464 + v * 4, 4)) continue;
            if (anySkinned && v >= skinnedViews) continue;
            if (foliage) {
                if (((foliageViews >> v) & 1u) == 0) continue;
            } else if (!anySkinned && v >= vk::RawBufferLoad<uint>(c + 492, 4)) {
                continue;
            }
            const uint64_t planes = c + (v == 0 ? 0 : 176 + (v - 1) * 96);
            bool visible = true;
            for (uint p = 0; p < 6; ++p) {
                const float4 plane = vk::RawBufferLoad<float4>(planes + p * 16, 16);
                if (dot(plane.xyz, centre) + plane.w < -radius) visible = false;
            }
            if (visible) seen |= 1u << v;
        }
        if (seen == 0) continue;

        uint skinBase = kNotSkinned;
        if (skinned && (vk::RawBufferLoad<uint>(m + 12, 4) & kVertexSkinned) != 0) {
            const uint vertexCount = vk::RawBufferLoad<uint>(m + 28, 4);
            uint base;
            gBuffers[NonUniformResourceIndex(counts)].InterlockedAdd(32, vertexCount, base);
            if (base + vertexCount > arenaVertices) continue; // the arena is full: not drawn this frame
            uint job;
            gBuffers[NonUniformResourceIndex(counts)].InterlockedAdd(48, 1, job);
            if (job >= maxJobs) continue;
            vk::RawBufferStore<uint4>(jobs + uint64_t(job) * 16, uint4(id.x, mesh, base, 0), 16);
            skinBase = base;
        }

        uint kind = vk::RawBufferLoad<uint>(m + 24, 4);
        // The modern water's pools after the scene's copy: in the translucent list (flag at 488).
        if (!shadowCasters && vk::RawBufferLoad<uint>(c + 488, 4) != 0 && (vk::RawBufferLoad<uint>(m + 20, 4) & 64) != 0) kind = kBucketBlended;
        if (shadowCasters) {
            const bool cutout = (vk::RawBufferLoad<uint>(m + 20, 4) & 1) != 0 || ((vk::RawBufferLoad<uint>(record + 52, 4) >> 8) & 0xFF) != 0;
            kind = cutout ? 1 : 0;
        }
        const uint bucket = kind + phase * kBuckets;
        for (uint v = 0; v < views; ++v) {
        if ((seen & (1u << v)) == 0) continue;
        const uint list = bucket + v * kListsPerView;
        uint slot;
        gBuffers[NonUniformResourceIndex(counts)].InterlockedAdd(CountOffset(v, bucket), 1, slot);
        if (slot >= maxDraws) continue; // full: the draw reads at most maxDraws
        const uint drawIndex = list * maxDraws + slot;
        const uint64_t command = commands + uint64_t(drawIndex) * 20;
        vk::RawBufferStore<uint>(command, vk::RawBufferLoad<uint>(m + 4, 4), 4);      // index count
        vk::RawBufferStore<uint>(command + 4, 1u, 4);                                  // instances
        vk::RawBufferStore<uint>(command + 8, vk::RawBufferLoad<uint>(m, 4), 4);      // first index
        vk::RawBufferStore<uint>(command + 12, 0u, 4);                                 // vertex offset
        vk::RawBufferStore<uint>(command + 16, drawIndex, 4);                          // first instance: the record
        vk::RawBufferStore<uint4>(draws + uint64_t(drawIndex) * 16, uint4(id.x, mesh, skinBase, 0), 16);
        }
    }
}
