#include "sr/gfx/WeatherPass.h"

#include <cstring>

#include "kWeatherPixelShader.spv.h"
#include "kWeatherVertexShader.spv.h"
#include "sr/core/Trace.h"

namespace sr::gfx {

namespace {

template <std::size_t N>
std::span<const std::byte> Spirv(const unsigned char (&code)[N]) {
    return {reinterpret_cast<const std::byte*>(code), N};
}

// Weather.hlsl's push constants (128 bytes).
struct Push {
    glm::mat4 viewProj{1.0f};
    std::uint64_t records = 0;
    std::uint32_t texture = 0;
    std::uint32_t kind = 0;
    glm::vec4 right{0.0f};
    glm::vec4 up{0.0f};
    glm::vec4 level{0.0f};
};
static_assert(sizeof(Push) == 128);

constexpr std::uint32_t kRain = 0, kSnow = 1, kRipple = 2, kFlash = 3;

} // namespace

bool WeatherPass::Init(rhi::Device& device, rhi::Format color, rhi::Format depth, std::string& error) {
    device_ = &device;
    rhi::GraphicsPipelineDesc desc;
    desc.vertexShader = Spirv(kWeatherVertexShader);
    desc.fragmentShader = Spirv(kWeatherPixelShader);
    desc.vertexEntry = "VSMain";
    desc.fragmentEntry = "PSMain";
    desc.colorFormat = color;
    desc.depthFormat = depth;
    // The original's states: alpha blended, depth tested, not written, no culling.
    desc.blend = rhi::BlendMode::AlphaBlend;
    desc.depthTest = true;
    desc.depthWrite = false;
    desc.debugName = "weather";
    particles_ = device.CreateGraphicsPipeline(desc);
    // The flash: over everything, source x alpha + destination x source colour.
    desc.blend = rhi::BlendMode::AddModulate;
    desc.depthTest = false;
    desc.debugName = "weather flash";
    flash_ = device.CreateGraphicsPipeline(desc);
    if (!particles_ || !flash_) {
        error = "weather pipelines could not be created";
        return false;
    }
    return true;
}

void WeatherPass::Shutdown() {
    if (device_ == nullptr) return;
    if (particles_) device_->DestroyPipeline(particles_);
    if (flash_) device_->DestroyPipeline(flash_);
    particles_ = flash_ = {};
    device_ = nullptr;
}

void WeatherPass::Draw(rhi::CommandList& cmd, const glm::mat4& viewProj, const WeatherView& view, const Textures& textures,
                       const glm::vec3& right, const glm::vec3& up, const glm::vec3& level) const {
    if (view.Empty()) return;
    SR_ZONE("render: weather");
    Push push;
    push.viewProj = viewProj;
    push.right = glm::vec4(right, 0.0f);
    push.up = glm::vec4(up, 0.0f);
    push.level = glm::vec4(level, 0.0f);
    bool bound = false;
    const auto draw = [&](const std::vector<glm::vec4>& records, std::uint32_t kind, std::uint32_t texture, std::uint32_t vertices) {
        if (records.empty() || texture == 0) return;
        const std::size_t bytes = records.size() * sizeof(glm::vec4);
        const rhi::TransientAllocation alloc = device_->AllocateTransient(bytes, 16);
        if (alloc.cpu == nullptr) return;
        std::memcpy(alloc.cpu, records.data(), bytes);
        if (!bound) cmd.BindPipeline(particles_);
        bound = true;
        push.records = alloc.gpuAddress;
        push.texture = texture;
        push.kind = kind;
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(static_cast<std::uint32_t>(records.size()) * vertices, 0);
    };
    // The original's order: rain, ripples, then the two kinds of snow, then the flash.
    draw(view.rain, kRain, textures.rain, 27);
    draw(view.ripples, kRipple, textures.ripple, 6);
    draw(view.snow[0], kSnow, textures.snow[0], 27);
    draw(view.snow[1], kSnow, textures.snow[1], 27);
    if (view.Flashing()) {
        push.kind = kFlash;
        push.right = view.flash;
        cmd.BindPipeline(flash_);
        cmd.PushConstants(&push, sizeof(push));
        cmd.Draw(3, 0);
    }
}

} // namespace sr::gfx
