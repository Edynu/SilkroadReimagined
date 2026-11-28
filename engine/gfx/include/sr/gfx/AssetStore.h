#pragma once

// Streams models, textures and animations from the file system to the GPU:
// a request returns an id at once; a Streaming job reads and decodes the file, writes GPU-ready
// bytes straight into the device's staging ring and submits the copies; the render thread's
// Update() makes the asset Resident once the transfer is done. Ids are reference counted; an
// unreferenced asset stays loaded in a keep list until memory runs short.
//
// Threads: Request*, AddRef, Release, State and the accessors from any thread. Update() on the
// render thread, once a frame after Device::BeginFrame. Resident data does not change until the
// asset is evicted, which only Update() does, and only for unreferenced assets.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

#include "sr/assets/Animation.h"
#include "sr/assets/Model.h"
#include "sr/assets/Texture.h"
#include "sr/core/AssetTable.h"
#include "sr/core/Jobs.h"
#include "sr/gfx/GeometryPool.h"
#include "sr/gfx/MeshTable.h"
#include "sr/gfx/Terrain.h"
#include "sr/rhi/Rhi.h"

namespace sr::assets {
class FileSystem;
}

namespace sr::gfx {

using AssetId = core::AssetHandle;

enum class AssetKind : std::uint8_t { Texture, Model, Animation, Region };
enum class AssetState : std::uint8_t {
    Queued,    // waiting for a decode job
    Loading,   // a job reads and decodes it
    Waiting,   // decoded; waiting for staging or geometry memory (tried again each frame)
    Uploading, // copies submitted (a model also waits here for its textures)
    Resident,  // usable by frames that begin from now on
    Failed,    // see Error()
};
const char* ToString(AssetState state);

// Vertex layouts in the geometry pool, read by address: position (float3),
// normal (float3), uv (float2) = 32 bytes; then a lightmap uv (float2) with kVertexLightmap; then
// two bone indices (bytes 0 and 1 of a uint, skeleton bone numbers) and two weights (float2) with
// kVertexSkinned.
constexpr std::uint32_t kVertexLightmap = 1;
constexpr std::uint32_t kVertexSkinned = 2;
std::uint32_t VertexStride(std::uint32_t vertexFlags);

// Material bits the model pass reads.
constexpr std::uint32_t kMaterialAlphaTest = 1;
// .bmt flag 8: unlit at night (the buildings' window lights, full bright after dark).
constexpr std::uint32_t kMaterialNightLight = 2;
// .bmt flag 1: two-sided, drawn without culling (the material state sets).
constexpr std::uint32_t kMaterialTwoSided = 4;
// Ours: a plant's leaves (a cut-out or blended part of a mesh under prim\mesh\nature\: not its trunk), which the Modern
// foliage sway moves.
constexpr std::uint32_t kMaterialFoliage = 8;
// The resource's always-on modifiers (assets::AmbientLook, "Object materials and texture
// animation"): translucent, drawn blended after the solid parts (the blend in bits 8-15, the tint
// in 16-23), alpha-tested if bit 1 is set too.
constexpr std::uint32_t kMaterialBlended = 16;
// Its texture scrolls (the rates in the mesh record's last three words, MeshTable.h).
constexpr std::uint32_t kMaterialScrolls = 32;
// Ours: a water surface on an object (a fountain's pool: scrolling, solid, its material or texture
// named for water), which the Modern water row shades like water.
constexpr std::uint32_t kMaterialWater = 64;
// With kMaterialAlphaTest: cut out only where the alpha is 0 (an environment map's test, ALPHAREF 1,
// assets::AmbientLook::envAlphaZero), not below a half.
constexpr std::uint32_t kMaterialAlphaZero = 128;
// A worn weapon's or shield's plus glow (assets::EquipGlow): the option texture on its
// coordinates moved over time, times a colour between two ends, added to the part's texture. Its mesh record holds
// the glow's texture in place of a lightmap and its words in place of the scroll rates (MeshTable.h).
constexpr std::uint32_t kMaterialEquipGlow = 1u << 24;
// Ours: a worn piece of cover mode 2 (assets::ModelPart::overSkin), laid over the skin it does not hide: its vertices are
// pushed out along their normals by kOverSkinLift, so a coarse sleeve's flat faces stay over the rounder arm under them
// (a garment's hands, 2026-11-22: the forearm showed through in streaks).
constexpr std::uint32_t kMaterialOverSkin = 1u << 25;
constexpr float kOverSkinLift = 0.06f; // model units (a character stands about 18 tall); the shaders' copy
// The Metallic Sheen (assets::AmbientLook::envMap: a model's environment-map modifier, 1.188's CRTModEnvMap): the part's
// alpha masks a sphere map added onto its texture; with kMaterialEnvHighlight, spheremap_highlight, else spheremap_gray.
constexpr std::uint32_t kMaterialEnvMap = 1u << 26;
constexpr std::uint32_t kMaterialEnvHighlight = 1u << 27;
// Blended parts: bits 8-11 the source factor, 12-15 the destination factor (D3DBLEND numbers),
// 16-23 the tint x 100.
constexpr std::uint32_t kMaterialBlendShift = 8;
constexpr std::uint32_t kMaterialTintShift = 16;

struct TextureInfo {
    rhi::TextureHandle handle;
    std::uint32_t bindless = 0; // index in the bindless table (0 = white)
    std::uint32_t width = 0, height = 0, mips = 0;
    assets::TextureFormat format = assets::TextureFormat::RGBA8;
    std::string sourceFormat; // "DXT1", "A1R5G5B5", ... (tools)
    bool premultipliedAlpha = false;
    std::uint64_t bytes = 0;
};

struct ModelPartInfo {
    std::string meshPath;
    std::uint64_t vertexOffset = 0; // bytes into the geometry pool
    std::uint64_t indexOffset = 0;  // bytes into the geometry pool (16-bit indices)
    std::uint32_t vertexCount = 0, indexCount = 0;
    std::uint32_t vertexFlags = 0;  // kVertex*
    std::uint32_t materialFlags = 0; // kMaterial*
    AssetId texture;                // the model holds a reference; null if untextured
    std::uint32_t textureBindless = 0; // filled when the model becomes Resident
    AssetId lightmap;               // lightmapped meshes (kVertexLightmap): the mesh's lightmap
    std::uint32_t lightmapBindless = 0;
    std::array<float, 4> diffuse{1, 1, 1, 1};
    std::array<float, 4> sphere{}; // bounding sphere in model space: centre, radius
    // kMaterialScrolls: the texture matrix's rates per second (m11, m12, m21, m22, offset u, v).
    std::array<float, 6> uvRate{};
    // kMaterialEquipGlow: the option texture and the record's three words (MeshTable.h).
    AssetId glowTexture;
    std::uint32_t glowBindless = 0;
    std::array<std::uint32_t, 3> glowWords{};
};

struct ModelInfo {
    std::vector<ModelPartInfo> parts;
    // The parts' records in the mesh table: what instances draw.
    std::uint32_t meshBase = 0, meshCount = 0;
    assets::Bounds bounds;
    // Ours: the box of its solid parts alone (neither cut out nor blended: a tree's trunk, not its leaves), what the follow
    // camera is kept out of; min above max when it has none.
    assets::Bounds solidBounds{{1.0f, 1.0f, 1.0f}, {-1.0f, -1.0f, -1.0f}};
    std::shared_ptr<const assets::SkeletonData> skeleton; // null for static objects
    // A worn item's own bones the wearer lacks (assets::ModelData::Socket): where they stand
    // relative to their parent bone's posed transform (column-major 4 x 4: the parent's posed
    // world times this is the socket's), so they move with the item as it is drawn.
    struct Socket {
        std::string name;
        std::string item; // normalized path of the item's .bsr
        int parent = -1;  // skeleton bone
        std::array<float, 16> local{};
    };
    std::vector<Socket> sockets;
    std::vector<std::string> animationPaths;
    std::vector<assets::ResourceData::AnimationSet> animationSets;
    std::vector<assets::ResourceData::ModifierSet> soundSets; // its modifier sets that have clip sounds (their sounds alone)
    // Effects the model always wears (its resource's ambient particles: Yeoha's green glows on its
    // hands and the ball between them), each on a bone of its own.
    std::vector<assets::ResourceData::ParticleModifier> ambientParticles;
    std::vector<std::string> warnings;
    std::uint64_t geometryBytes = 0;
    std::uint32_t triangles = 0;
};

struct AssetStoreDesc {
    std::size_t geometryBytes = 512ull << 20; // the reserved geometry pool
    // Texture memory the assets may hold before unreferenced ones are evicted: the device's
    // reserved texture memory, so streaming never allocates more (geometry is bounded by its pool).
    std::uint64_t textureBudgetBytes = 1ull << 30;
    // Decode jobs at once (0: half the workers, at least 2).
    unsigned maxJobs = 0;
    // Assets alive at once without growing the tables (the whole game has about 34,000 models
    // and textures).
    std::size_t expectedAssets = 65536;
    // Mesh records of resident models (all of the game's models have about 40,000 parts).
    std::uint32_t meshRecords = 1u << 17;
};

class AssetStore {
public:
    AssetStore(rhi::Device& device, const assets::FileSystem& files, core::JobSystem& jobs);
    ~AssetStore();
    AssetStore(const AssetStore&) = delete;
    AssetStore& operator=(const AssetStore&) = delete;

