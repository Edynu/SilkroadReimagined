// The original's lens flare: one textured square per sprite, placed in pixels along the
// sun-to-centre axis (pre-transformed, as the original's XYZRHW vertices), its alpha the
// sprite's byte scaled by the flare's visibility (read from a 1 x 1 target the GPU keeps).

struct PushData {
    float2 centre;   // pixels
    float halfSize;  // pixels
    float alpha;     // the sprite's alpha byte (0..255)
    float2 rcpViewport;
    uint textureIndex;
    uint visibilityIndex;
};
[[vk::push_constant]] PushData pc;

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float alpha : TEXCOORD1;
};

VSOutput VSMain(uint id : SV_VertexID) {
    // Two triangles: (0,0) (1,0) (0,1), (0,1) (1,0) (1,1).
    const float2 corners[6] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1)};
    const float2 c = corners[id];
    const float visibility = gTextures[NonUniformResourceIndex(pc.visibilityIndex)].SampleLevel(gSamplers[3], float2(0.5, 0.5), 0).r;
    VSOutput o;
    const float2 pixel = floor(pc.centre) + (c * 2.0 - 1.0) * pc.halfSize;
    o.position = float4(pixel * pc.rcpViewport * 2.0 - 1.0, 0.0, 1.0);
    o.uv = c;
    // Not drawn at all while hardly visible (at most a tenth).
    o.alpha = visibility > 0.1 ? floor(pc.alpha * visibility) / 255.0 : 0.0;
    if (visibility <= 0.1) o.position = float4(2.0, 2.0, 0.0, 1.0);
    return o;
}

float4 PSMain(VSOutput i) : SV_Target {
    const float4 t = gTextures[NonUniformResourceIndex(pc.textureIndex)].Sample(gSamplers[1], i.uv);
    return float4(t.rgb, t.a * i.alpha);
}
