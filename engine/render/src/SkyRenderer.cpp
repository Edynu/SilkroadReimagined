#include "sr/render/SkyRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "kSkyPixelShader.spv.h"
#include "kSkyVertexShader.spv.h"
#include "sr/render/TextureCache.h"

namespace sr::render {

namespace {

constexpr float kRadius = 20000.0f;
constexpr float kPi = 3.14159265358979f;

struct SkyVertex {
    float position[3];
    float uv[2];
    std::uint32_t color; // RGBA8, r low
    std::uint8_t extra[4]; // stars: twinkle group (10 steady), size in pixels
};

struct PushData {
    glm::mat4 viewProj;
    std::uint64_t frame;
    std::uint32_t kind;
    std::uint32_t textureIndex;
    float pixel[2];
    float starAlpha;
    float cloudAlpha;
    std::uint32_t flicker[4];
    glm::vec4 disc; // angle, half size, cloud scroll, 0
};
static_assert(sizeof(PushData) == 128);

// The C runtime's rand(): the original builds its star field with it from seed 1.
struct Rand {
    std::uint32_t state;
    int operator()() {
        state = state * 214013u + 2531011u;
        return static_cast<int>((state >> 16) & 0x7FFF);
    }
};

// A unit sphere: an octahedron split four times, each new point pushed out to the sphere.
void Sphere(std::vector<glm::vec3>& points, std::vector<std::array<int, 3>>& triangles) {
    points = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}, {0, 0, -1}, {-1, 0, 0}, {0, -1, 0}};
    triangles = {{0, 2, 1}, {0, 3, 2}, {0, 4, 3}, {0, 1, 4}, {5, 1, 2}, {5, 2, 3}, {5, 3, 4}, {5, 4, 1}};
    for (int pass = 0; pass < 4; ++pass) {
        std::map<std::pair<int, int>, int> middles;
        const auto middle = [&](int a, int b) {
            const auto key = std::minmax(a, b);
            if (const auto it = middles.find(key); it != middles.end()) return it->second;
            points.push_back(glm::normalize((points[static_cast<std::size_t>(a)] + points[static_cast<std::size_t>(b)]) * 0.5f));
            const int index = static_cast<int>(points.size()) - 1;
            middles.emplace(key, index);
            return index;
        };
        std::vector<std::array<int, 3>> next;
        for (const auto& [a, b, c] : triangles) {
            const int ab = middle(a, b), bc = middle(b, c), ca = middle(c, a);
            next.push_back({a, ab, ca});
            next.push_back({ab, bc, ca});
            next.push_back({b, bc, ab});
            next.push_back({bc, c, ca});
        }
        triangles = std::move(next);
    }
}

std::uint32_t Rgba(std::uint32_t rgb, std::uint8_t alpha) {
    return (rgb >> 16 & 0xFF) | (rgb & 0xFF00) | (rgb & 0xFF) << 16 | static_cast<std::uint32_t>(alpha) << 24;
}

} // namespace

