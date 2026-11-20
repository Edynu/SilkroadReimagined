#pragma once

// Region streaming on the game thread (C4a). The original loads the 3 x 3 regions
// around the player synchronously in the frame that crosses a border; we request
// regions as assets (gfx::AssetStore, decoded on workers) before they are needed:
// - wanted: the regions within `radius` of the camera's region, and those within 1 of where the
//   camera will be after `prefetchSeconds` at its velocity; requested nearest first;
// - released: regions farther than `keep` (and not wanted ahead): their instances removed, their
//   objects' models released when no region uses them any more;
// - placed: at most `placePerFrame` newly resident regions a frame turn their placements into
//   world instances (a border never becomes one long frame).
// Positions are relative to the origin region's south-west corner (floats stay precise over the
// distances one session flies; a teleport starts a new streamer).
// Navigation (C4b): with SetNavigation, each wanted region's navmesh is requested from the loader
// as well (decoded on a worker) and published into the game's NavWorld; released regions leave it.

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "frame/FrameSnapshot.h"
#include "sr/anim/Pose.h"
#include "sr/assets/Terrain.h"
#include "sr/game/NavLoader.h"
#include "sr/gfx/AssetStore.h"
#include "sr/gfx/Effects.h"

namespace sr::client {

class WorldStreamer {
public:
    struct Settings {
        int radius = 2;               // wanted around the camera
        int keep = 3;                 // released beyond
        float prefetchSeconds = 2.0f; // look ahead at the camera's velocity
        int placePerFrame = 1;
    };

    // `objects`: object.ifo (kept alive by the caller). The tile index must be set on the store.
    void Start(int originX, int originY, const assets::AssetIndex& objects, const Settings& settings);
    // Regions wanted around the camera from now on (the view distance option); released one beyond.
    void SetRadius(int radius) {
        settings_.radius = radius;
        settings_.keep = radius + 1;
    }
    int Radius() const { return settings_.radius; }
    // Every frame: what to request, release and place for a camera at `camera` (origin-relative)
    // moving at `velocity` (units a second); changes to the world go into `commands`.
    void Update(gfx::AssetStore& assets, const glm::vec3& camera, const glm::vec3& velocity, std::vector<WorldCommand>& commands);
    // Releases everything (the instances' removals into `commands`).
    void Stop(gfx::AssetStore& assets, std::vector<WorldCommand>& commands);
    // The placed objects' own effects (world/AttachedEffects.h: the teleport circles, lamps, glows),
    // started once each model is in and stopped as it goes; null: none. Before Start; it outlives the
    // streamer's use of it.
    void SetEffects(gfx::EffectSystem* effects) { effects_ = effects; }
    // Starts the effects of the objects whose models came in since the last call.
    void StartEffects(const gfx::AssetStore& assets);

    // Map objects that move (a hawk circling over the desert, res\nature\china\hawk.bsr with its hawk_fly.ban): a
    // placed object whose model has a skeleton and clips loops its standing clip (its "default" set's state 0, else its first
    // clip; ours, the original's choice not traced), each from a time of its own. Posed by the owner (WorldScreen) into the
    // palette slots it gives here from `base`: `small` slots of kSmallBones matrices (a tree's few bones), then `large` of
    // kLargeBones (none: nothing animates). Before Start.
    static constexpr std::uint32_t kSmallBones = 16, kLargeBones = 64;
    void SetPalettes(std::uint32_t base, std::uint32_t small, std::uint32_t large);
    // The objects whose models came in since the last call made skinned instances (their clips asked for), and the
    // clips that came in given to their poses. Each frame, after Update.
    void StartAnimations(gfx::AssetStore& assets, std::vector<WorldCommand>& commands);
    struct Animated {
        std::uint32_t key = 0;           // its PlacedKey
        std::uint32_t paletteOffset = 0; // its first matrix
        gfx::AssetId clip;
        std::shared_ptr<const assets::AnimationData> animation; // once in (till then the bind pose)
        anim::SkeletonPose pose;
        float phase = 0.0f; // seconds
    };
    // The moving objects, posed by the owner each frame (their palettes: Animated::paletteOffset).
    std::vector<Animated>& AnimatedObjects() { return animated_; }
    // Before Start; both outlive the streamer's use of them.
    void SetNavigation(game::NavLoader* loader, game::NavWorld* world) {
        navLoader_ = loader;
        navWorld_ = world;
    }

    // A handle for another world instance (characters share the handle space).
    std::uint32_t NewHandle();
    void FreeHandle(std::uint32_t handle) { freeHandles_.push_back(handle); }

