#include "screens/AssetCheckScreen.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

#include "sr/core/Log.h"
#include "sr/core/Trace.h"

namespace sr::client {

namespace {

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

double MiB(std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

} // namespace

void AssetCheckScreen::Enter(ScreenContext& context) {
    std::stringstream list(Lower(context.config.GetString("asset-check", "bsr,bms,ddj")));
    for (std::string type; std::getline(list, type, ',');) {
        if (!type.empty()) types_.push_back("." + type);
    }
    window_ = static_cast<std::size_t>(std::max(1, context.config.GetInt("asset-window", 256)));
    delay_ = context.config.GetDouble("asset-delay", 0.0); // seconds of plain frames first
    started_ = context.time;
    listing_ = std::make_shared<Listing>();
    context.jobs.Run(
        [listing = listing_, files = &context.files, types = types_] {
            SR_ZONE("asset check: list files");
            for (const std::string& path : files->ListFiles()) {
                const std::string lower = Lower(path);
                for (const std::string& type : types) {
                    if (lower.ends_with(type)) {
                        listing->paths.push_back(path);
                        break;
                    }
                }
            }
            listing->ready.store(true, std::memory_order_release);
        },
        core::JobPriority::Normal);
    camera_.yaw = -2.5f;
    camera_.pitch = 0.25f;
}

void AssetCheckScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    (void)snapshot;
    for (const InFlight& f : inFlight_) context.assets.Release(f.id);
    inFlight_.clear();
    if (shown_) context.assets.Release(shown_);
    shown_ = {};
}

void AssetCheckScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    gfx::AssetStore& assets = context.assets;
    if (!listing_->ready.load(std::memory_order_acquire) || context.time - started_ < delay_) return;
    if (streamStart_ < 0) streamStart_ = context.time;
    const std::vector<std::string>& paths = listing_->paths;

    // Keep a window of requests in flight; each finished one is counted and released (the
    // store keeps it until memory runs short, so eviction gets exercised too).
    while (!done_ && inFlight_.size() < window_ && next_ < paths.size()) {
        const std::string& path = paths[next_];
        const std::string lower = Lower(path);
        const bool texture = lower.ends_with(".ddj") || lower.ends_with(".dds");
        inFlight_.push_back({texture ? assets.RequestTexture(path) : assets.RequestModel(path), next_});
        ++next_;
    }
    std::size_t kept = 0;
    for (const InFlight& f : inFlight_) {
        const gfx::AssetState state = assets.State(f.id);
        if (state == gfx::AssetState::Resident) {
            ++resident_;
            loadMs_.push_back(assets.LoadMs(f.id));
            if (assets.Model(f.id)) {
                // Draw the newest model while the check runs.
                if (shown_) assets.Release(shown_);
                shown_ = f.id;
                continue; // its reference moves to shown_
            }
            assets.Release(f.id);
        } else if (state == gfx::AssetState::Failed) {
            ++failed_;
            failures_[assets.Error(f.id)].push_back(paths[f.path]);
            assets.Release(f.id);
        } else {
            inFlight_[kept++] = f;
        }
    }
    inFlight_.resize(kept);
    peakBytes_ = std::max(peakBytes_, assets.ResidentBytes());

