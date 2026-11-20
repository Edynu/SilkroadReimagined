#include "frame/Portraits.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "gui/UiData.h"
#include "sr/anim/Pose.h"
#include "sr/assets/Animation.h"
#include "sr/assets/Dress.h"
#include "sr/assets/Model.h"
#include "sr/core/Log.h"
#include "sr/core/Trace.h"

namespace sr::client {

namespace {

// Frames an asset is still held after the draw that used it was queued: the render thread may not
// have recorded it yet (the pipeline is one frame deep, the GPU two behind).
constexpr int kLateFrames = 4;

// A model standing at the origin unturned: models face -z, the portrait camera's side (the original's
// character at yaw 0 faces its camera); `height` its scale.
std::array<float, 12> FacingWorld(float height) {
    return {height, 0, 0, 0, 0, height, 0, 0, 0, 0, height, 0};
}

} // namespace

bool DressPortrait(const assets::FileSystem& files, const assets::CharacterTable& characters, const assets::ItemTable& items,
                   std::uint32_t model, std::span<const std::uint32_t> worn, std::uint8_t shape, PortraitLook& out, std::string& error,
                   const assets::VisualChangeTable* visuals) {
    assets::DressedCharacter dressed;
    if (!assets::Dress(files, characters, items, model, worn, shape, dressed, error, {}, nullptr, visuals)) return false;
    out.model = dressed.Key();
    out.stance = dressed.stance;
    out.height = dressed.height;
    out.volume = dressed.volume;
    return true;
}

void Portraits::Init(rhi::Device& device) {
    device_ = &device;
    slots_.resize(kLarge + kSmall + kFigure);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        s.large = i < kLarge;
        s.figure = i >= kLarge + kSmall;
        const std::uint32_t size = s.large ? kLargeSize : kSmallSize;
        rhi::TextureDesc color;
        color.width = s.figure ? kFigureWidth : size;
        color.height = s.figure ? kFigureHeight : size;
        color.format = rhi::Format::RGBA8Unorm;
        color.usage = rhi::TextureUsage::RenderTarget;
        color.debugName = "portrait";
        s.color = device.CreateTexture(color);
        rhi::TextureDesc depth = color;
        depth.format = rhi::Format::D32Float;
        depth.usage = rhi::TextureUsage::DepthStencil;
        depth.debugName = "portrait depth";
        s.depth = device.CreateTexture(depth);
    }
}

void Portraits::Exit(gfx::AssetStore& assets, FrameSnapshot& snapshot, UiTextures* textures) {
    for (Slot& s : slots_) {
        if (s.used && textures != nullptr) textures->RemoveExternal(PortraitTexture(s.key));
        if (s.model) assets.Release(s.model);
        if (s.clip) assets.Release(s.clip);
        snapshot.releaseTextures.push_back(s.color);
        snapshot.releaseTextures.push_back(s.depth);
    }
    slots_.clear();
    // A screen's last snapshot is recorded before the next screen's work: nothing draws these again.
    for (const Late& l : late_) assets.Release(l.id);
    late_.clear();
    device_ = nullptr;
}

void Portraits::ShowFigure(std::uint32_t key, const PortraitLook& look, float yaw) {
    if (look.model.empty()) return;
    for (Slot& s : slots_) {
        if (!s.figure || (s.used && s.key != key)) continue;
        s.used = true;
        s.key = key;
        s.wanted = true;
        s.yaw = yaw;
        if (!(s.look == look)) {
            s.look = look;
            s.restart = true;
            s.failed = false;
        }
        return;
    }
}

void Portraits::Show(std::uint32_t key, const PortraitLook& look, bool large) {
    if (look.model.empty()) return;
    Slot* slot = nullptr;
    for (Slot& s : slots_) {
        if (s.used && s.key == key && s.large == large && !s.figure) slot = &s;
    }
    if (slot == nullptr) {
        // A free target of its size; none: not shown (more at once than the cap).
        for (Slot& s : slots_) {
            if (!s.used && s.large == large && !s.figure) {
                slot = &s;
                break;
            }
        }
        if (slot == nullptr) return;
        slot->used = true;
        slot->key = key;
    }
    slot->wanted = true;
    if (!(slot->look == look)) {
        // A new look: drawn afresh once its model is in; the target keeps the old picture until then
        // (as the original's retries keep it).
        slot->look = look;
        slot->restart = true;
        slot->failed = false;
    }
}