bool SkyRenderer::Init(rhi::Device& device, rhi::Format colorFormat, rhi::Format depthFormat, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = std::as_bytes(std::span(kSkyVertexShader));
    desc.fragmentShader = std::as_bytes(std::span(kSkyPixelShader));
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.vertexStride = sizeof(SkyVertex);
    desc.attributes = {
        {0, rhi::VertexFormat::Float3, static_cast<std::uint32_t>(offsetof(SkyVertex, position))},
        {1, rhi::VertexFormat::Float2, static_cast<std::uint32_t>(offsetof(SkyVertex, uv))},
        {2, rhi::VertexFormat::RGBA8Unorm, static_cast<std::uint32_t>(offsetof(SkyVertex, color))},
        {3, rhi::VertexFormat::UByte4, static_cast<std::uint32_t>(offsetof(SkyVertex, extra))},
    };
    desc.blend = rhi::BlendMode::AlphaBlend;
    desc.colorFormat = colorFormat;
    desc.depthFormat = depthFormat;
    desc.debugName = "sky";
    pipeline_ = device.CreateGraphicsPipeline(desc);
    if (!pipeline_) {
        error = "sky pipeline failed";
        return false;
    }

    std::vector<SkyVertex> vertices;
    std::vector<std::uint32_t> indices;
    const auto vertex = [&](const glm::vec3& p, glm::vec2 uv = {}, std::uint32_t color = 0xFFFFFFFFu, std::uint8_t a = 0,
                            std::uint8_t b = 0) {
        vertices.push_back({{p.x, p.y, p.z}, {uv.x, uv.y}, color, {a, b, 0, 0}});
        return static_cast<std::uint32_t>(vertices.size() - 1);
    };
    const auto begin = [&](int kind) { ranges_[static_cast<std::size_t>(kind)].first = static_cast<std::uint32_t>(indices.size()); };
    const auto end = [&](int kind) {
        Range& r = ranges_[static_cast<std::size_t>(kind)];
        r.count = static_cast<std::uint32_t>(indices.size()) - r.first;
    };

    std::vector<glm::vec3> points;
    std::vector<std::array<int, 3>> triangles;
    Sphere(points, triangles);
    // 1: the dome (the upper half) and 6: the skirt (up to 2207 units above the horizon,
    // opaque below it, clear above).
    for (const int kind : {1, 6}) {
        std::vector<int> map(points.size(), -1);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const glm::vec3 p = points[i] * kRadius;
            if (kind == 1 ? points[i].y >= 0.0f : p.y <= 2207.0f) {
                map[i] = static_cast<int>(vertex(p, {}, kind == 6 && points[i].y > 0.0f ? 0x00FFFFFFu : 0xFFFFFFFFu));
            }
        }
        begin(kind);
        for (const auto& t : triangles) {
            if (map[static_cast<std::size_t>(t[0])] < 0 || map[static_cast<std::size_t>(t[1])] < 0 || map[static_cast<std::size_t>(t[2])] < 0) continue;
            for (const int i : t) indices.push_back(static_cast<std::uint32_t>(map[static_cast<std::size_t>(i)]));
        }
        end(kind);
    }

    // 2: stars, picked like the original: a random upper-dome triangle, a point inside it,
    // kept when more than 5000 up; a colour from the table; a quarter pulled toward the
    // horizon's middle; all moved 6666.67 along z.
    {
        static constexpr std::uint32_t kColours[8] = {0x5c4487, 0x595715, 0xa7ad01, 0x874467,
                                                      0x676295, 0xa8a550, 0xd9a1da, 0xbab3ff};
        std::vector<std::array<int, 3>> upper;
        for (const auto& t : triangles) {
            if (points[static_cast<std::size_t>(t[0])].y >= 0.0f && points[static_cast<std::size_t>(t[1])].y >= 0.0f &&
                points[static_cast<std::size_t>(t[2])].y >= 0.0f) {
                upper.push_back(t);
            }
        }
        Rand rand{1};
        begin(2);
        for (int star = 0; star < 3000;) {
            const auto& t = upper[static_cast<std::size_t>(rand()) % upper.size()];
            const glm::vec3 a = points[static_cast<std::size_t>(t[0])] * kRadius, b = points[static_cast<std::size_t>(t[1])] * kRadius,
                            c = points[static_cast<std::size_t>(t[2])] * kRadius;
            const glm::vec3 e0 = glm::mix(a, b, static_cast<float>(rand()) / 32767.0f);
            const glm::vec3 e1 = glm::mix(a, c, static_cast<float>(rand()) / 32767.0f);
            const glm::vec3 e2 = glm::mix(b, c, static_cast<float>(rand()) / 32767.0f);
            glm::vec3 p = glm::mix(e2, glm::mix(e0, e1, 0.5f), 2.0f / 3.0f);
            if (p.y < 5000.0f) continue;
            const std::uint32_t colour = Rgba(kColours[rand() % 8], 255);
            if (rand() % 4 == 0) p.z /= 10.0f;
            p.z += 6666.6665f;
            const std::uint8_t group = star < 1000 ? static_cast<std::uint8_t>(star / 100) : 10;
            const std::uint8_t size = star < 1000 ? 2 : 1;
            const std::uint32_t first = vertex(p, {-1, -1}, colour, group, size);
            vertex(p, {1, -1}, colour, group, size);
            vertex(p, {1, 1}, colour, group, size);
            vertex(p, {-1, 1}, colour, group, size);
            for (const std::uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(first + k);
            ++star;
        }
        end(2);
    }

    // 3, 4: the sun and the moon, one quad each (placed by the shader).
    for (const int kind : {3, 4}) {
        begin(kind);
        const std::uint32_t first = vertex({0, -1, 1}, {0, 0});
        vertex({0, 1, 1}, {1, 0});
        vertex({0, 1, -1}, {1, 1});
        vertex({0, -1, -1}, {0, 1});
        for (const std::uint32_t k : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(first + k);
        end(kind);
    }

    // 5: clouds, a 4 x 4 grid of cells 2,000,000 wide at the sky's height, 16 repeats a cell.
    {
        begin(5);
        const std::uint32_t first = static_cast<std::uint32_t>(vertices.size());
        for (int z = 0; z < 5; ++z) {
            for (int x = 0; x < 5; ++x) {
                vertex({(x - 2) * 2000000.0f, kRadius, (z - 2) * 2000000.0f}, {(x - 2) * 16.0f, (z - 2) * 16.0f});
            }
        }
        for (std::uint32_t z = 0; z < 4; ++z) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                const std::uint32_t a = first + z * 5 + x;
                for (const std::uint32_t k : {a, a + 1, a + 6, a, a + 6, a + 5}) indices.push_back(k);
            }
        }
        end(5);
    }

    vertices_ = device.CreateBuffer({vertices.size() * sizeof(SkyVertex), rhi::BufferUsage::Vertex, "sky", false},
                                    std::as_bytes(std::span(vertices)));
    indices_ = device.CreateBuffer({indices.size() * sizeof(std::uint32_t), rhi::BufferUsage::Index, "sky", false},
                                   std::as_bytes(std::span(indices)));
    if (!vertices_ || !indices_) {
        error = "sky buffers failed";
        return false;
    }
    return true;
}