    // The resident regions' terrain, origin-relative.
    void AppendTerrain(std::vector<TerrainItem>& out) const;
    // The resident region under (x, z) and the point in its coordinates (0..1920); null if none.
    std::shared_ptr<const gfx::TerrainInfo> TerrainAt(const gfx::AssetStore& assets, float x, float z, float& localX,
                                                      float& localZ) const;
    // Every wanted region near the camera (kNearRing) resident (or failed) and placed, its navigation
    // loaded, and every model it uses settled; the farther ones (a long view distance) stream in
    // behind, the loading screen does not wait for them.
    bool Settled(const gfx::AssetStore& assets) const;
    // 0..1: the share of what Settled waits for that is there (the loading screen's gauge).
    float Progress(const gfx::AssetStore& assets) const;
    // Origin-relative (x, z) as a region id and the point in that region (0..1920); and back.
    std::uint16_t RegionAt(float x, float z, float& localX, float& localZ) const;
    glm::vec2 RegionCorner(std::uint16_t region) const;

    int OriginX() const { return originX_; }
    int OriginY() const { return originY_; }
    struct Stats {
        std::size_t regions = 0, resident = 0, placements = 0, instances = 0, objects = 0;
        std::uint64_t requested = 0, released = 0; // regions over the streamer's life
        std::size_t unknownObjects = 0;
        game::NavLoader::Stats navigation;
    };
    Stats GetStats(const gfx::AssetStore& assets) const;

    // The rings the loading screen waits for, and those that keep their navigation (nothing walks
    // farther away: the navigation of a long view distance's far regions is never built).
    static constexpr int kNearRing = 2;
    static constexpr int kNavigationRing = 3;

    // The follow camera's spring (ours, 2026-11-13: the user's ask; the original only pulls the camera in before the
    // floors): the fraction of the segment from `from` (the character's chest) to `to` (the eye)
    // before it enters the box of a placed object that would hold the eye (1: none would). An object whose box holds
    // `from` (the floor or the hall the character stands in) never stops it. Origin-relative.
    float CameraLimit(const gfx::AssetStore& assets, const glm::vec3& from, const glm::vec3& to) const;

private:
    struct Region {
        int x = 0, y = 0;
        gfx::AssetId id;
        bool placed = false;
        bool navigation = false; // its navigation asked for
        std::vector<std::uint32_t> keys;    // the objects it lists (PlacedKey), each once
        std::vector<std::uint32_t> objects; // object.ifo ids they use
    };
    // An object as one instance however many blocks and regions list it (a big building is listed
    // in every block it covers, up to some 300 times: drawn once each, they shaded the same pixels
    // that many times). Its key: the region that owns it and its uid there (unique over the map's
    // 43,119 objects in 111,272 listings).
    static std::uint32_t PlacedKey(std::uint16_t region, std::int16_t uid) {
        return (std::uint32_t{region} << 16) | static_cast<std::uint16_t>(uid);
    }
    struct Placed {
        std::uint32_t handle = 0;
        std::uint32_t object = 0; // object.ifo id
        std::uint32_t uses = 0;   // regions listing it that are placed
        gfx::AssetId model;
        std::array<float, 12> world{}; // where it stands (its instance's rows)
        std::vector<std::uint32_t> effects; // its model's own effects, once started
    };
    struct ObjectModel {
        gfx::AssetId model;
        std::uint32_t uses = 0;
    };
    void Place(gfx::AssetStore& assets, Region& region, std::vector<WorldCommand>& commands);
    void Unplace(gfx::AssetStore& assets, Region& region, std::vector<WorldCommand>& commands);

    const assets::AssetIndex* objects_ = nullptr;
    game::NavLoader* navLoader_ = nullptr;
    game::NavWorld* navWorld_ = nullptr;
    Settings settings_;
    int originX_ = 0, originY_ = 0;
    int cx_ = 0, cy_ = 0; // the camera's region at the last Update
    std::unordered_map<std::uint16_t, Region> regions_; // by region id
    std::unordered_map<std::uint32_t, ObjectModel> models_; // by object.ifo id
    std::unordered_map<std::uint32_t, Placed> placed_;      // by PlacedKey
    gfx::EffectSystem* effects_ = nullptr;
    std::vector<std::uint32_t> effectsPending_;             // placed keys whose models' effects are not started yet
    std::vector<std::uint32_t> animationsPending_;          // placed keys whose models are not in yet (SetPalettes)
    std::vector<Animated> animated_;
    std::vector<std::uint32_t> freeSmall_, freeLarge_;      // palette slots' first matrices
    std::uint32_t largeBase_ = 0;                           // the large slots' first matrix (a slot's size from it)
    bool animating_ = false;
    std::unordered_set<std::uint32_t> animatedKinds_, stillKinds_; // object.ifo ids logged once
    // An animated object's pose let go (its instance gone).
    void StopAnimation(gfx::AssetStore& assets, std::uint32_t key);
    std::unordered_set<std::uint32_t> listed_;              // Place's scratch: keys this region listed
    std::vector<std::uint32_t> freeHandles_;
    std::uint32_t nextHandle_ = 0;
    std::uint64_t requested_ = 0, released_ = 0;
    std::size_t unknownObjects_ = 0;
    std::vector<std::uint16_t> scratch_; // wanted ids this frame
};

} // namespace sr::client
