#include "sr/gfx/AssetStore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "sr/assets/Dress.h"
#include "sr/assets/FileSystem.h"
#include "sr/assets/ItemGlow.h"
#include "sr/assets/Outfit.h"
#include "sr/core/Log.h"
#include "sr/core/Path.h"
#include "sr/core/Trace.h"

namespace sr::gfx {

namespace {

using Clock = std::chrono::steady_clock;
// Promotion and eviction each stop after this long in a frame.
constexpr std::chrono::microseconds kUpdateStepBudget{200};

rhi::Format ToRhi(assets::TextureFormat format) {
    switch (format) {
    case assets::TextureFormat::RGBA8: return rhi::Format::RGBA8Unorm;
    case assets::TextureFormat::BC1: return rhi::Format::BC1Unorm;
    case assets::TextureFormat::BC2: return rhi::Format::BC2Unorm;
    case assets::TextureFormat::BC3: return rhi::Format::BC3Unorm;
    }
    return rhi::Format::Unknown;
}

const char* KindPrefix(AssetKind kind) {
    switch (kind) {
    case AssetKind::Texture: return "t:";
    case AssetKind::Model: return "m:";
    case AssetKind::Animation: return "a:";
    case AssetKind::Region: return "r:";
    }
    return "?:";
}

std::uint64_t Align(std::uint64_t value, std::uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

} // namespace

const char* ToString(AssetState state) {
    switch (state) {
    case AssetState::Queued: return "queued";
    case AssetState::Loading: return "loading";
    case AssetState::Waiting: return "waiting";
    case AssetState::Uploading: return "uploading";
    case AssetState::Resident: return "resident";
    case AssetState::Failed: return "failed";
    }
    return "?";
}

std::uint32_t VertexStride(std::uint32_t vertexFlags) {
    return 32 + ((vertexFlags & kVertexLightmap) ? 8 : 0) + ((vertexFlags & kVertexSkinned) ? 12 : 0);
}

// A decoded asset waiting to be written into staging.
struct AssetStore::Pending {
    assets::TextureData texture;
    assets::ModelData model;
    std::shared_ptr<ModelInfo> info;
    std::vector<std::vector<std::uint8_t>> boneRemap; // per part: mesh bone -> skeleton bone
    std::vector<std::uint64_t> ranges;                // per part: vertices, indices (once allocated)
    // A region: its built terrain, the textures of its palette (null: missing) and its lightmap.
    TerrainBuild terrain;
    std::vector<AssetId> palette;
    AssetId lightmap;
    std::shared_ptr<TerrainInfo> terrainInfo;
};

struct AssetStore::Record {
    std::atomic<AssetState> state{AssetState::Failed};
    std::atomic<std::uint32_t> generation{0}; // the table generation this record serves
    AssetKind kind = AssetKind::Texture;
    int priority = 0;
    std::string path; // normalized
    std::string error;
    rhi::UploadId upload = 0;
    std::unique_ptr<Pending> pending;
    std::shared_ptr<const ModelInfo> model;     // Resident
    std::shared_ptr<ModelInfo> building;        // Uploading: bindless indices filled at promotion
    TextureInfo texture;
    std::shared_ptr<const assets::AnimationData> animation;
    std::shared_ptr<const TerrainInfo> terrain;     // Resident (a region)
    std::shared_ptr<TerrainInfo> buildingTerrain;   // Uploading: the lightmap filled at promotion
    AssetId lightmap;                               // a region's lightmap (also in dependencies)
    std::vector<AssetId> dependencies; // textures a model or a region holds a reference on
    std::vector<std::uint64_t> ranges; // geometry pool ranges it owns
    std::uint64_t bytes = 0;           // counted in residentBytes_
    Clock::time_point requested{};
    double loadMs = -1;
    bool tableFullLogged = false;
};

AssetStore::AssetStore(rhi::Device& device, const assets::FileSystem& files, core::JobSystem& jobs)
    : device_(device), files_(files), jobs_(jobs) {}

AssetStore::~AssetStore() {
    Shutdown();
    for (auto& block : blocks_) delete[] block.load(std::memory_order_relaxed);
}

bool AssetStore::Init(const AssetStoreDesc& desc, std::string& error) {
    desc_ = desc;
    maxJobs_ = desc.maxJobs != 0 ? desc.maxJobs : std::max(2u, jobs_.WorkerCount() / 2);
    table_.Reserve(desc.expectedAssets);
    return pool_.Init(device_, desc.geometryBytes, error) && meshes_.Init(device_, desc.meshRecords, error);
}

void AssetStore::Shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    jobs_.Wait(jobsDone_);
    // Everything goes: the device defers the GPU side until its frames are done.
    const std::uint32_t slots = table_.SlotCount();
    for (std::uint32_t i = 1; i < slots; ++i) {
        Record* r = blocks_[i / kBlockSize].load(std::memory_order_acquire);
        if (r == nullptr) continue;
        Record& rec = r[i % kBlockSize];
        if (rec.texture.handle) device_.DestroyTexture(rec.texture.handle);
        rec.texture = {};
        rec.model.reset();
        rec.building.reset();
        rec.pending.reset();
        rec.animation.reset();
        rec.terrain.reset();
        rec.buildingTerrain.reset();
    }
    pool_.Shutdown();
    meshes_.Shutdown();
}

AssetStore::Record& AssetStore::Slot(std::uint32_t index) {
    // Called with requestMutex_ held: blocks are created under it and never move.
    std::atomic<Record*>& block = blocks_[index / kBlockSize];
    Record* records = block.load(std::memory_order_acquire);
    if (records == nullptr) {
        records = new Record[kBlockSize];
        block.store(records, std::memory_order_release);
    }
    return records[index % kBlockSize];
}

AssetStore::Record* AssetStore::Get(AssetId id) const {
    // No lock: the record's generation is the table's while the entry lives and 0 once it is
    // evicted, so a stale id never matches (the render thread looks up thousands a frame while
    // the decode jobs request more).
    if (!id || id.index / kBlockSize >= kMaxBlocks) return nullptr;
    Record* records = blocks_[id.index / kBlockSize].load(std::memory_order_acquire);
    if (records == nullptr) return nullptr;
    Record& r = records[id.index % kBlockSize];
    return r.generation.load(std::memory_order_acquire) == id.generation ? &r : nullptr;
}

AssetId AssetStore::Request(AssetKind kind, std::string_view path, int priority) {
    if (shutDown_ || path.empty()) return {};
    const std::string key = core::NormalizeAssetPath(path);
    std::lock_guard lock(requestMutex_); // a second request for the same key sees the record set up
    bool created = false;
    const AssetId id = table_.Acquire(KindPrefix(kind) + key, created);
    if (!id || id.index / kBlockSize >= kMaxBlocks) return {};
    Record& r = Slot(id.index);
    if (created) {
        r.kind = kind;
        r.priority = priority;
        r.path = key;
        r.error.clear();
        r.upload = 0;
        r.pending.reset();
        r.model.reset();
        r.building.reset();
        r.texture = {};
        r.animation.reset();
        r.terrain.reset();
        r.buildingTerrain.reset();
        r.lightmap = {};
        r.tableFullLogged = false;
        r.dependencies.clear();
        r.ranges.clear();
        r.bytes = 0;
        r.requested = Clock::now();
        r.loadMs = -1;
        r.state.store(AssetState::Queued, std::memory_order_relaxed);
        r.generation.store(id.generation, std::memory_order_release);
        queue_.push({priority, sequence_++, id});
    } else if (priority < r.priority && r.state.load(std::memory_order_acquire) == AssetState::Queued) {
        r.priority = priority; // more urgent now: queued again in front (the old entry is skipped)
        queue_.push({priority, sequence_++, id});
    }
    return id;
}

AssetId AssetStore::RequestModel(std::string_view path, int priority) { return Request(AssetKind::Model, path, priority); }
AssetId AssetStore::RequestTexture(std::string_view path, int priority) { return Request(AssetKind::Texture, path, priority); }
AssetId AssetStore::RequestAnimation(std::string_view path, int priority) { return Request(AssetKind::Animation, path, priority); }
AssetId AssetStore::RequestRegion(int x, int y, int priority) {
    if (x < 0 || y < 0 || x > 255 || y > 255) return {};
    // A region the map has no terrain for (the sea, the map's edge: a third of the regions around
    // Constantinople) is not a failure: no asset, nothing logged (its state reads Failed: nothing
    // to place).
    const std::string path = assets::RegionFile(assets::RegionId(x, y), "");
    if (!files_.Exists(path + ".m")) return {};
    return Request(AssetKind::Region, path, priority);
}

void AssetStore::SetTileIndex(std::shared_ptr<const assets::AssetIndex> index) {
    std::lock_guard lock(requestMutex_);
    tileIndex_ = std::move(index);
}

void AssetStore::AddRef(AssetId id) { table_.AddRef(id); }
void AssetStore::Release(AssetId id) { table_.Release(id); }

AssetState AssetStore::State(AssetId id) const {
    const Record* r = Get(id);
    return r != nullptr ? r->state.load(std::memory_order_acquire) : AssetState::Failed;
}

std::string AssetStore::Error(AssetId id) const {
    std::lock_guard lock(payloadMutex_);
    const Record* r = Get(id);
    if (r == nullptr) return "no such asset";
    return r->state.load(std::memory_order_acquire) == AssetState::Failed ? r->error : std::string();
}

std::string AssetStore::Path(AssetId id) const {
    std::lock_guard lock(payloadMutex_);
    const Record* r = Get(id);
    return r != nullptr ? r->path : std::string();
}

std::shared_ptr<const ModelInfo> AssetStore::Model(AssetId id) const {
    std::lock_guard lock(payloadMutex_);
    const Record* r = Get(id);
    return r != nullptr && r->state.load(std::memory_order_acquire) == AssetState::Resident ? r->model : nullptr;
}

std::optional<TextureInfo> AssetStore::Texture(AssetId id) const {
    std::lock_guard lock(payloadMutex_);
    const Record* r = Get(id);
    if (r == nullptr || r->state.load(std::memory_order_acquire) != AssetState::Resident || r->kind != AssetKind::Texture) {
        return std::nullopt;
    }
    return r->texture;
}

std::shared_ptr<const TerrainInfo> AssetStore::Terrain(AssetId id) const {
    std::lock_guard lock(payloadMutex_);
    const Record* r = Get(id);
    return r != nullptr && r->state.load(std::memory_order_acquire) == AssetState::Resident ? r->terrain : nullptr;
}

std::shared_ptr<const assets::AnimationData> AssetStore::Animation(AssetId id) const {
    std::lock_guard lock(payloadMutex_);
    const Record* r = Get(id);
    return r != nullptr && r->state.load(std::memory_order_acquire) == AssetState::Resident ? r->animation : nullptr;
}

double AssetStore::LoadMs(AssetId id) const {
    std::lock_guard lock(payloadMutex_);
    const Record* r = Get(id);
    return r != nullptr ? r->loadMs : -1;
}

// ---------------------------------------------------------------- jobs

void AssetStore::Fail(Record& r, std::string error) {
    // Nothing of it reached the GPU: its resources go back at once.
    if (r.texture.handle) device_.DestroyTexture(r.texture.handle);
    r.texture = {};
    if (r.pending) {
        for (std::uint64_t offset : r.pending->ranges) pool_.Free(offset);
        r.pending.reset();
    }
    for (AssetId dep : r.dependencies) table_.Release(dep);
    r.dependencies.clear();
    if (failures_.fetch_add(1) < 40) core::LogWarning("assets: {} failed: {}", r.path, error);
    r.error = std::move(error);
    r.loadMs = std::chrono::duration<double, std::milli>(Clock::now() - r.requested).count();
    r.state.store(AssetState::Failed, std::memory_order_release);
}

void AssetStore::Load(AssetId id) {
    Record* r = Get(id);
    if (r == nullptr) return;
    switch (r->kind) {
    case AssetKind::Texture: LoadTexture(id, *r); break;
    case AssetKind::Model: LoadModel(id, *r); break;
    case AssetKind::Animation: LoadAnimation(id, *r); break;
    case AssetKind::Region: LoadRegion(id, *r); break;
    }
}

void AssetStore::LoadTexture(AssetId id, Record& r) {
    SR_ZONE("assets: texture");
    const auto file = files_.Read(r.path);
    if (!file) return Fail(r, "file not found");
    decodedBytes_ += file->Size();
    auto pending = std::make_unique<Pending>();
    assets::TextureData& texture = pending->texture;
    std::string error;
    // .ddj, .dds, or a region's .t lightmap.
    const bool dds = r.path.ends_with(".dds"), lightmap = r.path.ends_with(".t");
    const bool decoded = lightmap ? assets::DecodeMapLightmap(file->Bytes(), texture, error)
                         : dds    ? assets::DecodeDds(file->Bytes(), texture, error)
                                  : assets::DecodeDdj(file->Bytes(), texture, error);
    if (!decoded) return Fail(r, error);
    // The world's textures get their whole mip chain, as the original's loader makes them (the
    // interface is drawn 1:1).
    if (!r.path.starts_with("interface\\")) assets::CompleteMips(texture);
    const rhi::Format format = ToRhi(texture.format);
    std::uint64_t bytes = 0;
    for (const assets::TextureData::Mip& mip : texture.mips) {
        if (mip.size != rhi::MipSizeInBytes(format, mip.width, mip.height)) return Fail(r, "unexpected mip layout");
        bytes += mip.size;
    }
    rhi::TextureDesc desc;
    desc.width = texture.width;
    desc.height = texture.height;
    desc.mipLevels = static_cast<std::uint32_t>(texture.mips.size());
    desc.format = format;
    desc.debugName = r.path;
    desc.streamed = true;
    const rhi::TextureHandle handle = device_.CreateTexture(desc);
    if (!handle) return Fail(r, "cannot create the texture");
    r.texture.handle = handle;
    r.texture.bindless = device_.BindlessIndex(handle);
    r.texture.width = texture.width;
    r.texture.height = texture.height;
    r.texture.mips = desc.mipLevels;
    r.texture.format = texture.format;
    r.texture.sourceFormat = texture.sourceFormat;
    r.texture.premultipliedAlpha = texture.premultipliedAlpha;
    r.texture.bytes = bytes;
    r.pending = std::move(pending);
    Stage(id, r);
}

void AssetStore::LoadModel(AssetId id, Record& r) {
    SR_ZONE("assets: model");
    auto pending = std::make_unique<Pending>();
    assets::ModelData& model = pending->model;
    std::string error;
    std::vector<std::optional<assets::EquipGlow>> glows; // an outfit's items' plus glows (assets::WithGlow)
    if (r.path.find('|') != std::string::npos) {
        // An outfit: "body|item|item", the worn items merged onto the body's skeleton.
        std::vector<std::string> parts;
        for (std::size_t start = 0; start <= r.path.size();) {
            const std::size_t bar = std::min(r.path.find('|', start), r.path.size());
            parts.push_back(r.path.substr(start, bar - start));
            start = bar + 1;
        }
        std::string body = parts.front();
        parts.erase(parts.begin());
        for (std::string& item : parts) {
            assets::EquipGlow glow;
            glows.push_back(assets::SplitGlow(item, glow) ? std::optional<assets::EquipGlow>(std::move(glow)) : std::nullopt);
        }
        assets::OutfitReport report;
        if (!assets::AssembleOutfit(files_, body, parts, model, report, error)) return Fail(r, error);
        for (std::string& w : report.warnings) model.warnings.push_back(std::move(w));
    } else if (const std::size_t mark = r.path.find("#set"); mark != std::string::npos) {
        // "model.bsr#setN": dressed in the resource's material set N (a champion's look).
        if (!assets::LoadModel(files_, r.path.substr(0, mark), model, error)) return Fail(r, error);
        const auto set = static_cast<std::uint32_t>(std::strtoul(r.path.c_str() + mark + 4, nullptr, 10));
        if (set != 0 && !assets::UseMaterialSet(files_, model, set)) model.warnings.push_back("no material set " + std::to_string(set));
    } else if (!assets::LoadModel(files_, r.path, model, error)) {
        return Fail(r, error);
    }

    auto info = std::make_shared<ModelInfo>();
    const bool skeleton = !model.skeleton.bones.empty() && model.skeleton.bones.size() <= 256;
    for (const assets::ModelPart& part : model.parts) {
        const assets::MeshData& mesh = part.mesh;
        ModelPartInfo p;
        p.meshPath = part.meshPath;
        p.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
        p.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
        {
            // A bounding sphere around the part's box, in model space (skinned parts in their bind
            // pose; the culling pass widens it for animation).
            const auto& b = mesh.bounds;
            const float dx = b.max[0] - b.min[0], dy = b.max[1] - b.min[1], dz = b.max[2] - b.min[2];
            p.sphere = {(b.min[0] + b.max[0]) * 0.5f, (b.min[1] + b.max[1]) * 0.5f, (b.min[2] + b.max[2]) * 0.5f,
                        0.5f * std::sqrt(dx * dx + dy * dy + dz * dz)};
        }
        if ((mesh.flags & 0x400u) != 0 && mesh.lightmapUvs.size() == mesh.vertices.size()) {
            p.vertexFlags |= kVertexLightmap;
            // The mesh's baked light (prim\lightmap\...), multiplied over its texture.
            if (!mesh.lightmapPath.empty()) {
                p.lightmap = RequestTexture(mesh.lightmapPath, r.priority);
                if (p.lightmap) r.dependencies.push_back(p.lightmap);
            }
        }
        std::vector<std::uint8_t> remap;
        if (skeleton && mesh.IsSkinned() && mesh.boneIndices.size() == mesh.vertices.size()) {
            p.vertexFlags |= kVertexSkinned;
            // Mesh bone numbers refer to the mesh's own bone list: rewrite them as skeleton bones.
            remap.assign(mesh.boneNames.size(), 0);
            for (std::size_t b = 0; b < mesh.boneNames.size(); ++b) {
                const int bone = model.skeleton.Find(mesh.boneNames[b]);
                remap[b] = static_cast<std::uint8_t>(bone < 0 ? 0 : bone);
            }
        }
        if (part.hasMaterial) {
            const std::uint32_t flags = part.material.flags;
            // Cut out: the material's 0x200 flag, unless an environment map covers it (worn items' shine: its alpha is
            // the shine's mask), which cuts out only alpha 0 when it says so.
            if (part.ambient.envMap) {
                if (part.ambient.envAlphaZero) p.materialFlags |= kMaterialAlphaTest | kMaterialAlphaZero;
            } else if ((flags & 0x200u) != 0 && (flags & 0x30000000u) == 0) {
                p.materialFlags |= kMaterialAlphaTest;
            }
            if ((flags & 0x8u) != 0) p.materialFlags |= kMaterialNightLight;
            if (part.overSkin) p.materialFlags |= kMaterialOverSkin;
            if ((flags & 0x1u) != 0) p.materialFlags |= kMaterialTwoSided;
            if (core::NormalizeAssetPath(part.meshPath).find("\\nature\\") != std::string::npos) p.materialFlags |= kMaterialFoliage;
            std::copy(std::begin(part.material.diffuse), std::end(part.material.diffuse), p.diffuse.begin());
            const assets::AmbientLook& look = part.ambient;
            if (look.blended) {
                p.materialFlags &= ~(kMaterialAlphaTest | kMaterialAlphaZero | kMaterialTwoSided);
                p.materialFlags |= kMaterialBlended | (look.alphaTest ? kMaterialAlphaTest : 0u) | (look.twoSided ? kMaterialTwoSided : 0u);
                p.materialFlags |= (std::uint32_t{look.sourceBlend} & 15u) << kMaterialBlendShift;
                p.materialFlags |= (std::uint32_t{look.destinationBlend} & 15u) << (kMaterialBlendShift + 4);
                p.materialFlags |= static_cast<std::uint32_t>(std::clamp(look.tint * 100.0f + 0.5f, 0.0f, 255.0f)) << kMaterialTintShift;
            }
            if (look.scrolls) {
                p.materialFlags |= kMaterialScrolls;
                std::copy(std::begin(look.uvRate), std::end(look.uvRate), p.uvRate.begin());
                // A pool's surface: solid, its material or texture named for water (the Hotan and
                // Constantinople fountains; the waterfalls are blended).
                std::string names = part.material.name + "|" + part.material.texturePath;
                for (char& c : names) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                if (!look.blended && names.find("water") != std::string::npos) p.materialFlags |= kMaterialWater;
            }
            if (!part.material.texturePath.empty()) {
                p.texture = RequestTexture(part.material.texturePath, r.priority);
                if (p.texture) r.dependencies.push_back(p.texture);
            }
        }
        // The plus glow of the outfit's item this part belongs to.
        if (part.outfitItem >= 0 && static_cast<std::size_t>(part.outfitItem) < glows.size() && glows[static_cast<std::size_t>(part.outfitItem)]) {
            const assets::EquipGlow& g = *glows[static_cast<std::size_t>(part.outfitItem)];
            p.glowTexture = RequestTexture(g.texture, r.priority);
            if (p.glowTexture) {
                r.dependencies.push_back(p.glowTexture);
                p.materialFlags = (p.materialFlags & ~kMaterialScrolls) | kMaterialEquipGlow;
                // The offset grows by ms x the rate / 20: 50 times the rate a second.
                p.glowWords[0] = glm::packHalf2x16(glm::vec2(g.scroll[0] * 50.0f, g.scroll[1] * 50.0f));
                const auto rgba = [](const std::array<float, 3>& c, std::uint32_t a) {
                    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
                    return byte(c[0]) | (byte(c[1]) << 8) | (byte(c[2]) << 16) | (std::min(a, 255u) << 24);
                };
                p.glowWords[1] = rgba(g.from, (g.periodMs + 10) / 20);
                p.glowWords[2] = rgba(g.to, g.colorOp == 5 ? 2u : g.colorOp == 6 ? 4u : 1u);
                // Its alpha test at reference 1 (the item cut out where its alpha is 0).
                if (g.alphaTest) p.materialFlags |= kMaterialAlphaTest | kMaterialAlphaZero;
                core::LogInfo("assets: {} glows ({})", part.meshPath, g.texture);
            }
        }
        // A plant sways its leaves alone (cut out or blended), never its trunk; the camera keeps out of its solid parts alone.
        const bool seeThrough = (p.materialFlags & (kMaterialAlphaTest | kMaterialBlended)) != 0;
        if (!seeThrough) {
            p.materialFlags &= ~kMaterialFoliage;
            assets::Bounds& s = info->solidBounds;
            const bool first = s.min[0] > s.max[0];
            for (int k = 0; k < 3; ++k) {
                s.min[k] = first ? mesh.bounds.min[k] : std::min(s.min[k], mesh.bounds.min[k]);
                s.max[k] = first ? mesh.bounds.max[k] : std::max(s.max[k], mesh.bounds.max[k]);
            }
        }
        info->triangles += p.indexCount / 3;
        info->parts.push_back(std::move(p));
        pending->boneRemap.push_back(std::move(remap));
    }
    if (info->parts.empty()) return Fail(r, "no meshes");
    info->bounds = model.bounds;
    info->animationPaths = model.animationPaths;
    info->animationSets = model.animationSets;
    for (const assets::ResourceData::ModifierSet& set : model.resource.modifierSets) {
        if (set.kind == 2) info->ambientParticles.insert(info->ambientParticles.end(), set.particles.begin(), set.particles.end());
        if (!set.sounds.empty()) {
            assets::ResourceData::ModifierSet sounds;
            sounds.kind = set.kind;
            sounds.state = set.state;
            sounds.animationSet = set.animationSet;
            sounds.sounds = set.sounds;
            info->soundSets.push_back(std::move(sounds));
        }
    }
    info->warnings = model.warnings;
    if (skeleton) {
        info->skeleton = std::make_shared<const assets::SkeletonData>(model.skeleton);
        // Sockets: an item's vertices hang on its attach bone moved by that bone's bind position
        // (AssembleOutfit), so a socket in the item's space is placed the same way: the attach
        // bone's inverse bind, its bind position, then the socket's own place and turn.
        if (!model.sockets.empty()) {
            const auto quat = [](const std::array<float, 4>& q) { return glm::quat(q[3], q[0], q[1], q[2]); };
            const auto compose = [&](const std::array<float, 4>& r, const std::array<float, 3>& t) {
                glm::mat4 m = glm::mat4_cast(quat(r));
                m[3] = glm::vec4(t[0], t[1], t[2], 1.0f);
                return m;
            };
            std::vector<glm::mat4> bind(model.skeleton.bones.size());
            for (std::size_t i = 0; i < bind.size(); ++i) {
                const assets::Bone& bone = model.skeleton.bones[i];
                const glm::mat4 local = compose(bone.local.rotation, bone.local.translation);
                bind[i] = bone.parent >= 0 ? bind[static_cast<std::size_t>(bone.parent)] * local : local;
            }
            for (const assets::ModelData::Socket& s : model.sockets) {
                const int parent = model.skeleton.Find(s.parentBone);
                if (parent < 0) continue;
                const glm::mat4& b = bind[static_cast<std::size_t>(parent)];
                const glm::mat4 local = glm::inverse(b) * glm::translate(glm::mat4(1.0f), glm::vec3(b[3])) * compose(s.rotation, s.position);
                ModelInfo::Socket out;
                out.name = s.name;
                out.item = core::NormalizeAssetPath(s.item);
                out.parent = parent;
                std::memcpy(out.local.data(), glm::value_ptr(local), sizeof(out.local));
                info->sockets.push_back(std::move(out));
            }
        }
    }
    pending->info = std::move(info);
    r.pending = std::move(pending);
    Stage(id, r);
}

void AssetStore::LoadAnimation(AssetId id, Record& r) {
    SR_ZONE("assets: animation");
    (void)id;
    const auto file = files_.Read(r.path);
    if (!file) return Fail(r, "file not found");
    decodedBytes_ += file->Size();
    auto animation = std::make_shared<assets::AnimationData>();
    std::string error;
    if (!assets::DecodeBan(file->Bytes(), *animation, error)) return Fail(r, error);
    {
        std::lock_guard lock(payloadMutex_);
        r.animation = std::move(animation);
    }
    r.loadMs = std::chrono::duration<double, std::milli>(Clock::now() - r.requested).count();
    r.state.store(AssetState::Resident, std::memory_order_release);
}

void AssetStore::LoadRegion(AssetId id, Record& r) {
    SR_ZONE("assets: region");
    std::shared_ptr<const assets::AssetIndex> tiles;
    {
        std::lock_guard lock(requestMutex_);
        tiles = tileIndex_;
    }
    if (!tiles) return Fail(r, "no tile2d.ifo (SetTileIndex)");
    const auto file = files_.Read(r.path + ".m");
    if (!file) return Fail(r, "no terrain (.m)");
    decodedBytes_ += file->Size();
    auto region = std::make_unique<assets::TerrainRegion>();
    std::string error;
    if (!assets::DecodeMapMesh(file->Bytes(), *region, error)) return Fail(r, error);
    auto pending = std::make_unique<Pending>();
    if (!BuildTerrain(*region, pending->terrain, error)) return Fail(r, error);

    auto info = std::make_shared<TerrainInfo>();
    int y = 0, x = 0;
    if (std::sscanf(r.path.c_str(), "%d\\%d", &y, &x) == 2) info->regionId = assets::RegionId(x, y);
    info->blockHeights = pending->terrain.blockHeights;
    info->heights = pending->terrain.heights;
    for (std::size_t b = 0; b < region->blocks.size(); ++b) {
        const assets::TerrainBlock& block = region->blocks[b];
        info->environments[b] = block.environmentId;
        // The file's coverage byte: for water it equals what the original recomputes from the
        // heights (every block of the game), for ice it is what the original uses.
        info->water[b] = {block.waterType, block.waterCoverage, block.waterHeight};
    }
    info->crowdedTiles = pending->terrain.crowdedTiles;
    if (const auto objects = files_.Read(r.path + ".o2")) {
        decodedBytes_ += objects->Size();
        if (!assets::DecodeMapObjects(objects->Bytes(), info->placements, error)) info->placements.clear();
    }
    // Its textures load as assets of their own (shared with every region using them).
    for (const TerrainTexture& t : pending->terrain.palette) {
        const auto entry = tiles->find(t.id);
        const AssetId texture = entry != tiles->end() && !entry->second.path.empty() ? RequestTexture(entry->second.path, r.priority) : AssetId{};
        pending->palette.push_back(texture);
        if (texture) r.dependencies.push_back(texture);
    }
    if (files_.Exists(r.path + ".t")) {
        pending->lightmap = RequestTexture(r.path + ".t", r.priority);
        if (pending->lightmap) r.dependencies.push_back(pending->lightmap);
    }
    r.lightmap = pending->lightmap;
    pending->terrainInfo = std::move(info);
    r.pending = std::move(pending);
    Stage(id, r);
}

bool AssetStore::Stage(AssetId id, Record& r) {
    SR_ZONE("assets: stage");
    Pending& p = *r.pending;
    const auto wait = [&] {
        std::lock_guard lock(listMutex_);
        r.state.store(AssetState::Waiting, std::memory_order_release);
        waiting_.push_back(id);
        return false;
    };

    if (r.kind == AssetKind::Region) {
        // The palette needs its textures' bindless indices: they exist once each texture's job
        // has created it (a failed one draws white).
        for (AssetId texture : p.palette) {
            const Record* t = texture ? Get(texture) : nullptr;
            if (t == nullptr) continue;
            const AssetState s = t->state.load(std::memory_order_acquire);
            if (s == AssetState::Queued || s == AssetState::Loading) return wait();
        }
        if (p.ranges.empty()) {
            const auto range = pool_.Allocate(kTerrainBytes);
            if (!range) {
                geometryShort_ = true;
                return wait();
            }
            p.ranges.push_back(*range);
        }
        const rhi::StagingBlock block = device_.AllocateStaging(kTerrainBytes);
        if (!block) return wait();
        std::memset(block.cpu, 0, kTerrainSampleOffset);
        for (std::size_t i = 0; i < p.palette.size(); ++i) {
            const Record* t = p.palette[i] ? Get(p.palette[i]) : nullptr;
            const std::uint32_t entry[2] = {t != nullptr ? t->texture.bindless : 0u, p.terrain.palette[i].scale};
            std::memcpy(block.cpu + i * 8, entry, 8);
        }
        std::memcpy(block.cpu + kTerrainSampleOffset, p.terrain.samplesAndTiles.data(), p.terrain.samplesAndTiles.size());
        const rhi::StagedBufferCopy copy{0, kTerrainBytes, pool_.Buffer(), p.ranges[0]};
        r.upload = device_.SubmitStaging(block, std::span(&copy, 1), {});
        r.ranges = std::move(p.ranges);
        r.bytes = Align(kTerrainBytes, GeometryPool::kGranularity);
        p.terrainInfo->offset = r.ranges[0];
        std::lock_guard lock(payloadMutex_);
        r.buildingTerrain = std::move(p.terrainInfo);
    } else if (r.kind == AssetKind::Texture) {
        const assets::TextureData& texture = p.texture;
        const rhi::StagingBlock block = device_.AllocateStaging(r.texture.bytes);
        if (!block) return wait();
        std::size_t offset = 0;
        for (std::size_t m = 0; m < texture.mips.size(); ++m) {
            const auto bytes = texture.MipBytes(m);
            std::memcpy(block.cpu + offset, bytes.data(), bytes.size());
            offset += bytes.size();
        }
        const rhi::StagedTextureCopy copy{0, r.texture.handle, 0, r.texture.mips};
        r.upload = device_.SubmitStaging(block, {}, std::span(&copy, 1));
        r.bytes = r.texture.bytes;
    } else {
        ModelInfo& info = *p.info;
        const std::size_t parts = info.parts.size();
        // Geometry ranges first (kept across retries once taken).
        if (p.ranges.empty()) {
            for (const ModelPartInfo& part : info.parts) {
                const auto vertices = pool_.Allocate(std::uint64_t{part.vertexCount} * VertexStride(part.vertexFlags));
                const auto indices = vertices ? pool_.Allocate(std::uint64_t{part.indexCount} * 2) : std::nullopt;
                if (!vertices || !indices) {
                    if (vertices) p.ranges.push_back(*vertices);
                    for (std::uint64_t offset : p.ranges) pool_.Free(offset);
                    p.ranges.clear();
                    geometryShort_ = true;
                    return wait();
                }
                p.ranges.push_back(*vertices);
                p.ranges.push_back(*indices);
            }
        }
        std::uint64_t total = 0;
        for (const ModelPartInfo& part : info.parts) {
            total += Align(std::uint64_t{part.vertexCount} * VertexStride(part.vertexFlags), 16);
            total += Align(std::uint64_t{part.indexCount} * 2, 16);
        }
        const rhi::StagingBlock block = device_.AllocateStaging(total);
        if (!block) return wait();
        std::vector<rhi::StagedBufferCopy> copies;
        copies.reserve(parts * 2);
        std::size_t offset = 0;
        std::uint64_t bytes = 0;
        for (std::size_t i = 0; i < parts; ++i) {
            ModelPartInfo& part = info.parts[i];
            const assets::MeshData& mesh = p.model.parts[i].mesh;
            const std::uint32_t stride = VertexStride(part.vertexFlags);
            std::byte* out = block.cpu + offset;
            for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
                std::byte* vertex = out + v * stride;
                std::memcpy(vertex, &mesh.vertices[v], 32);
                std::size_t at = 32;
                if (part.vertexFlags & kVertexLightmap) {
                    std::memcpy(vertex + at, mesh.lightmapUvs[v].data(), 8);
                    at += 8;
                }
                if (part.vertexFlags & kVertexSkinned) {
                    const auto& remap = p.boneRemap[i];
                    const auto& local = mesh.boneIndices[v];
                    const std::uint32_t b0 = local[0] < remap.size() ? remap[local[0]] : 0;
                    const std::uint32_t b1 = local[1] < remap.size() ? remap[local[1]] : 0;
                    const std::uint32_t bones = b0 | (b1 << 8);
                    std::memcpy(vertex + at, &bones, 4);
                    std::memcpy(vertex + at + 4, mesh.boneWeights[v].data(), 8);
                }
            }
            const std::uint64_t vertexBytes = std::uint64_t{part.vertexCount} * stride;
            part.vertexOffset = p.ranges[i * 2];
            copies.push_back({offset, vertexBytes, pool_.Buffer(), part.vertexOffset});
            offset += Align(vertexBytes, 16);
            const std::uint64_t indexBytes = std::uint64_t{part.indexCount} * 2;
            std::memcpy(block.cpu + offset, mesh.indices.data(), indexBytes);
            part.indexOffset = p.ranges[i * 2 + 1];
            copies.push_back({offset, indexBytes, pool_.Buffer(), part.indexOffset});
            offset += Align(indexBytes, 16);
            bytes += Align(vertexBytes, GeometryPool::kGranularity) + Align(indexBytes, GeometryPool::kGranularity);
        }
        info.geometryBytes = bytes;
        r.upload = device_.SubmitStaging(block, copies, {});
        r.ranges = std::move(p.ranges);
        r.bytes = bytes;
        std::lock_guard lock(payloadMutex_);
        r.building = std::move(p.info);
    }
    residentBytes_ += r.bytes;
    if (r.kind == AssetKind::Texture) textureBytes_ += r.bytes;
    std::lock_guard lock(listMutex_);
    r.pending.reset();
    r.state.store(AssetState::Uploading, std::memory_order_release);
    uploading_.push_back(id);
    return true;
}

