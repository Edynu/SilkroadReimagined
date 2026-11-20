#include "screens/DungeonScreen.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "frame/GameUi.h"
#include "sr/core/Log.h"
#include "sr/game/Loading.h"
#include "sr/game/NavDungeon.h"

namespace sr::client {

void DungeonScreen::Enter(ScreenContext& context) {
    const std::string which = context.config.GetString("dungeon", "0x8001");
    const unsigned long value = std::strtoul(which.c_str(), nullptr, 0);
    region_ = static_cast<std::uint16_t>(value < 0x8000 ? (0x8000 | value) : value);
    startBlock_ = context.config.GetInt("block", -1);
    glow_ = context.config.GetBool("glow", true);

    camera_.fly = true;
    camera_.fovY = 60.0f * 3.14159265f / 180.0f;
    camera_.nearZ = 1.0f;
    camera_.farZ = 5000.0f;
    camera_.pitch = -0.1f;
    const std::string place = context.config.GetString("camera", "");
    if (!place.empty()) {
        if (std::sscanf(place.c_str(), "%f,%f,%f,%f,%f", &camera_.targetX, &camera_.targetY, &camera_.targetZ, &camera_.yaw,
                        &camera_.pitch) == 5) {
            cameraPlaced_ = true;
        } else {
            core::LogWarning("--camera {}: expected x,y,z,yaw,pitch", place);
        }
    }
    context.camera.Store(camera_.yaw, camera_.pitch);

    loading_.Begin(context.assets, game::LoadingPicture(game::LoadingKind::Teleport, region_, 0));
    scene_.Load(context.files, context.jobs, region_);
    navLoader_ = std::make_unique<game::NavLoader>(context.files, context.jobs);
    navLoader_->Request(region_);
}

DungeonScene::Handles DungeonScreen::Handles() {
    return {[this] {
                if (freeHandles_.empty()) return nextHandle_++;
                const std::uint32_t h = freeHandles_.back();
                freeHandles_.pop_back();
                return h;
            },
            [this](std::uint32_t h) { freeHandles_.push_back(h); }};
}

void DungeonScreen::Start() {
    started_ = true;
    if (!scene_.Error().empty()) {
        core::LogError("dungeon: {}", scene_.Error());
        return;
    }
    const assets::DungeonData& data = scene_.Data();
    const game::NavDungeon* nav = nav_.FindDungeon(region_);
    // The start block: --block, else the first entrance.
    std::int32_t start = startBlock_;
    if (start < 0 || start >= static_cast<std::int32_t>(data.blocks.size())) {
        start = 0;
        for (std::size_t b = 0; b < data.blocks.size(); ++b) {
            if (data.blocks[b].flags & 1) {
                start = static_cast<std::int32_t>(b);
                break;
            }
        }
    }
    startBlock_ = start;
    if (!cameraPlaced_ && !data.blocks.empty() && nav != nullptr) {
        // Over the middle of its floor at about a character's eye height (dungeon ceilings are low),
        // looking north.
        const assets::DungeonData::Block& block = data.blocks[static_cast<std::size_t>(start)];
        const game::NavDungeon::Block& b = nav->Blocks()[static_cast<std::size_t>(start)];
        float x = 0, z = 0;
        b.ToWorld(0.5f * (block.boundsMin[0] + block.boundsMax[0]), 0.5f * (block.boundsMin[2] + block.boundsMax[2]), x, z);
        const game::NavPosition floor = nav->Drop(x, block.position[1] + block.boundsMin[1] + 20.0f, z);
        camera_.targetX = x;
        camera_.targetY = (floor.Valid() ? floor.y : block.position[1]) + 30.0f;
        camera_.targetZ = z;
        cameraPlaced_ = true;
    }
    core::LogInfo("dungeon: {} ({:#06x}): {} blocks, {} doorway edges; start block {} ({})", scene_.Path(), data.regionId, data.blocks.size(),
                  nav ? nav->Doors() : 0, start, data.blocks.empty() ? std::string() : data.blocks[static_cast<std::size_t>(start)].name);
}

void DungeonScreen::Fly(ScreenContext& context) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        camera_.yaw += io.MouseDelta.x * 0.005f;
        camera_.pitch = std::clamp(camera_.pitch - io.MouseDelta.y * 0.005f, -1.5f, 1.5f);
    }
    if (!io.WantCaptureKeyboard) {
        const float cp = std::cos(camera_.pitch);
        const float fx = cp * std::sin(camera_.yaw), fy = std::sin(camera_.pitch), fz = cp * std::cos(camera_.yaw);
        const float rx = std::cos(camera_.yaw), rz = -std::sin(camera_.yaw);
        float forward = 0, right = 0, up = 0;
        if (ImGui::IsKeyDown(ImGuiKey_W)) forward += 1;
        if (ImGui::IsKeyDown(ImGuiKey_S)) forward -= 1;
        if (ImGui::IsKeyDown(ImGuiKey_D)) right += 1;
        if (ImGui::IsKeyDown(ImGuiKey_A)) right -= 1;
        if (ImGui::IsKeyDown(ImGuiKey_E)) up += 1;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) up -= 1;
        const float step = static_cast<float>(context.dt) * (io.KeyShift ? 600.0f : 150.0f);
        camera_.targetX += (fx * forward + rx * right) * step;
        camera_.targetY += (fy * forward + up) * step;
        camera_.targetZ += (fz * forward + rz * right) * step;
    }
    context.camera.Store(camera_.yaw, camera_.pitch);
}

void DungeonScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    scene_.Clear(context.assets, snapshot.worldCommands, Handles());
    nav_.Clear();
    navLoader_.reset(); // waits for its jobs
    loading_.End(context.assets);
    context.camera.Clear();
}

void DungeonScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    snapshot.scene.active = true;
    snapshot.scene.clearColor = {0, 0, 0, 1};
    navLoader_->Update(nav_);
    if (!scene_.Ready() || navLoader_->Loading(region_)) {
        context.holdCapture = true;
        snapshot.scene.camera = camera_;
        return;
    }
    if (!started_) Start();
    const game::NavDungeon* nav = nav_.FindDungeon(region_);
    if (!scene_.Error().empty() || nav == nullptr) {
        snapshot.scene.camera = camera_;
        loading_.End(context.assets);
        return;
    }
    if (!loading_.Active()) Fly(context);
    // The block whose floor is under the camera (the original takes the player's); the start block
    // until one is found.
    std::int32_t current = DungeonScene::BlockUnder(*nav, camera_.targetX, camera_.targetY, camera_.targetZ);
    if (current < 0 && scene_.Current() < 0) current = startBlock_;
    scene_.Show(context.assets, current, snapshot.worldCommands, Handles());
    scene_.Light(snapshot.scene);
    snapshot.scene.glow = glow_;
    snapshot.scene.frameSeconds = static_cast<float>(context.dt);
    snapshot.scene.seconds = context.time;
    snapshot.scene.camera = camera_;

    const bool settled = scene_.Settled(context.assets);
    if (loading_.Active()) {
        loading_.SetProgress(scene_.Progress(context.assets));
        if (settled) {
            core::LogInfo("dungeon: loading screen closed at {:.2f} s", context.time);
            loading_.End(context.assets);
        } else {
            loadingUi_.Clear();
            loading_.Draw(context.assets, loadingUi_, static_cast<float>(context.device.SwapchainWidth()),
                          static_cast<float>(context.device.SwapchainHeight()));
            AppendUi(snapshot.ui, loadingUi_);
            snapshot.scene.active = false;
            context.holdCapture = true;
            return;
        }
    }
    if (!settled) context.holdCapture = true;
    const assets::DungeonData::Block& block = scene_.Data().blocks[static_cast<std::size_t>(std::max(scene_.Current(), 0))];
    if (settled && !reported_) {
        reported_ = true;
        core::LogInfo("dungeon: block {} ({}) and {} visible blocks drawn: {} instances, {} models, in {:.2f} s", scene_.Current(), block.name,
                      scene_.Shown() - 1, scene_.Instances(), scene_.Models(), context.time);
    }

    if (context.toolsVisible) {
        ImGui::SetNextWindowPos({490, 16}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Dungeon")) {
            ImGui::Text("%s (%#06x)", scene_.Path().c_str(), scene_.Data().regionId);
            ImGui::Text("block %d %s%s, %zu drawn, %zu instances", scene_.Current(), block.name.c_str(), (block.flags & 1) ? " (entrance)" : "",
                        scene_.Shown(), scene_.Instances());
            ImGui::Text("fog %.0f..%.0f density %.4f", block.fog.start, block.fog.end, block.fog.density);
            ImGui::Text("camera %.0f %.0f %.0f  yaw %.2f pitch %.2f", camera_.targetX, camera_.targetY, camera_.targetZ, camera_.yaw,
                        camera_.pitch);
        }
        ImGui::End();
    }
}

} // namespace sr::client
