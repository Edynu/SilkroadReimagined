#pragma once

// The outdoor world as test scenes: from `--region X,Y`
// (default 78,105, Constantinople) the regions within `--world-radius R` (default 2) of the camera
// stream in as it moves (WorldStreamer); their terrain is drawn by the terrain pass and their
// objects become world instances. A fly camera starts where the prototype's World Viewer puts it:
// above the region's south edge, looking north and down. `--camera x,y,z,yaw,pitch` places it
// elsewhere (relative to the origin region's south-west corner). Right mouse drag looks around,
// WASD / QE fly (Shift faster). `--flight-to X,Y` flies the camera to that region at
// `--flight-speed` units a second (default 150: 3 x the run speed), 250 units over the ground.
//
// `--characters N` (C3d) adds N animated characters and monsters (`--character-models a.bsr,...`)
// in the origin region: world instances skinned on the GPU, posed on the workers into this snapshot
// slot's palette buffer, each with a blob shadow. They walk on the navigation (C4b): dropped onto
// the ground, each wanders towards random points nearby, over terrain cells and onto bridges and
// decks, and turns when something blocks it.
//
// The time of day (C3e): the original's game clock (50 game seconds a real second) from `--clock H`
// (default 12), or a fixed environment time with `--time-of-day t` (0..1: 0.25 sunrise, 0.5 noon,
// 0.75 sunset); `--day N` the clock's day (the moon's phase); `--view-distance 0..4` the
// original's presets (the fog range). The environment is the entry of the block under the camera.
// `--glow`, `--lens-flare` (both on by default) switch the original's glow and lens flare (C3g).
// `--hud` draws a small panel through the game UI pass (C3h) in the game's own font.
//
// Effects (V1): `--effect PATH` plays an .efp (again whenever it ends) on each of the first
// `--effects N` characters (default all of them), or in front of the camera without characters;
// `--effect-quality 0..2` the original's effect quality. `--effect-bone B` hangs it on a bone of
// theirs and keeps it ("name@item.bsr" a worn item's own bone: `--character-models` given an outfit
// "body|item|..." and `--effect-bone "ai_end@res\item\china\weapon\sword_01.bsr"` shows a rare
// sword's glow), `--effect-scale S` scales it.
//
// The crowd (`--dress-walkers`, on by default with `--characters N`; WorldCrowd.cpp): a share
// (`--walker-players F`, 0.5) of the walkers are the game's player figures (Chinese and European,
// men and women) in random equipment of their race and sex (some of it Seal of Star / Moon / Sun,
// glowing; some +N), dressed as the game screen dresses players; the rest are the monsters of the
// `--character-models` list as characterdata rows, or with `--walker-monsters K` K kinds of general
// monsters drawn from characterdata (the same every run), taken in turn. Every few seconds (`--walker-skill-every S`, 6)
// each plays a random skill of its weapon (a monster one of its own) with its clips and effects,
// now and then on a walker nearby (its hit effect, blood and flinch); some players wear a looping
// buff. `--dress-walkers false`: the plain models of the list, walking. `--characters-max M`
// plans a pool of M walkers of which `--characters N` show at first; the World tools' Players and
// Monsters sliders show and hide the pool's walkers while it runs.
//
// The menu (the screen on its own, not under the game screen; WorldMenu.cpp): Escape opens the
// original's system menu, with the Option window (the look changed while the scene runs) and Exit.
//
// Weather (C4c): `--weather rain|snow|clear` with `--weather-amount 0..255` (default 128; the
// original's server event snows at 20) sets game::Weather, as the original's developer commands
// do; its effective amount ramps over 5 s, the particles fall around the camera (WeatherEffect)
// and the environment greys. `--lightning-at S` strikes once at S seconds (for captures).
//
// Loading screens (C4e): the original's picture and gauge stay up while a place loads, at the
// first entry and after `--teleport-to X,Y` (at `--teleport-at S` seconds, default 5), which
// restarts the streamer at the destination; nothing else runs meanwhile. `--capture-loading`
// lets a capture take the loading screen itself.
//
// A dungeon: an origin region with bit 15 set (its y 128 or more: the game
// screen's arrival in one) shows that dungeon instead of the outdoor world (world/DungeonScene: the
// block of the floor under SetFocus's place, else the camera's, and its visible list; the block's
// light and fog; no sky, terrain or weather). Positions are the dungeon's own (OriginRelative gives
// them back as they are); its navigation (the NavLoader's game::NavDungeon) walks and picks.