    bool Init(const AssetStoreDesc& desc, std::string& error);
    // Waits for running jobs and frees everything (the device defers the GPU side).
    void Shutdown();

    // An id with one reference for the caller (Release it). A smaller priority loads first.
    // A .bsr / .cpd / .bms, or an outfit: "body.bsr|item.bsr|..." (the items merged onto the body's
    // skeleton, assets::AssembleOutfit), or "model.bsr#setN" (in its material set N: a champion's).
    AssetId RequestModel(std::string_view path, int priority = 0);
    AssetId RequestTexture(std::string_view path, int priority = 0);
    AssetId RequestAnimation(std::string_view path, int priority = 0);
    // A region's terrain and objects (C3c): Map.pk2 <y>\<x>.m, .o2 and the .t
    // lightmap. Needs the tile index first (SetTileIndex).
    AssetId RequestRegion(int x, int y, int priority = 0);
    // tile2d.ifo (id -> texture path), set once before regions are requested.
    void SetTileIndex(std::shared_ptr<const assets::AssetIndex> index);
    void AddRef(AssetId id);
    void Release(AssetId id);

    // Failed for a stale id.
    AssetState State(AssetId id) const;
    bool IsResident(AssetId id) const { return State(id) == AssetState::Resident; }
    std::string Error(AssetId id) const;
    std::string Path(AssetId id) const;
    // Resident data (empty otherwise).
    std::shared_ptr<const ModelInfo> Model(AssetId id) const;
    std::optional<TextureInfo> Texture(AssetId id) const;
    std::shared_ptr<const assets::AnimationData> Animation(AssetId id) const;
    std::shared_ptr<const TerrainInfo> Terrain(AssetId id) const;
    // Milliseconds from the request to Resident (or Failed); -1 while loading.
    double LoadMs(AssetId id) const;

