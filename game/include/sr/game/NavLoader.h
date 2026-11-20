#pragma once

// Loads the regions' navigation on the job system: a request reads the
// region's .nvm, decodes it, finds each object's navmesh (navmesh\object.ifo -> .bsr -> the .bms
// it names; shared between regions while any uses it) and builds the NavRegion, all on a worker.
// A dungeon's region (bit 15 set) loads its .dof (dungeon\dungeoninfo.txt)
// and each block's floor navmesh into a NavDungeon instead.
// The game thread publishes the finished regions into its NavWorld in Update(); nothing loads on
// the game thread.

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "sr/core/Jobs.h"
#include "sr/game/NavMesh.h"

namespace sr::assets {
class FileSystem;
}

namespace sr::game {

class NavLoader {
public:
    // `files` and `jobs` outlive the loader.
    NavLoader(const assets::FileSystem& files, core::JobSystem& jobs);
    ~NavLoader(); // waits for its jobs
    NavLoader(const NavLoader&) = delete;
    NavLoader& operator=(const NavLoader&) = delete;

    void Request(std::uint16_t region);
    // Forgets the region (a load still running is dropped when it finishes) and removes it from `world`.
    void Release(std::uint16_t region, NavWorld& world);
    // Publishes the regions finished since the last call.
    void Update(NavWorld& world);
    // Requested and not finished yet.
    bool Loading(std::uint16_t region) const;

    struct Stats {
        std::size_t regions = 0, loading = 0, ready = 0, missing = 0; // missing: no navmesh (not a region of the world)
        std::size_t objectMeshes = 0;                                // object navmeshes alive
        std::uint64_t built = 0;                                     // regions built over the loader's life
    };
    Stats GetStats() const;

    struct Shared; // the jobs' side (NavLoader.cpp)

private:
    enum class State : std::uint8_t { Loading, Ready, Missing };
    struct Entry {
        State state = State::Loading;
        std::uint32_t generation = 0;
    };

    std::shared_ptr<Shared> shared_;
    const assets::FileSystem& files_;
    core::JobSystem& jobs_;
    core::JobCounter counter_;
    std::unordered_map<std::uint16_t, Entry> regions_;
    std::uint32_t generation_ = 0;
    std::uint64_t built_ = 0;
};

} // namespace sr::game
