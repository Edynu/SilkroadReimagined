#pragma once

// The interface's textures warmed up for the run (ours; UiWarmupList says which): asked of the asset store at the
// program's start, behind the interface's own requests and ahead of the world's streaming. Each is held only until it
// is in: then the store keeps it like any texture nobody holds, under its budget, the longest unused going first when
// the world needs the room (the interface holds what it shows itself). By the time the game screen shows (it waits for
// the list, a few seconds at most) its HUD and the windows the player uses are in: nothing of the interface appears
// piece by piece. The UiTextures of every screen note what they draw; the list is written for the next run at the end.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "gui/UiWarmupList.h"
#include "sr/gfx/AssetStore.h"

namespace sr::client {

// The order the asset store loads in (a smaller number first; the world's nearest ring asks at 0): what the interface
// shows now, then the warm-up, then the world.
constexpr int kUiTexturePriority = -2;
constexpr int kUiWarmupPriority = -1;

class UiWarmup {
public:
    explicit UiWarmup(gfx::AssetStore& assets) : assets_(assets) {}
    ~UiWarmup() { ReleaseAll(); }
    UiWarmup(const UiWarmup&) = delete;
    UiWarmup& operator=(const UiWarmup&) = delete;

    // The last runs' list (`file`; none yet: nothing to warm), asked for at once.
    void Load(const std::string& file);
    // More to warm for the run (the world map's tiles), asked for once each; the loading screen does not wait for them.
    void Preload(std::span<const std::string> paths);
    // A texture an interface drew this run (noted for the next).
    void Note(std::string_view path) { list_.Note(path); }
    // An interface picture asked for while it was not in yet (it showed late, or its window waited): the measure of
    // what the warm-up missed, logged at the end.
    void NoteCold(std::string_view path) {
        if (++cold_ <= kColdNamed) coldNames_.emplace_back(path);
    }
    // Follows the loads (once a frame: each call goes on from the last); the list's textures all in (or failed).
    bool ListReady();
    // The list for the next run; false when the file cannot be written.
    bool Save(const std::string& file) const;
    void ReleaseAll();

private:
    // A group of requests followed until all are in, then let go.
    struct Batch {
        const char* name = "";
        std::vector<gfx::AssetId> ids;
        std::size_t settled = 0; // ids before this are known in (or failed)
        std::size_t total = 0;   // asked for so far (ids is emptied once they are in)
        double started = 0;
        bool done = true;
    };
    void Ask(Batch& batch, const std::string& path);
    bool Follow(Batch& batch);
    void Release(Batch& batch);
    gfx::AssetStore& assets_;
    UiWarmupList list_;
    Batch fromList_{"textures from the list"}, preloaded_{"preloaded textures"};
    std::unordered_set<std::string> asked_;
    std::size_t cold_ = 0;
    static constexpr std::size_t kColdNamed = 12; // the first ones named in the log
    std::vector<std::string> coldNames_;
};

} // namespace sr::client
