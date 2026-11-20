// The scene's depth at one pixel (ours: the clicks' occlusion): the game asks for the
// depth under the mouse, the renderer copies that texel into a slot of a mapped buffer, the game reads it a few frames
// later and keeps an entity behind what is drawn there from being picked.

[[vk::binding(0, 0)]] Texture2D gTextures[];

struct PushData {
    uint64_t output; // the slot: float depth, uint x, uint y, uint the frame's number
    uint depth;      // the depth target (bindless)
    uint x, y;       // its texel
    uint frame;
};
[[vk::push_constant]] PushData pc;

[numthreads(1, 1, 1)]
void CSMain() {
    const float d = gTextures[NonUniformResourceIndex(pc.depth)].Load(int3(int(pc.x), int(pc.y), 0)).r;
    vk::RawBufferStore<float>(pc.output, d);
    vk::RawBufferStore<uint>(pc.output + 4, pc.x);
    vk::RawBufferStore<uint>(pc.output + 8, pc.y);
    vk::RawBufferStore<uint>(pc.output + 12, pc.frame);
}
