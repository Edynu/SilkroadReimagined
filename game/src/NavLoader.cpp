#include "sr/game/NavLoader.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "sr/assets/Dungeon.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/Model.h"
#include "sr/assets/Terrain.h"
#include "sr/core/Log.h"
#include "sr/core/Path.h"
#include "sr/core/Trace.h"
#include "sr/game/NavDungeon.h"

namespace sr::game {

struct NavLoader::Shared {
    // navmesh\object.ifo and navmesh\mapinfo.mfo, read by the first job.
    std::once_flag indexOnce;
    assets::AssetIndex objects;
    assets::NavMapInfo map;
    std::string indexError;
    // dungeon\dungeoninfo.txt, read by the first dungeon's job.
    std::once_flag dungeonsOnce;
    std::vector<assets::DungeonInfoEntry> dungeons;
    std::string dungeonsError;

    std::mutex mutex; // guards what follows
    std::unordered_map<std::uint32_t, std::weak_ptr<const NavObjectMesh>> meshes; // by object id
    std::unordered_set<std::uint32_t> withoutMesh;
    std::unordered_map<std::string, std::weak_ptr<const NavObjectMesh>> blockMeshes; // dungeon floors by .bsr
    struct Result {
        std::uint16_t region = 0;
        std::uint32_t generation = 0;
        std::shared_ptr<const NavRegion> nav; // null: no navigation there
        std::shared_ptr<const NavDungeon> dungeon; // a dungeon's
        std::string error;
    };
    std::vector<Result> results;
};

namespace {

void LoadIndexes(NavLoader::Shared& s, const assets::FileSystem& files) {
    const auto objects = files.Read("navmesh\\object.ifo");
    const auto map = files.Read("navmesh\\mapinfo.mfo");
    std::string error;
    if (!objects || !map) {
        s.indexError = "navmesh\\object.ifo or navmesh\\mapinfo.mfo not found";
    } else if (!assets::DecodeObjectIndex(objects->Bytes(), s.objects, error) || !assets::DecodeNavMapInfo(map->Bytes(), s.map, error)) {
        s.indexError = "navmesh indexes: " + error;
    }
}

// An object's navmesh: the one in use if any, else object.ifo -> .bsr (or a .cpd's collision .bsr)
// -> the .bms it names.
std::shared_ptr<const NavObjectMesh> MeshFor(NavLoader::Shared& s, const assets::FileSystem& files, std::uint32_t objectId) {
    {
        std::lock_guard lock(s.mutex);
        if (s.withoutMesh.contains(objectId)) return nullptr;
        if (const auto it = s.meshes.find(objectId); it != s.meshes.end()) {
            if (auto mesh = it->second.lock()) return mesh;
        }
    }
    std::shared_ptr<const NavObjectMesh> mesh;
    const auto entry = s.objects.find(objectId);
    std::string error;
    assets::ObjectNavMesh nav;
    if (entry != s.objects.end() && assets::LoadObjectNavMesh(files, entry->second.path, nav, error) && !nav.empty()) {
        mesh = NavObjectMesh::Build(nav);
    }
    std::lock_guard lock(s.mutex);
    if (!mesh) {
        s.withoutMesh.insert(objectId);
        return nullptr;
    }
    auto& slot = s.meshes[objectId];
    if (auto existing = slot.lock()) return existing; // another job was quicker
    slot = mesh;
    return mesh;
}

// A dungeon block's floor: the navmesh its .bsr names (shared while any dungeon uses it).
std::shared_ptr<const NavObjectMesh> BlockMesh(NavLoader::Shared& s, const assets::FileSystem& files, const std::string& bsrPath) {
    {
        std::lock_guard lock(s.mutex);
        if (const auto it = s.blockMeshes.find(bsrPath); it != s.blockMeshes.end()) {
            if (auto mesh = it->second.lock()) return mesh;
        }
    }
    assets::ResourceData resource;
    std::string error;
    const auto bsr = files.Read(bsrPath);
    if (!bsr || !assets::DecodeBsr(bsr->Bytes(), resource, error) || resource.navMeshPath.empty()) return nullptr;
    const auto bms = files.Read(resource.navMeshPath);
    assets::ObjectNavMesh nav;
    if (!bms || !assets::DecodeBmsNavMesh(bms->Bytes(), nav, error) || nav.empty()) return nullptr;
    std::shared_ptr<const NavObjectMesh> mesh = NavObjectMesh::Build(nav);
    std::lock_guard lock(s.mutex);
    auto& slot = s.blockMeshes[bsrPath];
    if (auto existing = slot.lock()) return existing;
    slot = mesh;
    return mesh;
}

void LoadDungeon(NavLoader::Shared& s, const assets::FileSystem& files, std::uint16_t region, NavLoader::Shared::Result& result) {
    std::call_once(s.dungeonsOnce, [&] {
        const auto file = files.Read("dungeon\\dungeoninfo.txt");
        if (!file || !assets::DecodeDungeonInfo(file->Bytes(), s.dungeons, s.dungeonsError)) {
            s.dungeonsError = "dungeon\\dungeoninfo.txt: " + (file ? s.dungeonsError : std::string("not found"));
        }
    });
    if (!s.dungeonsError.empty()) {
        result.error = s.dungeonsError;
        return;
    }
    const auto entry = std::find_if(s.dungeons.begin(), s.dungeons.end(), [&](const assets::DungeonInfoEntry& e) { return e.regionId == region; });
    if (entry == s.dungeons.end()) return; // no such dungeon: no navigation there
    const auto file = files.Read(entry->path);
    assets::DungeonData data;
    if (!file || !assets::DecodeDof(file->Bytes(), data, result.error)) {
        result.error = entry->path + ": " + (file ? result.error : std::string("not found"));
        return;
    }
    result.dungeon = NavDungeon::Build(data, [&](std::uint32_t b) { return BlockMesh(s, files, data.blocks[b].path); });
}

} // namespace

NavLoader::NavLoader(const assets::FileSystem& files, core::JobSystem& jobs)
    : shared_(std::make_shared<Shared>()), files_(files), jobs_(jobs) {}

NavLoader::~NavLoader() { jobs_.Wait(counter_); }

void NavLoader::Request(std::uint16_t region) {
    if (regions_.contains(region)) return;
    const std::uint32_t generation = ++generation_;
    regions_[region] = Entry{State::Loading, generation};
    jobs_.Run(
        [shared = shared_, files = &files_, region, generation] {
            SR_ZONE("nav: region");
            std::call_once(shared->indexOnce, [&] { LoadIndexes(*shared, *files); });
            Shared::Result result;
            result.region = region;
            result.generation = generation;
            if (NavIsDungeon(region)) {
                LoadDungeon(*shared, *files, region, result);
            } else if (!shared->indexError.empty()) {
                result.error = shared->indexError;
            } else if (shared->map.Enabled(region)) {
                const std::string path = assets::RegionNavMeshFile(region);
                assets::RegionNavData data;
                if (const auto file = files->Read(path); !file) {
                    result.error = path + " not found";
                } else if (!assets::DecodeRegionNavMesh(file->Bytes(), data, result.error)) {
                    result.error = path + ": " + result.error;
                } else {
                    result.nav = NavRegion::Build(region, std::move(data),
                                                  [&](std::uint32_t objectId) { return MeshFor(*shared, *files, objectId); });
                }
            }
            std::lock_guard lock(shared->mutex);
            shared->results.push_back(std::move(result));
        },
        core::JobPriority::Streaming, &counter_);
}

void NavLoader::Release(std::uint16_t region, NavWorld& world) {
    regions_.erase(region);
    world.Remove(region);
}

void NavLoader::Update(NavWorld& world) {
    std::vector<Shared::Result> results;
    {
        std::lock_guard lock(shared_->mutex);
        results.swap(shared_->results);
    }
    for (Shared::Result& r : results) {
        const auto it = regions_.find(r.region);
        if (it == regions_.end() || it->second.generation != r.generation) continue; // released meanwhile
        if (!r.error.empty()) core::LogWarning("nav: region {:04x}: {}", r.region, r.error);
        if (r.nav || r.dungeon) {
            if (r.nav) world.Add(std::move(r.nav));
            if (r.dungeon) world.AddDungeon(std::move(r.dungeon));
            it->second.state = State::Ready;
            ++built_;
        } else {
            it->second.state = State::Missing;
        }
    }
}

bool NavLoader::Loading(std::uint16_t region) const {
    const auto it = regions_.find(region);
    return it != regions_.end() && it->second.state == State::Loading;
}

NavLoader::Stats NavLoader::GetStats() const {
    Stats s;
    s.regions = regions_.size();
    for (const auto& [id, entry] : regions_) {
        s.loading += entry.state == State::Loading ? 1 : 0;
        s.ready += entry.state == State::Ready ? 1 : 0;
        s.missing += entry.state == State::Missing ? 1 : 0;
    }
    s.built = built_;
    std::lock_guard lock(shared_->mutex);
    for (const auto& [id, mesh] : shared_->meshes) s.objectMeshes += mesh.expired() ? 0 : 1;
    for (const auto& [path, mesh] : shared_->blockMeshes) s.objectMeshes += mesh.expired() ? 0 : 1;
    return s;
}

} // namespace sr::game
