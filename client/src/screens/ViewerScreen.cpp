#include "screens/ViewerScreen.h"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "sr/core/Log.h"
#include "sr/core/Trace.h"
#include "sr/devui/ImGuiLayer.h"

namespace sr::client {

namespace {

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool IsTexture(std::string_view lowerPath) { return lowerPath.ends_with(".ddj") || lowerPath.ends_with(".dds"); }
bool IsEffect(std::string_view lowerPath) { return lowerPath.ends_with(".efp"); }
bool IsModel(std::string_view lowerPath) {
    return lowerPath.ends_with(".bsr") || lowerPath.ends_with(".cpd") || lowerPath.ends_with(".bms") || IsEffect(lowerPath);
}

double MiB(std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

} // namespace

void ViewerScreen::Enter(ScreenContext& context) {
    listing_ = std::make_shared<Listing>();
    // Walking every archive takes a while: a job, never the game thread.
    context.jobs.Run(
        [listing = listing_, files = &context.files] {
            SR_ZONE("viewer: list files");
            for (const std::string& path : files->ListFiles()) {
                const std::string lower = Lower(path);
                if (IsModel(lower)) listing->models.push_back(path);
                if (IsTexture(lower)) listing->textures.push_back(path);
            }
            listing->ready.store(true, std::memory_order_release);
        },
        core::JobPriority::Normal);
    animationIndex_ = context.config.GetInt("animation", -1);
    effects_.Init(context.assets, context.files, context.jobs);
    if (context.config.Has("open")) Open(context, context.config.GetString("open"));
    context.camera.Store(camera_.yaw, camera_.pitch);
}

void ViewerScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    (void)snapshot; // nothing of ours is in GPU memory outside the asset store
    Close(context);
    effects_.Shutdown();
    context.camera.Clear();
}

void ViewerScreen::Close(ScreenContext& context) {
    if (asset_) context.assets.Release(asset_);
    if (animation_) context.assets.Release(animation_);
    asset_ = {};
    animation_ = {};
    if (effect_ != 0) effects_.Stop(effect_);
    effect_ = 0;
    isEffect_ = false;
    model_.reset();
    playing_.reset();
    poseReady_ = false;
    path_.clear();
}

void ViewerScreen::Open(ScreenContext& context, const std::string& path) {
    const int keepAnimation = path_.empty() ? animationIndex_ : -1; // --animation applies to --open only
    Close(context);
    path_ = path;
    isTexture_ = IsTexture(Lower(path));
    isEffect_ = IsEffect(Lower(path));
    effectShown_ = 0;
    if (isEffect_) {
        // Framed on the origin, a little above it; played again whenever it ends (PresentEffect).
        camera_.targetX = camera_.targetZ = 0.0f;
        camera_.targetY = 10.0f;
        camera_.distance = 90.0f;
        camera_.yaw = -2.5f;
        camera_.pitch = 0.3f;
        context.camera.Store(camera_.yaw, camera_.pitch);
        return;
    }
    asset_ = isTexture_ ? context.assets.RequestTexture(path) : context.assets.RequestModel(path);
    framed_ = false;
    mip_ = -1;
    animationIndex_ = keepAnimation;
    animationTime_ = 0;
}

void ViewerScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            camera_.yaw -= io.MouseDelta.x * 0.005f;
            camera_.pitch = std::clamp(camera_.pitch + io.MouseDelta.y * 0.005f, -1.5f, 1.5f);
        }
        camera_.distance = std::clamp(camera_.distance * std::pow(0.9f, io.MouseWheel), 0.5f, 50000.0f);
    }
    context.camera.Store(camera_.yaw, camera_.pitch);

    if (context.toolsVisible) DrawBrowser(context);
    if ((context.toolsVisible || isTexture_) && !isEffect_) DrawDetails(context);
    // The animation clock runs from the open (or the pick), not from when the model or the
    // animation arrived: the pose at a given frame does not depend on load timing (repeatable
    // captures).
    if (!paused_ && asset_ && !isTexture_) animationTime_ += context.dt * 1000.0;
    if (isEffect_ && context.toolsVisible) {
        const gfx::EffectSystem::Stats& s = effects_.GetStats();
        ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 436, 16}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Effect")) {
            ImGui::TextWrapped("%s", path_.c_str());
            ImGui::Text("%zu effects, %zu elements: %zu sprites, %zu mesh instances, %zu lights", s.effects, s.elements, s.sprites,
                        s.meshInstances, s.lights);
            ImGui::Text("programs %zu, loading %zu, failed %zu", s.programs, s.loading, s.failed);
        }
        ImGui::End();
    }
    Present(context, snapshot);
    snapshot.drawImgui = true;
}

