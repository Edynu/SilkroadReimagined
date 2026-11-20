#include "screens/LogoScreen.h"

#include <algorithm>

#include "frame/GameUi.h"
#include "sr/assets/Texture.h"
#include "sr/core/Log.h"
#include "sr/core/Trace.h"
#include "sr/ui/Input.h"

namespace sr::client {

namespace {

constexpr const char* kLogo = "interface\\outer\\logo-big.ddj";
constexpr double kFadeIn = 0.6, kHold = 1.6, kFadeOut = 0.6;

rhi::Format ToRhi(assets::TextureFormat format) {
    switch (format) {
    case assets::TextureFormat::RGBA8: return rhi::Format::RGBA8Unorm;
    case assets::TextureFormat::BC1: return rhi::Format::BC1Unorm;
    case assets::TextureFormat::BC2: return rhi::Format::BC2Unorm;
    case assets::TextureFormat::BC3: return rhi::Format::BC3Unorm;
    }
    return rhi::Format::Unknown;
}

} // namespace

void LogoScreen::Enter(ScreenContext& context) {
    load_ = std::make_shared<Load>();
    // A Streaming job: read, decode, create the texture; the screen polls `state`.
    context.jobs.Run(
        [load = load_, files = &context.files, device = &context.device] {
            SR_ZONE("logo: load");
            std::string error;
            assets::TextureData data;
            const auto file = files->Read(kLogo);
            if (file && assets::DecodeDdj(file->Bytes(), data, error)) {
                std::vector<std::span<const std::byte>> mips;
                for (std::size_t i = 0; i < data.mips.size(); ++i) mips.push_back(data.MipBytes(i));
                const rhi::TextureInitData init{mips};
                rhi::TextureDesc desc;
                desc.width = data.width;
                desc.height = data.height;
                desc.mipLevels = static_cast<std::uint32_t>(mips.size());
                desc.format = ToRhi(data.format);
                desc.debugName = "logo";
                load->texture = device->CreateTexture(desc, &init);
                load->width = data.width;
                load->height = data.height;
            } else {
                core::LogWarning("logo: {}", file ? error : std::string("not in the archives"));
            }
            int expected = Load::Loading;
            if (!load->state.compare_exchange_strong(expected, Load::Loaded, std::memory_order_acq_rel) && load->texture) {
                // The screen left meanwhile: no frame has used the texture.
                device->DestroyTexture(load->texture);
                load->texture = {};
            }
        },
        core::JobPriority::Streaming);
}

void LogoScreen::Exit(ScreenContext&, FrameSnapshot& snapshot) {
    if (!load_) return;
    int expected = Load::Loading;
    if (!load_->state.compare_exchange_strong(expected, Load::Abandoned, std::memory_order_acq_rel) && load_->texture) {
        snapshot.releaseTextures.push_back(load_->texture); // loaded: frames may still draw it
    }
    load_.reset();
}

void LogoScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    snapshot.clearColor = {0, 0, 0, 1};
    const bool ready = load_ && load_->state.load(std::memory_order_acquire) == Load::Loaded;
    if (ready) shown_ += context.dt;
    // Skipped with Escape, Space or a click (the game's own input; a click the tools took is theirs).
    const ui::Input* input = context.input;
    const bool skip = input != nullptr && (input->KeyPressed(ui::Key::Escape) || input->KeyPressed(ui::Key::Space) ||
                                           (input->Pressed(ui::MouseButton::Left) && !context.mouseBlocked));
    if (skip || shown_ >= kFadeIn + kHold + kFadeOut || (ready && !load_->texture)) {
        next = context.config.GetString("after-logo", "crowd");
        return;
    }
    if (!ready || !load_->texture) return;

    const double t = shown_;
    const float alpha = static_cast<float>(t < kFadeIn ? t / kFadeIn : t < kFadeIn + kHold ? 1.0 : 1.0 - (t - kFadeIn - kHold) / kFadeOut);
    // Drawn by the game's own UI pass (ui::DrawList), centred at 60 % of the screen.
    const float width = static_cast<float>(context.device.SwapchainWidth()), height = static_cast<float>(context.device.SwapchainHeight());
    const float scale = std::min(width * 0.6f / static_cast<float>(load_->width), height * 0.6f / static_cast<float>(load_->height));
    const float w = static_cast<float>(load_->width) * scale, h = static_cast<float>(load_->height) * scale;
    list_.Clear();
    list_.Image(ui::TextureRef{context.device.BindlessIndex(load_->texture), false, false}, {(width - w) * 0.5f, (height - h) * 0.5f, w, h}, {0, 0},
                {1, 1}, ui::WithAlpha(ui::kWhite, std::clamp(alpha, 0.0f, 1.0f)));
    AppendUi(snapshot.ui, list_);
}

} // namespace sr::client