void Portraits::Forget(Slot& slot) {
    for (gfx::AssetId* id : {&slot.model, &slot.clip}) {
        if (*id) late_.push_back({*id, kLateFrames});
        *id = {};
    }
    slot.clipChosen = false;
    slot.clipName.clear();
}

void Portraits::Update(gfx::AssetStore& assets, FrameSnapshot& snapshot, UiTextures* textures) {
    SR_ZONE("game: portraits");
    for (Late& l : late_) {
        if (--l.frames <= 0) assets.Release(l.id);
    }
    std::erase_if(late_, [](const Late& l) { return l.frames <= 0; });
    for (Slot& s : slots_) {
        s.fresh = false;
        if (!s.used) continue;
        if (!s.wanted) {
            // Not asked for this frame: it goes, its target free for another.
            if (textures != nullptr) textures->RemoveExternal(PortraitTexture(s.key));
            Forget(s);
            s = Slot{s.color, s.depth, s.large, s.figure};
            continue;
        }
        s.wanted = false;
        if (s.restart) {
            Forget(s);
            s.restart = false;
        }
        if (s.failed || (s.drawn && s.shown == s.look && s.shownYaw == s.yaw)) continue;
        if (!Draw(s, assets, snapshot)) continue;
        // Drawn: the interface shows its target from this frame on (the render thread draws it before
        // the UI that samples it); the assets go once that draw is recorded.
        Forget(s);
        if (textures != nullptr) {
            UiTextures::Ref ref;
            ref.texture = ui::TextureRef{device_->BindlessIndex(s.color), false, true};
            ref.width = ref.height = float(s.large ? kLargeSize : kSmallSize);
            if (s.figure) {
                ref.width = float(kFigureWidth);
                ref.height = float(kFigureHeight);
            }
            ref.resident = true;
            textures->SetExternal(PortraitTexture(s.key), ref);
        }
    }
}