// ---------------------------------------------------------------- the render thread

void AssetStore::Promote() {
    const auto start = Clock::now();
    {
        // New arrivals join the render thread's own list; if a decode job holds the lock right
        // now they join next frame instead (the render thread never waits for the jobs).
        std::unique_lock lock(listMutex_, std::try_to_lock);
        if (lock.owns_lock()) {
            promoting_.insert(promoting_.end(), uploading_.begin(), uploading_.end());
            uploading_.clear();
        }
    }
    std::vector<Record*> resident, models, regions;
    std::size_t kept = 0, i = 0;
    for (; i < promoting_.size(); ++i) {
        // A time budget (the rest waits for the next frame): during a flood of loads the list is
        // thousands long.
        if (i % 64 == 63 && Clock::now() - start > kUpdateStepBudget) break;
        const AssetId id = promoting_[i];
        Record* r = Get(id);
        if (r == nullptr || r->state.load(std::memory_order_acquire) != AssetState::Uploading) continue;
        bool ready = device_.IsUploaded(r->upload);
        if (ready && (r->kind == AssetKind::Model || r->kind == AssetKind::Region)) {
            // Whole or not at all: a model or a region shows once its textures are there (or failed).
            for (AssetId dep : r->dependencies) {
                const AssetState s = State(dep);
                if (s != AssetState::Resident && s != AssetState::Failed) ready = false;
            }
        }
        if (!ready) {
            promoting_[kept++] = id;
            continue;
        }
        if (r->kind == AssetKind::Region) regions.push_back(r);
        if (r->kind == AssetKind::Model) {
            std::vector<MeshRecord> records;
            records.reserve(r->building->parts.size());
            for (ModelPartInfo& part : r->building->parts) {
                const auto bindless = [&](AssetId texture) -> std::uint32_t {
                    const Record* t = texture ? Get(texture) : nullptr;
                    return t != nullptr && t->state.load(std::memory_order_acquire) == AssetState::Resident ? t->texture.bindless : 0;
                };
                part.textureBindless = bindless(part.texture);
                part.lightmapBindless = bindless(part.lightmap);
                part.glowBindless = bindless(part.glowTexture);
                // A bare .bms is an effect's mesh: the effect textures it, not its material.
                const bool resource = r->path.find('|') != std::string::npos || r->path.find(".bsr") != std::string::npos ||
                                      r->path.find(".cpd") != std::string::npos;
                if (resource && part.textureBindless == 0 && (part.materialFlags & kMaterialBlended) == 0) {
                    // Drawn grey: say why (the first few; the count at the end of the session).
                    const std::size_t n = untextured_++;
                    if (n < 40) {
                        const Record* t = part.texture ? Get(part.texture) : nullptr;
                        const std::string why = !part.texture ? "no texture in its material"
                                                : t != nullptr ? t->path + ": " + t->error
                                                               : "its texture's id is stale";
                        std::string warnings;
                        for (const std::string& w : r->building->warnings) warnings += (warnings.empty() ? "" : "; ") + w;
                        core::LogWarning("assets: {}: part {} untextured ({}){}{}", r->path, part.meshPath, why, warnings.empty() ? "" : " - ", warnings);
                    }
                }
                MeshRecord m;
                m.firstIndex = static_cast<std::uint32_t>(part.indexOffset / 2);
                m.indexCount = part.indexCount;
                m.vertexOffset = static_cast<std::uint32_t>(part.vertexOffset);
                m.vertexFlags = part.vertexFlags;
                m.texture = part.textureBindless;
                m.materialFlags = part.materialFlags;
                m.bucket = (part.materialFlags & kMaterialBlended) != 0    ? kBucketBlended
                           : (part.materialFlags & kMaterialTwoSided) != 0 ? kBucketTwoSided
                                                                           : kBucketCulled;
                m.vertexCount = part.vertexCount;
                m.lightmap = part.lightmapBindless;
                if ((part.materialFlags & kMaterialEquipGlow) != 0) {
                    m.lightmap = part.glowBindless;
                    std::copy(part.glowWords.begin(), part.glowWords.end(), m.uvRate);
                }
                if ((part.materialFlags & kMaterialScrolls) != 0) {
                    const auto& u = part.uvRate;
                    m.uvRate[0] = glm::packHalf2x16(glm::vec2(u[4], u[5]));
                    m.uvRate[1] = glm::packHalf2x16(glm::vec2(u[0], u[1]));
                    m.uvRate[2] = glm::packHalf2x16(glm::vec2(u[2], u[3]));
                }
                std::copy(part.sphere.begin(), part.sphere.end(), m.sphere);
                records.push_back(m);
            }
            const auto base = meshes_.Add(records);
            if (!base) {
                // The mesh table is full: the model waits (and says so once).
                if (!r->tableFullLogged) core::LogWarning("assets: the mesh table is full ({} records)", meshes_.Capacity());
                r->tableFullLogged = true;
                promoting_[kept++] = id;
                continue;
            }
            r->building->meshBase = *base;
            r->building->meshCount = static_cast<std::uint32_t>(records.size());
            models.push_back(r);
        }
        resident.push_back(r);
    }
    // Those not looked at this frame go first next frame, then those not ready yet: a round over
    // the whole list. (Kept in front, the waiting models alone could use up every frame's budget,
    // and the textures they wait for, further down, would never be looked at.)
    std::vector<AssetId> next;
    next.reserve(kept + (promoting_.size() - i));
    next.insert(next.end(), promoting_.begin() + static_cast<std::ptrdiff_t>(i), promoting_.end());
    next.insert(next.end(), promoting_.begin(), promoting_.begin() + static_cast<std::ptrdiff_t>(kept));
    promoting_.swap(next);
    if (!models.empty() || !regions.empty()) {
        // One lock for every model and region published this frame.
        std::lock_guard lock(payloadMutex_);
        for (Record* r : models) r->model = std::move(r->building);
        for (Record* r : regions) {
            const Record* lightmap = r->lightmap ? Get(r->lightmap) : nullptr;
            r->buildingTerrain->lightmap =
                lightmap != nullptr && lightmap->state.load(std::memory_order_acquire) == AssetState::Resident ? lightmap->texture.bindless : 0;
            r->terrain = std::move(r->buildingTerrain);
        }
    }
    const auto now = Clock::now();
    for (Record* r : resident) {
        r->loadMs = std::chrono::duration<double, std::milli>(now - r->requested).count();
        r->state.store(AssetState::Resident, std::memory_order_release);
    }
}

