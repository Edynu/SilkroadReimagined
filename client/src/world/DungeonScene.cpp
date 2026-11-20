#include "world/DungeonScene.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "sr/assets/FileSystem.h"
#include "sr/core/Jobs.h"
#include "sr/core/Trace.h"
#include "sr/game/NavDungeon.h"
#include "world/AttachedEffects.h"

namespace sr::client {

namespace {

glm::vec3 Rgb(std::uint32_t argb) {
    return {static_cast<float>((argb >> 16) & 0xFF) / 255.0f, static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(argb & 0xFF) / 255.0f};
}

// Rows of a 3x4 matrix: rotation about +y by `yaw` (x' = x cos - z sin, as the map's objects), then
// the tilts about x and z, a scale, and the position.
std::array<float, 12> Placement(const float position[3], float yaw, float tiltX = 0.0f, float tiltZ = 0.0f, const float scale[3] = nullptr) {
    const glm::mat3 ry(glm::vec3(std::cos(yaw), 0, std::sin(yaw)), glm::vec3(0, 1, 0), glm::vec3(-std::sin(yaw), 0, std::cos(yaw)));
    const glm::mat3 rx(glm::vec3(1, 0, 0), glm::vec3(0, std::cos(tiltX), std::sin(tiltX)), glm::vec3(0, -std::sin(tiltX), std::cos(tiltX)));
    const glm::mat3 rz(glm::vec3(std::cos(tiltZ), std::sin(tiltZ), 0), glm::vec3(-std::sin(tiltZ), std::cos(tiltZ), 0), glm::vec3(0, 0, 1));
    glm::mat3 m = ry * rx * rz;
    if (scale != nullptr) m = m * glm::mat3(glm::vec3(scale[0], 0, 0), glm::vec3(0, scale[1], 0), glm::vec3(0, 0, scale[2]));
    // glm is column-major: m[column][row].
    return {m[0][0], m[1][0], m[2][0], position[0], m[0][1], m[1][1], m[2][1], position[1], m[0][2], m[1][2], m[2][2], position[2]};
}

} // namespace

void DungeonScene::Load(const assets::FileSystem& files, core::JobSystem& jobs, std::uint16_t region) {
    region_ = region;
    loaded_ = std::make_shared<Loaded>();
    jobs.Run(
        [loaded = loaded_, files = &files, region] {
            SR_ZONE("dungeon: load");
            std::string error;
            std::vector<assets::DungeonInfoEntry> info;
            const auto infoFile = files->Read("dungeon\\dungeoninfo.txt");
            if (!infoFile || !assets::DecodeDungeonInfo(infoFile->Bytes(), info, error)) {
                loaded->error = "dungeon\\dungeoninfo.txt: " + (infoFile ? error : std::string("not found"));
            } else {
                const auto entry = std::find_if(info.begin(), info.end(), [&](const auto& e) { return e.regionId == region; });
                const auto file = entry != info.end() ? files->Read(entry->path) : std::nullopt;
                if (entry == info.end()) {
                    loaded->error = std::format("no dungeon {:#06x} in dungeoninfo.txt", region);
                } else if (!file || !assets::DecodeDof(file->Bytes(), loaded->data, error)) {
                    loaded->error = entry->path + ": " + (file ? error : std::string("not found"));
                } else {
                    loaded->path = entry->path;
                }
            }
            loaded->ready.store(true, std::memory_order_release);
        },
        core::JobPriority::Streaming);
}

void DungeonScene::Prepare() {
    prepared_ = true;
    // Every block's lights in one table, so an instance's light index never changes.
    const assets::DungeonData& data = loaded_->data;
    lightBase_.resize(data.blocks.size());
    for (std::size_t b = 0; b < data.blocks.size(); ++b) {
        lightBase_[b] = static_cast<std::uint32_t>(lights_.size());
        for (const assets::DungeonData::Light& l : data.blocks[b].lights) {
            gfx::PointLight light;
            light.position = {l.position[0], l.position[1], l.position[2]};
            light.diffuse = {l.diffuse[0], l.diffuse[1], l.diffuse[2]};
            light.ambient = {l.ambient[0], l.ambient[1], l.ambient[2]};
            light.attenuation = {l.attenuation[0], l.attenuation[1], l.attenuation[2]};
            lights_.push_back(light);
        }
    }
}

std::int32_t DungeonScene::BlockUnder(const game::NavDungeon& nav, float x, float y, float z) {
    for (const float below : {0.0f, 20.0f, 200.0f, 400.0f, 800.0f}) {
        const game::NavPosition p = nav.Drop(x, y - below, z);
        if (p.Valid()) return p.instance;
    }
    return -1;
}

gfx::AssetId DungeonScene::Acquire(gfx::AssetStore& assets, const std::string& path) {
    Model& m = models_[path];
    if (m.uses++ == 0) m.id = assets.RequestModel(path, 0);
    return m.id;
}

void DungeonScene::ReleaseModel(gfx::AssetStore& assets, const std::string& path) {
    const auto it = models_.find(path);
    if (it == models_.end() || --it->second.uses > 0) return;
    assets.Release(it->second.id);
    models_.erase(it);
}

void DungeonScene::AddBlock(gfx::AssetStore& assets, std::uint32_t b, std::vector<WorldCommand>& commands, const Handles& handles) {
    const assets::DungeonData::Block& block = loaded_->data.blocks[b];
    BlockInstances instances;
    const auto add = [&](const std::string& path, const std::array<float, 12>& world, std::uint16_t light) {
        WorldCommand cmd;
        cmd.handle = handles.take();
        cmd.model = Acquire(assets, path);
        cmd.world = world;
        cmd.light = light;
        commands.push_back(cmd);
        instances.handles.push_back(cmd.handle);
        instances.models.push_back(path);
        instances.worlds.push_back(world);
        instances.effectsStarted.push_back(false);
    };
    add(block.path, Placement(block.position, block.yaw), 0);
    for (const assets::DungeonData::Object& o : block.objects) {
        if (o.flags & 4) continue; // a water quad, not a model
        // The block light of the object's index.
        const std::uint16_t light = o.index < block.lights.size() ? static_cast<std::uint16_t>(lightBase_[b] + o.index + 1) : std::uint16_t{0};
        add(o.path, Placement(o.position, o.rotation[1], o.rotation[0], o.rotation[2], o.scale), light);
    }
    instances.effectsPending = effects_ != nullptr;
    blocks_[b] = std::move(instances);
}

void DungeonScene::RemoveBlock(gfx::AssetStore& assets, std::uint32_t b, std::vector<WorldCommand>& commands, const Handles& handles) {
    const auto it = blocks_.find(b);
    if (it == blocks_.end()) return;
    for (std::uint32_t handle : it->second.handles) {
        WorldCommand cmd;
        cmd.kind = WorldCommand::Kind::Remove;
        cmd.handle = handle;
        commands.push_back(cmd);
        handles.give(handle);
    }
    if (effects_ != nullptr) StopAttachedEffects(*effects_, it->second.effects);
    for (const std::string& path : it->second.models) ReleaseModel(assets, path);
    blocks_.erase(it);
}

void DungeonScene::Show(gfx::AssetStore& assets, std::int32_t current, std::vector<WorldCommand>& commands, const Handles& handles) {
    if (!Ready() || !loaded_->error.empty()) return;
    if (!prepared_) Prepare();
    const assets::DungeonData& data = loaded_->data;
    if (current < 0 || static_cast<std::size_t>(current) >= data.blocks.size()) {
        if (current_ >= 0) return; // keeps the last block while the floor under the player is not found
        // None yet: the first entrance block (the original's player always stands on a floor).
        current = 0;
        for (std::size_t b = 0; b < data.blocks.size(); ++b) {
            if (data.blocks[b].flags & 1) {
                current = static_cast<std::int32_t>(b);
                break;
            }
        }
        if (data.blocks.empty()) return;
    }
    current_ = current;
    std::vector<std::uint32_t> wanted = data.blocks[static_cast<std::size_t>(current_)].visible;
    wanted.push_back(static_cast<std::uint32_t>(current_));
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    if (wanted == shown_) return;
    for (std::uint32_t b : shown_) {
        if (!std::binary_search(wanted.begin(), wanted.end(), b)) RemoveBlock(assets, b, commands, handles);
    }
    for (std::uint32_t b : wanted) {
        if (b < data.blocks.size() && !std::binary_search(shown_.begin(), shown_.end(), b)) AddBlock(assets, b, commands, handles);
    }
    shown_ = std::move(wanted);
}

void DungeonScene::Light(SceneView& scene) const {
    if (!Ready() || !loaded_->error.empty() || loaded_->data.blocks.empty()) return;
    // The current block's light and fog: its ambient colour for the lit models (the
    // directional light's diffuse is black), its fog colour as the background and the fog.
    const assets::DungeonData::Block& block = loaded_->data.blocks[static_cast<std::size_t>(std::max(current_, 0))];
    gfx::SceneLighting& light = scene.lighting;
    light.objectAmbient = Rgb(block.ambient);
    light.diffuse = glm::vec3(0.0f);
    light.objectLight = glm::vec3(0.0f, 0.0f, 1.0f);
    light.fog = true;
    light.fogColor = light.terrainFogColor = Rgb(block.fog.color);
    light.fogStart = block.fog.start;
    light.fogEnd = block.fog.end;
    light.objectFog = gfx::SceneLighting::ObjectFog::SquaredExponential;
    light.fogDensity = block.fog.density;
    scene.lights = lights_;
    scene.clearColor = {light.fogColor.r, light.fogColor.g, light.fogColor.b, 1.0f};
    scene.lensFlare = false;
}

bool DungeonScene::Settled(const gfx::AssetStore& assets) const {
    if (!Ready()) return false;
    if (!loaded_->error.empty()) return true; // nothing to wait for
    if (shown_.empty()) return false;
    for (const auto& [path, m] : models_) {
        const gfx::AssetState state = assets.State(m.id);
        if (state != gfx::AssetState::Resident && state != gfx::AssetState::Failed) return false;
    }
    return true;
}

float DungeonScene::Progress(const gfx::AssetStore& assets) const {
    if (models_.empty()) return 0.0f;
    std::size_t done = 0;
    for (const auto& [path, m] : models_) {
        const gfx::AssetState state = assets.State(m.id);
        done += state == gfx::AssetState::Resident || state == gfx::AssetState::Failed ? 1 : 0;
    }
    return static_cast<float>(done) / static_cast<float>(models_.size());
}

void DungeonScene::StartEffects(const gfx::AssetStore& assets) {
    if (effects_ == nullptr) return;
    for (auto& [b, instances] : blocks_) {
        if (!instances.effectsPending) continue;
        bool pending = false;
        for (std::size_t i = 0; i < instances.models.size(); ++i) {
            if (instances.effectsStarted[i]) continue;
            const auto model = models_.find(instances.models[i]);
            const gfx::AssetState state = model != models_.end() ? assets.State(model->second.id) : gfx::AssetState::Failed;
            if (state != gfx::AssetState::Resident && state != gfx::AssetState::Failed) {
                pending = true;
                continue;
            }
            instances.effectsStarted[i] = true;
            if (state != gfx::AssetState::Resident) continue;
            if (const std::shared_ptr<const gfx::ModelInfo> info = assets.Model(model->second.id)) {
                std::vector<std::uint32_t> started = StartAttachedEffects(*effects_, *info, instances.worlds[i]);
                instances.effects.insert(instances.effects.end(), started.begin(), started.end());
            }
        }
        instances.effectsPending = pending;
    }
}

std::size_t DungeonScene::Instances() const {
    std::size_t n = 0;
    for (const auto& [b, i] : blocks_) n += i.handles.size();
    return n;
}

void DungeonScene::Clear(gfx::AssetStore& assets, std::vector<WorldCommand>& commands, const Handles& handles) {
    for (std::uint32_t b : std::vector<std::uint32_t>(shown_)) RemoveBlock(assets, b, commands, handles);
    shown_.clear();
    for (auto& [path, m] : models_) assets.Release(m.id);
    models_.clear();
    current_ = -1;
}

} // namespace sr::client
