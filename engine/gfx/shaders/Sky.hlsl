// The original's sky ("Time of day, sky and fog (C3e)"), camera-centred, no depth.
// One pipeline per blend mode; the push constant names the part drawn.

[[vk::binding(0, 0)]] Texture2D gTextures[];
[[vk::binding(1, 0)]] SamplerState gSamplers[4]; // LinearWrap, LinearClamp, PointWrap, PointClamp

struct PushData {
    uint64_t constants; // SkyConstants (SkyPass.cpp)
    uint part;
    uint pad;
};
[[vk::push_constant]] PushData pc;

static const uint kPartDome = 0, kPartBand = 1, kPartStars = 2, kPartSun = 3, kPartMoon = 4, kPartClouds = 5;
static const float kRadius = 20000.0;

float4 Value(uint offset) { return vk::RawBufferLoad<float4>(pc.constants + offset, 16); }
float4x4 ViewProj() {
    return float4x4(Value(0), Value(16), Value(32), Value(48)); // GLM's columns as rows: mul(v, m)
}
float4 Project(float3 p) { return mul(float4(p, 1.0), ViewProj()); }

struct VSOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    float viewDepth : DEPTH;
};

VSOutput VSMain(uint vertexId : SV_VertexID) {
    VSOutput output;
    output.uv = 0.0;
    output.viewDepth = 0.0;
    const uint64_t geometry = vk::RawBufferLoad<uint64_t>(pc.constants + 256, 8);
    const uint domeVertices = vk::RawBufferLoad<uint>(pc.constants + 264, 4);
    if (pc.part == kPartDome || pc.part == kPartBand) {
        const uint index = pc.part == kPartDome ? vertexId : domeVertices + vertexId;
        const float3 unit = vk::RawBufferLoad<float3>(geometry + uint64_t(index) * 12, 4);
        const float3 p = unit * kRadius;
        output.position = Project(p);
        if (pc.part == kPartBand) {
            // The fog colour, opaque at and below the horizon, clear above.
            output.color = float4(Value(128).rgb, unit.y <= 0.0 ? 1.0 : 0.0);
        } else {
            // Horizon to zenith by the gradient, then toward the glow near the sun.
            const float4 zenith = Value(64), horizon = Value(80), glow = Value(96);
            const float h = saturate(p.y / 5000.0 * zenith.w / 2.0);
            const float3 base = lerp(horizon.rgb, zenith.rgb, h);
            const float d = min(1.0, length(p - Value(144).xyz) / horizon.w);
            output.color = float4(lerp(glow.rgb, base, d), 1.0);
        }
        return output;
    }
    // Quads: two triangles from corners (-1, -1) .. (1, 1).
    static const float2 kCorners[6] = {float2(-1, -1), float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(1, -1)};
    const float2 corner = kCorners[vertexId % 6];
    if (pc.part == kPartStars) {
        const uint star = vertexId / 6;
        const uint64_t s = vk::RawBufferLoad<uint64_t>(pc.constants + 272, 8) + uint64_t(star) * 16;
        const float3 p = vk::RawBufferLoad<float3>(s, 4);
        const uint color = vk::RawBufferLoad<uint>(s + 12, 4);
        const float visibility = Value(112).w;
        // The first 1000 twinkle in 10 batches at 2 pixels; the rest are 1 pixel.
        const bool bright = star < 1000;
        float brightness = 1.0;
        if (bright) {
            const uint batch = star / 100;
            brightness = Value(192 + (batch / 4) * 16)[batch % 4];
        }
        const float size = bright ? 2.0 : 1.0;
        float4 clip = Project(p);
        clip.xy += corner * size * Value(176).zw * clip.w; // `size` pixels across
        output.position = clip;
        output.color = float4(float((color >> 16) & 0xFF), float((color >> 8) & 0xFF), float(color & 0xFF), 255.0) / 255.0;
        output.color.a *= brightness * visibility;
        return output;
    }
    if (pc.part == kPartSun || pc.part == kPartMoon) {
        // A square tangent to the sphere at the body's position.
        const float4 body = pc.part == kPartSun ? Value(144) : Value(160);
        const float3 n = normalize(body.xyz);
        const float3 right = normalize(cross(n, float3(0, 0, 1)));
        const float3 up = cross(right, n);
        const float3 p = body.xyz + (right * corner.x + up * corner.y) * body.w;
        output.position = Project(p);
        output.uv = corner * 0.5 + 0.5;
        // The sun: its colour from the environment, the texture's alpha; the moon as it is.
        output.color = pc.part == kPartSun ? float4(Value(112).rgb, 1.0) : float4(1, 1, 1, 1);
        return output;
    }
    // The clouds: a plane 20000 above the camera, +-4,000,000 across, the texture 64 times.
    const float3 p = float3(corner.x * 4000000.0, 20000.0, corner.y * 4000000.0);
    output.position = Project(p);
    output.uv = p.xz / 125000.0 + float2(Value(128).w, 0.0);
    output.color = Value(96); // the glow colour, the cloud alpha
    output.viewDepth = output.position.w;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    const uint4 textures = vk::RawBufferLoad<uint4>(pc.constants + 240, 16);
    if (pc.part == kPartSun) {
        const float a = gTextures[NonUniformResourceIndex(textures.y)].Sample(gSamplers[1], input.uv).a;
        return float4(input.color.rgb, a);
    }
    if (pc.part == kPartMoon) return gTextures[NonUniformResourceIndex(textures.z)].Sample(gSamplers[1], input.uv);
    if (pc.part == kPartClouds) {
        const float4 texel = gTextures[NonUniformResourceIndex(textures.x)].Sample(gSamplers[0], input.uv);
        // Fogged out from 90,000 to 200,000 in the fog colour.
        const float keep = saturate((200000.0 - input.viewDepth) / 110000.0);
        return float4(lerp(Value(128).rgb, texel.rgb * input.color.rgb, keep), texel.a * input.color.a);
    }
    return input.color;
}