#include <atomic>
#include <cstdint>
#include <memory>
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "frame/FrameStats.h"
#include "frame/GameUi.h"
#include "screens/Screen.h"
#include "world/DungeonScene.h"
#include "world/LoadingScreen.h"
#include "world/WeatherEffect.h"
#include "world/WorldStreamer.h"
#include "sr/anim/Pose.h"
#include "sr/assets/Environment.h"
#include "sr/assets/Terrain.h"
#include "sr/gfx/AssetStore.h"
#include "sr/gfx/Effects.h"
#include "sr/gfx/Environment.h"
#include "sr/game/GameClock.h"
#include "sr/game/Loading.h"
#include "sr/game/NavLoader.h"
#include "sr/game/WorldProtocol.h"
#include "sr/game/Weather.h"

namespace sr::client {

class WorldScreen final : public Screen {
public:
    // As the title's backdrop (C5e): the world around the origin region, seen from the view its
    // owner sets each frame; no input, loading screen, test options or tools window.
    struct Backdrop {
        int originX = 78, originY = 105;
        std::size_t stageSlots = 0; // characters SetStage can place (the character select's dock)
        bool glow = true;           // the post pass's glow (the character select's stage has none)
        float fovY = 0.0f;          // the camera's vertical field of view in radians (0: the world's 60 degrees)
    };
    WorldScreen() = default;
    explicit WorldScreen(Backdrop backdrop) : backdrop_(backdrop) {}
    // A fly camera's eye (relative to the origin region's south-west corner) and angles.
    void SetView(float x, float y, float z, float yaw, float pitch) {
        camera_.targetX = x;
        camera_.targetY = y;
        camera_.targetZ = z;
        camera_.yaw = yaw;
        camera_.pitch = pitch;
        cameraPlaced_ = true;
    }
    // The follow camera's spring (ours): how far along the segment from `from` (the character's chest) to `to` (the eye
    // it wants; origin-relative) the eye may go without standing inside an object's box (1: all the way;
    // WorldStreamer::CameraLimit).
    float CameraLimit(const glm::vec3& from, const glm::vec3& to) const { return assets_ != nullptr ? streamer_.CameraLimit(*assets_, from, to) : 1.0f; }
    // Every region the camera wants, and its objects, resident (or failed).
    bool Settled() const { return settled_; }
    // A dungeon is shown (the origin region is one); its region.
    bool InDungeon() const { return dungeon_ != nullptr; }
    std::uint16_t DungeonRegion() const { return dungeonRegion_; }
    // The block shown in a dungeon follows this place's floor (the player's: the original takes the
    // player's navigation cell), else the camera's.
    void SetFocus(const game::proto::WorldPosition& p) { focus_ = p; }
    // The dungeon's current block (-1: none, or not in a dungeon) and the dungeon's data while it is read.
    std::int32_t DungeonBlock() const { return dungeon_ ? dungeon_->Current() : -1; }
    const assets::DungeonData* Dungeon() const { return dungeon_ && dungeon_->Ready() && dungeon_->Error().empty() ? &dungeon_->Data() : nullptr; }
    // How far the streaming around the camera is, 0..1 (a loading gauge).
    float StreamProgress(const gfx::AssetStore& assets) const { return dungeon_ ? dungeon_->Progress(assets) : streamer_.Progress(assets); }
    const OrbitCamera& Camera() const { return camera_; }
    // The time of day shown, 0..1 (0 midnight, 0.5 noon): --time-of-day, else the game clock.
    float TimeOfDay() const { return fixedTime_ >= 0.0f ? std::min(fixedTime_, 1.0f) : clock_.TimeOfDay(); }
    // The in-game view (C6c), in the world's coordinates (a region and a place in it):
    // where a point is relative to the origin region,
    glm::vec3 OriginRelative(const game::proto::WorldPosition& p) const {
        return {float((p.region & 0xFF) - originX_) * assets::kRegionSize + p.x, p.y,
                float((p.region >> 8) - originY_) * assets::kRegionSize + p.z};
    }
    // the ground there near its height (false while that region's navigation is not in),
    bool GroundHeight(const game::proto::WorldPosition& p, float& y) const;
    // the navigation itself (the walkers' floors),
    const game::NavWorld& Navigation() const { return navWorld_; }
    // and the click: the camera's ray through a screen point against the terrain and the objects'
    // navmeshes, the nearest hit within 1,000 ("The click pick").
    // `toward`: when the ray meets nothing (the sky, land beyond reach), a point along its heading from the walker
    // instead (a click there still walks; `sky` set then).
    bool Pick(float screenX, float screenY, float width, float height, game::proto::WorldPosition& out, bool toward = false,
              bool* sky = nullptr) const;
    // Whether that ray passes through an upright cylinder standing on `base` (a gate's crystal),
    // and how far along it (to pick the nearest).
    bool RayHitsCylinder(float screenX, float screenY, float width, float height, const game::proto::WorldPosition& base, float radius,
                         float tall, float& distance) const;
    // Whether that ray meets a stage model's box (its model's bounds placed by its transform, its scale too) and how far
    // along it: the original's entity pick, only a model drawn
    // (within the view distance) and within the ray's 5,000.
    bool RayHitsStage(float screenX, float screenY, float width, float height, std::size_t slot, float& distance) const;
    // The clicks' occlusion (ours; the original has none): the pixel whose scene depth
    // the renderer reads back each frame (the mouse's), and whether something drawn there stands nearer than
    // `distance` along the ray of Pick (an entity's box hit behind a wall). False while the depth is not known, or when
    // the read pixel is not the cursor's (the mouse moved since).
    void SetProbePoint(float x, float y) { probePoint_ = {x, y}; }
    bool Occluded(float cursorX, float cursorY, float width, float height, float distance) const;
    // The other way: where a point `lift` above a place is on the screen (pixels); false behind
    // the camera.
    bool Project(const game::proto::WorldPosition& p, float lift, float width, float height, float& x, float& y) const;
    // And its depth there (0..1, as the scene's depth target holds it).
    bool Project(const game::proto::WorldPosition& p, float lift, float width, float height, float& x, float& y, float& depth) const;
    // The stage (C5e): a model standing on a slot (origin-relative, `yaw` 0 facing +z) playing the
    // standing clip of its stance; a slot changes look or place, or is cleared. A new look replaces
    // the old one once it is loaded (no gap while a dressed model is assembled).
    struct StageLook {
        std::string model;              // a .bsr, or an outfit "body|item|..." (assets::DressedCharacter::Key)
        std::string stance = "default"; // the animation set it stands in
        std::uint32_t state = 0;        // its state in that set (0 standing; 14 sitting)
        // The set its state's clip comes from when not its stance (a skill's row's group, skillaniset2
        // col 5: the 1.188 client plays a skill's clips from it).
        std::string clipSet;
        std::uint32_t take = 0;         // a new value plays the state's clip again from its start (a repeated swing)
        float height = 1.0f;            // a uniform scale
        // Grown or shrunk by a presentation (SCT_CHAR_SCALE: berserk's 1.1 and 1.2), times the height; a change does not
        // make the model again.
        float grow = 1.0f;
        std::vector<std::pair<std::string, float>> volume; // bone -> thickness
        bool shadow = true;
        bool overlay = false; // drawn in the scene's overlay, over the UI under it (character windows)
        bool held = false;    // its pose held where it is (frozen)
        bool hideWeapon = false; // its weapon and shield hidden (a skill's hide-weapon flag, a thrown weapon)
        // Its spine turned towards a target above or below (a skill's spine aim): axis times radians.
        glm::vec3 spine{0.0f};
        // A one-shot (gold's coin shower): its clip from the first frame when it appears, its root bone
        // free to move (a walker's is kept in place).
        bool once = false;
        // A hit's flinch (the 1.188 client's: its stance's DAMAGE1, four bones, Spine1, Neck, Neck1, Head, 500 ms, in at
        // once, out over 200 ms): a new value plays it as a one-shot over
        // whatever plays (the mixer averages it with a swing's: half and half on its bones).
        std::uint32_t flinch = 0;
        // The cyclic clip under `state` (the mixer's cyclic layer): standing, running, the combat stance, a
        // body lying or dead (its stance's state). kNoState: none given (a looping state's clip is the cycle; another
        // plays once over the cycle running).
        static constexpr std::uint32_t kNoState = 0xFFFFFFFFu;
        std::uint32_t cycle = kNoState;
        std::string cycleSet; // the set its cycle comes from (a skill's wait loop under its READY: P18; empty the stance)
        bool operator==(const StageLook&) const = default;
    };
    void SetStage(std::size_t slot, const StageLook& look, float x, float y, float z, float yaw);
    // How much of a stage character shows (1 whole, 0 gone): fading in as it appears, out as it goes.
    void SetStageOpacity(std::size_t slot, float opacity);
    // The weather (the server's, where the player stands): eased into over game::Weather::kRampSeconds.
    void SetWeather(game::WeatherKind kind, int amount) { weather_.Set(kind, amount); }
    // The game clock: the server's (proto::WorldClock) or one carried from the last place; it runs on from there.
    void SetClock(double hours, int day) { clock_.Set(hours, day); }
    double ClockHours() const { return clock_.Hours(); }
    int ClockDay() const { return clock_.Day(); }
    // Its model's material colour now (a skill's SCT_MAT: every light on it times the colour; null: none).
    void SetStageTint(std::size_t slot, const glm::vec3* colour);
    void SetStage(std::size_t slot, const std::string& model, float x, float y, float z, float yaw) {
        SetStage(slot, StageLook{model}, x, y, z, yaw);
    }
    // A stage slot placed by a whole transform instead (3 x 4, rows; origin-relative), until SetStage.
    void SetStageTransform(std::size_t slot, const std::array<float, 12>& world);
    // Whether a slot shows its latest look.
    bool StageReady(std::size_t slot) const;
    void ClearStage(std::size_t slot);
    // A model put down once, scaled (the pedestal and its idols).
    void AddProp(const std::string& model, float x, float y, float z, float yaw, float scale);
    // Items lying on the ground (C7i): a static model by key at an origin-relative place, shown once
    // it is in (never waited for), taken away by the same key. Not `shown`: its model is asked for now
    // but it stays hidden until ShowGroundItem (gold's heap under its coin shower).
    void SetGroundItem(std::uint32_t key, const std::string& model, float x, float y, float z, float yaw, bool shown = true);
    void ShowGroundItem(std::uint32_t key);
    // How a ground item stands at that place (rows of a 3x4 matrix): a stage slot put there with
    // SetStageTransform stands as the item would.
    static std::array<float, 12> GroundPlace(float x, float y, float z, float yaw);
    void ClearGroundItem(std::uint32_t key);
    bool HasGroundItem(std::uint32_t key) const { return ground_.contains(key); }
    // A model in flight (an arrow): at `world` (rows of a 3x4 matrix, relative to the origin region),
    // moved by each call; ClearFlying takes it away.
    void SetFlying(std::uint32_t key, const std::string& model, const std::array<float, 12>& world);
    void ClearFlying(std::uint32_t key);
    bool HasFlying(std::uint32_t key) const { return flying_.contains(key); }
    // A prop's box in the world (origin-relative), once its model is in; false before.
    bool PropBounds(std::size_t index, assets::Bounds& out) const;
    // Effects (V1, gfx/Effects.h), stepped and drawn with the world. Anchors name a stage slot
    // (StageAnchor) or a model in flight (FlyingAnchor), and a bone of theirs as the stage lists say.
    gfx::EffectSystem& Effects() { return effects_; }
    static std::uint32_t StageAnchor(std::size_t slot) { return static_cast<std::uint32_t>(slot) + 1; }
    static std::uint32_t FlyingAnchor(std::uint32_t key) { return key | kFlyingAnchor; }
    // A stage slot's bone in the world this frame (origin-relative; an empty name: the model's root),
    // posed as it is drawn; false while the slot shows nothing or has no such bone.
    bool StageBone(std::size_t slot, std::string_view bone, glm::mat4& out) const;
    // The body's height on a slot: its model's bounding box top, scaled as it stands (as the original measures it,
    // read for "*" and for its sounds' place), where "*" lifts effects to and its
    // name stands; 0 while its model is not in.
    float StageHeight(std::size_t slot) const;
    // The hit and effect moments (code-1 events, seconds into the clip) of an animation state as a
    // slot's character plays it: its stance's clip, else the empty-handed set's, as ClipFor picks.
    std::vector<double> StageEvents(std::size_t slot, std::string_view set, std::uint32_t state, int code = 1) const;
    // The sounds its clip of that state names, by their moments (the clip's sound track).
    std::vector<assets::ResourceData::SoundEvent> StageSounds(std::size_t slot, std::string_view set, std::uint32_t state) const;
    // The length (seconds) of a state's clip from a set (empty: the stance) as a slot's character plays
    // it; negative while it loads (asked for here) or when it has none.
    double StageClipSeconds(std::size_t slot, std::string_view set, std::uint32_t state);
    // Where an effect's anchor is this frame (the effect system's Place; StageEffects' held models).
    bool PlaceEffect(const gfx::EffectSystem::Anchor& anchor, glm::mat4& out) const;
    // How the characters' animation states found their clips so far (the skill bench's check): in
    // the set asked for (a skill's group, else the weapon's stance), in another (the stance, the
    // empty-handed set: ClipPath's fallbacks), nowhere.
    struct ClipCounts {
        std::uint64_t inStance = 0, fromDefault = 0, missing = 0; // in the set asked for, from another, none
    };
    const ClipCounts& Clips() const { return clipCounts_; }

