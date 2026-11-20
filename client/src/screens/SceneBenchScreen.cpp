#include "screens/SceneBenchScreen.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <sstream>

#include "sr/core/Log.h"

namespace sr::client {

namespace {

// Buildings and trees of the game: different sizes, part counts and materials.
constexpr const char* kDefaultModels =
    "res\\bldg\\china\\dunhuang\\buildings\\w_cd_inn.bsr,res\\bldg\\west asia\\wingtribe\\rock_mt_wingtribe_tree01.bsr,"
    "res\\bldg\\china\\cj_ferry\\cj_ferry_wagon.bsr,res\\bldg\\china\\cj_thiefvill\\cj_thiefvill_house_01_01.bsr";
constexpr float kSpacing = 80.0f;

std::uint32_t Hash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

} // namespace

void SceneBenchScreen::Enter(ScreenContext& context) {
    std::stringstream list(context.config.GetString("models", kDefaultModels));
    for (std::string path; std::getline(list, path, ',');) {
        if (!path.empty()) models_.push_back(context.assets.RequestModel(path));
    }
    count_ = static_cast<std::uint32_t>(std::max(1, context.config.GetInt("instances", 10000)));
    const auto side = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(count_))));
    extent_ = static_cast<float>(side) * kSpacing * 0.5f;
    camera_.distance = std::max(400.0f, extent_ * 0.5f);
    camera_.pitch = 0.35f;
    context.camera.Store(camera_.yaw, camera_.pitch);
}

void SceneBenchScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    if (placed_) {
        for (std::uint32_t i = 0; i < count_; ++i) {
            WorldCommand c;
            c.kind = WorldCommand::Kind::Remove;
            c.handle = i;
            snapshot.worldCommands.push_back(c);
        }
    }
    for (gfx::AssetId id : models_) context.assets.Release(id);
    models_.clear();
    context.camera.Clear();
}

void SceneBenchScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    if (!placed_ && !models_.empty()) {
        // Every instance once; the render thread keeps them.
        const auto side = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(count_))));
        snapshot.worldCommands.reserve(count_);
        for (std::uint32_t i = 0; i < count_; ++i) {
            const std::uint32_t h = Hash(i);
            const float yaw = static_cast<float>(h % 6283u) * 0.001f;
            const float x = (static_cast<float>(i % side) + 0.5f) * kSpacing - extent_;
            const float z = (static_cast<float>(i / side) + 0.5f) * kSpacing - extent_;
            const float c = std::cos(yaw), s = std::sin(yaw);
            WorldCommand cmd;
            cmd.handle = i;
            cmd.model = models_[h % models_.size()];
            cmd.world = {c, 0, s, x, 0, 1, 0, 0, -s, 0, c, z};
            snapshot.worldCommands.push_back(cmd);
        }
        placed_ = true;
        core::LogInfo("scene bench: {} instances of {} models over {:.0f} x {:.0f} units", count_, models_.size(),
                      extent_ * 2, extent_ * 2);
    }
    // Circle the field: what is visible changes every frame.
    camera_.yaw += static_cast<float>(context.dt) * 0.25f;
    context.camera.Store(camera_.yaw, camera_.pitch);
    snapshot.scene.active = true;
    snapshot.scene.camera = camera_;
    if (context.toolsVisible) {
        ImGui::SetNextWindowPos({490, 16}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Scene bench")) {
            std::size_t resident = 0;
            for (gfx::AssetId id : models_) resident += context.assets.IsResident(id) ? 1 : 0;
            ImGui::Text("%u instances, %zu / %zu models resident", count_, resident, models_.size());
        }
        ImGui::End();
        snapshot.drawImgui = true;
    }
}

} // namespace sr::client
