#pragma once

// The frame as a graph of passes. Each frame the render thread declares
// passes with the resources they read and write; Compile() keeps the declaration order, drops
// passes whose results nothing needs, and derives the barriers between passes from the declared
// uses; Run() records them. v1 places buffer barriers; render-target layouts are still changed
// by the RHI's passes themselves (a pass's targets are ready to sample after it).
//
// Transient textures come from a pool that keeps them across frames (matched by description),
// so after the first frames nothing is allocated; entries unused for a while are released.
// Render thread only.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sr/rhi/Rhi.h"

namespace sr::gfx {

enum class PassKind : std::uint8_t { Graphics, Compute, Transfer };

// How a pass uses a resource. Buffer uses map to rhi::BufferAccess; texture uses only order and
// keep passes in v1.
enum class Use : std::uint8_t {
    IndirectRead,
    IndexRead,
    VertexRead,
    FragmentRead,
    ComputeRead,
    ComputeWrite,
    TransferWrite,
    TransferRead,
    ColorWrite,
    DepthWrite,
    Sampled,
    External, // the work before the graph (earlier frames, uploads): an imported buffer's start
};
bool IsWrite(Use use);
rhi::BufferAccess ToBufferAccess(Use use);

class RenderGraph {
public:
    using Resource = std::uint32_t; // 0: none

    struct TextureDesc {
        std::uint32_t width = 0, height = 0;
        rhi::Format format = rhi::Format::RGBA8Unorm;
        rhi::TextureUsage usage = rhi::TextureUsage::RenderTarget;
        std::uint32_t samples = 1;
        friend bool operator==(const TextureDesc&, const TextureDesc&) = default;
    };

    class Builder {
    public:
        Builder& Read(Resource resource, Use use);
        Builder& Write(Resource resource, Use use);
        // The pass matters by itself (it writes something outside the graph, e.g. the swapchain).
        Builder& SideEffect();

    private:
        friend class RenderGraph;
        Builder(RenderGraph& graph, std::size_t pass) : graph_(graph), pass_(pass) {}
        RenderGraph& graph_;
        std::size_t pass_;
    };
    using Execute = std::function<void(rhi::CommandList&, const RenderGraph&)>;

    explicit RenderGraph(rhi::Device* device = nullptr) : device_(device) {}
    ~RenderGraph();
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // Starts a new frame's graph (pooled transients stay).
    void Reset();
    // Resources that outlive the frame: writing one keeps the writer.
    Resource ImportBuffer(rhi::BufferHandle buffer, const char* name);
    Resource ImportTexture(rhi::TextureHandle texture, const char* name);
    // A target made for this frame (from the pool).
    Resource CreateTexture(const TextureDesc& desc, const char* name);

    void AddPass(const char* name, PassKind kind, const std::function<void(Builder&)>& setup, Execute execute);

    // Culls and plans barriers. Without a device (tests) only the plan is made.
    void Compile();
    // Records the passes that survived, each after its barriers.
    void Run(rhi::CommandList& cmd);

    rhi::BufferHandle Buffer(Resource resource) const;
    rhi::TextureHandle Texture(Resource resource) const;

    // The plan, for tests and the tools.
    struct PlannedBarrier {
        Resource resource = 0;
        Use from = Use::IndirectRead, to = Use::IndirectRead;
    };
    struct PlannedPass {
        std::string name;
        PassKind kind = PassKind::Graphics;
        bool culled = false;
        std::vector<PlannedBarrier> barriers; // before the pass
    };
    const std::vector<PlannedPass>& Plan() const { return plan_; }
    std::size_t PooledTextures() const { return pool_.size(); }

private:
    struct ResourceInfo {
        std::string name;
        bool imported = false;
        bool isTexture = false;
        rhi::BufferHandle buffer;
        rhi::TextureHandle texture;
        TextureDesc desc;
    };
    struct Access {
        Resource resource = 0;
        Use use = Use::IndirectRead;
    };
    struct Pass {
        std::string name;
        PassKind kind = PassKind::Graphics;
        std::vector<Access> reads, writes;
        bool sideEffect = false;
        Execute execute;
    };
    struct PooledTexture {
        TextureDesc desc;
        rhi::TextureHandle texture;
        std::uint64_t lastFrame = 0;
        bool inUse = false;
    };

    rhi::Device* device_ = nullptr;
    std::vector<ResourceInfo> resources_{1}; // 0 is "none"
    std::vector<Pass> passes_;
    std::vector<PlannedPass> plan_;
    std::vector<PooledTexture> pool_;
    std::uint64_t frame_ = 0;
};

} // namespace sr::gfx