    const char* Name() const override { return "world"; }
    void Enter(ScreenContext& context) override;
    void Exit(ScreenContext& context, FrameSnapshot& snapshot) override;
    void Update(ScreenContext& context, FrameSnapshot& snapshot) override;

private:
    void Start(ScreenContext& context);
    void Fly(ScreenContext& context);
    void Flight(ScreenContext& context);
    void UpdateCharacters(ScreenContext& context, FrameSnapshot& snapshot);
    void PoseObjects(ScreenContext& context, FrameSnapshot& snapshot);
    void UpdateEnvironment(ScreenContext& context, FrameSnapshot& snapshot);
    void UpdateWeather(ScreenContext& context, FrameSnapshot& snapshot);
    void ResetCamera();
    void StartLoading(ScreenContext& context, game::LoadingKind kind);
    bool UpdateLoading(ScreenContext& context, FrameSnapshot& snapshot); // true while the loading screen stays
    void Teleport(ScreenContext& context, FrameSnapshot& snapshot);
    void DrawHud(ScreenContext& context, FrameSnapshot& snapshot);
    void UpdateEffects(ScreenContext& context, FrameSnapshot& snapshot);
    static constexpr std::uint32_t kFlyingAnchor = 0x80000000u;
    gfx::EffectSystem effects_;
    std::string testEffect_; // --effect
    std::string testEffectBone_; // --effect-bone
    float testEffectScale_ = 1.0f; // --effect-scale
    std::vector<gfx::EffectSystem::Handle> testEffects_;

