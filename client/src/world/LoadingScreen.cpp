#include "world/LoadingScreen.h"

#include <algorithm>
#include <optional>

#include "sr/game/Loading.h"

namespace sr::client {

namespace {

// The original's rectangles (x, y, w, h) in its 1600 x 1200 design space.
constexpr float kForm[4] = {241, 973, 1121, 64};
constexpr float kGauge[4] = {268, 985, 1064, 20};
constexpr float kCaption[4] = {268, 1025, 252, 35};

ui::Rect Scaled(const float (&r)[4], float width, float height) {
    const float sx = width / 1600.0f, sy = height / 1200.0f;
    return {r[0] * sx, r[1] * sy, r[2] * sx, r[3] * sy};
}

} // namespace

void LoadingScreen::Begin(gfx::AssetStore& assets, const std::string& picture) {
    if (active_) End(assets);
    active_ = true;
    progress_ = 0.0f;
    picture_ = assets.RequestTexture(picture);
    form_ = assets.RequestTexture("interface\\loading\\loading_form.ddj");
    gauge_ = assets.RequestTexture("interface\\loading\\gauge_loading.ddj");
    caption_ = assets.RequestTexture("interface\\loading\\nowloading.ddj");
}

void LoadingScreen::End(gfx::AssetStore& assets) {
    for (gfx::AssetId* id : {&picture_, &fallback_, &form_, &gauge_, &caption_}) {
        if (*id) assets.Release(*id);
        *id = {};
    }
    active_ = false;
}

void LoadingScreen::SetProgress(float progress) { progress_ = std::max(progress_, std::clamp(progress, 0.0f, 1.0f)); }

bool LoadingScreen::Ready(const gfx::AssetStore& assets) const {
    for (gfx::AssetId id : {picture_, fallback_, form_, gauge_, caption_}) {
        if (!id) continue;
        const gfx::AssetState state = assets.State(id);
        if (state != gfx::AssetState::Resident && state != gfx::AssetState::Failed) return false;
    }
    return !(picture_ && assets.State(picture_) == gfx::AssetState::Failed && !fallback_);
}

void LoadingScreen::Draw(gfx::AssetStore& assets, ui::DrawList& list, float width, float height) {
    if (!active_) return;
    // A picture the archive lacks: the China one instead (ours; the original's case was not traced).
    if (picture_ && !fallback_ && assets.State(picture_) == gfx::AssetState::Failed) {
        fallback_ = assets.RequestTexture(game::kLoadingFallback);
    }
    const auto texture = [&](gfx::AssetId id) -> std::optional<gfx::TextureInfo> { return id ? assets.Texture(id) : std::nullopt; };
    const auto ref = [](const gfx::TextureInfo& t) { return ui::TextureRef{t.bindless, t.premultipliedAlpha, false}; };
    list.SetWhite(ui::TextureRef{0, false, false}, {0.5f, 0.5f});
    list.Fill({0, 0, width, height}, ui::Rgba(0, 0, 0, 255));
    auto picture = texture(picture_);
    if (!picture) picture = texture(fallback_);
    if (picture) {
        const float w = height * 4.0f / 3.0f;
        list.Image(ref(*picture), {(width - w) * 0.5f, 0.0f, w, height});
    }
    if (const auto form = texture(form_)) list.Image(ref(*form), Scaled(kForm, width, height));
    if (const auto gauge = texture(gauge_); gauge && progress_ > 0.0f) {
        // The bar fills from the left (inferred: the gauge draws a fraction of its width).
        ui::Rect bar = Scaled(kGauge, width, height);
        bar.w *= progress_;
        list.Image(ref(*gauge), bar);
    }
    if (const auto caption = texture(caption_)) list.Image(ref(*caption), Scaled(kCaption, width, height));
}

} // namespace sr::client