    if (shown_) {
        if (const auto model = assets.Model(shown_)) {
            const auto& b = model->bounds;
            const float dx = b.max[0] - b.min[0], dy = b.max[1] - b.min[1], dz = b.max[2] - b.min[2];
            camera_.targetX = (b.min[0] + b.max[0]) * 0.5f;
            camera_.targetY = (b.min[1] + b.max[1]) * 0.5f;
            camera_.targetZ = (b.min[2] + b.max[2]) * 0.5f;
            camera_.distance = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 0.5f) / std::sin(camera_.fovY * 0.5f) * 1.05f;
            snapshot.scene.active = true;
            snapshot.scene.camera = camera_;
            SceneItem item;
            item.model = shown_; // static pose: bind pose without a palette
            snapshot.scene.items.push_back(item);
        }
    }

    if (context.toolsVisible) {
        ImGui::SetNextWindowPos({16, 16}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({460, 0}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Asset check")) {
            const gfx::AssetStore::Stats s = assets.GetStats();
            ImGui::Text("%zu / %zu requested, %zu resident, %zu failed, %zu in flight", next_, paths.size(), resident_, failed_,
                        inFlight_.size());
            ImGui::ProgressBar(paths.empty() ? 1.0f : static_cast<float>(resident_ + failed_) / static_cast<float>(paths.size()));
            ImGui::Text("textures %.0f / %.0f MiB (all assets: peak %.0f), %llu evicted", MiB(s.textureBytes), MiB(s.textureBudgetBytes), MiB(peakBytes_),
                        static_cast<unsigned long long>(s.evicted));
            ImGui::Text("geometry %.0f / %.0f MiB", MiB(s.geometry.used), MiB(s.geometry.capacity));
        }
        ImGui::End();
        snapshot.drawImgui = true;
    }

    if (!done_ && next_ >= paths.size() && inFlight_.empty()) Finish(context);
}

void AssetCheckScreen::Finish(ScreenContext& context) {
    done_ = true;
    std::sort(loadMs_.begin(), loadMs_.end());
    const auto at = [&](double q) { return loadMs_.empty() ? 0.0 : loadMs_[std::min(loadMs_.size() - 1, static_cast<std::size_t>(q * loadMs_.size()))]; };
    const gfx::AssetStore::Stats s = context.assets.GetStats();
    const rhi::Device::StreamingInfo streaming = context.device.GetStreamingInfo();
    std::string types;
    for (const std::string& t : types_) types += t + " ";
    core::LogInfo("asset check ({}): {} files in {:.1f} s: {} resident, {} failed", types, listing_->paths.size(),
                  context.time - streamStart_, resident_, failed_);
    core::LogInfo("asset check: load ms p50 {:.1f} p90 {:.1f} p99 {:.1f} worst {:.1f} (request to resident, {} in flight at most)", at(0.5),
                  at(0.9), at(0.99), loadMs_.empty() ? 0.0 : loadMs_.back(), window_);
    core::LogInfo("asset check: memory peak {:.0f} MiB (textures within a {:.0f} MiB budget), {} evicted; geometry pool {:.0f} / {:.0f} MiB used, largest free {:.0f} MiB; decoded {:.0f} MiB of files",
                  MiB(peakBytes_), MiB(s.textureBudgetBytes), s.evicted, MiB(s.geometry.used), MiB(s.geometry.capacity),
                  MiB(s.geometry.largestFree), MiB(s.decodedBytes));
    core::LogInfo("asset check: {} staging blocks uploaded, {} transfer queue", streaming.uploaded,
                  streaming.dedicatedTransferQueue ? "on a dedicated" : "on a shared");
    // The game data's own gaps, the same ones GameDataTests counts: developer test files in older
    // formats, effect-only resources without meshes, and resources naming files the archives do
    // not contain. Anything else is a failure of ours.
    const auto known = [](const std::string& reason) {
        return reason.find("not a JMXVRES 0109 file") != std::string::npos || reason == "the resource has no meshes" ||
               reason.starts_with("missing mesh") || reason.starts_with("missing material set");
    };
    std::size_t unexpected = 0;
    for (const auto& [reason, paths] : failures_) {
        if (known(reason)) {
            core::LogInfo("asset check: {} known gap(s) in the game data: {} (e.g. {})", paths.size(), reason, paths.front());
        } else {
            unexpected += paths.size();
            core::LogWarning("asset check: {} failed: {} (e.g. {})", paths.size(), reason, paths.front());
        }
    }
    core::LogInfo("asset check: {} unexpected failures", unexpected);
    context.quit = true;
    context.exitCode = unexpected > 0 ? 5 : 0;
}

} // namespace sr::client