    // object.ifo, tile2d.ifo and environment.ifo, read on a job.
    struct Indexes {
        std::atomic<bool> ready{false};
        std::string error;
        assets::AssetIndex objects;
        std::shared_ptr<assets::AssetIndex> tiles;
        assets::EnvironmentData environment; // empty if it could not be read (logged)
    };
    std::shared_ptr<Indexes> indexes_;
    std::optional<Backdrop> backdrop_;
    gfx::AssetStore* assets_ = nullptr; // a backdrop's, for the stage
    bool settled_ = false;
    bool started_ = false;

    int originX_ = 78, originY_ = 105;
    WorldStreamer streamer_;
    // A dungeon instead of the streamed world.
    std::unique_ptr<DungeonScene> dungeon_;
    std::uint16_t dungeonRegion_ = 0;
    std::optional<game::proto::WorldPosition> focus_;
    DungeonScene::Handles DungeonHandles();
    bool DungeonSettled(const gfx::AssetStore& assets) const;
    // Navigation (C4b): loaded with the regions, queried by the characters.
    std::unique_ptr<game::NavLoader> navLoader_;
    game::NavWorld navWorld_;
    WorldStreamer::Settings streaming_;

    OrbitCamera camera_;
    bool cameraPlaced_ = false;
    std::optional<OrbitCamera> placedCamera_; // --camera
    bool reported_ = false;
    glm::vec3 lastCamera_{0.0f};
    // The flight (C4a's benchmark).
    bool flight_ = false, arrived_ = false;
    glm::vec2 flightTarget_{0.0f};
    float flightSpeed_ = 150.0f;

