// The sound occlusion's depth cube (ours, experimental; SR_AUDIO_OCCLUSION): the six faces' depth atlas copied into a slot
// of a mapped buffer, one float a texel, row by row, for the game to read a few frames later.

[[vk::binding(0, 0)]] Texture2D gTextures[];

struct PushData {
    uint64_t output; // the slot: width x height floats
    uint depth;      // the atlas (bindless)
    uint width, height;
};
[[vk::push_constant]] PushData pc;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.width || id.y >= pc.height) return;
    const float d = gTextures[NonUniformResourceIndex(pc.depth)].Load(int3(int(id.x), int(id.y), 0)).r;
    vk::RawBufferStore<float>(pc.output + uint64_t((id.y * pc.width + id.x) * 4u), d);
}
