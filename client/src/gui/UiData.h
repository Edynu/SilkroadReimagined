#pragma once

// The interface's data (shared by the title screens and the game's interface): the original's
// layouts (resinfo\*.txt), its interface text table and font, read on a job; interface textures
// from the asset store by path.

#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sr/assets/GameGuide.h"
#include "sr/assets/WorldMap.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/GameData.h"
#include "sr/assets/UiLayout.h"
#include "sr/assets/UiLayout2dt.h"
#include "sr/core/Jobs.h"
#include "sr/game/Spirit.h"
#include "sr/gfx/AssetStore.h"
#include "sr/ui/Font.h"

namespace sr::client {

class UiWarmup;

// Layouts, the text table and the font, loaded together on a job.
class UiData {
public:
    UiData() = default;
    ~UiData() { Wait(); }
    UiData(const UiData&) = delete;
    UiData& operator=(const UiData&) = delete;
    // Waits for the loading job (it reads the file system, which must outlive it).
    void Wait() {
        if (jobs_) jobs_->Wait(loading_);
    }
    // Starts loading `layouts` (resinfo paths) with the text table and the interface font.
    void Load(core::JobSystem& jobs, const assets::FileSystem& files, std::vector<std::string> layouts);
    bool Ready() const { return state_ && state_->ready.load(std::memory_order_acquire); }
    // Null until ready, or when the file could not be read.
    const assets::UiLayout* Layout(const std::string& path) const;
    // A 1.188 window's binary layout (res_ui\*.2dt: the guild window's); null until ready or missing.
    const std::vector<assets::Ui2dtRecord>* Layout2dt(const std::string& path) const;
    // The English text of a key; the key itself when it has none.
    const std::string& Text(const std::string& key) const;
    bool HasText(const std::string& key) const { return Ready() && state_->text.contains(key); }
    // The area a region belongs to (regioncode.txt: RN_CH_JANGAN...); empty when none.
    const std::string& RegionKey(std::uint16_t region) const;
    ui::Font* Font() const { return Ready() && state_->fontLoaded ? &state_->font : nullptr; }
    // magicoption.txt (the item tooltip's blues); null until ready.
    const std::vector<assets::MagicOptionRow>* MagicOptions() const { return Ready() ? &state_->magicOptions : nullptr; }
    // The Devil's Spirit's options by plus (its tooltip's groups).
    const game::SpiritTable* Spirits() const { return Ready() ? &state_->spirits : nullptr; }
    // refmagicoptgroup.txt (the socket stones' skills by level); null until ready.
    const std::vector<assets::MagicOptGroupRow>* MagicOptGroups() const { return Ready() ? &state_->magicOptGroups : nullptr; }
    const std::vector<assets::ActionRow>* Actions() const { return Ready() ? &state_->actions : nullptr; }
    // The world map's maps, floors and labels (worldmap_mapinfo.txt, worldmap_localinfo.txt); null until ready.
    const assets::WorldMapData* WorldMap() const { return Ready() ? &state_->worldMap : nullptr; }
    // The game guide's topics and events (gameguidedata.txt, eventguidedata.txt); null until ready.
    const assets::GameGuideData* GameGuide() const { return Ready() ? &state_->gameGuide : nullptr; }
    // A dungeon region's minimap folder (regioninfo.txt: the third field of its #FIELD line, "donwhang",
    // "jinsi"; minimap_d\<folder>\); empty when none.
    std::string DungeonFolder(std::uint16_t region) const {
        if (!Ready()) return {};
        const auto it = state_->dungeonFolders.find(region);
        return it == state_->dungeonFolders.end() ? std::string() : it->second;
    }
    // An NPC's second text key (npcchat.txt's last column: what it says when talked to); empty when none.
    std::string NpcTalk(const std::string& codename) const {
        if (!Ready()) return {};
        const auto it = state_->npcTalks.find(codename);
        return it == state_->npcTalks.end() ? std::string() : it->second;
    }
    // An NPC's or gate's greeting text key (npcchat.txt, by its codename); empty when none.
    std::string NpcGreeting(const std::string& codename) const {
        if (!Ready()) return {};
        const auto it = state_->npcGreetings.find(codename);
        return it == state_->npcGreetings.end() ? std::string() : it->second;
    }

private:
    struct State {
        std::atomic<bool> ready{false};
        std::unordered_map<std::string, assets::UiLayout> layouts;
        std::unordered_map<std::string, std::vector<assets::Ui2dtRecord>> layouts2dt;
        assets::TextCatalog text;
        std::unordered_map<std::uint16_t, std::string> regionKeys;
        std::vector<assets::MagicOptionRow> magicOptions;
        game::SpiritTable spirits;
        std::vector<assets::MagicOptGroupRow> magicOptGroups;
        std::vector<assets::ActionRow> actions; // the Action window's icons
        assets::WorldMapData worldMap;
        assets::GameGuideData gameGuide;
        std::unordered_map<std::uint16_t, std::string> dungeonFolders; // regioninfo.txt's dungeon fields
        std::unordered_map<std::string, std::string> npcGreetings, npcTalks;
        ui::Font font;
        bool fontLoaded = false;
    };
    std::shared_ptr<State> state_;
    core::JobSystem* jobs_ = nullptr;
    core::JobCounter loading_;
};

// Interface textures by path, requested from the asset store on first use and kept until
// ReleaseAll (the screen's exit).
class UiTextures {
public:
    // `warmup`: told every texture asked for (the next run's warm-up, UiWarmup.h); null: none.
    explicit UiTextures(gfx::AssetStore& assets, UiWarmup* warmup = nullptr) : assets_(assets), warmup_(warmup) {}
    ~UiTextures() { ReleaseAll(); }
    struct Ref {
        ui::TextureRef texture;
        float width = 0, height = 0;
        bool resident = false;
    };
    Ref Get(const std::string& path);
    // Asked for ahead of being shown (behind what is shown, ahead of the world), held like the others; neither noted
    // for the warm-up nor counted late until it is shown (Get).
    void Prefetch(const std::string& path);
    bool Exists(const std::string& path); // false once the store failed to load it
    // Every requested texture resident or failed (a capture waits for it).
    bool Settled() const;
    void ReleaseAll();
    // Textures the program made (a portrait's target, client/src/frame/Portraits.h), drawn by a path
    // that starts with '@' (no game file does): never asked of the store, missing until set.
    void SetExternal(const std::string& path, const Ref& ref) { externals_[path] = ref; }
    void RemoveExternal(const std::string& path) { externals_.erase(path); }

private:
    gfx::AssetStore& assets_;
    UiWarmup* warmup_ = nullptr;
    std::unordered_set<std::string> cold_; // asked for while not in (counted once each)
    struct Held {
        gfx::AssetId id;
        bool shown = false; // asked for by Get (not only prefetched): noted for the warm-up
    };
    std::unordered_map<std::string, Held> ids_;
    std::unordered_map<std::string, Ref> externals_;
};

// Every layout in resinfo\ (the .txt files), for UiData::Load.
std::vector<std::string> AllLayouts(const assets::FileSystem& files);

} // namespace sr::client