    // The time of day (C3e).
    gfx::EnvironmentState environment_;
    game::GameClock clock_;
    float fixedTime_ = -1.0f; // >= 0: --time-of-day
    float fogRange_ = 2500.0f;
    float viewDistance_ = 5500.0f;
    glm::vec2 probePoint_{-1.0f, -1.0f}; // the pixel asked for (none: x < 0)
    DepthProbe probe_;                    // the renderer's last answer
    // The HUD (C3h): the font, read on a job.
    struct HudFont {
        std::atomic<bool> ready{false};
        ui::Font font;
    };
    std::shared_ptr<HudFont> hudFont_;
    FontAtlas hudAtlas_;
    ui::DrawList hud_;
    bool glow_ = true, lensFlare_ = true;
    std::uint16_t environmentKey_ = 0;
    // The loading screen (C4e) and a teleport.
    LoadingScreen loading_;
    ui::DrawList loadingUi_;
    double loadingSince_ = 0;
    std::uint64_t loadingFrames_ = 0;
    std::uint32_t loadingRandom_ = 0;
    int teleportX_ = 0, teleportY_ = 0;
    double teleportAt_ = -1.0;
    bool captureLoading_ = false;
    // Weather (C4c).
    game::Weather weather_;
    float wetness_ = 0.0f, snowLying_ = 0.0f; // the ground under the weather (UpdateWeather)
    WeatherEffect weatherEffect_;
    double lightningAt_ = -1.0;

