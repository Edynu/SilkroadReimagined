// Rain, snow, ripples and the lightning flash (gfx/WeatherPass.h). Each
// rain or snow record is a cluster: 9 triangles on the original's 50-unit 3 x 3 grid around it.

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4];

static const uint kRain = 0;
static const uint kSnow = 1;
static const uint kRipple = 2;
static const uint kFlash = 3;

struct PushData {
    float4x4 viewProj; // GLM column-major
    uint64_t records;  // float4 each
    uint texture;
    uint kind;
    float4 right;      // the camera's right (snow); flash: the colour
    float4 up;         // the camera's up (snow)
    float4 level;      // the camera's right kept level (rain)
};
[[vk::push_constant]] PushData pc;

// The original's cluster offsets, in its order.
static const float2 kGrid[9] = {float2(0, 0),    float2(50, 0),  float2(0, 50),  float2(50, 50), float2(-50, 50),
                                float2(-50, -50), float2(50, -50), float2(0, -50), float2(-50, 0)};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float alpha : ALPHA;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
    VSOutput output;
    output.alpha = 1.0;
    if (pc.kind == kFlash) {
        // One triangle over the screen.
        const float2 p = float2((vertexId << 1) & 2, vertexId & 2);
        output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
        output.uv = p;
        return output;
    }
    float3 world;
    if (pc.kind == kRipple) {
        // A flat quad growing to a half-size of 4 over 1.2 s and fading (alpha 255 - 63 per 0.3 s).
        const uint corners[6] = {0, 1, 2, 0, 2, 3};
        const float2 quad[4] = {float2(-1, 1), float2(1, 1), float2(1, -1), float2(-1, -1)};
        const float2 uvs[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};
        const float4 record = vk::RawBufferLoad<float4>(pc.records + uint64_t(vertexId / 6) * 16, 16);
        const uint corner = corners[vertexId % 6];
        const float size = record.w / 0.3;
        world = record.xyz + float3(quad[corner].x, 0.0, quad[corner].y) * size;
        output.uv = uvs[corner];
        output.alpha = saturate((255.0 - round(63.0 * size)) / 255.0);
    } else {
        const float4 record = vk::RawBufferLoad<float4>(pc.records + uint64_t(vertexId / 27) * 16, 16);
        const uint member = (vertexId % 27) / 3, corner = vertexId % 3;
        const float3 centre = record.xyz + float3(kGrid[member].x, 0.0, kGrid[member].y);
        if (pc.kind == kRain) {
            // A streak 0.6 wide and 10 tall, tip up, turned about the vertical to face the camera.
            const float2 local = corner == 0 ? float2(0.0, 10.0) : float2(corner == 1 ? 0.3 : -0.3, 0.0);
            world = centre + pc.level.xyz * local.x + float3(0.0, local.y, 0.0);
            output.uv = corner == 0 ? float2(0.5, 0.0) : float2(corner == 1 ? 0.7 : 0.3, 1.0);
        } else {
            // A flake 1 x 1 facing the camera.
            const float3 right = pc.right.xyz * 0.5, up = pc.up.xyz * 0.5;
            world = corner == 0 ? centre + up : centre + (corner == 1 ? right : -right) - up;
            output.uv = corner == 0 ? float2(0.5, 0.0) : float2(corner == 1 ? 1.0 : 0.0, 1.0);
        }
    }
    output.position = mul(pc.viewProj, float4(world, 1.0));
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    if (pc.kind == kFlash) return pc.right;
    const float4 texel = gTextures[NonUniformResourceIndex(pc.texture)].Sample(gSamplers[0], input.uv);
    return float4(texel.rgb, texel.a * input.alpha);
}
