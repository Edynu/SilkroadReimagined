#include "world/WorldStreamer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "sr/core/Log.h"
#include "sr/core/Trace.h"
#include "world/AttachedEffects.h"

namespace sr::client {

namespace {

int RegionIndex(float coordinate) { return static_cast<int>(std::floor(coordinate / assets::kRegionSize)); }

bool OnMap(int x, int y) { return x >= 0 && y >= 0 && x <= 255 && y <= 255; }

} // namespace

void WorldStreamer::Start(int originX, int originY, const assets::AssetIndex& objects, const Settings& settings) {
    originX_ = originX;
    originY_ = originY;
    objects_ = &objects;
    settings_ = settings;
}

std::uint32_t WorldStreamer::NewHandle() {
    if (!freeHandles_.empty()) {
        const std::uint32_t h = freeHandles_.back();
        freeHandles_.pop_back();
        return h;
    }
    return nextHandle_++;
}

void WorldStreamer::Update(gfx::AssetStore& assets, const glm::vec3& camera, const glm::vec3& velocity,
                           std::vector<WorldCommand>& commands) {
    SR_ZONE("world: streaming");
    const int cx = originX_ + RegionIndex(camera.x), cy = originY_ + RegionIndex(camera.z);
    cx_ = cx;
    cy_ = cy;
    const glm::vec3 ahead = camera + velocity * settings_.prefetchSeconds;
    const int ax = originX_ + RegionIndex(ahead.x), ay = originY_ + RegionIndex(ahead.z);

    // Wanted: around the camera, nearest first; then around where it is heading.
    scratch_.clear();
    const auto want = [&](int x, int y, int priority) {
        if (!OnMap(x, y)) return;
        const std::uint16_t id = assets::RegionId(x, y);
        scratch_.push_back(id);
        if (regions_.contains(id)) return;
        Region region;
        region.x = x;
        region.y = y;
        region.id = assets.RequestRegion(x, y, priority);
        regions_.emplace(id, std::move(region));
        ++requested_;
    };
    for (int ring = 0; ring <= settings_.radius; ++ring) {
        for (int y = cy - ring; y <= cy + ring; ++y) {
            for (int x = cx - ring; x <= cx + ring; ++x) {
                if (std::max(std::abs(x - cx), std::abs(y - cy)) == ring) want(x, y, ring);
            }
        }
    }
    if (ax != cx || ay != cy) {
        for (int y = ay - 1; y <= ay + 1; ++y) {
            for (int x = ax - 1; x <= ax + 1; ++x) want(x, y, std::max(std::abs(x - cx), std::abs(y - cy)) + 1);
        }
    }

    // Released: beyond `keep`, unless wanted ahead.
    for (auto it = regions_.begin(); it != regions_.end();) {
        Region& r = it->second;
        const int ring = std::max(std::abs(r.x - cx), std::abs(r.y - cy));
        if (ring <= settings_.keep || std::find(scratch_.begin(), scratch_.end(), it->first) != scratch_.end()) {
            ++it;
            continue;
        }
        Unplace(assets, r, commands);
        assets.Release(r.id);
        if (navLoader_ != nullptr && r.navigation) navLoader_->Release(it->first, *navWorld_);
        ++released_;
        it = regions_.erase(it);
    }
    // Navigation: asked for within kNavigationRing, let go a ring beyond it.
    if (navLoader_ != nullptr) {
        for (auto& [id, r] : regions_) {
            const int ring = std::max(std::abs(r.x - cx), std::abs(r.y - cy));
            if (!r.navigation && ring <= kNavigationRing) {
                navLoader_->Request(id);
                r.navigation = true;
            } else if (r.navigation && ring > kNavigationRing + 1) {
                navLoader_->Release(id, *navWorld_);
                r.navigation = false;
            }
        }
    }
    // Navigation finished on the workers joins the game's world (before anything walks this frame).
    if (navLoader_ != nullptr) navLoader_->Update(*navWorld_);

    // Placed: newly resident regions, the nearest first, a few a frame.
    int placed = 0;
    while (placed < settings_.placePerFrame) {
        Region* best = nullptr;
        int bestRing = 0;
        for (auto& [id, r] : regions_) {
            if (r.placed) continue;
            const gfx::AssetState state = assets.State(r.id);
            if (state == gfx::AssetState::Failed) {
                r.placed = true; // no such region (the sea, the map's edge): nothing to place
                continue;
            }
            if (state != gfx::AssetState::Resident) continue;
            const int ring = std::max(std::abs(r.x - cx), std::abs(r.y - cy));
            if (best == nullptr || ring < bestRing) {
                best = &r;
                bestRing = ring;
            }
        }
        if (best == nullptr) break;
        Place(assets, *best, commands);
        ++placed;
    }
}

// The region's objects as world instances, each object once (one listed by a region placed before
// is shared). Positions are relative to the region a placement names, which can be a neighbour of
// the one listing it.
void WorldStreamer::Place(gfx::AssetStore& assets, Region& region, std::vector<WorldCommand>& commands) {
    region.placed = true;
    const std::shared_ptr<const gfx::TerrainInfo> info = assets.Terrain(region.id);
    if (!info || objects_ == nullptr) return;
    region.keys.reserve(info->placements.size());
    listed_.clear();
    for (const assets::ObjectPlacement& p : info->placements) {
        // A fortress's placements and the Togui doors: only what the server spawns shows.
        if (p.isStruct) continue;
        const std::uint32_t key = PlacedKey(p.regionId, p.uid);
        if (!listed_.insert(key).second) continue; // another block of this region lists it too
        auto placed = placed_.find(key);
        if (placed != placed_.end()) {
            // Placed by a neighbour already: shared.
            ++placed->second.uses;
            region.keys.push_back(key);
            region.objects.push_back(placed->second.object);
            continue;
        }
        auto model = models_.find(p.objectId);
        if (model == models_.end()) {
            const auto entry = objects_->find(p.objectId);
            if (entry == objects_->end()) {
                ++unknownObjects_;
                continue;
            }
            const int ring = std::max(std::abs(region.x - originX_), std::abs(region.y - originY_));
            model = models_.emplace(p.objectId, ObjectModel{assets.RequestModel(entry->second.path, ring + 1), 0}).first;
        }
        ++model->second.uses;
        const float x = static_cast<float>((p.regionId & 0xFF) - originX_) * assets::kRegionSize + p.position[0];
        const float z = static_cast<float>((p.regionId >> 8) - originY_) * assets::kRegionSize + p.position[2];
        // The map's yaw turns +X toward +Z.
        const float c = std::cos(p.yaw), s = std::sin(p.yaw);
        WorldCommand cmd;
        cmd.handle = NewHandle();
        cmd.model = model->second.model;
        cmd.world = {c, 0, -s, x, 0, 1, 0, p.position[1], s, 0, c, z};
        commands.push_back(cmd);
        placed_.emplace(key, Placed{cmd.handle, p.objectId, 1, cmd.model, cmd.world, {}});
        if (effects_ != nullptr) effectsPending_.push_back(key);
        if (animating_) animationsPending_.push_back(key);
        region.keys.push_back(key);
        region.objects.push_back(p.objectId);
    }
}

void WorldStreamer::Unplace(gfx::AssetStore& assets, Region& region, std::vector<WorldCommand>& commands) {
    for (std::uint32_t key : region.keys) {
        const auto placed = placed_.find(key);
        if (placed == placed_.end() || --placed->second.uses > 0) continue; // another region still lists it
        WorldCommand c;
        c.kind = WorldCommand::Kind::Remove;
        c.handle = placed->second.handle;
        commands.push_back(c);
        FreeHandle(placed->second.handle);
        if (effects_ != nullptr) StopAttachedEffects(*effects_, placed->second.effects);
        StopAnimation(assets, key);
        if (const auto it = models_.find(placed->second.object); it != models_.end() && --it->second.uses == 0) {
            assets.Release(it->second.model);
            models_.erase(it);
        }
        placed_.erase(placed);
    }
    region.keys.clear();
    region.objects.clear();
    region.placed = false;
}

void WorldStreamer::StartEffects(const gfx::AssetStore& assets) {
    if (effects_ == nullptr || effectsPending_.empty()) return;
    std::erase_if(effectsPending_, [&](std::uint32_t key) {
        const auto placed = placed_.find(key);
        if (placed == placed_.end()) return true; // gone again
        const gfx::AssetState state = assets.State(placed->second.model);
        if (state == gfx::AssetState::Failed) return true;
        if (state != gfx::AssetState::Resident) return false;
        if (const std::shared_ptr<const gfx::ModelInfo> info = assets.Model(placed->second.model)) {
            placed->second.effects = StartAttachedEffects(*effects_, *info, placed->second.world);
        }
        return true;
    });
}

void WorldStreamer::SetPalettes(std::uint32_t base, std::uint32_t small, std::uint32_t large) {
    animating_ = small + large != 0;
    freeSmall_.clear();
    freeLarge_.clear();
    for (std::uint32_t i = small; i-- > 0;) freeSmall_.push_back(base + i * kSmallBones);
    largeBase_ = base + small * kSmallBones;
    for (std::uint32_t i = large; i-- > 0;) freeLarge_.push_back(largeBase_ + i * kLargeBones);
}

void WorldStreamer::StartAnimations(gfx::AssetStore& assets, std::vector<WorldCommand>& commands) {
    if (!animationsPending_.empty()) {
        std::erase_if(animationsPending_, [&](std::uint32_t key) {
            const auto placed = placed_.find(key);
            if (placed == placed_.end()) return true; // gone again
            const gfx::AssetState state = assets.State(placed->second.model);
            if (state == gfx::AssetState::Failed) return true;
            if (state != gfx::AssetState::Resident) return false;
            const std::shared_ptr<const gfx::ModelInfo> info = assets.Model(placed->second.model);
            if (!info || !info->skeleton || info->animationPaths.empty()) return true; // still
            const std::size_t bones = info->skeleton->bones.size();
            std::vector<std::uint32_t>& pool = bones <= kSmallBones && !freeSmall_.empty() ? freeSmall_ : freeLarge_;
            if (pool.empty() || bones > kLargeBones) {
                if (stillKinds_.insert(placed->second.object).second) {
                    core::LogWarning("world: object {} stays still ({})", placed->second.object, bones > kLargeBones ? "too many bones" : "no palette slot left");
                }
                return true;
            }
            assets::ResourceData sets;
            sets.animationPaths = info->animationPaths;
            sets.animationSets = info->animationSets;
            std::string clip = sets.StateAnimation("default", 0);
            if (clip.empty()) clip = info->animationPaths.front();
            Animated a;
            a.key = key;
            a.paletteOffset = pool.back();
            pool.pop_back();
            a.clip = assets.RequestAnimation(clip, 0);
            a.phase = float(key * 2654435761u % 10000u) / 1000.0f; // each its own moment of the loop
            a.pose.Init(*info->skeleton);
            // Its instance again, skinned (an instance's palette is fixed at its Add).
            const std::uint32_t handle = NewHandle();
            WorldCommand remove;
            remove.kind = WorldCommand::Kind::Remove;
            remove.handle = placed->second.handle;
            commands.push_back(remove);
            FreeHandle(placed->second.handle);
            WorldCommand add;
            add.handle = handle;
            add.model = placed->second.model;
            add.world = placed->second.world;
            add.paletteOffset = a.paletteOffset;
            commands.push_back(add);
            placed->second.handle = handle;
            if (animatedKinds_.insert(placed->second.object).second) core::LogInfo("world: object {} moves ({})", placed->second.object, clip);
            animated_.push_back(std::move(a));
            return true;
        });
    }
    for (Animated& a : animated_) {
        if (a.animation) continue;
        if (auto clip = assets.Animation(a.clip)) {
            a.animation = std::move(clip);
            a.pose.SetAnimation(a.animation.get());
        }
    }
}

void WorldStreamer::StopAnimation(gfx::AssetStore& assets, std::uint32_t key) {
    const auto it = std::find_if(animated_.begin(), animated_.end(), [&](const Animated& a) { return a.key == key; });
    if (it == animated_.end()) return;
    assets.Release(it->clip);
    (it->paletteOffset >= largeBase_ ? freeLarge_ : freeSmall_).push_back(it->paletteOffset);
    animated_.erase(it);
}

void WorldStreamer::Stop(gfx::AssetStore& assets, std::vector<WorldCommand>& commands) {
    for (auto& [id, r] : regions_) {
        Unplace(assets, r, commands);
        assets.Release(r.id);
        if (navLoader_ != nullptr && r.navigation) navLoader_->Release(id, *navWorld_);
    }
    regions_.clear();
    placed_.clear();
    effectsPending_.clear();
    animationsPending_.clear();
    for (Animated& a : animated_) {
        assets.Release(a.clip);
        (a.paletteOffset >= largeBase_ ? freeLarge_ : freeSmall_).push_back(a.paletteOffset);
    }
    animated_.clear();
    for (auto& [object, m] : models_) assets.Release(m.model); // none left unless a region failed to place
    models_.clear();
}

void WorldStreamer::AppendTerrain(std::vector<TerrainItem>& out) const {
    for (const auto& [id, r] : regions_) {
        out.push_back({r.id, {static_cast<float>(r.x - originX_) * assets::kRegionSize, 0.0f,
                              static_cast<float>(r.y - originY_) * assets::kRegionSize}});
    }
}

std::shared_ptr<const gfx::TerrainInfo> WorldStreamer::TerrainAt(const gfx::AssetStore& assets, float x, float z, float& localX,
                                                                  float& localZ) const {
    const int rx = RegionIndex(x), rz = RegionIndex(z);
    const auto it = regions_.find(assets::RegionId(originX_ + rx, originY_ + rz));
    if (it == regions_.end()) return nullptr;
    localX = x - static_cast<float>(rx) * assets::kRegionSize;
    localZ = z - static_cast<float>(rz) * assets::kRegionSize;
    return assets.Terrain(it->second.id);
}

bool WorldStreamer::Settled(const gfx::AssetStore& assets) const {
    const auto settled = [&](gfx::AssetId id) {
        const gfx::AssetState state = assets.State(id);
        return state == gfx::AssetState::Resident || state == gfx::AssetState::Failed;
    };
    for (const auto& [id, r] : regions_) {
        if (std::max(std::abs(r.x - cx_), std::abs(r.y - cy_)) > kNearRing) continue;
        const gfx::AssetState state = assets.State(r.id);
        if (state != gfx::AssetState::Failed && (state != gfx::AssetState::Resident || !r.placed)) return false;
        if (navLoader_ != nullptr && (!r.navigation || navLoader_->Loading(id))) return false;
        for (std::uint32_t object : r.objects) {
            const auto m = models_.find(object);
            if (m != models_.end() && !settled(m->second.model)) return false;
        }
    }
    return true;
}

float WorldStreamer::Progress(const gfx::AssetStore& assets) const {
    const auto settled = [&](gfx::AssetId id) {
        const gfx::AssetState state = assets.State(id);
        return state == gfx::AssetState::Resident || state == gfx::AssetState::Failed;
    };
    std::size_t total = 0, done = 0;
    for (const auto& [id, r] : regions_) {
        if (std::max(std::abs(r.x - cx_), std::abs(r.y - cy_)) > kNearRing) continue;
        total += 3; // the region, its placement, its navigation
        done += (settled(r.id) ? 1 : 0) + (r.placed ? 1 : 0) + (navLoader_ == nullptr || (r.navigation && !navLoader_->Loading(id)) ? 1 : 0);
        for (std::uint32_t object : r.objects) {
            const auto m = models_.find(object);
            if (m == models_.end()) continue;
            ++total;
            done += settled(m->second.model) ? 1 : 0;
        }
    }
    return total == 0 ? 1.0f : static_cast<float>(done) / static_cast<float>(total);
}

WorldStreamer::Stats WorldStreamer::GetStats(const gfx::AssetStore& assets) const {
    Stats s;
    s.regions = regions_.size();
    for (const auto& [id, r] : regions_) {
        s.resident += assets.IsResident(r.id) ? 1 : 0;
        s.placements += r.keys.size();
    }
    s.instances = placed_.size();
    s.objects = models_.size();
    s.requested = requested_;
    s.released = released_;
    s.unknownObjects = unknownObjects_;
    if (navLoader_ != nullptr) s.navigation = navLoader_->GetStats();
    return s;
}

std::uint16_t WorldStreamer::RegionAt(float x, float z, float& localX, float& localZ) const {
    const int rx = RegionIndex(x), rz = RegionIndex(z);
    localX = x - static_cast<float>(rx) * assets::kRegionSize;
    localZ = z - static_cast<float>(rz) * assets::kRegionSize;
    return assets::RegionId(std::clamp(originX_ + rx, 0, 255), std::clamp(originY_ + rz, 0, 255));
}

glm::vec2 WorldStreamer::RegionCorner(std::uint16_t region) const {
    return {static_cast<float>((region & 0xFF) - originX_) * assets::kRegionSize,
            static_cast<float>((region >> 8) - originY_) * assets::kRegionSize};
}

float WorldStreamer::CameraLimit(const gfx::AssetStore& assets, const glm::vec3& from, const glm::vec3& to) const {
    float limit = 1.0f;
    for (const auto& [key, p] : placed_) {
        const std::shared_ptr<const gfx::ModelInfo> model = assets.Model(p.model);
        if (!model) continue;
        // Its solid parts alone (a tree's trunk, not the leaves: the camera passes through those).
        const assets::Bounds& b = model->solidBounds;
        if (b.min[0] > b.max[0]) continue;
        const glm::vec3 lo(b.min[0], b.min[1], b.min[2]), hi(b.max[0], b.max[1], b.max[2]);
        const glm::mat4 world(glm::vec4(p.world[0], p.world[4], p.world[8], 0.0f), glm::vec4(p.world[1], p.world[5], p.world[9], 0.0f),
                              glm::vec4(p.world[2], p.world[6], p.world[10], 0.0f), glm::vec4(p.world[3], p.world[7], p.world[11], 1.0f));
        // Only an object near the eye: its bounding sphere about it.
        const glm::vec3 centre(world * glm::vec4((lo + hi) * 0.5f, 1.0f));
        const float radius = glm::length(hi - lo) * 0.5f * glm::length(glm::vec3(world[0]));
        if (glm::length(centre - to) > radius) continue;
        const glm::mat4 inverse = glm::inverse(world);
        const glm::vec3 a(inverse * glm::vec4(from, 1.0f)), e(inverse * glm::vec4(to, 1.0f));
        const auto inside = [&](const glm::vec3& q) { return glm::all(glm::greaterThanEqual(q, lo)) && glm::all(glm::lessThanEqual(q, hi)); };
        if (inside(a) || !inside(e)) continue;
        // Where the segment enters the box (it starts outside and ends inside).
        const glm::vec3 d = e - a;
        float enter = 0.0f;
        for (int k = 0; k < 3; ++k) {
            if (std::abs(d[k]) < 1e-6f) continue;
            const float t0 = (lo[k] - a[k]) / d[k], t1 = (hi[k] - a[k]) / d[k];
            enter = std::max(enter, std::min(t0, t1));
        }
        limit = std::min(limit, std::clamp(enter, 0.0f, 1.0f));
    }
    return limit;
}

} // namespace sr::client