void ViewerScreen::DrawBrowser(ScreenContext& context) {
    ImGui::SetNextWindowPos({490, 16}, ImGuiCond_FirstUseEver); // right of the Engine window
    ImGui::SetNextWindowSize({380, 600}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Assets")) {
        if (!listing_->ready.load(std::memory_order_acquire)) {
            ImGui::TextUnformatted("listing the archives...");
        } else {
            if (ImGui::RadioButton("Models", !showTextures_)) showTextures_ = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Textures", showTextures_)) showTextures_ = true;
            ImGui::InputText("filter", filter_, sizeof(filter_));
            const std::vector<std::string>& list = showTextures_ ? listing_->textures : listing_->models;
            const std::string key = std::string(showTextures_ ? "t" : "m") + Lower(filter_);
            if (key != filteredFor_) {
                filteredFor_ = key;
                filtered_.clear();
                const std::string needle = Lower(filter_);
                for (std::size_t i = 0; i < list.size(); ++i) {
                    if (needle.empty() || Lower(list[i]).find(needle) != std::string::npos) filtered_.push_back(static_cast<int>(i));
                }
            }
            ImGui::Text("%zu of %zu", filtered_.size(), list.size());
            if (ImGui::BeginChild("files")) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(filtered_.size()));
                while (clipper.Step()) {
                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                        const std::string& path = list[static_cast<std::size_t>(filtered_[static_cast<std::size_t>(row)])];
                        if (ImGui::Selectable(path.c_str(), path == path_)) Open(context, path);
                    }
                }
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

void ViewerScreen::DrawDetails(ScreenContext& context) {
    gfx::AssetStore& assets = context.assets;
    // Textures get their own window, sized to the image; models a fixed one on the right.
    if (isTexture_) {
        ImGui::SetNextWindowPos({886, 16}, ImGuiCond_FirstUseEver);
    } else {
        ImGui::SetNextWindowPos({ImGui::GetIO().DisplaySize.x - 436, 16}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({420, 420}, ImGuiCond_FirstUseEver);
    }
    if (ImGui::Begin(isTexture_ ? "Texture" : "Asset", nullptr, isTexture_ ? ImGuiWindowFlags_AlwaysAutoResize : 0)) {
        if (path_.empty()) {
            ImGui::TextUnformatted("pick a model or a texture");
        } else {
            const gfx::AssetState state = assets.State(asset_);
            ImGui::TextWrapped("%s", path_.c_str());
            ImGui::Text("%s", gfx::ToString(state));
            const double ms = assets.LoadMs(asset_);
            if (ms >= 0) {
                ImGui::SameLine();
                ImGui::Text("in %.1f ms", ms);
            }
            if (state == gfx::AssetState::Failed) ImGui::TextColored({1, 0.4f, 0.4f, 1}, "%s", assets.Error(asset_).c_str());

            if (isTexture_) {
                if (const auto texture = assets.Texture(asset_)) {
                    ImGui::Text("%u x %u, %u mips, %s (%s), %.2f MiB", texture->width, texture->height, texture->mips,
                                assets::ToString(texture->format), texture->sourceFormat.c_str(), MiB(texture->bytes));
                    ImGui::SliderInt("mip", &mip_, -1, static_cast<int>(texture->mips) - 1, mip_ < 0 ? "auto" : "%d");
                    ImGui::Combo("channel", &channel_, "RGBA\0RGB\0Alpha\0Red\0Green\0Blue\0");
                    ImGui::SliderFloat("zoom", &zoom_, 0.125f, 8.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
                    devui::TextureView view;
                    view.bindlessIndex = texture->bindless;
                    view.sampler = zoom_ >= 2.0f ? rhi::SamplerIndex::PointClamp : rhi::SamplerIndex::LinearClamp;
                    view.channel = static_cast<devui::ChannelView>(channel_);
                    view.mip = mip_;
                    view.premultipliedAlpha = texture->premultipliedAlpha;
                    const float scale = mip_ > 0 ? std::ldexp(1.0f, -mip_) : 1.0f;
                    const ImVec2 size{static_cast<float>(texture->width) * scale * zoom_,
                                      static_cast<float>(texture->height) * scale * zoom_};
                    ImGui::Image(ImTextureRef(devui::MakeTextureId(view)), size);
                }
            } else if (model_) {
                ImGui::Text("%zu parts, %u triangles, %.2f MiB of geometry", model_->parts.size(), model_->triangles,
                            MiB(model_->geometryBytes));
                if (model_->skeleton) ImGui::Text("skeleton: %zu bones", model_->skeleton->bones.size());
                if (!model_->animationPaths.empty() && ImGui::CollapsingHeader("Animations", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Checkbox("paused", &paused_);
                    if (playing_) {
                        ImGui::SameLine();
                        ImGui::Text("%s, %.0f / %u ms", playing_->name.c_str(), std::fmod(animationTime_, std::max(1.0, double(playing_->durationMs))),
                                    playing_->durationMs);
                    }
                    if (ImGui::BeginChild("animations", {0, 160}, ImGuiChildFlags_Borders)) {
                        for (std::size_t i = 0; i < model_->animationPaths.size(); ++i) {
                            const std::string& path = model_->animationPaths[i];
                            const std::size_t slash = path.find_last_of('\\');
                            const char* name = path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
                            if (ImGui::Selectable(name, static_cast<int>(i) == animationIndex_)) {
                                animationIndex_ = static_cast<int>(i);
                                if (animation_) assets.Release(animation_);
                                animation_ = assets.RequestAnimation(path);
                                playing_.reset();
                                animationTime_ = 0;
                            }
                        }
                    }
                    ImGui::EndChild();
                }
                for (const std::string& w : model_->warnings) ImGui::TextColored({1, 0.8f, 0.3f, 1}, "%s", w.c_str());
            }
        }
        if (ImGui::CollapsingHeader("Store")) {
            const gfx::AssetStore::Stats s = assets.GetStats();
            ImGui::Text("assets %zu (%zu unreferenced), resident %zu, loading %zu, failed %zu", s.live, s.unreferenced, s.resident,
                        s.queued + s.loading + s.waiting + s.uploading, s.failed);
            ImGui::Text("textures %.0f / %.0f MiB, all assets %.0f MiB, %llu evicted", MiB(s.textureBytes), MiB(s.textureBudgetBytes), MiB(s.residentBytes),
                        static_cast<unsigned long long>(s.evicted));
            ImGui::Text("geometry %.0f / %.0f MiB, largest free %.0f MiB", MiB(s.geometry.used), MiB(s.geometry.capacity),
                        MiB(s.geometry.largestFree));
        }
    }
    ImGui::End();
}

void ViewerScreen::PresentEffect(ScreenContext& context, FrameSnapshot& snapshot) {
    if (effect_ == 0 || !effects_.Alive(effect_)) {
        gfx::EffectSystem::Request request;
        request.path = path_;
        request.kind = gfx::EffectSystem::Kind::World; // shown whatever the effect quality
        request.seconds = 3.0f; // kept 3 s (a flight's or an aura's look), then let finish
        effect_ = effects_.Start(request);
    }
    // The orbit camera as the scene renderer builds it.
    const float width = static_cast<float>(std::max(1u, context.device.SwapchainWidth()));
    const float height = static_cast<float>(std::max(1u, context.device.SwapchainHeight()));
    const glm::vec3 target(camera_.targetX, camera_.targetY, camera_.targetZ);
    const glm::vec3 dir(std::cos(camera_.pitch) * std::sin(camera_.yaw), std::sin(camera_.pitch), std::cos(camera_.pitch) * std::cos(camera_.yaw));
    glm::mat4 proj = glm::perspective(camera_.fovY, width / height, std::max(0.05f, camera_.distance * 0.01f), camera_.distance * 20.0f + 1000.0f);
    proj[1][1] *= -1.0f;
    gfx::EffectSystem::View view;
    view.eye = target + dir * camera_.distance;
    view.viewProj = proj * glm::lookAt(view.eye, target, glm::vec3(0, 1, 0));
    view.forward = -dir;
    effects_.Update(context.dt, view, {}, snapshot.scene.effects);
    for (const std::string& failure : effects_.TakeFailures()) core::LogWarning("viewer: {}", failure);
    const gfx::EffectSystem::Stats& stats = effects_.GetStats();
    if (stats.sprites + stats.meshInstances > 0) effectShown_ += context.dt;
    if (effectShown_ < 0.5 && stats.failed == 0) context.holdCapture = true; // a capture shows it playing
    snapshot.scene.active = true;
    snapshot.scene.camera = camera_;
}

void ViewerScreen::Present(ScreenContext& context, FrameSnapshot& snapshot) {
    gfx::AssetStore& assets = context.assets;
    if (isEffect_) {
        PresentEffect(context, snapshot);
        return;
    }
    if (isTexture_ || !asset_) return;
    if (!model_) {
        model_ = assets.Model(asset_);
        if (!model_) return;
    }
    if (!framed_) {
        // Fit the camera to the model's bounds.
        const auto& b = model_->bounds;
        const float cx = (b.min[0] + b.max[0]) * 0.5f, cy = (b.min[1] + b.max[1]) * 0.5f, cz = (b.min[2] + b.max[2]) * 0.5f;
        const float dx = b.max[0] - b.min[0], dy = b.max[1] - b.min[1], dz = b.max[2] - b.min[2];
        const float radius = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 0.5f);
        camera_.targetX = cx;
        camera_.targetY = cy;
        camera_.targetZ = cz;
        camera_.distance = radius / std::sin(camera_.fovY * 0.5f) * 1.05f;
        camera_.yaw = -2.5f; // in front of the model, a little to its right (models face -z)
        camera_.pitch = 0.2f;
        context.camera.Store(camera_.yaw, camera_.pitch);
        framed_ = true;
    }

    SceneItem item;
    item.model = asset_;
    if (model_->skeleton) {
        if (!poseReady_) {
            pose_.Init(*model_->skeleton);
            poseReady_ = true;
            if (animationIndex_ >= 0 && static_cast<std::size_t>(animationIndex_) < model_->animationPaths.size()) {
                animation_ = assets.RequestAnimation(model_->animationPaths[static_cast<std::size_t>(animationIndex_)]);
                core::LogInfo("viewer: clip {} of {}: {}", animationIndex_, model_->animationPaths.size(), model_->animationPaths[static_cast<std::size_t>(animationIndex_)]);
            }
        }
        if (animation_ && !playing_) {
            playing_ = assets.Animation(animation_);
            if (playing_) pose_.SetAnimation(playing_.get());
        }
        pose_.Evaluate(static_cast<float>(animationTime_), true);
        item.paletteOffset = static_cast<std::uint32_t>(snapshot.scene.palette.size());
        snapshot.scene.palette.insert(snapshot.scene.palette.end(), pose_.Palette().begin(), pose_.Palette().end());
    }
    snapshot.scene.active = true;
    snapshot.scene.camera = camera_;
    snapshot.scene.items.push_back(item);
}

} // namespace sr::client