void AssetStore::Evict(AssetId id, Record& r) {
    if (r.texture.handle) device_.DestroyTexture(r.texture.handle);
    for (std::uint64_t offset : r.ranges) pool_.Free(offset);
    if (r.model && r.model->meshCount > 0) meshes_.Free(r.model->meshBase);
    residentBytes_ -= r.bytes;
    if (r.kind == AssetKind::Texture) textureBytes_ -= r.bytes;
    std::vector<AssetId> deps;
    {
        std::lock_guard lock(payloadMutex_);
        deps.swap(r.dependencies);
        r.texture = {};
        r.ranges.clear();
        r.bytes = 0;
        r.model.reset();
        r.building.reset();
        r.animation.reset();
        r.terrain.reset();
        r.buildingTerrain.reset();
        r.state.store(AssetState::Failed, std::memory_order_release);
        r.error = "evicted";
        r.generation.store(0, std::memory_order_release); // every id of it is stale from now on
        table_.Remove(id);
    }
    for (AssetId dep : deps) table_.Release(dep); // its textures may be evicted later
    ++evicted_;
}

void AssetStore::EvictOverBudget() {
    const auto start = Clock::now();
    std::size_t skip = 0;
    for (int evictions = 0; evictions < 64; ) {
        if (evictions > 0 && Clock::now() - start > kUpdateStepBudget) break; // more next frame
        const bool overBudget = textureBytes_.load() > desc_.textureBudgetBytes;
        const bool needGeometry = geometryShort_.load();
        if (!overBudget && !needGeometry) break;
        const AssetId candidate = table_.OldestUnreferenced(skip);
        if (!candidate) break;
        Record* r = Get(candidate);
        const AssetState s = r != nullptr ? r->state.load(std::memory_order_acquire) : AssetState::Queued;
        // Only finished assets can go; for geometry only models help.
        if (r == nullptr || (s != AssetState::Resident && s != AssetState::Failed) ||
            (!overBudget && r->kind != AssetKind::Model)) {
            ++skip;
            continue;
        }
        Evict(candidate, *r);
        ++evictions;
    }
    geometryShort_ = false; // the waiting assets try again; still short, they say so again
}

