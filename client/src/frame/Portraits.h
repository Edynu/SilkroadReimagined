#pragma once

// The HUD's portraits: a character's head drawn into a small target of its
// own and shown by the interface through a round mask, as the 1.188 client draws the player's bar
// (CIFPlayerMiniInfo: 128 x 128 under pmi_face.ddj) and the party's bars (CIFQuickPartySlot: 64 x 64
// under qpt_face.ddj; the party page shows the same targets under pt_face.ddj).
//
// Game thread. A screen says each frame which portraits it wants and how each character looks
// (Show); Update poses a portrait and queues its drawing in the snapshot (gfx::PortraitPass draws it
// on the render thread) only when its look changed and its model and clip are in: the original
// draws one when it is asked to (a new look), never every frame, and keeps the picture in between.
// The interface draws a portrait by its texture path (PortraitTexture), an external texture of the
// screen's UiTextures, set once it is drawn.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "frame/FrameSnapshot.h"
#include "gui/PortraitTexture.h"
#include "sr/gfx/AssetStore.h"
#include "sr/rhi/Rhi.h"

namespace sr::assets {
class FileSystem;
class CharacterTable;
class ItemTable;
class VisualChangeTable;
} // namespace sr::assets

namespace sr::client {

class UiTextures;

// How a character looks in its portrait: its model (a .bsr, or an outfit "body|item|..."), the
// animation set it stands in, its size and the body's volume (as the world's stage looks).
struct PortraitLook {
    std::string model;
    std::string stance = "default";
    float height = 1.0f;
    std::vector<std::pair<std::string, float>> volume;
    bool operator==(const PortraitLook&) const = default;
};

// A player character's look from the game's tables (assets::Dress): its figure (characterdata),
// what it wears (itemdata ids) and its body shape byte. False when the figure is unknown.
bool DressPortrait(const assets::FileSystem& files, const assets::CharacterTable& characters, const assets::ItemTable& items,
                   std::uint32_t model, std::span<const std::uint32_t> worn, std::uint8_t shape, PortraitLook& out, std::string& error,
                   const assets::VisualChangeTable* visuals = nullptr);

// The original's camera and light on a portrait: it looks at the head
// bone raised by 1 from 6.5 away, turned 0.2 rad about the vertical, with a 30 degree field of view.
struct PortraitCamera {
    static constexpr const char* kHeadBone = "Bip01 Head";
    static constexpr float kRaise = 1.0f, kDistance = 6.5f, kYaw = 0.2f, kFovDegrees = 30.0f;
    // The near and far planes are the world camera's in the original (copied); ours fixed around
    // the head.
    static constexpr float kNear = 1.0f, kFar = 100.0f;
    // The animation state it is posed in: ANI_POSE (the 1.188 registry's 0x3C), at its start.
    static constexpr std::uint32_t kPoseState = 60;
};

class Portraits {
public:
    // The original's targets: the player's bar's 128 x 128, a party bar's 64 x 64.
    static constexpr std::uint32_t kLargeSize = 128, kSmallSize = 64;
    // The item mall's preview of the whole figure (its CIFRenderStatic 123, 215 x 332): a
    // target of that shape (ours: 2 : 3), the figure from its feet to above its head.
    static constexpr std::uint32_t kFigureWidth = 256, kFigureHeight = 384;
    // How many live at once (ours): the player's and seven party members' (the quick party window's
    // seven slots), and one figure.
    static constexpr std::size_t kLarge = 1, kSmall = 7, kFigure = 1;

    // Makes the targets (a screen's Enter: GPU memory is reserved before play).
    void Init(rhi::Device& device);
    // Gives back the targets (through the snapshot) and the assets, and takes the textures out of the
    // interface's.
    void Exit(gfx::AssetStore& assets, FrameSnapshot& snapshot, UiTextures* textures);

    // This frame wants the portrait `key` of a character looking like `look`, in a large target (the
    // player's bar) or a small one (a party bar). A look of no model is not drawn.
    void Show(std::uint32_t key, const PortraitLook& look, bool large);
    // The same for the whole figure (the item mall's preview, turned `yaw` radians about the vertical).
    void ShowFigure(std::uint32_t key, const PortraitLook& look, float yaw = 0.0f);
    // After the frame's Show calls: the portraits not asked for go, the new or changed ones are drawn
    // once their model and clip are in; the interface's textures follow.
    void Update(gfx::AssetStore& assets, FrameSnapshot& snapshot, UiTextures* textures);

    // Every portrait asked for shows its look in the interface (a capture waits for it), or cannot
    // be drawn.
    bool Settled() const;

    // A party member's look as the original's party bars draw it: the figure made
    // from its characterdata id alone, so as created, in its default garments (the party's record
    // names no worn items). Kept by id; a look of no model when the figure is unknown.
    const PortraitLook& FigureLook(std::uint32_t model, const assets::FileSystem& files, const assets::CharacterTable& characters,
                                   const assets::ItemTable& items);
    std::uint64_t Drawn() const { return drawn_; } // drawings so far

private:
    struct Slot {
        rhi::TextureHandle color, depth;
        bool large = false;
        bool figure = false; // the whole figure (kFigureWidth x kFigureHeight)
        float yaw = 0.0f, shownYaw = 0.0f;
        bool used = false, wanted = false;
        std::uint32_t key = 0;
        PortraitLook look;      // asked for
        PortraitLook shown;     // what the target holds (once `drawn`)
        bool drawn = false;
        bool fresh = false;     // drawn by this frame's Update (the interface shows it from the next)
        bool restart = false;   // the look changed: its assets asked for afresh
        bool failed = false;    // the look's model or skeleton cannot be had
        gfx::AssetId model, clip; // the look's, held until its drawing is recorded
        bool clipChosen = false;
        std::string clipName;
    };
    // Queues the slot's drawing once its model and clip are in (false: not yet, or failed).
    bool Draw(Slot& slot, gfx::AssetStore& assets, FrameSnapshot& snapshot);
    // Gives the slot's assets back, a few frames late (a draw queued with them may not be recorded yet).
    void Forget(Slot& slot);

    rhi::Device* device_ = nullptr;
    std::vector<Slot> slots_;
    struct Late {
        gfx::AssetId id;
        int frames = 0;
    };
    std::vector<Late> late_;
    std::uint64_t drawn_ = 0;
    std::unordered_map<std::uint32_t, PortraitLook> figures_;
};

} // namespace sr::client
