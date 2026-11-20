#pragma once

// A dungeon drawn as the original draws one, for the dungeon test
// screen and the world screen when the player stands in a dungeon: its .dof (dungeon\dungeoninfo.txt
// by region) read on a job; the current block (the one the player's floor is in; the original takes
// the player's navigation cell) and every block of its visible list shown with their objects; no
// sky; the background is the current block's fog colour; lit models take the block's ambient light,
// their block light (the object's light index) and the squared exponential fog of the block's
// density. The navigation is not here: the NavLoader loads a dungeon's (game::NavDungeon).

#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "frame/FrameSnapshot.h"
#include "sr/assets/Dungeon.h"
#include "sr/gfx/AssetStore.h"
#include "sr/gfx/Effects.h"

namespace sr::assets {
class FileSystem;
}
namespace sr::core {
class JobSystem;
}
namespace sr::game {
class NavDungeon;
}

namespace sr::client {

class DungeonScene {
public:
    // Instance handles, from the owner's pool (the world's instances share it).
    struct Handles {
        std::function<std::uint32_t()> take;
        std::function<void(std::uint32_t)> give;
    };

    void Load(const assets::FileSystem& files, core::JobSystem& jobs, std::uint16_t region);
    bool Ready() const { return loaded_ && loaded_->ready.load(std::memory_order_acquire); }
    // After Ready: why it could not be read (empty: it could).
    const std::string& Error() const { return loaded_->error; }
    const std::string& Path() const { return loaded_->path; }
    const assets::DungeonData& Data() const { return loaded_->data; }
    std::uint16_t Region() const { return region_; }

    // The block whose floor is under (x, y, z) (searched a little further down each time); -1 none.
    static std::int32_t BlockUnder(const game::NavDungeon& nav, float x, float y, float z);
    // The blocks shown: `current` and its visible list (instances of blocks coming in added, of those
    // leaving removed). Ready only.
    void Show(gfx::AssetStore& assets, std::int32_t current, std::vector<WorldCommand>& commands, const Handles& handles);
    std::int32_t Current() const { return current_; }
    // The current block's light and fog, its lights and its fog colour as the background.
    void Light(SceneView& scene) const;
    // Every model the shown blocks need resident (or failed); the share of them that is (a gauge).
    bool Settled(const gfx::AssetStore& assets) const;
    float Progress(const gfx::AssetStore& assets) const;
    std::size_t Shown() const { return shown_.size(); }
    std::size_t Instances() const;
    std::size_t Models() const { return models_.size(); }
    // Takes every instance out and gives the models back.
    void Clear(gfx::AssetStore& assets, std::vector<WorldCommand>& commands, const Handles& handles);
    // The shown blocks' and objects' own effects (world/AttachedEffects.h: the teleport circles on the
    // zones, flames, lights), started once each model is in, stopped as its block goes; null: none.
    void SetEffects(gfx::EffectSystem* effects) { effects_ = effects; }
    void StartEffects(const gfx::AssetStore& assets);

private:
    void Prepare(); // the lights' table, once read
    void AddBlock(gfx::AssetStore& assets, std::uint32_t block, std::vector<WorldCommand>& commands, const Handles& handles);
    void RemoveBlock(gfx::AssetStore& assets, std::uint32_t block, std::vector<WorldCommand>& commands, const Handles& handles);
    gfx::AssetId Acquire(gfx::AssetStore& assets, const std::string& path);
    void ReleaseModel(gfx::AssetStore& assets, const std::string& path);

    struct Loaded {
        std::atomic<bool> ready{false};
        std::string error, path;
        assets::DungeonData data;
    };
    std::shared_ptr<Loaded> loaded_;
    std::uint16_t region_ = 0;
    bool prepared_ = false;
    std::int32_t current_ = -1;
    std::vector<std::uint32_t> shown_;     // the current block and its visible list, sorted
    std::vector<std::uint32_t> lightBase_; // per block: its first light in the light table
    std::vector<gfx::PointLight> lights_;  // every block's lights (stable indices)
    struct BlockInstances {
        std::vector<std::uint32_t> handles;
        std::vector<std::string> models; // the paths they show (one each)
        std::vector<std::array<float, 12>> worlds; // where each stands
        std::vector<bool> effectsStarted;
        std::vector<std::uint32_t> effects; // their models' own effects
        bool effectsPending = false;
    };
    gfx::EffectSystem* effects_ = nullptr;
    std::unordered_map<std::uint32_t, BlockInstances> blocks_;
    struct Model {
        gfx::AssetId id;
        std::uint32_t uses = 0;
    };
    std::unordered_map<std::string, Model> models_;
};

} // namespace sr::client