void AssetStore::Update() {
    SR_ZONE("assets: update");
    if (shutDown_) return;
    pool_.Collect();
    meshes_.Collect();
    Promote();
    EvictOverBudget();

    // Decoded assets that found no staging or geometry memory try again (next frame if a decode
    // job holds the list right now).
    std::vector<AssetId> retry;
    {
        std::unique_lock lock(listMutex_, std::try_to_lock);
        if (lock.owns_lock()) retry.swap(waiting_);
    }
    for (AssetId id : retry) {
        Record* r = Get(id);
        if (r == nullptr) continue;
        AssetState expected = AssetState::Waiting;
        if (!r->state.compare_exchange_strong(expected, AssetState::Loading)) continue;
        ++running_;
        jobs_.Run([this, id] {
            if (Record* rec = Get(id)) Stage(id, *rec);
            --running_;
        }, core::JobPriority::Streaming, &jobsDone_);
    }

    // New work, most urgent first, up to maxJobs_ jobs at once. A job that finishes takes the next
    // asset itself (StreamFrom): started only here, the jobs were held to a few assets a frame, and
    // a flood (entering the world: over a thousand) waited on the frame rate with the workers idle.
    const unsigned running = running_.load();
    std::vector<AssetId> start;
    if (running < maxJobs_) {
        std::lock_guard lock(requestMutex_);
        for (AssetId id; start.size() < maxJobs_ - running && (id = TakeQueuedLocked());) start.push_back(id);
    }
    for (AssetId id : start) {
        ++running_;
        jobs_.Run([this, id] { StreamFrom(id); }, core::JobPriority::Streaming, &jobsDone_);
    }
}