void SkyRenderer::Shutdown() {
    if (device_ == nullptr) return;
    if (pipeline_) device_->DestroyPipeline(pipeline_);
    device_->DestroyBuffer(vertices_);
    device_->DestroyBuffer(indices_);
    pipeline_ = {};
    device_ = nullptr;
}

void SkyRenderer::Update(TextureCache& textures, double seconds) {
    if (device_ == nullptr) return;
    if (!texturesLoaded_) {
        texturesLoaded_ = true;
        sunTexture_ = device_->BindlessIndex(textures.Get("sun\\lens2.ddj").handle);
        cloudTexture_ = device_->BindlessIndex(textures.Get("skybox\\cloud1.ddj").handle);
        for (std::size_t i = 0; i < moonTextures_.size(); ++i) {
            char path[32];
            std::snprintf(path, sizeof path, "sun\\moon%02d.ddj", static_cast<int>(i + 1));
            moonTextures_[i] = device_->BindlessIndex(textures.Get(path).handle);
        }
    }
    // Ten times a second each group may light up again (a tenth of the time), to 128..255.
    if (seconds < flickerTick_) flickerTick_ = seconds;
    if (seconds - flickerTick_ < 0.1) return;
    flickerTick_ += std::floor((seconds - flickerTick_) / 0.1) * 0.1;
    Rand rand{random_};
    for (std::uint8_t& b : flicker_) {
        if (rand() % 10 == 0) b = static_cast<std::uint8_t>((rand() & 127) + 128);
    }
    random_ = rand.state;
}

void SkyRenderer::Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, const glm::vec3& cameraPosition, std::uint64_t frame,
                       const State& state, float width, float height) const {
    if (!pipeline_ || frame == 0) return;
    PushData push{};
    push.viewProj = viewProj * glm::translate(glm::mat4(1.0f), cameraPosition); // the sky moves with the camera
    push.frame = frame;
    push.pixel[0] = 1.0f / std::max(width, 1.0f);
    push.pixel[1] = 1.0f / std::max(height, 1.0f);
    push.starAlpha = std::clamp(state.starAlpha, -1.0f, 1.0f);
    push.cloudAlpha = state.cloudAlpha;
    for (std::size_t i = 0; i < flicker_.size(); ++i) push.flicker[i / 4] |= static_cast<std::uint32_t>(flicker_[i]) << (i % 4 * 8);
    const float t = state.timeOfDay;
    cmd.BindPipeline(pipeline_);
    cmd.BindVertexBuffer(vertices_, 0);
    cmd.BindIndexBuffer(indices_, 0, true);
    const auto draw = [&](int kind, std::uint32_t texture, glm::vec4 disc) {
        const Range& r = ranges_[static_cast<std::size_t>(kind)];
        if (r.count == 0) return;
        push.kind = static_cast<std::uint32_t>(kind);
        push.textureIndex = texture;
        push.disc = disc;
        cmd.PushConstants(&push, sizeof(push));
        cmd.DrawIndexed(r.count, r.first, 0);
    };
    draw(1, 0, {});
    if (push.starAlpha > 0.0f) draw(2, 0, {});
    if (t >= 0.25f && t <= 0.875f && sunTexture_ != 0) draw(3, sunTexture_, {(t - 0.25f) * 2.0f * kPi, 666.666687f, 0, 0});
    if ((t > 0.75f || t < 0.5f) && state.lunarDay >= 0 && state.lunarDay < 29) {
        const float w = std::fmod(t + 0.25f, 1.0f);
        const std::uint32_t moon = moonTextures_[static_cast<std::size_t>(state.lunarDay)];
        if (moon != 0) draw(4, moon, {(0.25f + (w - 0.25f) * 1.2f) * 2.0f * kPi, 1000.0f, 0, 0});
    }
    if (state.cloudAlpha > 0.0f && cloudTexture_ != 0) {
        draw(5, cloudTexture_, {0, 0, static_cast<float>(std::fmod(state.seconds, 500.0) / 500.0), 0});
    }
    draw(6, 0, {});
}

} // namespace sr::render