    // Characters (C3d).
    struct CharacterModel {
        std::string path;
        gfx::AssetId model, animation;
        std::shared_ptr<const gfx::ModelInfo> info;
        std::uint8_t weaponTail = 0xFF; // its parts at the end that are a weapon or a shield (0xFF: not counted yet)
        std::shared_ptr<const assets::AnimationData> clip;
        std::string clipName;
        float shadowSize = 0;
        bool ready = false; // model and animation resident
        bool idle = false;  // standing on the stage: its idle clip, not a walk
        std::string stance = "default"; // a stage model's animation set
        std::uint32_t state = 0;        // and its state there
        bool released = false; // no character uses it any more: its assets went back
        // A stage model's clips by set and animation state (ClipKey), asked for when a character
        // first plays one (ClipPath gives the file); `sets` its animation sets.
        struct Clip {
            gfx::AssetId id;
            std::shared_ptr<const assets::AnimationData> data;
            bool missing = false;
        };
        std::unordered_map<std::uint32_t, Clip> clips;
        assets::ResourceData sets;
        bool warned = false;   // its failure was logged
        mutable std::unordered_map<std::string, int> bones; // bone names effects asked for -> index (-1 none)
        int spineBone = -2; // the spine aim's bone (Spine_Base, else Bip01 Spine; -1 none, -2 not looked for)
    };
    struct Character {
        std::uint32_t type = 0;
        std::uint32_t handle = 0;
        std::uint32_t paletteOffset = 0; // first matrix in the palette buffers
        float phase = 0;
        std::uint32_t random = 0;        // its own random sequence
        game::NavPosition nav;           // where it stands
        float targetX = 0, targetZ = 0;  // where it walks to (origin-relative)
        bool repath = true;              // pick a new target
        float x = 0, y = 0, z = 0;       // origin-relative
        float dirX = 0, dirZ = 1;        // facing
        bool spawned = false;
        bool active = true; // a stage slot is inactive while cleared
        bool stage = false; // placed by SetStage: stands still, no navigation
        bool dressed = false; // a walker with a look (the crowd's): plays its look's states as a stage character does
        bool still = false;   // a dressed walker that stands (swinging, flinching)
        bool pinned = false;  // --walker-at: stands at pinX, pinZ (origin-region units) and never walks
        float pinX = 0, pinZ = 0;
        StageLook look;    // a stage slot's look
        std::uint32_t nextType = kNoType; // a new look loading; the current one stays until then
        std::uint32_t playing = kNoType;  // the animation state its pose plays (look.state once its clip is in)
        std::uint32_t playingTake = 0;    // and the take
        std::string playingSet;           // and the set it is from (empty: its stance): a rider's "cart" state 0 is not its stance's
        float opacity = 1.0f;             // SetStageOpacity
        bool tinted = false;              // SetStageTint
        glm::vec3 tint{1.0f};
        // The mixer's (a stage or dressed character's: anim::SkeletonPose's two layers): the cyclic layer's state, the top
        // one-shot playing (0: none), the cycle the look asked for, the flinch's take started last, the time a frozen
        // pose has held still (its clips' clocks stand).
        bool mixing = false;
        std::uint32_t playingCycle = StageLook::kNoState;
        std::string playingCycleSet;
        std::uint32_t onceId = 0;
        std::uint32_t flinchPlaying = 0;
        double frozenMs = 0;
        std::optional<std::array<float, 12>> transform; // SetStageTransform's
        // Its model's ambient effects (gfx::ModelInfo::ambientParticles), started for the instance
        // `ambientFor` (its handle + 1; 0 none): a new instance (a new look) starts them afresh.
        std::vector<std::uint32_t> ambient;
        std::uint32_t ambientFor = 0;
        anim::SkeletonPose pose;
    };
    static constexpr std::uint32_t kNoType = 0xFFFFFFFFu;
    std::uint32_t StageModel(const StageLook& look);
    // The clip of a stage model's state from a set (empty: its stance; null while it loads; `missing`
    // when it has none).
    const assets::AnimationData* ClipFor(CharacterModel& model, std::string_view set, std::uint32_t state, bool& missing);
    // A model's clip entry of a set and state, asked for (and counted) when first wanted.
    CharacterModel::Clip& ClipEntry(CharacterModel& model, std::string_view set, std::uint32_t state);
    // The clip's file: the state in `set`, else in the stance, else in the empty-handed set.
    static std::string ClipPath(const CharacterModel& model, std::string_view set, std::uint32_t state, bool* inSet = nullptr);
    // The sets clips were asked from, by index (0: the stance); a clip's key is (index << 24) | state.
    std::vector<std::string> clipSets_{std::string()};
    std::uint32_t ClipKey(std::string_view set, std::uint32_t state);
    void ReleaseUnused(ScreenContext& context);
    struct Prop {
        gfx::AssetId model;
        std::array<float, 12> world{};
        std::uint32_t handle = 0;
        bool added = false;
        bool hidden = false; // SetGroundItem not shown yet
        // A ground item's own effects (its drop model's ambient particles), once it is shown.
        std::vector<std::uint32_t> ambient;
        bool ambientStarted = false;
    };
    std::vector<Prop> props_;
    std::unordered_map<std::uint32_t, Prop> ground_;
    static constexpr std::uint32_t kStagePropKey = 0xF0000000u; // ground_'s keys of the stage slots shown as still models
    std::unordered_map<std::size_t, std::string> stageProps_;   // a stage slot -> the model it shows as one
    std::unordered_map<std::uint32_t, Prop> flying_;
    std::vector<std::uint32_t> pendingRemoves_; // stage instances to take out of the scene
    std::vector<CharacterModel> characterModels_;
    ClipCounts clipCounts_;
    std::vector<Character> characters_;
    std::uint32_t paletteCapacity_ = 0; // matrices in each palette buffer
    std::uint32_t objectPalettes_ = 0;  // the moving map objects' first matrix (after the characters')
    std::vector<rhi::BufferHandle> palettes_; // one per snapshot slot, CPU-visible
    std::size_t spawned_ = 0;
    // The size a character is drawn at (a stage character's or a dressed walker's height).
    static float ScaleOf(const Character& c) { return c.stage || c.dressed ? c.look.height * c.look.grow : 1.0f; }
    // The body's volume (StageLook::volume): its bones thicker or thinner across their length, on the character's pose.
    static void ApplyVolume(Character& c, const assets::SkeletonData& skeleton);

    // The crowd (WorldCrowd.cpp): the walkers dressed as the game's players and monsters, fighting
    // with their skills and wearing their buffs and glows.
    struct Crowd;
    std::shared_ptr<Crowd> crowd_;
    void StartCrowd(ScreenContext& context, std::size_t shown);
    void UpdateCrowd(ScreenContext& context);
    void CrowdTools(); // the World tools' sliders (players and monsters shown)
    bool CrowdWaiting() const; // its walkers not dressed yet
    void ExitCrowd(ScreenContext& context);
    struct Menu;
    std::shared_ptr<Menu> menu_;
    void StartMenu(ScreenContext& context);
    void UpdateMenu(ScreenContext& context, FrameSnapshot& snapshot);
    void ExitMenu(FrameSnapshot& snapshot);
    bool MenuHasMouse() const; // the mouse is over one of the menu's windows
};

} // namespace sr::client