AssetId AssetStore::TakeQueuedLocked() {
    while (!queue_.empty()) {
        const AssetId id = queue_.top().id;
        queue_.pop();
        Record* r = Get(id);
        AssetState expected = AssetState::Queued;
        // Skipped: an entry queued again in front (more urgent) and taken already, or a stale id.
        if (r != nullptr && r->state.compare_exchange_strong(expected, AssetState::Loading)) return id;
    }
    return {};
}

void AssetStore::StreamFrom(AssetId id) {
    // One of the running_ jobs: loads `id`, then the queue's next most urgent while there is one.
    while (id) {
        Load(id);
        if (shutDown_.load(std::memory_order_acquire)) break;
        std::lock_guard lock(requestMutex_);
        id = TakeQueuedLocked();
    }
    --running_;
}

AssetStore::Stats AssetStore::GetStats() const {
    Stats s;
    s.live = table_.Live();
    s.unreferenced = table_.Unreferenced();
    const std::uint32_t slots = table_.SlotCount();
    for (std::uint32_t i = 1; i < slots; ++i) {
        const Record* records = blocks_[i / kBlockSize].load(std::memory_order_acquire);
        if (records == nullptr) continue;
        const Record& r = records[i % kBlockSize];
        if (r.generation.load(std::memory_order_acquire) == 0) continue; // evicted or never used
        switch (r.state.load(std::memory_order_acquire)) {
        case AssetState::Queued: ++s.queued; break;
        case AssetState::Loading: ++s.loading; break;
        case AssetState::Waiting: ++s.waiting; break;
        case AssetState::Uploading: ++s.uploading; break;
        case AssetState::Resident: ++s.resident; break;
        case AssetState::Failed: ++s.failed; break;
        }
    }
    s.residentBytes = residentBytes_.load();
    s.textureBytes = textureBytes_.load();
    s.textureBudgetBytes = desc_.textureBudgetBytes;
    s.evicted = evicted_;
    s.untextured = untextured_.load();
    s.decodedBytes = decodedBytes_.load();
    s.geometry = pool_.GetStats();
    return s;
}

} // namespace sr::gfx
