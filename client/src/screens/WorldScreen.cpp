#include "screens/WorldScreen.h"

#include "sr/game/NavDungeon.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>

#include <algorithm>
#include <format>
#include <span>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "audio/GameAudio.h"
#include "sr/assets/Animation.h"
#include "sr/assets/FileSystem.h"
#include "sr/core/Log.h"
#include "sr/core/Path.h"
#include "sr/core/Trace.h"

namespace sr::client {

namespace {

// Characters and monsters of the game: different skeletons, sizes and part counts. Dressed walkers
// (the crowd) are the game's player figures instead of the res\char models; the monsters stay.
constexpr const char* kDefaultCharacters =
    "res\\char\\europe\\europeman_knight.bsr,res\\char\\europe\\europewoman_amazoness.bsr,res\\char\\europe\\europeman_warrior.bsr,"
    "res\\char\\china\\chinaman_fighter.bsr,res\\char\\china\\chinaman_monk.bsr,res\\char\\china\\chinaman_adventurer.bsr,"
    "res\\mob\\china\\tiger.bsr,res\\mob\\europe\\wolf.bsr,res\\mob\\china\\whitetiger.bsr,res\\mob\\europe\\ladon.bsr,"
    "res\\mob\\oasis\\blackrobberarcher.bsr,res\\mob\\roc\\wingtribe.bsr";
constexpr std::uint32_t kMaxBones = 128;      // palette room per character
// The moving map objects' palettes (WorldStreamer::SetPalettes), after the characters': its small slots (a tree's few
// bones; Jangan's view holds some 570 moving objects) and its large ones.
constexpr std::uint32_t kObjectSmall = 3072, kObjectLarge = 256;
constexpr std::uint32_t kObjectMatrices = kObjectSmall * WorldStreamer::kSmallBones + kObjectLarge * WorldStreamer::kLargeBones;
constexpr float kWalkSpeed = 40.0f;           // units a second

std::uint32_t Hash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float Random(std::uint32_t& state) {
    state = Hash(state + 0x9e3779b9U);
    return static_cast<float>(state & 0xFFFFFF) / static_cast<float>(0x1000000);
}

// A looping clip to walk with: the plainest one named like running (not a skill's; the shortest
// name), else walking, else the first.
int PickClip(const std::vector<std::string>& paths, bool idle = false) {
    static constexpr const char* kWalk[] = {"run", "walk"};
    static constexpr const char* kIdle[] = {"stand", "wait", "idle"};
    for (const char* word : idle ? std::span<const char* const>(kIdle) : std::span<const char* const>(kWalk)) {
        int best = -1;
        std::size_t bestLength = 0;
        for (std::size_t i = 0; i < paths.size(); ++i) {
            std::string name = paths[i].substr(paths[i].find_last_of('\\') + 1);
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (name.find(word) == std::string::npos || name.find("skill") != std::string::npos) continue;
            if (best < 0 || name.size() < bestLength) {
                best = static_cast<int>(i);
                bestLength = name.size();
            }
        }
        if (best >= 0) return best;
    }
    return paths.empty() ? -1 : 0;
}

} // namespace

void WorldScreen::Enter(ScreenContext& context) {
    const std::string region = backdrop_ ? std::format("{},{}", backdrop_->originX, backdrop_->originY)
                                         : context.config.GetString("region", "78,105");
    if (std::sscanf(region.c_str(), "%d,%d", &originX_, &originY_) != 2) {
        core::LogWarning("--region {}: expected x,y; using 78,105", region);
        originX_ = 78;
        originY_ = 105;
    }
    if (originY_ >= 128) { // a dungeon's region (bit 15 set): its own space instead of the world's
        dungeonRegion_ = assets::RegionId(originX_, originY_);
        dungeon_ = std::make_unique<DungeonScene>();
    }
    streaming_.radius = std::clamp(context.config.GetInt("world-radius", 2), 0, 7);
    streaming_.keep = streaming_.radius + 1;
    const std::string flight = context.config.GetString("flight-to", "");
    int fx = 0, fy = 0;
    if (!backdrop_ && !flight.empty() && std::sscanf(flight.c_str(), "%d,%d", &fx, &fy) == 2) {
        flight_ = true;
        flightTarget_ = glm::vec2((static_cast<float>(fx - originX_) + 0.5f) * assets::kRegionSize,
                                  (static_cast<float>(fy - originY_) + 0.5f) * assets::kRegionSize);
        flightSpeed_ = static_cast<float>(context.config.GetDouble("flight-speed", 150.0));
    }
    indexes_ = std::make_shared<Indexes>();
    navLoader_ = std::make_unique<game::NavLoader>(context.files, context.jobs);
    effects_.Init(context.assets, context.files, context.jobs);
    effects_.SetQuality(std::clamp(context.config.GetInt("effect-quality", 2), 0, 2)); // the original's video option 13
    context.jobs.Run(
        [indexes = indexes_, files = &context.files] {
            SR_ZONE("world: indexes");
            const auto objects = files->Read("object.ifo");
            const auto tiles = files->Read("tile2d.ifo");
            auto tileIndex = std::make_shared<assets::AssetIndex>();
            if (!objects || !tiles) {
                indexes->error = "object.ifo / tile2d.ifo not found";
            } else if (!assets::DecodeObjectIndex(objects->Bytes(), indexes->objects, indexes->error) ||
                       !assets::DecodeTileIndex(tiles->Bytes(), *tileIndex, indexes->error)) {
                indexes->objects.clear();
            }
            indexes->tiles = std::move(tileIndex);
            std::string environmentError;
            const auto environment = files->Read("environment.ifo");
            if (!environment || !assets::DecodeEnvironment(environment->Bytes(), indexes->environment, environmentError)) {
                core::LogWarning("world: environment.ifo: {}", environment ? environmentError : std::string("not found"));
                indexes->environment = {};
            }
            indexes->ready.store(true, std::memory_order_release);
        },
        core::JobPriority::Streaming);

    // The prototype World Viewer's camera, placed once the origin region's heights are known.
    camera_.fly = true;
    camera_.fovY = backdrop_ && backdrop_->fovY > 0.0f ? backdrop_->fovY : 60.0f * 3.14159265f / 180.0f;
    camera_.nearZ = 1.0f;
    camera_.farZ = 6000.0f;
    ResetCamera();
    // --camera x,y,z,yaw,pitch (relative to the origin region's south-west corner) instead.
    const std::string place = backdrop_ ? std::string() : context.config.GetString("camera", "");
    if (!place.empty()) {
        if (std::sscanf(place.c_str(), "%f,%f,%f,%f,%f", &camera_.targetX, &camera_.targetY, &camera_.targetZ, &camera_.yaw,
                        &camera_.pitch) == 5) {
            cameraPlaced_ = true;
            placedCamera_ = camera_; // also after a teleport, relative to the destination
        } else {
            core::LogWarning("--camera {}: expected x,y,z,yaw,pitch", place);
        }
    }
    if (!backdrop_) context.camera.Store(camera_.yaw, camera_.pitch);

    clock_.Set(context.config.GetDouble("clock", 12.0), std::max(0, context.config.GetInt("day", 0)));
    fixedTime_ = static_cast<float>(context.config.GetDouble("time-of-day", -1.0));
    // The original's view distance presets 1500 .. 5500; the fog range is 0.8 of it, at most 2500.
    const int preset = std::clamp(context.config.GetInt("view-distance", 4), 0, 4);
    viewDistance_ = 1500.0f + 1000.0f * static_cast<float>(preset);
    fogRange_ = std::min(0.8f * viewDistance_, 2500.0f);
    glow_ = context.config.GetBool("glow", true) && (!backdrop_ || backdrop_->glow);
    lensFlare_ = context.config.GetBool("lens-flare", true);
    if (backdrop_) {
        // The stage's slots, their palettes reserved now (creating GPU memory during play stalls).
        if (backdrop_->stageSlots > 0) {
            characters_.resize(backdrop_->stageSlots);
            for (std::size_t i = 0; i < characters_.size(); ++i) {
                Character& c = characters_[i];
                c.active = false;
                c.stage = true;
                c.paletteOffset = static_cast<std::uint32_t>(i) * kMaxBones;
            }
            objectPalettes_ = static_cast<std::uint32_t>(characters_.size()) * kMaxBones;
            paletteCapacity_ = objectPalettes_ + kObjectMatrices;
            rhi::BufferDesc desc;
            desc.size = std::size_t{paletteCapacity_} * sizeof(glm::mat4);
            desc.usage = rhi::BufferUsage::Storage;
            desc.cpuVisible = true;
            desc.debugName = "stage palettes";
            for (unsigned slot = 0; slot < SnapshotQueue::kSlots; ++slot) palettes_.push_back(context.device.CreateBuffer(desc));
        }
        assets_ = &context.assets;
        return; // the test options below are not the title's
    }
    if (context.config.GetBool("hud", false)) {
        hudFont_ = std::make_shared<HudFont>();
        context.jobs.Run(
            [font = hudFont_, files = &context.files] {
                SR_ZONE("world: hud font");
                std::string error;
                // The game's interface font (Media.pk2 fonts\<"English font" in CP949>.ttf).
                if (const auto file = files->Read("fonts\\\xBF\xB5\xB9\xAE\xBC\xAD\xC3\xBC.ttf")) {
                    const auto bytes = file->Bytes();
                    if (font->font.Load(std::vector<std::byte>(bytes.begin(), bytes.end()), error)) {
                        font->ready.store(true, std::memory_order_release);
                        return;
                    }
                }
                core::LogWarning("world: no interface font: {}", error.empty() ? std::string("not found") : error);
            },
            core::JobPriority::Streaming);
    }
    // A teleport (C4e): --teleport-to X,Y at --teleport-at S seconds.
    const std::string teleport = context.config.GetString("teleport-to", "");
    if (!teleport.empty()) {
        if (std::sscanf(teleport.c_str(), "%d,%d", &teleportX_, &teleportY_) == 2) {
            teleportAt_ = context.config.GetDouble("teleport-at", 5.0);
        } else {
            core::LogWarning("--teleport-to {}: expected x,y", teleport);
        }
    }
    captureLoading_ = context.config.GetBool("capture-loading", false);
    testEffect_ = context.config.GetString("effect", "");
    testEffectBone_ = context.config.GetString("effect-bone", "");
    testEffectScale_ = static_cast<float>(context.config.GetDouble("effect-scale", 1.0));
    {
        const std::string kind = context.config.GetString("weather", "clear");
        const game::WeatherKind weather = kind == "rain" ? game::WeatherKind::Rain
                                          : kind == "snow" ? game::WeatherKind::Snow
                                                           : game::WeatherKind::Clear;
        if (kind != "rain" && kind != "snow" && kind != "clear") core::LogWarning("--weather {}: rain, snow or clear", kind);
        weather_.Set(weather, context.config.GetInt("weather-amount", 128));
        lightningAt_ = context.config.GetDouble("lightning-at", -1.0);
    }

    StartMenu(context);
    // --characters N shown of a pool of --characters-max M (the crowd's sliders show the rest).
    const int shown = std::max(0, context.config.GetInt("characters", 0));
    const int characters = std::max(shown, context.config.GetInt("characters-max", shown));
    if (characters > 0) {
        std::stringstream list(context.config.GetString("character-models", kDefaultCharacters));
        for (std::string path; std::getline(list, path, ',');) {
            if (path.empty()) continue;
            CharacterModel m;
            m.path = path;
            m.model = context.assets.RequestModel(path, 0);
            characterModels_.push_back(std::move(m));
        }
        characters_.resize(static_cast<std::size_t>(characters));
        for (std::size_t i = 0; i < characters_.size(); ++i) {
            Character& c = characters_[i];
            c.type = static_cast<std::uint32_t>(i % std::max<std::size_t>(1, characterModels_.size()));
            c.random = Hash(12345u + static_cast<std::uint32_t>(i));
            c.phase = Random(c.random) * 10.0f;
            c.paletteOffset = static_cast<std::uint32_t>(i) * kMaxBones;
        }
        // --walker-at x,z: the first walker stands there (origin-region units), the second 60 east of
        // it facing it (a target for its skills): close-ups of one character's skills and glows.
        if (const std::string at = context.config.GetString("walker-at", ""); !at.empty()) {
            float px = 0, pz = 0;
            if (std::sscanf(at.c_str(), "%f,%f", &px, &pz) == 2) {
                for (std::size_t i = 0; i < std::min<std::size_t>(2, characters_.size()); ++i) {
                    Character& c = characters_[i];
                    c.pinned = true;
                    c.pinX = px + (i == 0 ? 0.0f : 60.0f);
                    c.pinZ = pz;
                    c.dirX = i == 0 ? 1.0f : -1.0f;
                    c.dirZ = 0.0f;
                }
            }
        }
        objectPalettes_ = static_cast<std::uint32_t>(characters_.size()) * kMaxBones;
        paletteCapacity_ = objectPalettes_ + kObjectMatrices;
        if (!testEffect_.empty()) {
            testEffects_.assign(std::min<std::size_t>(characters_.size(), std::max(0, context.config.GetInt("effects", characters))), 0);
        }
        // Reserved now, at the screen's start: creating GPU memory during play stalls the driver.
        rhi::BufferDesc desc;
        desc.size = std::size_t{paletteCapacity_} * sizeof(glm::mat4);
        desc.usage = rhi::BufferUsage::Storage;
        desc.cpuVisible = true;
        desc.debugName = "world palettes";
        for (unsigned slot = 0; slot < SnapshotQueue::kSlots; ++slot) palettes_.push_back(context.device.CreateBuffer(desc));
        if (context.config.GetBool("dress-walkers", true)) StartCrowd(context, static_cast<std::size_t>(shown));
    }
}

void WorldScreen::Exit(ScreenContext& context, FrameSnapshot& snapshot) {
    ExitCrowd(context);
    ExitMenu(snapshot);
    {
        // What the session streamed, and the memory the store held at the end (C4a's check).
        const WorldStreamer::Stats s = streamer_.GetStats(context.assets);
        const gfx::AssetStore::Stats a = context.assets.GetStats();
        const auto mib = [](std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
        core::LogInfo("world: {} regions requested, {} released; store: {} assets ({} resident; {} queued, {} loading, {} waiting, {} uploading, "
                      "{} failed), {:.0f} MiB textures of {:.0f}, geometry {:.0f} of {:.0f} MiB, {} evicted, {} parts untextured",
                      s.requested, s.released, a.live, a.resident, a.queued, a.loading, a.waiting, a.uploading, a.failed, mib(a.textureBytes),
                      mib(a.textureBudgetBytes), mib(a.geometry.used), mib(a.geometry.capacity), a.evicted, a.untextured);
        core::LogInfo("world: navigation: {} regions held ({} without a navmesh), {} built, {} object navmeshes in use",
                      s.navigation.ready, s.navigation.missing, s.navigation.built, s.navigation.objectMeshes);
        if (weather_.Falling()) {
            const WeatherEffect::Stats w = weatherEffect_.GetStats();
            core::LogInfo("world: weather {} at {}: {} rain clusters, {} snow clusters, {} ripples, {} lightning strikes",
                          static_cast<int>(weather_.Kind()), weather_.Effective(), w.rain, w.snow, w.ripples, w.strikes);
        }
        if (!characters_.empty()) {
            std::size_t onObjects = 0;
            for (const Character& c : characters_) onObjects += c.spawned && c.nav.instance >= 0 ? 1 : 0;
            core::LogInfo("world: {} characters walking, {} of them on objects", spawned_, onObjects);
        }
    }
    for (const Character& c : characters_) {
        if (!c.spawned) continue;
        WorldCommand remove;
        remove.kind = WorldCommand::Kind::Remove;
        remove.handle = c.handle;
        snapshot.worldCommands.push_back(remove);
    }
    streamer_.Stop(context.assets, snapshot.worldCommands);
    if (dungeon_) dungeon_->Clear(context.assets, snapshot.worldCommands, DungeonHandles());
    if (const gfx::EffectSystem::Stats& e = effects_.GetStats(); e.programs + e.loading + e.failed > 0) {
        core::LogInfo("world: effects: {} alive ({} seen) at the end, {} elements, {} sprites, {} mesh instances, {} lights; {} programs, {} failed",
                      e.effects, e.visible, e.elements, e.sprites, e.meshInstances, e.lights, e.programs, e.failed);
    }
    effects_.Shutdown();
    navWorld_.Clear();
    navLoader_.reset(); // waits for its jobs
    loading_.End(context.assets);
    for (const CharacterModel& m : characterModels_) {
        if (m.released) continue;
        context.assets.Release(m.model);
        if (m.animation) context.assets.Release(m.animation);
    }
    for (const Prop& p : props_) {
        if (p.added) {
            WorldCommand remove;
            remove.kind = WorldCommand::Kind::Remove;
            remove.handle = p.handle;
            snapshot.worldCommands.push_back(remove);
        }
        context.assets.Release(p.model);
    }
    props_.clear();
    for (auto& [key, p] : ground_) {
        if (p.added) {
            WorldCommand remove;
            remove.kind = WorldCommand::Kind::Remove;
            remove.handle = p.handle;
            snapshot.worldCommands.push_back(remove);
        }
        context.assets.Release(p.model);
    }
    ground_.clear();
    for (auto& [key, p] : flying_) {
        if (p.added) {
            WorldCommand remove;
            remove.kind = WorldCommand::Kind::Remove;
            remove.handle = p.handle;
            snapshot.worldCommands.push_back(remove);
        }
        context.assets.Release(p.model);
    }
    flying_.clear();
    characterModels_.clear();
    characters_.clear();
    for (rhi::BufferHandle buffer : palettes_) snapshot.releaseBuffers.push_back(buffer);
    if (const rhi::TextureHandle atlas = hudAtlas_.Release()) snapshot.releaseTextures.push_back(atlas);
    palettes_.clear();
    context.camera.Clear();
}

void WorldScreen::Start(ScreenContext& context) {
    started_ = true;
    if (!indexes_->error.empty()) {
        core::LogError("world: {}", indexes_->error);
        return;
    }
    context.assets.SetTileIndex(indexes_->tiles);
    if (dungeon_) {
        dungeon_->SetEffects(&effects_);
        dungeon_->Load(context.files, context.jobs, dungeonRegion_);
        navLoader_->Request(dungeonRegion_);
    } else {
        // The moving objects' palettes: after the characters' (made with them), else on their own now.
        if (palettes_.empty()) {
            objectPalettes_ = 0;
            paletteCapacity_ = kObjectMatrices;
            rhi::BufferDesc desc;
            desc.size = std::size_t{paletteCapacity_} * sizeof(glm::mat4);
            desc.usage = rhi::BufferUsage::Storage;
            desc.cpuVisible = true;
            desc.debugName = "world palettes";
            for (unsigned slot = 0; slot < SnapshotQueue::kSlots; ++slot) palettes_.push_back(context.device.CreateBuffer(desc));
        }
        streamer_.SetPalettes(objectPalettes_, kObjectSmall, kObjectLarge);
        streamer_.SetEffects(&effects_);
        streamer_.SetNavigation(navLoader_.get(), &navWorld_);
        streamer_.Start(originX_, originY_, indexes_->objects, streaming_);
    }
    lastCamera_ = glm::vec3(camera_.targetX, camera_.targetY, camera_.targetZ);
    if (!backdrop_) StartLoading(context, game::LoadingKind::FirstEntry);
}

std::uint32_t WorldScreen::StageModel(const StageLook& look) {
    for (std::size_t i = 0; i < characterModels_.size(); ++i) {
        const CharacterModel& m = characterModels_[i];
        if (!m.released && m.idle && m.path == look.model && m.stance == look.stance) {
            return static_cast<std::uint32_t>(i);
        }
    }
    CharacterModel m;
    m.path = look.model;
    m.stance = look.stance;
    m.state = look.state;
    m.idle = true;
    m.model = assets_->RequestModel(look.model, 0);
    // A released entry is reused (looks change often on the create screen).
    for (std::size_t i = 0; i < characterModels_.size(); ++i) {
        if (characterModels_[i].released) {
            characterModels_[i] = std::move(m);
            return static_cast<std::uint32_t>(i);
        }
    }
    characterModels_.push_back(std::move(m));
    return static_cast<std::uint32_t>(characterModels_.size() - 1);
}

std::uint32_t WorldScreen::ClipKey(std::string_view set, std::uint32_t state) {
    std::uint32_t index = 0;
    if (!set.empty()) {
        const auto it = std::find(clipSets_.begin(), clipSets_.end(), set);
        index = static_cast<std::uint32_t>(it - clipSets_.begin());
        if (it == clipSets_.end()) clipSets_.emplace_back(set);
    }
    return (index << 24) | (state & 0xFFFFFFu);
}

std::string WorldScreen::ClipPath(const CharacterModel& m, std::string_view set, std::uint32_t state, bool* inSet) {
    std::string name = m.sets.StateAnimation(set.empty() ? std::string_view(m.stance) : set, state);
    if (inSet != nullptr) *inSet = !name.empty();
    if (name.empty() && !set.empty()) name = m.sets.StateAnimation(m.stance, state);
    if (name.empty()) name = m.sets.StateAnimation("default", state);
    return name;
}

WorldScreen::CharacterModel::Clip& WorldScreen::ClipEntry(CharacterModel& m, std::string_view set, std::uint32_t state) {
    CharacterModel::Clip& clip = m.clips[ClipKey(set, state)];
    if (clip.id || clip.missing || assets_ == nullptr) return clip;
    // In the set asked for (a skill's group; none: the weapon's stance), else the stance, else the
    // empty-handed set (counted: the skill bench reports them).
    bool inSet = false;
    const std::string name = ClipPath(m, set, state, &inSet);
    const std::string_view from = set.empty() ? std::string_view(m.stance) : set;
    if (name.empty()) {
        ++clipCounts_.missing;
        core::LogWarning("world: {} has no clip for state {} (set {})", m.path, state, from);
        clip.missing = true;
        return clip;
    }
    if (inSet) {
        ++clipCounts_.inStance;
    } else {
        ++clipCounts_.fromDefault;
        if (from != "default") core::LogInfo("world: {} plays state {} from another set than {}", m.path, state, from);
    }
    clip.id = assets_->RequestAnimation(name, 0);
    return clip;
}

const assets::AnimationData* WorldScreen::ClipFor(CharacterModel& m, std::string_view set, std::uint32_t state, bool& missing) {
    missing = false;
    if (state == m.state && (set.empty() || set == m.stance)) {
        missing = m.clip == nullptr;
        return m.clip.get();
    }
    CharacterModel::Clip& clip = ClipEntry(m, set, state);
    if (clip.missing || !clip.id) {
        missing = true;
        return nullptr;
    }
    if (!clip.data) clip.data = assets_->Animation(clip.id);
    if (!clip.data && assets_->State(clip.id) == gfx::AssetState::Failed) clip.missing = missing = true;
    return clip.data.get();
}

double WorldScreen::StageClipSeconds(std::size_t slot, std::string_view set, std::uint32_t state) {
    if (slot >= characters_.size()) return -1.0;
    const Character& c = characters_[slot];
    if (!c.active || c.type >= characterModels_.size()) return -1.0;
    bool missing = false;
    const assets::AnimationData* clip = ClipFor(characterModels_[c.type], set, state, missing);
    return clip != nullptr ? double(clip->durationMs) / 1000.0 : -1.0;
}

void WorldScreen::SetStageOpacity(std::size_t slot, float opacity) {
    if (slot < characters_.size()) characters_[slot].opacity = opacity;
}

void WorldScreen::SetStageTint(std::size_t slot, const glm::vec3* colour) {
    if (slot >= characters_.size()) return;
    characters_[slot].tinted = colour != nullptr;
    if (colour != nullptr) characters_[slot].tint = *colour;
}

void WorldScreen::SetStage(std::size_t slot, const StageLook& look, float x, float y, float z, float yaw) {
    if (slot >= characters_.size() || !assets_) return;
    Character& c = characters_[slot];
    c.x = x;
    c.y = y;
    c.z = z;
    c.dirX = std::sin(yaw);
    c.dirZ = std::cos(yaw);
    c.transform.reset();
    if (c.active && c.look == look) return;
    // Only the state changed (or its pose held): the same model plays another clip (UpdateCharacters
    // blends into it).
    StageLook same = look;
    same.state = c.look.state;
    same.clipSet = c.look.clipSet;
    same.take = c.look.take;
    same.held = c.look.held;
    same.hideWeapon = c.look.hideWeapon;
    same.spine = c.look.spine;
    same.flinch = c.look.flinch;
    same.cycle = c.look.cycle;
    same.cycleSet = c.look.cycleSet;
    same.grow = c.look.grow;
    if (c.active && c.look == same) {
        c.look.state = look.state;
        c.look.clipSet = look.clipSet;
        c.look.take = look.take;
        c.look.held = look.held;
        c.look.hideWeapon = look.hideWeapon;
        c.look.spine = look.spine;
        c.look.flinch = look.flinch;
        c.look.cycle = look.cycle;
        c.look.cycleSet = look.cycleSet;
        c.look.grow = look.grow;
        return;
    }
    if (!c.active || !c.spawned) {
        // Nothing shown yet: the new look takes the slot at once.
        ClearStage(slot);
        c.type = StageModel(look);
        c.nextType = kNoType;
        c.look = look;
        c.active = true;
        return;
    }
    const bool volumeOnly = c.look.volume != look.volume && StageModel(look) == c.type;
    c.look = look;
    c.nextType = StageModel(look);
    if (c.nextType == c.type) c.nextType = kNoType;
    // The same model with another volume (the create screen's Body slider): no new spawn, the pose's bones change now.
    if (volumeOnly && c.type < characterModels_.size()) {
        const CharacterModel& m = characterModels_[c.type];
        if (m.ready && m.info && m.info->skeleton) ApplyVolume(c, *m.info->skeleton);
    }
}

void WorldScreen::ApplyVolume(Character& c, const assets::SkeletonData& skeleton) {
    std::vector<float> radial(skeleton.bones.size(), 1.0f);
    for (const auto& [bone, factor] : c.look.volume) {
        const int b = skeleton.Find(bone);
        if (b >= 0) radial[static_cast<std::size_t>(b)] = factor;
    }
    c.pose.SetRadialScale(std::move(radial));
}

void WorldScreen::SetStageTransform(std::size_t slot, const std::array<float, 12>& world) {
    if (slot >= characters_.size()) return;
    Character& c = characters_[slot];
    c.transform = world;
    c.x = world[3];
    c.y = world[7];
    c.z = world[11];
}

bool WorldScreen::StageReady(std::size_t slot) const {
    return slot < characters_.size() && characters_[slot].active && characters_[slot].spawned && characters_[slot].nextType == kNoType;
}

void WorldScreen::ReleaseUnused(ScreenContext& context) {
    for (std::size_t i = 0; i < characterModels_.size(); ++i) {
        CharacterModel& m = characterModels_[i];
        if (!m.idle || m.released) continue;
        const bool used = std::any_of(characters_.begin(), characters_.end(), [&](const Character& c) {
            return c.active && (c.type == i || c.nextType == i);
        });
        if (used) continue;
        context.assets.Release(m.model);
        if (m.animation) context.assets.Release(m.animation);
        for (auto& [state, clip] : m.clips) {
            if (clip.id) context.assets.Release(clip.id);
        }
        m = CharacterModel{};
        m.released = true;
    }
}

void WorldScreen::ClearStage(std::size_t slot) {
    if (slot >= characters_.size()) return;
    Character& c = characters_[slot];
    if (c.spawned) {
        pendingRemoves_.push_back(c.handle);
        --spawned_;
    }
    c.spawned = false;
    c.active = false;
    c.opacity = 1.0f;
    c.tinted = false;
    c.nextType = kNoType;
    c.look = {};
}

void WorldScreen::AddProp(const std::string& model, float x, float y, float z, float yaw, float scale) {
    if (!assets_) return;
    Prop p;
    p.model = assets_->RequestModel(model, 0);
    const float c = std::cos(yaw) * scale, s = std::sin(yaw) * scale;
    p.world = {c, 0, -s, x, 0, scale, 0, y, s, 0, c, z}; // as the characters' moves
    props_.push_back(p);
}

void WorldScreen::SetGroundItem(std::uint32_t key, const std::string& model, float x, float y, float z, float yaw, bool shown) {
    if (!assets_ || ground_.contains(key)) return;
    Prop p;
    p.model = assets_->RequestModel(model, 0);
    p.world = GroundPlace(x, y, z, yaw);
    p.hidden = !shown;
    ground_[key] = p;
}

void WorldScreen::ShowGroundItem(std::uint32_t key) {
    if (const auto it = ground_.find(key); it != ground_.end()) it->second.hidden = false;
}

std::array<float, 12> WorldScreen::GroundPlace(float x, float y, float z, float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {c, 0, -s, x, 0, 1, 0, y, s, 0, c, z};
}

void WorldScreen::SetFlying(std::uint32_t key, const std::string& model, const std::array<float, 12>& world) {
    if (!assets_) return;
    auto it = flying_.find(key);
    if (it == flying_.end()) {
        Prop p;
        p.model = assets_->RequestModel(model, 0);
        it = flying_.emplace(key, p).first;
    }
    it->second.world = world;
}

void WorldScreen::ClearFlying(std::uint32_t key) {
    const auto it = flying_.find(key);
    if (it == flying_.end()) return;
    if (it->second.added) pendingRemoves_.push_back(it->second.handle);
    if (assets_) assets_->Release(it->second.model);
    flying_.erase(it);
}

void WorldScreen::ClearGroundItem(std::uint32_t key) {
    const auto it = ground_.find(key);
    if (it == ground_.end()) return;
    for (const std::uint32_t h : it->second.ambient) effects_.Stop(h);
    if (it->second.added) pendingRemoves_.push_back(it->second.handle);
    if (assets_) assets_->Release(it->second.model);
    ground_.erase(it);
}

bool WorldScreen::PropBounds(std::size_t index, assets::Bounds& out) const {
    if (index >= props_.size() || !assets_) return false;
    const Prop& p = props_[index];
    const std::shared_ptr<const gfx::ModelInfo> info = assets_->Model(p.model);
    if (!info) return false;
    const assets::Bounds& b = info->bounds;
    out.min = {1e30f, 1e30f, 1e30f};
    out.max = {-1e30f, -1e30f, -1e30f};
    for (int corner = 0; corner < 8; ++corner) {
        const float v[3] = {corner & 1 ? b.max[0] : b.min[0], corner & 2 ? b.max[1] : b.min[1], corner & 4 ? b.max[2] : b.min[2]};
        for (int r = 0; r < 3; ++r) {
            const float w = p.world[4 * r] * v[0] + p.world[4 * r + 1] * v[1] + p.world[4 * r + 2] * v[2] + p.world[4 * r + 3];
            out.min[r] = std::min(out.min[r], w);
            out.max[r] = std::max(out.max[r], w);
        }
    }
    return true;
}

bool WorldScreen::GroundHeight(const game::proto::WorldPosition& p, float& y) const {
    if (!navWorld_.Holds(p.region)) return false;
    const game::NavPosition at = navWorld_.Drop(p.region, p.x, p.y + 5.0f, p.z);
    if (!at.Valid()) return false;
    y = at.y;
    return true;
}

bool WorldScreen::Pick(float screenX, float screenY, float width, float height, game::proto::WorldPosition& out, bool toward, bool* sky) const {
    if (sky != nullptr) *sky = false;
    if (width <= 0.0f || height <= 0.0f) return false;
    // The fly camera's view and projection, as the scene renderer builds them (left-handed, depth
    // 0..1, y down), undone for the screen point.
    const glm::vec3 eye(camera_.targetX, camera_.targetY, camera_.targetZ);
    const glm::vec3 dir(std::cos(camera_.pitch) * std::sin(camera_.yaw), std::sin(camera_.pitch), std::cos(camera_.pitch) * std::cos(camera_.yaw));
    const glm::mat4 view = glm::lookAt(eye, eye + dir, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(camera_.fovY, width / height, camera_.nearZ > 0 ? camera_.nearZ : 1.0f, camera_.farZ > 0 ? camera_.farZ : 6000.0f);
    proj[1][1] *= -1.0f;
    const glm::mat4 inverse = glm::inverse(proj * view);
    const float nx = screenX / width * 2.0f - 1.0f, ny = screenY / height * 2.0f - 1.0f;
    glm::vec4 nearPoint = inverse * glm::vec4(nx, ny, 0.0f, 1.0f), farPoint = inverse * glm::vec4(nx, ny, 1.0f, 1.0f);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    const glm::vec3 ray = glm::normalize(glm::vec3(farPoint) - glm::vec3(nearPoint));
    if (dungeon_) {
        // A dungeon's floors, in its own space (the origin-relative space here).
        const float origin[3] = {eye.x, eye.y, eye.z};
        const float direction[3] = {ray.x, ray.y, ray.z};
        game::NavHit hit;
        if (navWorld_.Ray(dungeonRegion_, origin, direction, 2500.0f, hit)) {
            out = {dungeonRegion_, hit.x, hit.y, hit.z};
            return true;
        }
        if (!toward) return false;
        // Nothing met: a point along the heading on the floor there, if any (a walk stops at walls).
        const glm::vec2 heading(ray.x, ray.z);
        if (glm::length(heading) < 1e-3f) return false;
        const glm::vec2 to = glm::vec2(eye.x, eye.z) + glm::normalize(heading) * 400.0f;
        const float y = focus_ ? focus_->y : eye.y;
        const game::NavPosition floor = navWorld_.Drop(dungeonRegion_, to.x, y + 20.0f, to.y);
        if (!floor.Valid()) return false;
        out = {dungeonRegion_, floor.x, floor.y, floor.z};
        if (sky != nullptr) *sky = true;
        return true;
    }
    // The ray starts in the region the eye is in.
    const int rx = originX_ + int(std::floor(eye.x / assets::kRegionSize)), rz = originY_ + int(std::floor(eye.z / assets::kRegionSize));
    if (rx < 0 || rx > 255 || rz < 0 || rz > 255) return false;
    const float origin[3] = {eye.x - float(rx - originX_) * assets::kRegionSize, eye.y, eye.z - float(rz - originY_) * assets::kRegionSize};
    const float direction[3] = {ray.x, ray.y, ray.z};
    game::NavHit hit;
    if (navWorld_.Ray(assets::RegionId(rx, rz), origin, direction, 2500.0f, hit)) {
        out = {hit.region, hit.x, hit.y, hit.z};
        return true;
    }
    if (!toward) return false;
    // Nothing met (the sky, the far land): a point along the ray's heading, 800 from the walker the camera follows (else
    // from the eye), at the walker's height (no terrain under an object's floor is wanted: the walk is flat); the walk
    // there stops where the way is blocked. Ours: the original's click on the sky moves nobody (the Option window's
    // Beta page says a click on the sky runs that way).
    const glm::vec2 heading(ray.x, ray.z);
    if (glm::length(heading) < 1e-3f) return false;
    int baseX = rx, baseZ = rz;
    glm::vec2 from(origin[0], origin[2]);
    float y = eye.y;
    const bool fromWalker = focus_.has_value() && (focus_->region & 0x8000) == 0;
    if (fromWalker) {
        baseX = focus_->region & 0xFF;
        baseZ = focus_->region >> 8;
        from = {focus_->x, focus_->z};
        y = focus_->y;
    }
    const glm::vec2 to = from + glm::normalize(heading) * 800.0f;
    float x = to.x, z = to.y;
    const int ox = static_cast<int>(std::floor(x / assets::kRegionSize)), oz = static_cast<int>(std::floor(z / assets::kRegionSize));
    if (baseX + ox < 0 || baseX + ox > 255 || baseZ + oz < 0 || baseZ + oz > 255) return false;
    const std::uint16_t region = assets::RegionId(baseX + ox, baseZ + oz);
    x -= float(ox) * assets::kRegionSize;
    z -= float(oz) * assets::kRegionSize;
    if (!fromWalker) navWorld_.TerrainHeight(region, x, z, y);
    out = {region, x, y, z};
    if (sky != nullptr) *sky = true;
    return true;
}

bool WorldScreen::RayHitsStage(float screenX, float screenY, float width, float height, std::size_t slot, float& distance) const {
    if (width <= 0.0f || height <= 0.0f || slot >= characters_.size()) return false;
    const Character& c = characters_[slot];
    if (!c.active || c.opacity <= 0.0f || c.type >= characterModels_.size()) return false;
    const CharacterModel& m = characterModels_[c.type];
    if (!m.info) return false;
    glm::mat4 world;
    if (c.spawned) {
        if (!StageBone(slot, {}, world)) return false;
    } else {
        // A look without a skeleton stands as a still model (a ground item of the slot's key): its box where that stands.
        const auto shown = ground_.find(kStagePropKey + std::uint32_t(slot));
        if (!stageProps_.contains(slot) || shown == ground_.end()) return false;
        const std::array<float, 12>& r = shown->second.world;
        world = glm::mat4(r[0], r[4], r[8], 0.0f, r[1], r[5], r[9], 0.0f, r[2], r[6], r[10], 0.0f, r[3], r[7], r[11], 1.0f);
    }
    // The ray of Pick, origin-relative.
    const glm::vec3 eye(camera_.targetX, camera_.targetY, camera_.targetZ);
    const glm::vec3 dir(std::cos(camera_.pitch) * std::sin(camera_.yaw), std::sin(camera_.pitch), std::cos(camera_.pitch) * std::cos(camera_.yaw));
    const glm::mat4 view = glm::lookAt(eye, eye + dir, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(camera_.fovY, width / height, camera_.nearZ > 0 ? camera_.nearZ : 1.0f, camera_.farZ > 0 ? camera_.farZ : 6000.0f);
    proj[1][1] *= -1.0f;
    const glm::mat4 inverse = glm::inverse(proj * view);
    const float nx = screenX / width * 2.0f - 1.0f, ny = screenY / height * 2.0f - 1.0f;
    glm::vec4 nearPoint = inverse * glm::vec4(nx, ny, 0.0f, 1.0f), farPoint = inverse * glm::vec4(nx, ny, 1.0f, 1.0f);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    const glm::vec3 o(nearPoint), d = glm::normalize(glm::vec3(farPoint) - o);
    // Drawn: within the view distance (the original's frustum's far plane is its view distance option).
    const glm::vec3 centre(world[3]);
    if (glm::length(centre - o) > viewDistance_ + 200.0f) return false;
    // In the model's own space the box is its bounds; the ray's parameter stays the world's distance.
    const glm::mat4 toModel = glm::inverse(world);
    const glm::vec3 lo(toModel * glm::vec4(o, 1.0f)), ld(toModel * glm::vec4(d, 0.0f));
    const assets::Bounds& b = m.info->bounds;
    float tNear = 0.0f, tFar = 5000.0f; // the original's ray: 5,000 from the camera
    for (int axis = 0; axis < 3; ++axis) {
        const float lowB = b.min[axis], highB = b.max[axis];
        if (std::abs(ld[axis]) < 1e-8f) {
            if (lo[axis] < lowB || lo[axis] > highB) return false;
            continue;
        }
        float t0 = (lowB - lo[axis]) / ld[axis], t1 = (highB - lo[axis]) / ld[axis];
        if (t0 > t1) std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
        if (tNear > tFar) return false;
    }
    distance = tNear;
    return true;
}

bool WorldScreen::Occluded(float cursorX, float cursorY, float width, float height, float distance) const {
    // The depth read where the cursor is now (within a few pixels: a mouse that moved since gives no answer), not the sky.
    if (!probe_.valid || width <= 0.0f || height <= 0.0f || probe_.depth >= 0.99999f) return false;
    if (std::abs(probe_.x - cursorX) > 4.0f || std::abs(probe_.y - cursorY) > 4.0f) return false;
    // That depth back to a point (the projection of Pick), and its distance along the cursor's ray from its near point.
    const glm::vec3 eye(camera_.targetX, camera_.targetY, camera_.targetZ);
    const glm::vec3 dir(std::cos(camera_.pitch) * std::sin(camera_.yaw), std::sin(camera_.pitch), std::cos(camera_.pitch) * std::cos(camera_.yaw));
    const glm::mat4 view = glm::lookAt(eye, eye + dir, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(camera_.fovY, width / height, camera_.nearZ > 0 ? camera_.nearZ : 1.0f, camera_.farZ > 0 ? camera_.farZ : 6000.0f);
    proj[1][1] *= -1.0f;
    const glm::mat4 inverse = glm::inverse(proj * view);
    const float nx = probe_.x / width * 2.0f - 1.0f, ny = probe_.y / height * 2.0f - 1.0f;
    glm::vec4 nearPoint = inverse * glm::vec4(nx, ny, 0.0f, 1.0f), drawn = inverse * glm::vec4(nx, ny, probe_.depth, 1.0f);
    nearPoint /= nearPoint.w;
    drawn /= drawn.w;
    const float drawnDistance = glm::length(glm::vec3(drawn) - glm::vec3(nearPoint));
    // The box is looser than the model drawn in it: a hit up to 25 behind what is drawn is the entity itself.
    return distance > drawnDistance + 25.0f;
}

bool WorldScreen::RayHitsCylinder(float screenX, float screenY, float width, float height, const game::proto::WorldPosition& base,
                                  float radius, float tall, float& distance) const {
    if (width <= 0.0f || height <= 0.0f) return false;
    // The ray of Pick, origin-relative.
    const glm::vec3 eye(camera_.targetX, camera_.targetY, camera_.targetZ);
    const glm::vec3 dir(std::cos(camera_.pitch) * std::sin(camera_.yaw), std::sin(camera_.pitch), std::cos(camera_.pitch) * std::cos(camera_.yaw));
    const glm::mat4 view = glm::lookAt(eye, eye + dir, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(camera_.fovY, width / height, camera_.nearZ > 0 ? camera_.nearZ : 1.0f, camera_.farZ > 0 ? camera_.farZ : 6000.0f);
    proj[1][1] *= -1.0f;
    const glm::mat4 inverse = glm::inverse(proj * view);
    const float nx = screenX / width * 2.0f - 1.0f, ny = screenY / height * 2.0f - 1.0f;
    glm::vec4 nearPoint = inverse * glm::vec4(nx, ny, 0.0f, 1.0f), farPoint = inverse * glm::vec4(nx, ny, 1.0f, 1.0f);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    const glm::vec3 o(nearPoint), d = glm::normalize(glm::vec3(farPoint) - o);
    // Against the infinite upright cylinder in the ground plane, then its height.
    const glm::vec3 c = OriginRelative(base);
    const float dx = d.x, dz = d.z, ox = o.x - c.x, oz = o.z - c.z;
    const float a = dx * dx + dz * dz, b = 2.0f * (ox * dx + oz * dz), k = ox * ox + oz * oz - radius * radius;
    if (a < 1e-6f) return false;
    const float disc = b * b - 4.0f * a * k;
    if (disc < 0.0f) return false;
    for (const float t : {(-b - std::sqrt(disc)) / (2.0f * a), (-b + std::sqrt(disc)) / (2.0f * a)}) {
        if (t < 0.0f) continue;
        const float y = o.y + d.y * t;
        if (y >= c.y && y <= c.y + tall) {
            distance = t;
            return true;
        }
    }
    return false;
}

bool WorldScreen::Project(const game::proto::WorldPosition& p, float lift, float width, float height, float& x, float& y) const {
    float depth = 0;
    return Project(p, lift, width, height, x, y, depth);
}

bool WorldScreen::Project(const game::proto::WorldPosition& p, float lift, float width, float height, float& x, float& y, float& depth) const {
    if (width <= 0.0f || height <= 0.0f) return false;
    // The view and projection of Pick, forward.
    const glm::vec3 eye(camera_.targetX, camera_.targetY, camera_.targetZ);
    const glm::vec3 dir(std::cos(camera_.pitch) * std::sin(camera_.yaw), std::sin(camera_.pitch), std::cos(camera_.pitch) * std::cos(camera_.yaw));
    const glm::mat4 view = glm::lookAt(eye, eye + dir, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(camera_.fovY, width / height, camera_.nearZ > 0 ? camera_.nearZ : 1.0f, camera_.farZ > 0 ? camera_.farZ : 6000.0f);
    proj[1][1] *= -1.0f;
    const glm::vec4 clip = proj * view * glm::vec4(OriginRelative(p) + glm::vec3(0.0f, lift, 0.0f), 1.0f);
    if (clip.w <= 0.1f) return false;
    x = (clip.x / clip.w * 0.5f + 0.5f) * width;
    y = (clip.y / clip.w * 0.5f + 0.5f) * height;
    depth = clip.z / clip.w;
    return clip.z >= 0.0f && clip.z <= clip.w;
}

namespace {

glm::mat4 FromRows(const std::array<float, 12>& r) {
    return glm::mat4(r[0], r[4], r[8], 0.0f, r[1], r[5], r[9], 0.0f, r[2], r[6], r[10], 0.0f, r[3], r[7], r[11], 1.0f);
}

} // namespace

std::vector<double> WorldScreen::StageEvents(std::size_t slot, std::string_view set, std::uint32_t state, int code) const {
    std::vector<double> out;
    if (slot >= characters_.size()) return out;
    const Character& c = characters_[slot];
    if (!c.active || c.type >= characterModels_.size()) return out;
    const CharacterModel& m = characterModels_[c.type];
    // The set its clip comes from, as ClipPath picks it.
    std::string_view from = "default";
    if (!set.empty() && !m.sets.StateAnimation(set, state).empty()) {
        from = set;
    } else if (!m.sets.StateAnimation(m.stance, state).empty()) {
        from = m.stance;
    }
    for (const int ms : m.sets.StateEvents(from, state, code)) out.push_back(double(ms) / 1000.0);
    return out;
}

std::vector<assets::ResourceData::SoundEvent> WorldScreen::StageSounds(std::size_t slot, std::string_view set, std::uint32_t state) const {
    if (slot >= characters_.size()) return {};
    const Character& c = characters_[slot];
    if (!c.active || c.type >= characterModels_.size()) return {};
    const CharacterModel& m = characterModels_[c.type];
    // The set its clip comes from, as ClipPath picks it.
    std::string_view from = "default";
    if (!set.empty() && !m.sets.StateAnimation(set, state).empty()) {
        from = set;
    } else if (!m.sets.StateAnimation(m.stance, state).empty()) {
        from = m.stance;
    }
    return m.sets.StateSounds(from, state);
}

bool WorldScreen::StageBone(std::size_t slot, std::string_view bone, glm::mat4& out) const {
    if (slot >= characters_.size()) return false;
    const Character& c = characters_[slot];
    if (!c.active || !c.spawned) return false;
    // Its instance's transform, as UpdateCharacters moves it.
    const float s = ScaleOf(c);
    const float rc = -c.dirZ * s, rs = c.dirX * s;
    out = FromRows(c.transform ? *c.transform : std::array<float, 12>{rc, 0, -rs, c.x, 0, s, 0, c.y, rs, 0, rc, c.z});
    if (bone.empty()) return true;
    const CharacterModel& m = characterModels_[c.type];
    if (!m.info || !m.info->skeleton) return false;
    if (const std::size_t at = bone.find('@'); at != std::string_view::npos) {
        // "name@item.bsr": a worn item's own bone (a socket), placed on its parent as it is posed.
        const std::string item = core::NormalizeAssetPath(bone.substr(at + 1));
        const std::string_view name = bone.substr(0, at);
        const std::vector<glm::mat4>& world = c.pose.World();
        for (const gfx::ModelInfo::Socket& s : m.info->sockets) {
            if (s.name != name || s.item != item || s.parent < 0 || static_cast<std::size_t>(s.parent) >= world.size()) continue;
            out = out * world[static_cast<std::size_t>(s.parent)] * glm::make_mat4(s.local.data());
            return true;
        }
        // No socket: the item's bone is the wearer's own (the same bone, AssembleOutfit), posed with it.
        if (const int own = m.info->skeleton->Find(std::string(name)); own >= 0 && static_cast<std::size_t>(own) < world.size()) {
            out = out * world[static_cast<std::size_t>(own)];
            return true;
        }
        return false;
    }
    auto it = m.bones.find(std::string(bone));
    if (it == m.bones.end()) it = m.bones.emplace(std::string(bone), m.info->skeleton->Find(std::string(bone))).first;
    const std::vector<glm::mat4>& world = c.pose.World();
    if (it->second < 0) {
        // Not on the body: a bone of what it holds (a stage on ai_end, the crossbow's tip: 132 of
        // the player skills' effect stages), the weapon's first, else any worn item's.
        for (const bool weapon : {true, false}) {
            for (const gfx::ModelInfo::Socket& s : m.info->sockets) {
                if (s.name != bone || s.parent < 0 || static_cast<std::size_t>(s.parent) >= world.size()) continue;
                if (weapon && s.item.find("\\weapon\\") == std::string::npos) continue;
                out = out * world[static_cast<std::size_t>(s.parent)] * glm::make_mat4(s.local.data());
                return true;
            }
        }
        return false;
    }
    if (static_cast<std::size_t>(it->second) >= world.size()) return false;
    out = out * world[static_cast<std::size_t>(it->second)];
    return true;
}

bool WorldScreen::PlaceEffect(const gfx::EffectSystem::Anchor& anchor, glm::mat4& out) const {
    if ((anchor.target & kFlyingAnchor) != 0) {
        const auto it = flying_.find(anchor.target & ~kFlyingAnchor);
        if (it == flying_.end()) return false;
        out = FromRows(it->second.world);
        // A bone of the model (an arrow's Bone01, where SCT_ARROW hangs the skill's trail and force),
        // in its bind pose; the model's root while it loads or has none.
        if (!anchor.bone.empty() && assets_ != nullptr) {
            if (const auto model = assets_->Model(it->second.model); model && model->skeleton) {
                const assets::SkeletonData& skeleton = *model->skeleton;
                glm::mat4 bind(1.0f);
                for (int b = skeleton.Find(anchor.bone); b >= 0; b = skeleton.bones[static_cast<std::size_t>(b)].parent) {
                    const assets::BoneTransform& t = skeleton.bones[static_cast<std::size_t>(b)].local;
                    const glm::quat q(t.rotation[3], t.rotation[0], t.rotation[1], t.rotation[2]);
                    bind = glm::translate(glm::mat4(1.0f), glm::vec3(t.translation[0], t.translation[1], t.translation[2])) * glm::mat4_cast(q) * bind;
                }
                out = out * bind;
            }
        }
        out = out * glm::translate(glm::mat4(1.0f), anchor.offset);
        return true;
    }
    // A stage slot's character, as the 1.188 client places a stage: a bone's place and
    // turn, its place with no turn (the character's scale kept), or a point in its frame; else the
    // root, raised to the middle of the body for "*" or a bone it lacks (a model still loading). The
    // offset is in the root's turned frame, but in the bone's for a point.
    const std::size_t slot = anchor.target - 1;
    glm::mat4 root(1.0f);
    if (!StageBone(slot, {}, root)) return false;
    // Turned by another (the caster): its root's turn on this place, the offset in its frame.
    glm::mat4 turn = root;
    if (anchor.turnOf != 0 && !StageBone(anchor.turnOf - 1, {}, turn)) turn = root;
    glm::mat4 bone(1.0f);
    if (!anchor.bone.empty() && StageBone(slot, anchor.bone, bone)) {
        out = anchor.keepTurn ? bone : glm::scale(glm::mat4(1.0f), glm::vec3(glm::length(glm::vec3(root[0]))));
        if (anchor.inBone) {
            out[3] = bone * glm::vec4(anchor.offset, 1.0f);
            return true;
        }
        out[3] = bone[3];
    } else {
        out = root;
        if (anchor.atHeight || anchor.atMiddle || !anchor.bone.empty()) {
            // "*" raises the root by the body's height: its bounding box's top, scaled (keeps it,
            // reads it for): a quest's mark, a stun's stars over the head. Our hits without a
            // bone, the flights and a bone the model lacks take its middle.
            const auto& info = characterModels_[characters_[slot].type].info;
            const float share = anchor.atHeight ? 1.0f : 0.5f;
            if (info) out = out * glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, info->bounds.max[1] * share, 0.0f));
        }
    }
    if (anchor.turnOf != 0) {
        const glm::vec4 place = out[3];
        out = turn;
        out[3] = place;
    }
    out[3] += turn * glm::vec4(anchor.offset, 0.0f);
    return true;
}

float WorldScreen::StageHeight(std::size_t slot) const {
    if (slot >= characters_.size()) return 0.0f;
    const auto& info = characterModels_[characters_[slot].type].info;
    glm::mat4 root(1.0f);
    if (!info || !StageBone(slot, {}, root)) return 0.0f;
    return info->bounds.max[1] * glm::length(glm::vec3(root[1]));
}

void WorldScreen::UpdateEffects(ScreenContext& context, FrameSnapshot& snapshot) {
    // The camera as the scene renderer builds it (Pick's view).
    const float width = static_cast<float>(std::max(1u, context.device.SwapchainWidth()));
    const float height = static_cast<float>(std::max(1u, context.device.SwapchainHeight()));
    const glm::vec3 eye(camera_.targetX, camera_.targetY, camera_.targetZ);
    const glm::vec3 dir(std::cos(camera_.pitch) * std::sin(camera_.yaw), std::sin(camera_.pitch), std::cos(camera_.pitch) * std::cos(camera_.yaw));
    glm::mat4 proj = glm::perspective(camera_.fovY, width / height, camera_.nearZ > 0 ? camera_.nearZ : 1.0f, camera_.farZ > 0 ? camera_.farZ : 6000.0f);
    proj[1][1] *= -1.0f;
    gfx::EffectSystem::View view;
    view.viewProj = proj * glm::lookAt(eye, eye + dir, glm::vec3(0, 1, 0));
    view.eye = eye;
    view.forward = dir;
    if (!testEffect_.empty()) {
        // --effect: on the walkers, or 150 in front of the camera; again whenever one ends.
        if (testEffects_.empty()) testEffects_.assign(1, 0);
        for (std::size_t i = 0; i < testEffects_.size(); ++i) {
            if (testEffects_[i] != 0 && effects_.Alive(testEffects_[i])) continue;
            gfx::EffectSystem::Request request;
            request.path = testEffect_;
            request.kind = gfx::EffectSystem::Kind::World;
            if (characters_.empty()) {
                request.at = glm::translate(glm::mat4(1.0f), eye + dir * 150.0f);
            } else if (characters_[i].spawned) {
                request.anchor.target = StageAnchor(i);
                request.anchor.bone = testEffectBone_;
                request.scale = testEffectScale_;
                if (!testEffectBone_.empty()) request.seconds = -1.0f; // kept, as an item's glow
            } else {
                continue;
            }
            testEffects_[i] = effects_.Start(request);
        }
    }
    effects_.Update(context.dt, view, [this](const gfx::EffectSystem::Anchor& a, glm::mat4& m) { return PlaceEffect(a, m); },
                    snapshot.scene.effects);
    for (const std::string& failure : effects_.TakeFailures()) core::LogWarning("effects: {}", failure);
}

// The prototype World Viewer's camera, placed once the origin region's heights are known.
void WorldScreen::ResetCamera() {
    if (placedCamera_) {
        camera_ = *placedCamera_;
        cameraPlaced_ = true;
        return;
    }
    camera_.yaw = 0.0f;
    camera_.pitch = -0.5f;
    camera_.targetX = assets::kRegionSize * 0.5f;
    camera_.targetY = 350.0f;
    camera_.targetZ = -250.0f;
    cameraPlaced_ = false;
}

void WorldScreen::StartLoading(ScreenContext& context, game::LoadingKind kind) {
    loadingRandom_ = Hash(loadingRandom_ + 1u);
    const std::string picture = game::LoadingPicture(kind, assets::RegionId(originX_, originY_), loadingRandom_);
    loading_.Begin(context.assets, picture);
    loadingSince_ = context.time;
    loadingFrames_ = 0;
    core::LogInfo("world: loading screen {} for {},{}", picture, originX_, originY_);
}

// A dungeon's instances take their handles from the streamer's pool, as everything else here does.
DungeonScene::Handles WorldScreen::DungeonHandles() {
    return {[this] { return streamer_.NewHandle(); }, [this](std::uint32_t h) { streamer_.FreeHandle(h); }};
}

// Its shown blocks' models and its navigation in.
bool WorldScreen::DungeonSettled(const gfx::AssetStore& assets) const {
    return dungeon_ && dungeon_->Settled(assets) && !navLoader_->Loading(dungeonRegion_);
}

// The original's loading screen while a place loads: the streaming goes on, nothing
// else runs and only the screen is drawn; it closes the frame everything wanted is there.
bool WorldScreen::UpdateLoading(ScreenContext& context, FrameSnapshot& snapshot) {
    loading_.SetProgress(StreamProgress(context.assets));
    ++loadingFrames_;
    if ((dungeon_ ? DungeonSettled(context.assets) : streamer_.Settled(context.assets)) && cameraPlaced_) {
        core::LogInfo("world: loading screen closed after {:.2f} s ({} frames), {},{} settled", context.time - loadingSince_,
                      loadingFrames_, originX_, originY_);
        loading_.End(context.assets);
        return false;
    }
    loadingUi_.Clear();
    loading_.Draw(context.assets, loadingUi_, static_cast<float>(context.device.SwapchainWidth()),
                  static_cast<float>(context.device.SwapchainHeight()));
    AppendUi(snapshot.ui, loadingUi_);
    snapshot.scene.active = false;
    // --capture-loading captures the screen itself, once its pictures are in.
    if (!captureLoading_ || !loading_.Ready(context.assets) || loading_.Progress() <= 0.0f) context.holdCapture = true;
    return true;
}

// A teleport: everything of the old place released, the streamer restarted at the destination,
// the loading screen up until it is settled.
void WorldScreen::Teleport(ScreenContext& context, FrameSnapshot& snapshot) {
    core::LogInfo("world: teleport from {},{} to {},{}", originX_, originY_, teleportX_, teleportY_);
    for (Character& c : characters_) {
        if (!c.spawned) continue;
        WorldCommand remove;
        remove.kind = WorldCommand::Kind::Remove;
        remove.handle = c.handle;
        snapshot.worldCommands.push_back(remove);
        streamer_.FreeHandle(c.handle);
        c.spawned = false;
        c.nav = {};
        c.repath = true;
    }
    spawned_ = 0;
    streamer_.Stop(context.assets, snapshot.worldCommands);
    navWorld_.Clear();
    weatherEffect_.Clear();
    originX_ = teleportX_;
    originY_ = teleportY_;
    streamer_.Start(originX_, originY_, indexes_->objects, streaming_);
    ResetCamera();
    lastCamera_ = glm::vec3(camera_.targetX, camera_.targetY, camera_.targetZ);
    reported_ = false;
    StartLoading(context, game::LoadingKind::Teleport);
}

// The flight: straight to the target region, 250 units over the ground where it is loaded.
void WorldScreen::Flight(ScreenContext& context) {
    const float dt = static_cast<float>(context.dt);
    const glm::vec2 at(camera_.targetX, camera_.targetZ);
    const glm::vec2 to = flightTarget_ - at;
    const float distance = glm::length(to);
    if (distance < 1.0f) {
        if (!arrived_) core::LogInfo("world: flight arrived at {:.1f} s", context.time);
        arrived_ = true;
        return;
    }
    const glm::vec2 dir = to / distance;
    const glm::vec2 next = at + dir * std::min(distance, flightSpeed_ * dt);
    camera_.targetX = next.x;
    camera_.targetZ = next.y;
    camera_.yaw = std::atan2(dir.x, dir.y);
    camera_.pitch = -0.3f;
    float lx = 0, lz = 0;
    if (const auto ground = streamer_.TerrainAt(context.assets, next.x, next.y, lx, lz)) {
        const float target = gfx::TerrainHeight(*ground, lx, lz) + 250.0f;
        camera_.targetY += (target - camera_.targetY) * std::min(1.0f, 2.0f * dt);
    }
    context.camera.Store(camera_.yaw, camera_.pitch);
}

void WorldScreen::Fly(ScreenContext& context) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && !MenuHasMouse() && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        camera_.yaw += io.MouseDelta.x * 0.005f;
        camera_.pitch = std::clamp(camera_.pitch - io.MouseDelta.y * 0.005f, -1.5f, 1.5f);
    }
    if (!io.WantCaptureKeyboard) {
        const float cp = std::cos(camera_.pitch);
        const float fx = cp * std::sin(camera_.yaw), fy = std::sin(camera_.pitch), fz = cp * std::cos(camera_.yaw);
        const float rx = std::cos(camera_.yaw), rz = -std::sin(camera_.yaw);
        float forward = 0, right = 0, up = 0;
        if (ImGui::IsKeyDown(ImGuiKey_W)) forward += 1;
        if (ImGui::IsKeyDown(ImGuiKey_S)) forward -= 1;
        if (ImGui::IsKeyDown(ImGuiKey_D)) right += 1;
        if (ImGui::IsKeyDown(ImGuiKey_A)) right -= 1;
        if (ImGui::IsKeyDown(ImGuiKey_E)) up += 1;
        if (ImGui::IsKeyDown(ImGuiKey_Q)) up -= 1;
        const float step = static_cast<float>(context.dt) * (io.KeyShift ? 1500.0f : 300.0f);
        camera_.targetX += (fx * forward + rx * right) * step;
        camera_.targetY += (fy * forward + up) * step;
        camera_.targetZ += (fz * forward + rz * right) * step;
    }
    context.camera.Store(camera_.yaw, camera_.pitch);
}

void WorldScreen::UpdateCharacters(ScreenContext& context, FrameSnapshot& snapshot) {
    SR_ZONE("world: characters");
    for (const std::uint32_t handle : pendingRemoves_) {
        WorldCommand remove;
        remove.kind = WorldCommand::Kind::Remove;
        remove.handle = handle;
        snapshot.worldCommands.push_back(remove);
    }
    pendingRemoves_.clear();
    const auto addProp = [&](Prop& p) {
        if (p.added || p.hidden || !context.assets.Model(p.model)) return;
        p.handle = streamer_.NewHandle();
        p.added = true;
        WorldCommand add;
        add.handle = p.handle;
        add.model = p.model;
        add.world = p.world;
        snapshot.worldCommands.push_back(add);
    };
    for (Prop& p : props_) addProp(p);
    for (auto& [key, p] : ground_) {
        addProp(p);
        // Its own effects: gold's item_drop_money.efp, a rare's item_drop_equip_rare.efp, ... (all in the
        // drop models' resources), kept while it lies there.
        if (!p.added || p.ambientStarted) continue;
        const std::shared_ptr<const gfx::ModelInfo> info = assets_->Model(p.model);
        if (!info) continue;
        p.ambientStarted = true;
        for (const assets::ResourceData::ParticleModifier& particle : info->ambientParticles) {
            gfx::EffectSystem::Request request;
            request.path = particle.effect;
            request.at = glm::translate(glm::mat4(1.0f), glm::vec3(p.world[3] + particle.offset[0], p.world[7] + particle.offset[1], p.world[11] + particle.offset[2]));
            request.seconds = -1.0f; // until it is picked up
            if (const std::uint32_t h = effects_.Start(request); h != 0) p.ambient.push_back(h);
        }
    }
    // Models in flight: added once in, moved every frame.
    for (auto& [key, p] : flying_) {
        if (!p.added) {
            addProp(p);
            continue;
        }
        WorldCommand move;
        move.kind = WorldCommand::Kind::Move;
        move.handle = p.handle;
        move.world = p.world;
        snapshot.worldCommands.push_back(move);
    }
    if (characters_.empty()) return;
    const std::uint16_t origin = assets::RegionId(originX_, originY_);
    const bool navReady = navWorld_.Holds(origin); // the walkers wait for it

    // Models, then their clips.
    for (CharacterModel& m : characterModels_) {
        if (m.ready || m.released) continue;
        if (!m.info) {
            m.info = context.assets.Model(m.model);
            if (!m.info) continue;
            // Standing on the stage: the resource's own standing state (the "default" set's state 0,
            // STAND1, empty-handed) - a name search can find another race's cart clip first.
            if (m.idle) {
                m.sets.animationPaths = m.info->animationPaths;
                m.sets.animationSets = m.info->animationSets;
                m.sets.modifierSets = m.info->soundSets;
                const assets::ResourceData& stances = m.sets;
                // The state in its stance's set, else in the default set; else standing.
                m.clipName = stances.StateAnimation(m.stance, m.state);
                if (m.clipName.empty()) m.clipName = stances.StateAnimation("default", m.state);
                if (m.clipName.empty()) m.clipName = stances.StateAnimation(m.stance, 0);
                if (m.clipName.empty()) m.clipName = stances.StateAnimation("default", 0);
            }
            if (m.clipName.empty()) {
                const int clip = PickClip(m.info->animationPaths, m.idle);
                if (clip >= 0) m.clipName = m.info->animationPaths[static_cast<std::size_t>(clip)];
            }
            if (!m.clipName.empty()) m.animation = context.assets.RequestAnimation(m.clipName, 0);
            // The original's shadow: a per-model size plus 5; ours from the model's
            // footprint, the larger of its horizontal half extents.
            const assets::Bounds& b = m.info->bounds;
            const float footprint = 0.5f * std::max(b.max[0] - b.min[0], b.max[2] - b.min[2]);
            m.shadowSize = std::min(footprint + 5.0f, gfx::TerrainPass::kMaxShadowSize);
        }
        if (m.animation) m.clip = context.assets.Animation(m.animation);
        m.ready = !m.animation || m.clip != nullptr || context.assets.State(m.animation) == gfx::AssetState::Failed;
        if (m.ready) {
            core::LogInfo("world: {} walks with {} ({} bones), shadow {:.0f}, {} ambient effects", m.path, m.clip ? m.clipName : std::string("no clip"),
                          m.info->skeleton ? m.info->skeleton->bones.size() : 0, m.shadowSize, m.info->ambientParticles.size());
        }
    }

    // A stage look whose model has no skeleton (a fortress's heart, a structure's rubble) cannot stand and be posed: it
    // shows as a still model at the slot's place, the slot's body (if any) taken away.
    for (std::size_t i = 0; i < characters_.size(); ++i) {
        Character& c = characters_[i];
        if (!c.stage) continue;
        const std::uint32_t key = kStagePropKey + std::uint32_t(i);
        const auto shown = stageProps_.find(i);
        const std::uint32_t type = !c.active ? kNoType : c.nextType != kNoType ? c.nextType : c.type;
        const CharacterModel* m = type != kNoType ? &characterModels_[type] : nullptr;
        if (m != nullptr && m->ready && (!m->info || !m->info->skeleton) && m->info) {
            if (c.spawned) {
                pendingRemoves_.push_back(c.handle);
                c.spawned = false;
                --spawned_;
            }
            c.type = type;
            c.nextType = kNoType;
            if (shown == stageProps_.end() || shown->second != m->path) {
                ClearGroundItem(key);
                SetGroundItem(key, m->path, c.x, c.y, c.z, std::atan2(c.dirX, c.dirZ));
                stageProps_[i] = m->path;
                core::LogInfo("world: {} has no skeleton: shown as a still model at {:.0f}, {:.0f}, {:.0f}", m->path, c.x, c.y, c.z);
            }
        } else if (shown != stageProps_.end() && (m == nullptr || m->ready)) {
            ClearGroundItem(key);
            stageProps_.erase(shown);
        }
    }

    // Spawn those whose model is ready, dropped onto the ground of the origin region in front of
    // the start camera (a few tries a frame for a point that is not blocked).
    bool swapped = false;
    for (Character& c : characters_) {
        if (!c.active) continue;
        if (c.stage && c.spawned && c.nextType != kNoType) {
            // A new look: shown once it is in (or given up when it failed).
            const CharacterModel& next = characterModels_[c.nextType];
            const bool failed = context.assets.State(next.model) == gfx::AssetState::Failed;
            if (!(next.ready && next.info && next.info->skeleton) && !failed) continue;
            if (failed) {
                c.nextType = kNoType;
                continue;
            }
            pendingRemoves_.push_back(c.handle);
            c.spawned = false;
            --spawned_;
            c.type = std::exchange(c.nextType, kNoType);
            swapped = true;
        }
        if (c.spawned) continue;
        const CharacterModel& m = characterModels_[c.type];
        if (!m.ready || !m.info->skeleton) continue;
        if (c.stage) {
            c.pose.Init(*m.info->skeleton);
            // Its model's clip the cycle from now (a one-shot look, gold's coin shower, from its first frame).
            c.mixing = true;
            c.frozenMs = 0;
            c.onceId = 0;
            c.pose.SetCycle(m.clip.get(), context.time * 1000.0, 0.0f);
            c.playing = m.state;
            c.playingTake = c.look.take;
            c.playingSet.clear(); // the model's own clip is its stance's
            c.playingCycle = StageLook::kNoState;
            ApplyVolume(c, *m.info->skeleton);
            c.handle = streamer_.NewHandle();
            c.spawned = true;
            ++spawned_;
            WorldCommand add;
            add.handle = c.handle;
            add.model = m.model;
            add.paletteOffset = c.paletteOffset;
            add.overlay = c.look.overlay;
            add.fade = std::uint8_t(std::lround((1.0f - std::clamp(c.opacity, 0.0f, 1.0f)) * 255.0f));
            snapshot.worldCommands.push_back(add);
            continue;
        }
        if (!navReady) continue;
        for (int attempt = 0; attempt < 8 && !c.nav.Valid(); ++attempt) {
            const float lx = c.pinned ? c.pinX : 300.0f + Random(c.random) * 1320.0f;
            const float lz = c.pinned ? c.pinZ : 150.0f + Random(c.random) * 1300.0f;
            float ground = 0;
            // Dropped from above: onto a plaza, a deck or a bridge where one stands over the ground.
            if (navWorld_.TerrainHeight(origin, lx, lz, ground)) c.nav = navWorld_.Drop(origin, lx, ground + 200.0f, lz);
        }
        if (!c.nav.Valid()) continue;
        const glm::vec2 corner = streamer_.RegionCorner(c.nav.region);
        c.x = corner.x + c.nav.x;
        c.y = c.nav.y;
        c.z = corner.y + c.nav.z;
        if (c.pinned) core::LogInfo("world: a pinned walker stands at {:.0f},{:.0f},{:.0f} (--camera)", c.x, c.y, c.z);
        c.pose.Init(*m.info->skeleton);
        if (m.clip) c.pose.SetAnimation(m.clip.get());
        if (c.dressed) {
            // As a stage character: the mixer, its model's clip the cycle; and the body's volume.
            c.mixing = true;
            c.frozenMs = 0;
            c.onceId = 0;
            c.pose.SetCycle(m.clip.get(), context.time * 1000.0, 0.0f);
            c.playing = m.state;
            c.playingTake = c.look.take;
            c.playingSet.clear();
            c.playingCycle = StageLook::kNoState;
            ApplyVolume(c, *m.info->skeleton);
        }
        c.handle = streamer_.NewHandle();
        c.spawned = true;
        ++spawned_;
        WorldCommand add;
        add.handle = c.handle;
        add.model = m.model;
        add.paletteOffset = c.paletteOffset;
        snapshot.worldCommands.push_back(add);
    }

    if (swapped) ReleaseUnused(context);

    // The models' ambient effects (Yeoha's green glows), on their bones while the instance is shown.
    for (std::size_t i = 0; i < characters_.size(); ++i) {
        Character& c = characters_[i];
        const std::uint32_t want = c.active && c.spawned ? c.handle + 1 : 0;
        if (c.ambientFor == want) continue;
        for (const std::uint32_t h : c.ambient) effects_.Stop(h);
        c.ambient.clear();
        c.ambientFor = want;
        if (want == 0 || c.type >= characterModels_.size() || !characterModels_[c.type].info) continue;
        for (const assets::ResourceData::ParticleModifier& p : characterModels_[c.type].info->ambientParticles) {
            gfx::EffectSystem::Request request;
            request.path = p.effect;
            request.anchor.target = StageAnchor(i);
            if (!p.atRoot) request.anchor.bone = p.bone;
            request.anchor.offset = glm::vec3(p.offset[0], p.offset[1], p.offset[2]);
            request.seconds = -1.0f; // while it is shown
            request.pull = 3.0f;     // the eyes' glows sit inside the head
            if (const std::uint32_t h = effects_.Start(request); h != 0) c.ambient.push_back(h);
        }
    }

    // Walk, on the workers: the navigation does not change while they read it (it changes only in
    // the streamer's update, before this). Each walks a step towards its target; a new target when
    // it is there or something stopped it.
    const float dt = static_cast<float>(context.dt);
    context.jobs.ParallelFor(characters_.size(), 64, [&](std::size_t begin, std::size_t end) {
        SR_ZONE("world: walk");
        for (std::size_t i = begin; i < end; ++i) {
            Character& c = characters_[i];
            if (!c.spawned || c.stage || c.still || c.pinned) continue;
            float dx = c.targetX - c.x, dz = c.targetZ - c.z;
            float distance = std::hypot(dx, dz);
            if (c.repath || distance < 2.0f) {
                const float angle = Random(c.random) * 6.2831853f, reach = 60.0f + Random(c.random) * 240.0f;
                c.targetX = c.x + std::cos(angle) * reach;
                c.targetZ = c.z + std::sin(angle) * reach;
                c.repath = false;
                dx = c.targetX - c.x;
                dz = c.targetZ - c.z;
                distance = std::hypot(dx, dz);
            }
            const float step = std::min(distance, kWalkSpeed * dt);
            float lx = 0, lz = 0;
            const std::uint16_t region = streamer_.RegionAt(c.x + dx / distance * step, c.z + dz / distance * step, lx, lz);
            game::NavPosition to;
            const std::uint32_t result = navWorld_.Walk(c.nav, region, lx, lz, to);
            if ((result & (game::nav_walk::kBlocked | game::nav_walk::kFailed | game::nav_walk::kLimit)) != 0) c.repath = true;
            if (!to.Valid()) continue;
            const glm::vec2 corner = streamer_.RegionCorner(to.region);
            const float nx = corner.x + to.x, nz = corner.y + to.z;
            const float moved = std::hypot(nx - c.x, nz - c.z);
            if (moved > 1e-3f) {
                c.dirX = (nx - c.x) / moved;
                c.dirZ = (nz - c.z) / moved;
            }
            c.nav = to;
            c.x = nx;
            c.y = to.y;
            c.z = nz;
        }
    });

    // The instances move, the shadows follow.
    for (const Character& c : characters_) {
        if (!c.spawned) continue;
        // Models face -z: turn -z to the direction of travel (a stage character at its height).
        const float s = ScaleOf(c);
        const float rc = -c.dirZ * s, rs = c.dirX * s;
        WorldCommand move;
        move.kind = WorldCommand::Kind::Move;
        move.handle = c.handle;
        move.world = c.transform ? *c.transform : std::array<float, 12>{rc, 0, -rs, c.x, 0, s, 0, c.y, rs, 0, rc, c.z};
        move.fade = std::uint8_t(std::lround((1.0f - std::clamp(c.opacity, 0.0f, 1.0f)) * 255.0f));
        if (c.tinted && snapshot.scene.tints.size() < 127) {
            snapshot.scene.tints.push_back(glm::vec4(c.tint, 1.0f));
            move.tint = std::uint8_t(snapshot.scene.tints.size());
        }
        if (c.look.hideWeapon && c.type < characterModels_.size()) {
            // Its weapon and shield: the outfit's last parts (items in slot order, docs: assets::Dress).
            CharacterModel& m = characterModels_[c.type];
            if (m.weaponTail == 0xFF && m.info) {
                std::uint8_t tail = 0;
                for (auto p = m.info->parts.rbegin(); p != m.info->parts.rend() && tail < 0xFE; ++p) {
                    const std::string path = core::NormalizeAssetPath(p->meshPath);
                    if (path.find("\\weapon\\") == std::string::npos && path.find("\\shield\\") == std::string::npos) break;
                    ++tail;
                }
                m.weaponTail = tail;
            }
            if (m.weaponTail != 0xFF) move.hiddenTail = m.weaponTail;
        }
        snapshot.worldCommands.push_back(move);
        const float size = characterModels_[c.type].shadowSize * s * std::clamp(c.opacity, 0.0f, 1.0f);
        if (size > 0 && (!c.stage || c.look.shadow)) snapshot.scene.shadows.push_back({c.x, c.y, c.z, size});
    }

    // A stage character's clips through the mixer (anim::SkeletonPose's two layers):
    // the look's cycle (or its state's clip when that loops: a cast's wait loop under its READY, P18) as the cyclic
    // layer, cross-faded over 200 ms; any other state's clip a one-shot over it with its own clock (a skill's holding its
    // first frame through its 200 ms blend-in; StageEffects::kClipHold times its stages the same way),
    // the one before blending out over 200 ms; one that comes back while it still plays goes on where it is (P6). One the
    // model has no clip for keeps what it plays.
    const double time = context.time;
    // --mix-log: each change of a stage character's layers (the slot, the look, what the mixer was told).
    static const bool mixLog = context.config.GetBool("mix-log", false);
    constexpr std::uint64_t kFlinchTag = ~std::uint64_t{0};
    const auto tagOf = [](std::uint32_t state, std::uint32_t take, const std::string& set) {
        return (std::uint64_t(state) << 32) ^ std::uint64_t(take) ^ (std::uint64_t(std::hash<std::string>{}(set)) << 8);
    };
    for (Character& c : characters_) {
        if (!c.spawned || !(c.stage || c.dressed) || !c.mixing || c.type >= characterModels_.size()) continue;
        CharacterModel& m = characterModels_[c.type];
        const double at = time * 1000.0 - c.frozenMs;
        // A hit's flinch: its stance's DAMAGE1 (state 3) as a one-shot from its first frame, no blend in, out over 200 ms (P2).
        if (c.look.flinch != c.flinchPlaying) {
            bool missing = false;
            const assets::AnimationData* clip = c.look.flinch != 0 ? ClipFor(m, std::string_view(), 3, missing) : nullptr;
            if (c.look.flinch == 0 || clip != nullptr || missing) {
                c.flinchPlaying = c.look.flinch;
                if (clip != nullptr) c.pose.PlayOnce(clip, at, 0.0f, 200.0f, false, kFlinchTag);
            }
        }
        if (c.look.state == c.playing && c.look.take == c.playingTake && c.look.clipSet == c.playingSet && c.look.cycle == c.playingCycle &&
            c.look.cycleSet == c.playingCycleSet) continue;
        bool missing = false;
        const assets::AnimationData* top = ClipFor(m, c.look.clipSet, c.look.state, missing);
        if (top == nullptr && !missing) continue; // loading
        const assets::AnimationData* cycle = nullptr;
        if (top != nullptr && top->loop) {
            cycle = top;
        } else if (c.look.cycle != StageLook::kNoState) {
            bool cycleMissing = false;
            cycle = ClipFor(m, c.look.cycleSet, c.look.cycle, cycleMissing);
            if (cycle == nullptr && !cycleMissing) continue; // loading
        }
        if (cycle != nullptr) c.pose.SetCycle(cycle, at, 200.0f);
        if (mixLog) {
            core::LogInfo("mix: {:.3f} slot {} state {} take {} set '{}' cycle {} '{}' top {} ({}) cycle clip {}", time, std::size_t(&c - characters_.data()),
                          c.look.state, c.look.take, c.look.clipSet, c.look.cycle, c.look.cycleSet, top != nullptr ? top->name : std::string("-"),
                          top != nullptr && top->loop ? "loop" : "once", cycle != nullptr ? cycle->name : std::string("-"));
        }
        if (top != nullptr && !top->loop) {
            // The one-shot: the same one still playing goes on (P6), else a new one from its start.
            const std::uint64_t tag = tagOf(c.look.state, c.look.take, c.look.clipSet);
            std::uint32_t id = c.pose.FindOnce(tag, at);
            if (id != 0) {
                c.pose.ResumeOnce(id);
            } else {
                const bool skill = !c.look.clipSet.empty();
                id = c.pose.PlayOnce(top, at, 200.0f, 200.0f, skill, tag);
            }
            if (c.onceId != 0 && c.onceId != id) c.pose.StopOnce(c.onceId, at, 200.0f);
            c.onceId = id;
        } else if (c.onceId != 0) {
            // Back to a loop (its motion over, or given up): the one-shot blends out from where it is.
            c.pose.StopOnce(c.onceId, at, 200.0f);
            c.onceId = 0;
        }
        c.playing = c.look.state;
        c.playingTake = c.look.take;
        c.playingSet = c.look.clipSet;
        c.playingCycle = c.look.cycle;
        c.playingCycleSet = c.look.cycleSet;
    }

    // The spine aim's bone of a model that aims (looked up once its skeleton is in).
    for (const Character& c : characters_) {
        if (!c.spawned || c.type >= characterModels_.size() || glm::length(c.look.spine) <= 1e-4f) continue;
        CharacterModel& m = characterModels_[c.type];
        if (m.spineBone != -2 || !m.info || !m.info->skeleton) continue;
        m.spineBone = m.info->skeleton->Find("Spine_Base");
        if (m.spineBone < 0) m.spineBone = m.info->skeleton->Find("Bip01 Spine");
    }
    // Pose everyone on the workers, straight into this slot's palettes (the GPU is done with them).
    auto* memory = static_cast<std::byte*>(context.device.MappedMemory(palettes_[context.slot]));
    if (memory == nullptr) return;
    const float blendMs = static_cast<float>(context.dt * 1000.0);
    static const bool mixFrameLog = context.config.GetBool("mix-frame-log", false); // each frame's layers of the first character
    context.jobs.ParallelFor(characters_.size(), 16, [&](std::size_t begin, std::size_t end) {
        SR_ZONE("world: pose");
        for (std::size_t i = begin; i < end; ++i) {
            Character& c = characters_[i];
            if (!c.spawned) continue;
            // A held pose stays where it is: its clips' time does not move.
            if (c.look.held) {
                c.phase -= static_cast<float>(context.dt);
                c.frozenMs += context.dt * 1000.0;
            }
            c.pose.AdvanceBlend(blendMs);
            // A skill's spine aim (1.188's bone controller Spine_Base).
            if (const float angle = glm::length(c.look.spine); c.type < characterModels_.size()) {
                const CharacterModel& m = characterModels_[c.type];
                const int bone = m.spineBone;
                c.pose.SetBoneTurn(angle > 1e-4f ? bone : -1, angle > 1e-4f ? glm::angleAxis(angle, c.look.spine / angle) : glm::quat(1, 0, 0, 0));
            }
            if (c.mixing) {
                if (mixFrameLog && &c == &characters_.front()) core::LogInfo("mixf: {:.3f} {}", time, c.pose.MixSummary(time * 1000.0 - c.frozenMs));
                c.pose.EvaluateMix(time * 1000.0 - c.frozenMs, !c.look.once);
            } else {
                c.pose.Evaluate(static_cast<float>((time + c.phase) * 1000.0), !c.look.once);
            }
            const std::vector<glm::mat4>& palette = c.pose.Palette();
            const std::size_t bones = std::min<std::size_t>(palette.size(), kMaxBones);
            std::memcpy(memory + std::size_t{c.paletteOffset} * sizeof(glm::mat4), palette.data(), bones * sizeof(glm::mat4));
        }
    });
    snapshot.scene.worldPalettes = palettes_[context.slot];
}

// The moving map objects (WorldStreamer::AnimatedObjects): each its clip looped from its own moment, on the workers,
// into this slot's palettes after the characters'.
void WorldScreen::PoseObjects(ScreenContext& context, FrameSnapshot& snapshot) {
    std::vector<WorldStreamer::Animated>& objects = streamer_.AnimatedObjects();
    if (dungeon_ || objects.empty() || palettes_.empty()) return;
    SR_ZONE("world: objects' poses");
    auto* memory = static_cast<std::byte*>(context.device.MappedMemory(palettes_[context.slot]));
    if (memory == nullptr) return;
    const double time = context.time;
    context.jobs.ParallelFor(objects.size(), 32, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            WorldStreamer::Animated& a = objects[i];
            a.pose.Evaluate(static_cast<float>((time + a.phase) * 1000.0), false);
            const std::vector<glm::mat4>& palette = a.pose.Palette();
            const std::size_t bones = std::min<std::size_t>(palette.size(), WorldStreamer::kLargeBones);
            std::memcpy(memory + std::size_t{a.paletteOffset} * sizeof(glm::mat4), palette.data(), bones * sizeof(glm::mat4));
        }
    });
    snapshot.scene.worldPalettes = palettes_[context.slot];
}

void WorldScreen::UpdateEnvironment(ScreenContext& context, FrameSnapshot& snapshot) {
    if (dungeon_) {
        // The current block's ambient light, fog and lights; no sky, no sun.
        dungeon_->Light(snapshot.scene);
        snapshot.scene.glow = glow_;
        snapshot.scene.lensFlare = false;
        snapshot.scene.viewDistance = viewDistance_;
        snapshot.scene.frameSeconds = static_cast<float>(context.dt);
        return;
    }
    // The original's game clock (20 ms a game second), unless the time is fixed.
    if (fixedTime_ < 0.0f) clock_.Advance(context.dt);
    const float time = fixedTime_ >= 0.0f ? std::clamp(fixedTime_, 0.0f, 1.0f) : clock_.TimeOfDay();

    // The block under the camera: its environment entry, and whether the camera is in its water.
    // The effects' quality: the original's row 13 (all off, damage only, all on), unless set on
    // the command line.
    if (context.options != nullptr && !context.config.Has("effect-quality")) {
        effects_.SetQuality(std::clamp(context.options->video.current()[13], 0, 2));
    }
    gfx::EnvironmentOptions options;
    // Our view distance row: the fog pushed out, the far plane and the loaded regions after it.
    const int farther = context.options != nullptr ? std::clamp(context.options->video.viewDistance, 0, 3) : 0;
    options.fogRange = fogRange_ * static_cast<float>(1 + farther);
    camera_.farZ = std::max(6000.0f, options.fogRange * 1.3f + 1000.0f);
    const int radius = farther == 0 ? streaming_.radius
                                     : std::clamp(std::max(streaming_.radius, static_cast<int>(std::ceil(options.fogRange / assets::kRegionSize)) + 1), 0, 7);
    if (radius != streamer_.Radius()) streamer_.SetRadius(radius);
    options.glow = glow_; // the light is brighter with glow on (x 0.8 instead of 0.6)
    // The Modern look with the sun's shadows: its path tilted 35 degrees (shadows turn around).
    options.sunTilt = context.options != nullptr && context.options->video.sunShadows > 0 ? 0.61f : 0.0f;
    float lx = 0, lz = 0;
    if (const auto info = streamer_.TerrainAt(context.assets, camera_.targetX, camera_.targetZ, lx, lz)) {
        const int bx = std::clamp(static_cast<int>(lx / assets::kBlockSize), 0, assets::kBlocksPerRegion - 1);
        const int bz = std::clamp(static_cast<int>(lz / assets::kBlockSize), 0, assets::kBlocksPerRegion - 1);
        const auto b = static_cast<std::size_t>(bz * assets::kBlocksPerRegion + bx);
        environmentKey_ = info->environments[b];
        const gfx::TerrainWater& water = info->water[b];
        options.underWater = water.Visible() && camera_.targetY - 0.5f < water.height;
        options.weather = weather_.Falling();
    }
    if (const assets::EnvironmentEntry* entry = indexes_->environment.Find(environmentKey_)) {
        environment_.Update(*entry, time, static_cast<float>(context.dt), clock_.Day(), options);
    }
    if (environment_.Valid()) {
        snapshot.scene.lighting = environment_.Lighting();
        snapshot.scene.sky = environment_.Sky();
    }
    snapshot.scene.glow = glow_;
    snapshot.scene.lensFlare = lensFlare_ && !weather_.Falling(); // the original hides the sun under rain and snow
    snapshot.scene.viewDistance = viewDistance_;
    snapshot.scene.frameSeconds = static_cast<float>(context.dt);
}

void WorldScreen::UpdateWeather(ScreenContext& context, FrameSnapshot& snapshot) {
    if (dungeon_) return; // no rain or snow underground
    const float dt = static_cast<float>(context.dt);
    weather_.Advance(dt);
    if (lightningAt_ >= 0.0 && context.time >= lightningAt_) {
        weatherEffect_.Strike(0);
        lightningAt_ = -1.0;
    }
    // A cluster that starts over water ends on it in a ripple: the block's water under (x, z).
    const auto waterAt = [&](float x, float z, float& height) {
        float lx = 0, lz = 0;
        const auto info = streamer_.TerrainAt(context.assets, x, z, lx, lz);
        if (!info) return false;
        const int bx = std::clamp(static_cast<int>(lx / assets::kBlockSize), 0, assets::kBlocksPerRegion - 1);
        const int bz = std::clamp(static_cast<int>(lz / assets::kBlockSize), 0, assets::kBlocksPerRegion - 1);
        const gfx::TerrainWater& water = info->water[static_cast<std::size_t>(bz * assets::kBlocksPerRegion + bx)];
        if (!water.Visible()) return false;
        height = water.height;
        return true;
    };
    weatherEffect_.Update(weather_.Kind(), weather_.Effective(), glm::vec3(camera_.targetX, camera_.targetY, camera_.targetZ), dt,
                          waterAt);
    // A strike's thunder as the original's: lightning1..3.wav by the strike's pattern, heard where the camera looks.
    if (const int thunder = weatherEffect_.TakeThunder(); thunder >= 0 && context.audio != nullptr) {
        context.audio->PlayEffect(std::format("prim\\snd\\etc\\lightning{}.wav", thunder + 1), glm::vec3(camera_.targetX, camera_.targetY, camera_.targetZ));
    }
    weatherEffect_.Fill(snapshot.scene.weather);
    // The ground under it (ours, the Modern look): wet within about 30 s of rain (its strength: the amount over 150), dry
    // again over 2 minutes; snow (the climates' 3..10 a light fall, 8 and more a full one) lies within 90 s and melts
    // over 3 minutes.
    const int amount = weather_.Effective();
    const float rain = weather_.Kind() == game::WeatherKind::Rain ? std::min(1.0f, float(amount) / 150.0f) : 0.0f;
    const float snow = weather_.Kind() == game::WeatherKind::Snow ? std::min(1.0f, float(amount) / 8.0f) : 0.0f;
    const auto towards = [&](float& value, float target, float upSeconds, float downSeconds) {
        value = value < target ? std::min(target, value + dt / upSeconds) : std::max(target, value - dt / downSeconds);
    };
    towards(wetness_, rain, 30.0f, 120.0f);
    towards(snowLying_, snow, 90.0f, 180.0f);
    snapshot.scene.weather.ground = glm::vec4(wetness_, snowLying_, rain, 0.0f);
}

void WorldScreen::DrawHud(ScreenContext& context, FrameSnapshot& snapshot) {
    if (!hudFont_ || !hudFont_->ready.load(std::memory_order_acquire)) return;
    ui::Font& font = hudFont_->font;
    hud_.Clear();
    hud_.SetWhite(font.texture(), font.whiteUv());
    const int minutes = static_cast<int>(clock_.Hours() * 60.0);
    char lines[3][96];
    std::snprintf(lines[0], sizeof(lines[0]), "SroReimagined - region %d,%d", originX_, originY_);
    std::snprintf(lines[1], sizeof(lines[1]), "time of day %.3f (clock %02d:%02d, day %d)", snapshot.scene.lighting.timeOfDay,
                  minutes / 60, minutes % 60, clock_.Day());
    std::snprintf(lines[2], sizeof(lines[2]), "%zu characters, %zu placements", spawned_, streamer_.GetStats(context.assets).placements);
    const float size = 15.0f, line = font.LineHeight(size);
    float width = 0;
    for (const char* text : lines) width = std::max(width, font.Measure(text, size).x);
    hud_.Fill({12, 12, width + 20, line * 3 + 16}, ui::Rgba(0, 0, 0, 150));
    hud_.Outline({12, 12, width + 20, line * 3 + 16}, ui::Rgba(200, 170, 110, 220));
    for (int i = 0; i < 3; ++i) font.Draw(hud_, lines[i], {22, 20 + line * static_cast<float>(i)}, size, ui::Rgba(240, 230, 210));
    // New glyphs go to the GPU before the frame that draws them.
    hudAtlas_.Sync(context.device, font);
    AppendUi(snapshot.ui, hud_);
}

void WorldScreen::Update(ScreenContext& context, FrameSnapshot& snapshot) {
    snapshot.scene.active = true;
    // The clicks' occlusion: the scene's depth under the mouse, asked for and read back a few frames later.
    snapshot.scene.probeX = probePoint_.x;
    snapshot.scene.probeY = probePoint_.y;
    if (context.feedback != nullptr) {
        const bool had = probe_.valid;
        probe_ = context.feedback->Probe();
        if (!had && probe_.valid) core::LogInfo("world: the scene's depth under the mouse read back ({:.5f} at {:.0f},{:.0f})", probe_.depth, probe_.x, probe_.y);
    }
    snapshot.scene.clearColor = {0.55f, 0.66f, 0.8f, 1.0f}; // a placeholder sky until C3e
    if (!indexes_->ready.load(std::memory_order_acquire)) {
        context.holdCapture = true;
        snapshot.scene.camera = camera_;
        return;
    }
    if (!started_) Start(context);
    if (teleportAt_ >= 0.0 && context.time >= teleportAt_) {
        teleportAt_ = -1.0;
        Teleport(context, snapshot);
    }
    const bool loadingScreen = loading_.Active();
    if (backdrop_) {
        // The owner places the eye from the rotation (an orbit around what it looks at): a newer
        // rotation latched onto an older eye would swing the view off its target, so none.
        context.camera.Clear();
    } else if (!loadingScreen) {
        if (flight_) Flight(context);
        else Fly(context);
    }
    // The camera's velocity drives the prefetch.
    const glm::vec3 position(camera_.targetX, camera_.targetY, camera_.targetZ);
    const glm::vec3 velocity = context.dt > 0 ? (position - lastCamera_) / static_cast<float>(context.dt) : glm::vec3(0.0f);
    lastCamera_ = position;
    if (dungeon_) {
        // The dungeon: its navigation from the loader, the blocks of the focus's floor (else the camera's).
        navLoader_->Update(navWorld_);
        if (const game::NavDungeon* nav = navWorld_.FindDungeon(dungeonRegion_); nav != nullptr && dungeon_->Ready()) {
            const glm::vec3 at = focus_ ? glm::vec3(focus_->x, focus_->y + 5.0f, focus_->z) : position;
            dungeon_->Show(context.assets, DungeonScene::BlockUnder(*nav, at.x, at.y, at.z), snapshot.worldCommands, DungeonHandles());
        } else if (dungeon_->Ready() && !navLoader_->Loading(dungeonRegion_)) {
            dungeon_->Show(context.assets, -1, snapshot.worldCommands, DungeonHandles()); // no navigation: its entrance
        }
        if (dungeon_->Ready() && !dungeon_->Error().empty() && !reported_) {
            core::LogError("world: {}", dungeon_->Error());
            reported_ = true;
        }
        dungeon_->StartEffects(context.assets); // the blocks' own: the teleport circles, flames
    } else {
        streamer_.Update(context.assets, position, velocity, snapshot.worldCommands);
        streamer_.StartEffects(context.assets); // the map objects' own: the portal glows, lamps, the tomb's door
        streamer_.StartAnimations(context.assets, snapshot.worldCommands); // the moving ones (a hawk)
    }
    if (!cameraPlaced_ && !dungeon_) {
        // Above the origin region's highest point, once it is in.
        float lx = 0, lz = 0;
        if (const auto info = streamer_.TerrainAt(context.assets, assets::kRegionSize * 0.5f, assets::kRegionSize * 0.5f, lx, lz)) {
            float highest = info->blockHeights[0][1];
            for (const auto& h : info->blockHeights) highest = std::max(highest, h[1]);
            camera_.targetY = highest + 350.0f;
            lastCamera_.y = camera_.targetY;
            cameraPlaced_ = true;
        }
    }
    if (loadingScreen && UpdateLoading(context, snapshot)) return;
    UpdateCrowd(context);
    UpdateCharacters(context, snapshot);
    PoseObjects(context, snapshot);
    UpdateWeather(context, snapshot);
    UpdateEnvironment(context, snapshot);
    UpdateEffects(context, snapshot);
    DrawHud(context, snapshot);
    UpdateMenu(context, snapshot);
    snapshot.scene.camera = camera_;
    snapshot.scene.seconds = context.time;
    streamer_.AppendTerrain(snapshot.scene.terrain);

    // Loading: every wanted region and its objects resident or failed.
    // A character whose model failed, or came without a skeleton (it cannot stand and be posed),
    // is not waited for: it never spawns as a walker (a boneless one stands as a still model, above).
    std::size_t wanted = 0;
    for (const Character& c : characters_) {
        if (!c.active) continue;
        CharacterModel& m = characterModels_[c.type];
        const bool failed = context.assets.State(m.model) == gfx::AssetState::Failed;
        const bool boneless = m.ready && (!m.info || !m.info->skeleton);
        if (failed && !m.warned) {
            m.warned = true;
            core::LogWarning("world: {} failed to load: not shown", m.path);
        }
        if (!failed && !boneless) ++wanted;
    }
    bool propsIn = true;
    for (const Prop& p : props_) propsIn = propsIn && p.added;
    const bool loaded = (dungeon_ ? DungeonSettled(context.assets) : streamer_.Settled(context.assets)) && spawned_ >= wanted && propsIn && !CrowdWaiting();
    settled_ = loaded;
    const WorldStreamer::Stats stats = streamer_.GetStats(context.assets);
    if (!loaded) context.holdCapture = true;
    if (loaded && !reported_) {
        reported_ = true;
        core::LogInfo("world: {} regions around {},{} in {:.2f} s; {} placements, {} objects ({} unknown ids)", stats.resident, originX_,
                      originY_, context.time, stats.placements, stats.objects, stats.unknownObjects);
    }

    if (context.toolsVisible && !backdrop_) {
        ImGui::SetNextWindowPos({490, 16}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("World")) {
            ImGui::Text("origin %d,%d  radius %d", originX_, originY_, streaming_.radius);
            ImGui::Text("regions %zu / %zu resident, %llu requested, %llu released", stats.resident, stats.regions,
                        static_cast<unsigned long long>(stats.requested), static_cast<unsigned long long>(stats.released));
            ImGui::Text("%zu objects (%zu unknown ids), %zu instances", stats.objects, stats.unknownObjects, stats.instances);
            ImGui::Text("navigation: %zu regions (%zu loading, %zu without), %zu object navmeshes", stats.navigation.ready,
                        stats.navigation.loading, stats.navigation.missing, stats.navigation.objectMeshes);
            const WeatherEffect::Stats w = weatherEffect_.GetStats();
            ImGui::Text("weather %d, amount %d (now %d): %zu rain, %zu snow, %zu ripples", static_cast<int>(weather_.Kind()), weather_.Amount(),
                        weather_.Effective(), w.rain, w.snow, w.ripples);
            ImGui::Text("camera %.0f %.0f %.0f  yaw %.2f pitch %.2f", camera_.targetX, camera_.targetY, camera_.targetZ, camera_.yaw,
                        camera_.pitch);
            if (!characters_.empty()) {
                ImGui::Text("characters %zu / %zu", spawned_, characters_.size());
                CrowdTools();
            }
            const gfx::EffectSystem::Stats& e = effects_.GetStats();
            ImGui::Text("effects %zu (%zu seen): %zu elements, %zu sprites, %zu mesh instances, %zu lights", e.effects, e.visible, e.elements,
                        e.sprites, e.meshInstances, e.lights);
            const int minutes = static_cast<int>(clock_.Hours() * 60.0);
            ImGui::Text("clock day %d %02d:%02d, time of day %.3f, environment %u", clock_.Day(), minutes / 60, minutes % 60,
                        snapshot.scene.lighting.timeOfDay, environmentKey_);
            // The time held (0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset), or the clock's.
            bool fixed = fixedTime_ >= 0.0f;
            if (ImGui::Checkbox("fixed time", &fixed)) fixedTime_ = fixed ? clock_.TimeOfDay() : -1.0f;
            if (fixed) {
                ImGui::SameLine();
                float t = fixedTime_;
                if (ImGui::SliderFloat("##time", &t, 0.0f, 1.0f, "%.3f")) fixedTime_ = t;
            }
            ImGui::Checkbox("glow", &glow_);
            ImGui::SameLine();
            ImGui::Checkbox("lens flare", &lensFlare_);
        }
        ImGui::End();
        snapshot.drawImgui = true;
    }
}

} // namespace sr::client
