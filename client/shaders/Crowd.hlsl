// The crowd benchmark: one small box per bone, instanced. Each
// instance's bone transform comes from the frame's palette, which the game thread's pose jobs
// wrote straight into GPU memory: 64 bytes per bone, three rows of a 3x4 world matrix (row-major)
// and a fourth float4 with the colour (rgb) and the box size (w).

struct PushData {
    float4x4 viewProj;      // GLM column-major
    uint64_t palette;       // address of the frame's bone transforms
    uint instanceBase;      // the chunk's first bone
    uint pad;
};
[[vk::push_constant]] PushData pc;

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
};

struct VSOutput {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR0;
};

VSOutput VSMain(VSInput input, uint instance : SV_InstanceID) {
    const uint64_t base = pc.palette + uint64_t(pc.instanceBase + instance) * 64;
    const float4 r0 = vk::RawBufferLoad<float4>(base, 16);
    const float4 r1 = vk::RawBufferLoad<float4>(base + 16, 16);
    const float4 r2 = vk::RawBufferLoad<float4>(base + 32, 16);
    const float4 extra = vk::RawBufferLoad<float4>(base + 48, 16);
    const float3 local = input.position * extra.w;
    const float3 world = float3(dot(r0.xyz, local) + r0.w, dot(r1.xyz, local) + r1.w, dot(r2.xyz, local) + r2.w);
    VSOutput output;
    output.position = mul(pc.viewProj, float4(world, 1.0));
    output.normal = float3(dot(r0.xyz, input.normal), dot(r1.xyz, input.normal), dot(r2.xyz, input.normal));
    output.color = extra.rgb;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    const float3 light = normalize(float3(0.45, 0.8, 0.35));
    const float lambert = saturate(dot(normalize(input.normal), light));
    return float4(input.color * (0.35 + 0.65 * lambert), 1.0);
}
