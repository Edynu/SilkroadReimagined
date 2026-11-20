// Copies a bindless texture over the whole target (a scene view into the swapchain pass).

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

struct PushData {
    uint textureIndex;
    uint samplerIndex;
};
[[vk::push_constant]] PushData pc;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertex : SV_VertexID) {
    // One triangle covering the screen.
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    VSOutput output;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv = uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    return gTextures[NonUniformResourceIndex(pc.textureIndex)].Sample(gSamplers[pc.samplerIndex], input.uv);
}