bool Portraits::Draw(Slot& s, gfx::AssetStore& assets, FrameSnapshot& snapshot) {
    if (!s.model) s.model = assets.RequestModel(s.look.model, 0);
    const std::shared_ptr<const gfx::ModelInfo> info = assets.Model(s.model);
    if (!info) {
        if (assets.State(s.model) == gfx::AssetState::Failed) {
            core::LogWarning("portraits: {} cannot be drawn: {}", s.look.model, assets.Error(s.model));
            s.failed = true;
        }
        return false;
    }
    if (!info->skeleton) {
        core::LogWarning("portraits: {} has no skeleton", s.look.model);
        s.failed = true;
        return false;
    }
    // Its clip: ANI_POSE in its stance's set, else in the empty-handed one; a figure without it
    // stands (STAND1; ours, the original's model would keep the clip it had).
    if (!s.clipChosen) {
        s.clipChosen = true;
        assets::ResourceData sets;
        sets.animationPaths = info->animationPaths;
        sets.animationSets = info->animationSets;
        for (const std::uint32_t state : {PortraitCamera::kPoseState, 0u}) {
            s.clipName = sets.StateAnimation(s.look.stance, state);
            if (s.clipName.empty()) s.clipName = sets.StateAnimation("default", state);
            if (!s.clipName.empty()) break;
        }
        if (!s.clipName.empty()) s.clip = assets.RequestAnimation(s.clipName, 0);
    }
    std::shared_ptr<const assets::AnimationData> clip;
    if (s.clip) {
        clip = assets.Animation(s.clip);
        if (!clip && assets.State(s.clip) != gfx::AssetState::Failed) return false; // still loading
    }

    // Posed at the clip's start, with the body's volume.
    const assets::SkeletonData& skeleton = *info->skeleton;
    anim::SkeletonPose pose;
    pose.Init(skeleton);
    pose.SetAnimation(clip.get());
    std::vector<float> radial(skeleton.bones.size(), 1.0f);
    for (const auto& [bone, factor] : s.look.volume) {
        const int b = skeleton.Find(bone);
        if (b >= 0) radial[static_cast<std::size_t>(b)] = factor;
    }
    pose.SetRadialScale(std::move(radial));
    pose.Evaluate(0.0f, true);

    gfx::PortraitDraw draw;
    draw.color = s.color;
    draw.depth = s.depth;
    draw.model = s.model;
    draw.world = FacingWorld(s.look.height);
    draw.palette = pose.Palette();
    // The camera on the head: the bone's place as the model stands, raised by 1; the eye 6.5 back
    // towards -z, turned 0.2 rad about the vertical (builds the eye so).
    glm::vec3 head(0.0f, 16.0f, 0.0f); // no head bone: about where a figure's head is
    const int bone = skeleton.Find(PortraitCamera::kHeadBone);
    if (bone >= 0 && static_cast<std::size_t>(bone) < pose.World().size()) head = glm::vec3(pose.World()[static_cast<std::size_t>(bone)][3]);
    const auto& w = draw.world;
    const glm::vec3 placed(w[0] * head.x + w[1] * head.y + w[2] * head.z + w[3], w[4] * head.x + w[5] * head.y + w[6] * head.z + w[7],
                           w[8] * head.x + w[9] * head.y + w[10] * head.z + w[11]);
    glm::vec3 at = placed + glm::vec3(0.0f, PortraitCamera::kRaise, 0.0f);
    glm::vec3 back(std::sin(PortraitCamera::kYaw) * PortraitCamera::kDistance, 0.0f, std::cos(PortraitCamera::kYaw) * PortraitCamera::kDistance);
    float fov = PortraitCamera::kFovDegrees, aspect = 1.0f, nearPlane = PortraitCamera::kNear, farPlane = PortraitCamera::kFar;
    if (s.figure) {
        // The whole figure (ours: the original's preview camera,'s 13.0 / 35.0 / 500.0, not read whole):
        // the middle of the figure, the eye far enough back for the figure and a little room to fill the height.
        const float top = std::max(placed.y + 2.0f, w[7] + 4.0f), bottom = w[7];
        fov = 35.0f;
        aspect = float(kFigureWidth) / float(kFigureHeight);
        const float distance = (top - bottom) * 0.58f / std::tan(glm::radians(fov) * 0.5f);
        at = glm::vec3(w[3], (top + bottom) * 0.5f, w[11]);
        back = glm::vec3(std::sin(s.yaw) * distance, 0.0f, std::cos(s.yaw) * distance);
        nearPlane = 1.0f;
        farPlane = 500.0f;
    }
    const glm::mat4 view = glm::lookAt(at - back, at, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);
    proj[1][1] *= -1.0f; // Vulkan's y points down
    draw.viewProj = proj * view;
    snapshot.portraits.push_back(std::move(draw));

    core::LogInfo("portraits: {} drawn ({}, {}, {})", s.key, s.look.model, clip ? s.clipName : std::string("unposed"),
                  bone >= 0 ? "on its head bone" : "no head bone");
    s.shown = s.look;
    s.shownYaw = s.yaw;
    s.drawn = true;
    s.fresh = true;
    ++drawn_;
    return true;
}

const PortraitLook& Portraits::FigureLook(std::uint32_t model, const assets::FileSystem& files, const assets::CharacterTable& characters,
                                          const assets::ItemTable& items) {
    const auto [it, added] = figures_.try_emplace(model);
    if (added) {
        std::string error;
        if (!DressPortrait(files, characters, items, model, {}, assets::kNeutralShape, it->second, error)) {
            core::LogWarning("portraits: figure {}: {}", model, error);
            it->second = {};
        }
    }
    return it->second;
}

bool Portraits::Settled() const {
    for (const Slot& s : slots_) {
        // A portrait drawn this frame shows from the next one's interface on.
        if (s.used && !s.failed && (s.fresh || !(s.drawn && s.shown == s.look))) return false;
    }
    return true;
}

} // namespace sr::client
