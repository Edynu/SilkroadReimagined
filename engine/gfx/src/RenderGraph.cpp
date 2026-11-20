#include "sr/gfx/RenderGraph.h"

#include <algorithm>
#include <unordered_set>

#include "sr/core/Trace.h"

namespace sr::gfx {

namespace {
// Pooled targets unused this many frames are released.
constexpr std::uint64_t kPoolKeepFrames = 240;
} // namespace

bool IsWrite(Use use) {
    return use == Use::ComputeWrite || use == Use::TransferWrite || use == Use::ColorWrite || use == Use::DepthWrite ||
           use == Use::External;
}

rhi::BufferAccess ToBufferAccess(Use use) {
    switch (use) {
    case Use::IndirectRead: return rhi::BufferAccess::IndirectRead;
    case Use::IndexRead: return rhi::BufferAccess::IndexRead;
    case Use::VertexRead: return rhi::BufferAccess::VertexShaderRead;
    case Use::FragmentRead: return rhi::BufferAccess::FragmentRead;
    case Use::ComputeRead: return rhi::BufferAccess::ComputeRead;
    case Use::ComputeWrite: return rhi::BufferAccess::ComputeWrite;
    case Use::TransferWrite: return rhi::BufferAccess::TransferWrite;
    case Use::TransferRead: return rhi::BufferAccess::TransferRead;
    case Use::ColorWrite:
    case Use::DepthWrite:
    case Use::Sampled:
    case Use::External: break;
    }
    return rhi::BufferAccess::Any;
}

RenderGraph::~RenderGraph() {
    if (device_ == nullptr) return;
    for (const PooledTexture& p : pool_) {
        if (p.texture) device_->DestroyTexture(p.texture);
    }
}

RenderGraph::Builder& RenderGraph::Builder::Read(Resource resource, Use use) {
    if (resource != 0) graph_.passes_[pass_].reads.push_back({resource, use});
    return *this;
}

RenderGraph::Builder& RenderGraph::Builder::Write(Resource resource, Use use) {
    if (resource != 0) graph_.passes_[pass_].writes.push_back({resource, use});
    return *this;
}

RenderGraph::Builder& RenderGraph::Builder::SideEffect() {
    graph_.passes_[pass_].sideEffect = true;
    return *this;
}

void RenderGraph::Reset() {
    ++frame_;
    resources_.resize(1);
    passes_.clear();
    plan_.clear();
    std::size_t kept = 0;
    for (PooledTexture& p : pool_) {
        p.inUse = false;
        if (frame_ - p.lastFrame > kPoolKeepFrames) {
            if (device_ != nullptr && p.texture) device_->DestroyTexture(p.texture);
            continue;
        }
        pool_[kept++] = p;
    }
    pool_.resize(kept);
}

RenderGraph::Resource RenderGraph::ImportBuffer(rhi::BufferHandle buffer, const char* name) {
    ResourceInfo info;
    info.name = name;
    info.imported = true;
    info.buffer = buffer;
    resources_.push_back(std::move(info));
    return static_cast<Resource>(resources_.size() - 1);
}

RenderGraph::Resource RenderGraph::ImportTexture(rhi::TextureHandle texture, const char* name) {
    ResourceInfo info;
    info.name = name;
    info.imported = true;
    info.isTexture = true;
    info.texture = texture;
    resources_.push_back(std::move(info));
    return static_cast<Resource>(resources_.size() - 1);
}

RenderGraph::Resource RenderGraph::CreateTexture(const TextureDesc& desc, const char* name) {
    PooledTexture* found = nullptr;
    for (PooledTexture& p : pool_) {
        if (!p.inUse && p.desc == desc) {
            found = &p;
            break;
        }
    }
    if (found == nullptr) {
        // A new target: only in the first frames, or after a resize.
        PooledTexture p;
        p.desc = desc;
        if (device_ != nullptr) {
            rhi::TextureDesc t;
            t.width = desc.width;
            t.height = desc.height;
            t.format = desc.format;
            t.usage = desc.usage;
            t.samples = desc.samples;
            t.debugName = name;
            p.texture = device_->CreateTexture(t);
        }
        pool_.push_back(p);
        found = &pool_.back();
    }
    found->inUse = true;
    found->lastFrame = frame_;
    ResourceInfo info;
    info.name = name;
    info.isTexture = true;
    info.texture = found->texture;
    info.desc = desc;
    resources_.push_back(std::move(info));
    return static_cast<Resource>(resources_.size() - 1);
}

namespace {
// A pass's name for its GPU timer: the device reads it frames later, so it must outlive the
// frame's passes (render thread only; a set's elements keep their addresses).
const char* TimerName(const std::string& name) {
    static std::unordered_set<std::string> names;
    return names.insert(name).first->c_str();
}
} // namespace

void RenderGraph::AddPass(const char* name, PassKind kind, const std::function<void(Builder&)>& setup, Execute execute) {
    Pass pass;
    pass.name = name;
    pass.kind = kind;
    pass.execute = std::move(execute);
    passes_.push_back(std::move(pass));
    Builder builder(*this, passes_.size() - 1);
    setup(builder);
}

void RenderGraph::Compile() {
    SR_ZONE("render graph: compile");
    const std::size_t count = passes_.size();
    plan_.assign(count, {});

    // Culling, backwards: a pass stays if it has side effects, writes an imported resource, or
    // writes something a kept pass after it reads.
    std::vector<bool> needed(resources_.size(), false);
    for (std::size_t i = count; i-- > 0;) {
        const Pass& pass = passes_[i];
        bool keep = pass.sideEffect;
        for (const Access& w : pass.writes) keep = keep || resources_[w.resource].imported || needed[w.resource];
        plan_[i].name = pass.name;
        plan_[i].kind = pass.kind;
        plan_[i].culled = !keep;
        if (!keep) continue;
        for (const Access& r : pass.reads) needed[r.resource] = true;
    }

    // Barriers, forwards, for buffers: read after write (one per kind of reader), write after
    // reads, write after write. An imported buffer starts as written by the work before.
    struct State {
        bool written = false;
        Use write = Use::External;
        std::vector<Use> reads; // kinds of readers since the last write
    };
    std::vector<State> state(resources_.size());
    for (std::size_t r = 1; r < resources_.size(); ++r) {
        if (resources_[r].imported) state[r].written = true;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (plan_[i].culled) continue;
        const Pass& pass = passes_[i];
        std::vector<PlannedBarrier>& barriers = plan_[i].barriers;
        for (const Access& a : pass.reads) {
            if (resources_[a.resource].isTexture) continue; // v1: the RHI's passes change texture layouts
            State& s = state[a.resource];
            if (s.written && std::find(s.reads.begin(), s.reads.end(), a.use) == s.reads.end()) {
                barriers.push_back({a.resource, s.write, a.use});
                s.reads.push_back(a.use);
            }
        }
        for (const Access& a : pass.writes) {
            if (resources_[a.resource].isTexture) continue;
            State& s = state[a.resource];
            if (!s.reads.empty()) {
                for (Use read : s.reads) barriers.push_back({a.resource, read, a.use});
            } else if (s.written) {
                barriers.push_back({a.resource, s.write, a.use});
            }
            s.written = true;
            s.write = a.use;
            s.reads.clear();
        }
    }
}

void RenderGraph::Run(rhi::CommandList& cmd) {
    std::vector<rhi::BufferBarrier> barriers;
    for (std::size_t i = 0; i < passes_.size(); ++i) {
        if (plan_[i].culled) continue;
        barriers.clear();
        for (const PlannedBarrier& b : plan_[i].barriers) {
            barriers.push_back({resources_[b.resource].buffer, ToBufferAccess(b.from), ToBufferAccess(b.to)});
        }
        if (!barriers.empty()) cmd.Barriers(barriers);
        if (!passes_[i].execute) continue;
        // Each pass's GPU time (rhi::Device::LastGpuTimers; the F1 Engine panel lists them).
        cmd.BeginTimer(TimerName(passes_[i].name));
        passes_[i].execute(cmd, *this);
        cmd.EndTimer();
    }
}

rhi::BufferHandle RenderGraph::Buffer(Resource resource) const {
    return resource < resources_.size() ? resources_[resource].buffer : rhi::BufferHandle{};
}

rhi::TextureHandle RenderGraph::Texture(Resource resource) const {
    return resource < resources_.size() ? resources_[resource].texture : rhi::TextureHandle{};
}

} // namespace sr::gfx
