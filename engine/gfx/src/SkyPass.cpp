#include "sr/gfx/SkyPass.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include "kSkyPixelShader.spv.h"
#include "kSkyVertexShader.spv.h"
#include "sr/core/Trace.h"

namespace sr::gfx {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

// Sky.hlsl's constants.
struct SkyConstants {
    glm::mat4 viewProj{1.0f};     // 0
    glm::vec4 zenith{0.0f};       // 64: w gradient
    glm::vec4 horizon{0.0f};      // 80: w glow radius
    glm::vec4 glow{0.0f};         // 96: w cloud alpha
    glm::vec4 sun{0.0f};          // 112: w star visibility
    glm::vec4 band{0.0f};         // 128: w cloud scroll (u)
    glm::vec4 sunPosition{0.0f};  // 144: w half size
    glm::vec4 moonPosition{0.0f}; // 160: w half size
    glm::vec4 viewport{0.0f};     // 176: width, height, 1 / width, 1 / height
    glm::vec4 twinkle[3]{};       // 192: the 10 batches' brightness
    std::uint32_t textures[4]{};  // 240: clouds, sun, moon
    std::uint64_t geometry = 0;   // 256
    std::uint32_t domeVertices = 0, bandVertices = 0; // 264
    std::uint64_t stars = 0;      // 272
    std::uint64_t pad = 0;
};
static_assert(sizeof(SkyConstants) == 288);

struct Push {
    std::uint64_t constants = 0;
    std::uint32_t part = 0; // Sky.hlsl's kPart*
    std::uint32_t pad = 0;
};

constexpr std::uint32_t kPartDome = 0, kPartBand = 1, kPartStars = 2, kPartSun = 3, kPartMoon = 4, kPartClouds = 5;
constexpr std::uint32_t kStarCount = 3000;
constexpr float kRadius = 20000.0f;

void Subdivide(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, int depth, std::vector<std::array<glm::vec3, 3>>& out) {
    if (depth == 0) {
        out.push_back({a, b, c});
        return;
    }
    const glm::vec3 ab = glm::normalize(a + b), bc = glm::normalize(b + c), ca = glm::normalize(c + a);
    Subdivide(a, ab, ca, depth - 1, out);
    Subdivide(ab, b, bc, depth - 1, out);
    Subdivide(ca, bc, c, depth - 1, out);
    Subdivide(ab, bc, ca, depth - 1, out);
}

std::uint32_t Next(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

} // namespace

bool SkyPass::Init(rhi::Device& device, rhi::Format color, rhi::Format depth, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = Spirv(kSkyVertexShader);
    desc.fragmentShader = Spirv(kSkyPixelShader);
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.colorFormat = color;
    desc.depthFormat = depth; // in the scene pass, but no depth test or write
    desc.debugName = "sky dome";
    dome_ = device.CreateGraphicsPipeline(desc);
    desc.blend = rhi::BlendMode::AlphaBlend;
    desc.debugName = "sky";
    blended_ = device.CreateGraphicsPipeline(desc);

    // The dome: an octahedron subdivided 4 times onto the unit sphere. The dome's triangles touch
    // the upper half; the band's reach below the first ring above the horizon (y 2207 of 20000).
    std::vector<std::array<glm::vec3, 3>> triangles;
    const glm::vec3 x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            for (int sz = -1; sz <= 1; sz += 2) {
                const glm::vec3 a = x * static_cast<float>(sx), b = y * static_cast<float>(sy), c = z * static_cast<float>(sz);
                Subdivide(a, b, c, 4, triangles);
            }
        }
    }
    std::vector<glm::vec3> dome, band;
    for (const auto& t : triangles) {
        const float top = std::max({t[0].y, t[1].y, t[2].y}), bottom = std::min({t[0].y, t[1].y, t[2].y});
        if (top > 0.0f) dome.insert(dome.end(), t.begin(), t.end());
        if (bottom <= 2207.0f / kRadius) band.insert(band.end(), t.begin(), t.end());
    }
    // The stars: random points in random dome triangles, kept above y 5000; one in four squeezed
    // toward z = 0 (a band), then all moved 6666.67 toward +z (as the original's).
    struct Star {
        glm::vec3 position;
        std::uint32_t color;
    };
    static constexpr std::uint32_t kColors[8] = {0x5C4487, 0x595715, 0xA7AD01, 0x874467, 0x676295, 0xA8A550, 0xD9A1DA, 0xBAB3FF};
    std::vector<Star> stars;
    std::uint32_t random = 12345;
    while (stars.size() < kStarCount) {
        const auto& t = triangles[Next(random) % triangles.size()];
        float u = static_cast<float>(Next(random) % 10000) / 10000.0f, v = static_cast<float>(Next(random) % 10000) / 10000.0f;
        if (u + v > 1.0f) {
            u = 1.0f - u;
            v = 1.0f - v;
        }
        glm::vec3 p = (t[0] + (t[1] - t[0]) * u + (t[2] - t[0]) * v) * kRadius;
        if (p.y < 5000.0f) continue;
        if (Next(random) % 4 == 0) p.z /= 10.0f;
        p.z += 6666.67f;
        stars.push_back({p, kColors[Next(random) % 8]});
    }

    const std::size_t domeBytes = (dome.size() + band.size()) * sizeof(glm::vec3);
    starOffset_ = (domeBytes + 15) & ~std::size_t{15};
    std::vector<std::byte> bytes(starOffset_ + stars.size() * sizeof(Star));
    std::memcpy(bytes.data(), dome.data(), dome.size() * sizeof(glm::vec3));
    std::memcpy(bytes.data() + dome.size() * sizeof(glm::vec3), band.data(), band.size() * sizeof(glm::vec3));
    std::memcpy(bytes.data() + starOffset_, stars.data(), stars.size() * sizeof(Star));
    rhi::BufferDesc buffer;
    buffer.size = bytes.size();
    buffer.usage = rhi::BufferUsage::Storage;
    buffer.debugName = "sky geometry";
    geometry_ = device.CreateBuffer(buffer, bytes);
    domeVertices_ = static_cast<std::uint32_t>(dome.size());
    bandVertices_ = static_cast<std::uint32_t>(band.size());
    stars_ = static_cast<std::uint32_t>(stars.size());
    if (!dome_ || !blended_ || !geometry_) {
        error = "cannot create the sky pass";
        return false;
    }
    return true;
}