    // Render thread, once a frame after BeginFrame.
    void Update();

    GeometryPool& Geometry() { return pool_; }
    // Render thread: the parts of resident models, and the pass that copies
    // the records written since the last frame (outside render passes, before they are read).
    const MeshTable& Meshes() const { return meshes_; }
    void UploadMeshes(rhi::CommandList& cmd) { meshes_.Upload(cmd); }
    // The world's textures load without their `skip` largest mips (the original's texture detail: 2 - its row's value, so
    // high none, low two; the interface and the lightmaps whole). Textures loaded from then on.
    void SetMipSkip(int skip) { mipSkip_.store(std::clamp(skip, 0, 2), std::memory_order_relaxed); }

    struct Stats {
        std::size_t live = 0, unreferenced = 0;
        std::size_t queued = 0, loading = 0, waiting = 0, uploading = 0, resident = 0, failed = 0;
        std::uint64_t residentBytes = 0;                     // geometry and textures
        std::uint64_t textureBytes = 0, textureBudgetBytes = 0;
        std::uint64_t evicted = 0;         // assets evicted so far
        std::size_t untextured = 0;        // opaque parts of resources (.bsr, outfits) made resident without a texture (grey)
        std::uint64_t decodedBytes = 0;    // file bytes decoded so far
        GeometryPool::Stats geometry;
    };
    // Walks every asset: for tools, not every frame.
    Stats GetStats() const;
    // GPU memory the assets hold now (cheap).
    std::uint64_t ResidentBytes() const { return residentBytes_.load(std::memory_order_relaxed); }

private:
    struct Pending;
    struct Record;
    AssetId Request(AssetKind kind, std::string_view path, int priority);
    Record* Get(AssetId id) const;     // null for stale ids
    Record& Slot(std::uint32_t index); // creates the block of records if needed
    void Load(AssetId id);             // a Streaming job
    void LoadTexture(AssetId id, Record& r);
    void LoadModel(AssetId id, Record& r);
    void LoadAnimation(AssetId id, Record& r);
    void LoadRegion(AssetId id, Record& r);
    // Writes a decoded asset into staging and submits it; false (Waiting) if there is no room.
    bool Stage(AssetId id, Record& r);
    void Fail(Record& r, std::string error);
    void Promote();
    // The most urgent queued asset, marked Loading (null when none); requestMutex_ held.
    AssetId TakeQueuedLocked();
    // A decode job: loads `id`, then takes the next queued asset itself until none is left.
    void StreamFrom(AssetId id);
    void Evict(AssetId id, Record& r);
    void EvictOverBudget();

    rhi::Device& device_;
    const assets::FileSystem& files_;
    core::JobSystem& jobs_;
    AssetStoreDesc desc_;
    GeometryPool pool_;
    MeshTable meshes_;
    core::AssetTable table_;

    static constexpr std::uint32_t kBlockSize = 1024;
    static constexpr std::uint32_t kMaxBlocks = 1024;
    std::array<std::atomic<Record*>, kMaxBlocks> blocks_{};

    // Three locks, so the render thread's Update rarely meets the decode jobs: requestMutex_
    // guards requests and the queue, listMutex_ the waiting and uploading lists, payloadMutex_ the
    // published data (models, animations, errors) that any thread may read.
    mutable std::mutex requestMutex_, listMutex_, payloadMutex_;
    struct QueueEntry {
        int priority = 0;
        std::uint64_t sequence = 0;
        AssetId id;
        bool operator<(const QueueEntry& o) const {
            return priority != o.priority ? priority > o.priority : sequence > o.sequence; // min-heap
        }
    };
    std::priority_queue<QueueEntry> queue_;
    std::uint64_t sequence_ = 0;
    std::vector<AssetId> waiting_, uploading_;
    std::vector<AssetId> promoting_; // the render thread's: uploads it watches until Resident
    std::atomic<unsigned> running_{0};
    unsigned maxJobs_ = 2;
    core::JobCounter jobsDone_;
    std::atomic<std::uint64_t> residentBytes_{0}, textureBytes_{0};
    std::atomic<std::uint64_t> decodedBytes_{0};
    std::atomic<int> mipSkip_{0};
    std::atomic<bool> geometryShort_{false}; // a job found the pool full: evict before retrying
    std::shared_ptr<const assets::AssetIndex> tileIndex_; // set before regions are requested
    std::uint64_t evicted_ = 0;
    std::atomic<std::size_t> untextured_{0}; // opaque parts promoted without a texture (drawn grey)
    std::atomic<std::size_t> failures_{0};   // the first few are logged
    std::atomic<bool> shutDown_{false}; // read by the decode jobs (they stop taking work)
};

} // namespace sr::gfx