void SkyPass::Shutdown() {
    if (device_ == nullptr) return;
    device_->DestroyPipeline(dome_);
    device_->DestroyPipeline(blended_);
    device_->DestroyBuffer(geometry_);
    device_ = nullptr;
}

void SkyPass::Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, const SkyState& sky, const Textures& textures, double seconds,
                   std::uint32_t width, std::uint32_t height) const {
    if (!sky.active || width == 0 || height == 0) return;
    SR_ZONE("render: sky");
    SkyConstants c;
    c.viewProj = viewProj;
    c.zenith = glm::vec4(sky.zenith, sky.gradient);
    c.horizon = glm::vec4(sky.horizon, sky.glowRadius);
    c.glow = glm::vec4(sky.sunGlow, sky.cloudAlpha);
    c.sun = glm::vec4(sky.sunColor, sky.stars);
    // The clouds move one texture repeat (125,000 units) every 500 s along x.
    c.band = glm::vec4(sky.band, static_cast<float>(std::fmod(seconds / 500.0, 1.0)));
    c.sunPosition = glm::vec4(sky.sunPosition, 666.67f);
    c.moonPosition = glm::vec4(sky.moonPosition, 1000.0f);
    c.viewport = glm::vec4(static_cast<float>(width), static_cast<float>(height), 1.0f / static_cast<float>(width),
                           1.0f / static_cast<float>(height));
    std::memcpy(c.twinkle, sky.twinkle.data(), sizeof(float) * sky.twinkle.size());
    c.textures[0] = textures.clouds;
    c.textures[1] = textures.sun;
    c.textures[2] = textures.moon;
    c.geometry = device_->BufferAddress(geometry_);
    c.domeVertices = domeVertices_;
    c.bandVertices = bandVertices_;
    c.stars = c.geometry + starOffset_;
    const rhi::TransientAllocation alloc = device_->AllocateTransient(sizeof(c), 16);
    if (alloc.cpu == nullptr) return;
    std::memcpy(alloc.cpu, &c, sizeof(c));

    Push push;
    push.constants = alloc.gpuAddress;
    const auto part = [&](rhi::PipelineHandle pipeline, std::uint32_t which, std::uint32_t vertices) {
        if (vertices == 0) return;
        push.part = which;
        cmd.BindPipeline(pipeline);
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(vertices, 0);
    };
    // The original's order: dome, stars, sun, moon, clouds, the horizon band.
    part(dome_, kPartDome, domeVertices_);
    if (sky.stars > 0.0f) part(blended_, kPartStars, stars_ * 6);
    if (sky.sunVisible && textures.sun != 0) part(blended_, kPartSun, 6);
    if (sky.moonVisible && textures.moon != 0) part(blended_, kPartMoon, 6);
    if (sky.cloudAlpha > 0.0f && textures.clouds != 0) part(blended_, kPartClouds, 6);
    part(blended_, kPartBand, bandVertices_);
}

} // namespace sr::gfx
