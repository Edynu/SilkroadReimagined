#pragma once

// SroReimagined render hardware interface. Game and tool code talk only to
// this header; the Vulkan backend lives in src/vulkan and nothing outside
// sr_rhi sees a Vulkan type.
//
// Model:
//  - Resources are typed handles (index + generation); 0 is "null".
//  - Textures are bindless: every sampled texture has a stable index into one
//    global descriptor array, passed to shaders through push constants.
//  - Samplers are a fixed global table (SamplerIndex).
//  - One universal pipeline layout: global bindless set + 128 B push constants.
//  - Destroy*() is deferred until the GPU no longer uses the resource.
//  - Per-frame dynamic data (UI vertices etc.) comes from AllocateTransient().
//  - Initial contents of textures and buffers are copied by the GPU before the next frame
//    submitted; creating one never waits for the GPU.
//
// Threads: Create*/Destroy*/UpdateTexture, BindlessIndex, BufferAddress,
// MappedMemory, the streaming calls (AllocateStaging .. IsUploaded), MemoryBudget and the frame
// queries may be called from any thread. Everything about frames (BeginFrame .. EndFrame, Resize,
// SetVsync, SetAnisotropy, WaitIdle, LastFrameStats, LastGpuTimers, SwapchainWidth/Height)
// belongs to one thread, the render thread. A CommandList is recorded by one thread at a time; the
// lists BeginParallel() returns may be recorded concurrently, one thread each.
//
// Streaming: asset data goes through a staging ring to a transfer queue.
// A worker takes a StagingBlock, writes the GPU-ready bytes, and submits it with its copies; the
// render thread's BeginFrame sends submitted blocks to the transfer queue and makes finished ones
// visible. IsUploaded(id) turns true once frames begun from then on may use the data.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace sr::rhi {

template <typename Tag>
struct Handle {
    std::uint32_t index = 0; // 0 = null
    std::uint32_t generation = 0;
    explicit operator bool() const { return index != 0; }
    friend bool operator==(Handle a, Handle b) = default;
};
using TextureHandle = Handle<struct TextureTag>;
using BufferHandle = Handle<struct BufferTag>;
using PipelineHandle = Handle<struct PipelineTag>;

enum class Format : std::uint8_t {
    Unknown,
    RGBA8Unorm,
    BGRA8Unorm,
    BC1Unorm, // DXT1 (with 1-bit alpha)
    BC2Unorm, // DXT2/DXT3
    BC3Unorm, // DXT4/DXT5
    D32Float,
    RGBA16Float, // HDR render targets
};

bool IsBlockCompressed(Format format);
// Bytes for one mip level of `format` at width x height (tightly packed).
std::size_t MipSizeInBytes(Format format, std::uint32_t width, std::uint32_t height);

// Shader-visible sampler table, in this order.
enum class SamplerIndex : std::uint32_t {
    LinearWrap = 0,
    LinearClamp = 1,
    PointWrap = 2,
    PointClamp = 3,
    Count
};

struct NativeWindow {
    void* hinstance = nullptr; // Win32
    void* hwnd = nullptr;      // Win32
};

struct DeviceDesc {
    NativeWindow window;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool vsync = true;
    bool validation = false;     // Vulkan validation layers + debug messenger
    bool syncValidation = false; // with validation: also the layers' synchronisation checks (slow)
    bool gpuValidation = false;  // with validation: also GPU-assisted checks of shaders' memory accesses (slow)
    // Reserved at start: the staging ring, and GPU memory for streamed
    // textures (TextureDesc::streamed), so streaming never allocates device memory during play.
    // The texture reserve is capped at 40% of the device-local memory budget.
    std::size_t stagingBytes = 64ull << 20;
    std::size_t textureReserveBytes = 1ull << 30;
};

enum class TextureUsage : std::uint8_t {
    Sampled,      // shader input only
    RenderTarget, // colour attachment, then sampled (e.g. a tool viewport shown in ImGui)
    DepthStencil, // depth attachment
    ShadowMap,    // depth attachment (D32Float) that shaders sample afterwards: bindless, depth in .r
};

struct TextureDesc {
    std::uint32_t width = 1;
    std::uint32_t height = 1;
    std::uint32_t mipLevels = 1;
    Format format = Format::RGBA8Unorm;
    std::string debugName;
    TextureUsage usage = TextureUsage::Sampled;
    // RenderTarget / DepthStencil: 1, 2, 4 or 8 samples. A multisampled target is never
    // sampled: a pass resolves it into a single-sample RenderTarget (RenderPassDesc::resolve).
    std::uint32_t samples = 1;
    // Sampled only: the contents arrive through SubmitStaging (never pass init data), from the
    // reserved texture memory. Sample it only once its upload IsUploaded; destroy it only then.
    bool streamed = false;
};

// Initial contents: one tightly packed span per mip level, largest first.
struct TextureInitData {
    std::span<const std::span<const std::byte>> mips;
};

enum class BufferUsage : std::uint32_t {
    Vertex = 1 << 0,
    Index = 1 << 1,
    Uniform = 1 << 2,
    Storage = 1 << 3,
    Indirect = 1 << 4, // draw arguments and counts
};
constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

struct BufferDesc {
    std::size_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    std::string debugName;
    // true: persistently mapped, CPU-writable (dynamic data).
    // false: GPU-only memory filled once from initialData (meshes).
    bool cpuVisible = true;
};

enum class VertexFormat : std::uint8_t { Float2, Float3, Float4, RGBA8Unorm, UByte4, UInt4 };

struct VertexAttribute {
    std::uint32_t location = 0;
    VertexFormat format = VertexFormat::Float3;
    std::uint32_t offset = 0;
};

// AddModulate: source x source alpha + destination x source colour (the original's screen flash).
enum class BlendMode : std::uint8_t { Opaque, AlphaBlend, PremultipliedAlpha, Additive, AddModulate };
enum class CullMode : std::uint8_t { None, Back, Front };

// A pipeline works in passes of any sample count: the backend builds a variant for the
// pass's sample count the first time it is bound there.
struct GraphicsPipelineDesc {
    std::span<const std::byte> vertexShader;   // SPIR-V
    std::span<const std::byte> fragmentShader; // SPIR-V; empty: none (a depth-only pipeline)
    const char* vertexEntry = "main";
    const char* fragmentEntry = "main";
    std::uint32_t vertexStride = 0;
    std::vector<VertexAttribute> attributes;
    BlendMode blend = BlendMode::Opaque;
    CullMode cull = CullMode::None;
    bool depthTest = false;
    bool depthWrite = false;
    bool wireframe = false;
    Format colorFormat = Format::Unknown; // Unknown = the swapchain format
    Format depthFormat = Format::Unknown; // Unknown = no depth attachment
    bool depthOnly = false;               // no colour attachment (shadow maps)
    // Depth bias (shadow maps): constant in depth units, slope-scaled by the triangle's slope.
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    std::string debugName;
};

// Compute: same universal layout as graphics (bindless set + 128 B push constants).
struct ComputePipelineDesc {
    std::span<const std::byte> shader; // SPIR-V
    const char* entry = "main";
    std::string debugName;
};

// How a pass uses a buffer: the render graph derives the barriers
// between passes from these.
enum class BufferAccess : std::uint8_t {
    None,
    IndirectRead,     // draw arguments and counts
    IndexRead,        // bound as the index buffer
    VertexShaderRead, // read by address in vertex shaders
    FragmentRead,     // read by address in fragment shaders
    ComputeRead,
    ComputeWrite,     // includes read-modify-write (atomics)
    TransferWrite,    // FillBuffer, UpdateBuffer, copies
    TransferRead,
    Any,              // all earlier (or later) work, reads and writes: a buffer's first use in a frame
};
struct BufferBarrier {
    BufferHandle buffer;
    BufferAccess from = BufferAccess::None;
    BufferAccess to = BufferAccess::None;
};

// One indexed indirect draw as the GPU writes it (VkDrawIndexedIndirectCommand).
struct DrawIndexedIndirectCommand {
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t vertexOffset = 0;
    std::uint32_t firstInstance = 0; // shaders read it as the draw's record (vk::builtin("BaseInstance"))
};

struct Viewport {
    float x = 0, y = 0, width = 0, height = 0;
};
struct Rect {
    std::int32_t x = 0, y = 0;
    std::uint32_t width = 0, height = 0;
};

// CPU-writable memory valid until the end of the current frame.
struct TransientAllocation {
    void* cpu = nullptr;
    BufferHandle buffer;
    std::size_t offset = 0;
    std::uint64_t gpuAddress = 0; // buffer device address of `cpu`, for shaders (vk::RawBufferLoad)
};

// Staging memory for one streamed asset (AllocateStaging): write `size` bytes at `cpu`, then
// SubmitStaging (or CancelStaging) it exactly once.
struct StagingBlock {
    std::byte* cpu = nullptr;
    std::size_t size = 0;
    std::uint64_t id = 0; // the device's; 0 = no block
    explicit operator bool() const { return cpu != nullptr; }
};
// Copies out of a staging block. A buffer copy fills [dstOffset, dstOffset + size) of `buffer`
// (a GPU-only buffer). A texture copy fills mips firstMip .. firstMip + mipCount - 1 of a streamed
// texture from tightly packed levels stored back to back at srcOffset (largest first); srcOffset
// must be a multiple of 16.
struct StagedBufferCopy {
    std::size_t srcOffset = 0;
    std::size_t size = 0;
    BufferHandle buffer;
    std::size_t dstOffset = 0;
};
struct StagedTextureCopy {
    std::size_t srcOffset = 0;
    TextureHandle texture;
    std::uint32_t firstMip = 0;
    std::uint32_t mipCount = 1;
};
// Identifies a submitted staging block; ids grow in submission order. 0 = nothing.
using UploadId = std::uint64_t;

// Offscreen pass. Attachments are always cleared; afterwards the colour target
// is ready to be sampled (e.g. shown with ImGui::Image).
struct RenderPassDesc {
    TextureHandle color;      // TextureUsage::RenderTarget; may be null for a depth-only pass
    float clearColor[4] = {0, 0, 0, 1};
    TextureHandle depth;      // optional, TextureUsage::DepthStencil
    float clearDepth = 1.0f;
    // Keep the colour target's contents instead of clearing them (depth is still
    // cleared). Only for a target an earlier pass already rendered into, e.g. to
    // draw a foreground layer with its own camera over a finished scene.
    bool loadColor = false;
    // Keep a ShadowMap depth target's contents (from its last pass) instead of clearing it;
    // ClearDepth clears parts of it.
    bool loadDepth = false;
    // With a multisampled colour target: the single-sample RenderTarget it is resolved into at
    // the end of the pass (that one is then ready to be sampled). The depth target must have
    // the colour's sample count.
    TextureHandle resolve;
    // The pass's commands are recorded in parallel: only BeginParallel / EndParallel may be
    // used on the command list between BeginRenderPass and EndPass.
    bool parallel = false;
};

class CommandList {
public:
    virtual ~CommandList() = default;

    // Starts rendering into the swapchain image; clears to `clearColor`. Every pass starts with
    // the whole target as viewport and scissor.
    virtual void BeginSwapchainPass(const float clearColor[4]) = 0;
    virtual void BeginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void EndPass() = 0;

    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetScissor(const Rect& rect) = 0;
    virtual void BindPipeline(PipelineHandle pipeline) = 0;
    virtual void BindVertexBuffer(BufferHandle buffer, std::size_t offset) = 0;
    virtual void BindIndexBuffer(BufferHandle buffer, std::size_t offset, bool index32) = 0;
    virtual void PushConstants(const void* data, std::uint32_t size) = 0; // size <= 128
    virtual void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) = 0;
    virtual void DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::int32_t vertexOffset) = 0;
    // `instanceCount` copies; shaders tell them apart by SV_InstanceID (0 .. count - 1).
    virtual void DrawIndexedInstanced(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                                      std::int32_t vertexOffset) = 0;
    // GPU time of the commands between BeginTimer and EndTimer (not nested; `name` a string
    // literal), reported by Device::LastGpuTimers a few frames later.
    // Clears a rectangle of the pass's depth target.
    virtual void ClearDepth(const Rect& rect, float depth) = 0;
    // Outside passes: copies `data` into a GPU-only buffer at `offset` (multiple of 4), ordered
    // after earlier reads in this frame and before every later use in it (instance records).
    virtual void UpdateBuffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) = 0;

    // Compute and GPU-driven drawing. Outside passes: compute pipelines,
    // Dispatch, FillBuffer and Barriers. Unlike UpdateBuffer these place no barriers of their
    // own: the caller (the render graph) does, with Barriers.
    virtual void BindComputePipeline(PipelineHandle pipeline) = 0;
    virtual void Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) = 0;
    // The group counts are three uint32 at `buffer` + `offset` (multiple of 4), written by the GPU
    // (read as BufferAccess::IndirectRead).
    virtual void DispatchIndirect(BufferHandle buffer, std::size_t offset) = 0;
    // Fills `size` bytes (multiple of 4; 0: to the end) at `offset` with `value`.
    virtual void FillBuffer(BufferHandle buffer, std::size_t offset, std::size_t size, std::uint32_t value) = 0;
    virtual void Barriers(std::span<const BufferBarrier> buffers) = 0;
    // Inside a pass: up to `maxDraws` DrawIndexedIndirectCommands at `commands` + `offset`, as
    // many as the uint32 at `count` + `countOffset` says.
    virtual void DrawIndexedIndirectCount(BufferHandle commands, std::size_t offset, BufferHandle count,
                                          std::size_t countOffset, std::uint32_t maxDraws) = 0;
    virtual void BeginTimer(const char* name) = 0;
    virtual void EndTimer() = 0;

    // In a pass begun with RenderPassDesc::parallel: `count` command lists for the pass's
    // contents, each recorded by one thread (they may run concurrently); their commands execute
    // in list order. Each starts with the pass's full viewport and scissor and no pipeline.
    // Timers and passes cannot be used in them. Call EndParallel() on this list once all are
    // recorded, then EndPass().
    virtual std::span<CommandList* const> BeginParallel(std::uint32_t count) = 0;
    virtual void EndParallel() = 0;
};

class Device {
public:
    virtual ~Device() = default;

    // Returns nullptr when nothing can be drawn this frame (e.g. minimized);
    // then do not call EndFrame().
    virtual CommandList* BeginFrame() = 0;
    // Submits and presents. With `capture`, also copies the presented image
    // into RGBA8 pixels (blocking; for screenshots and automated checks).
    virtual void EndFrame(std::vector<std::uint8_t>* capture = nullptr) = 0;
    virtual void Resize(std::uint32_t width, std::uint32_t height) = 0;
    virtual void WaitIdle() = 0;
    // Presents with vertical sync (true) or as fast as frames come (mailbox / immediate);
    // takes effect when the next frame begins.
    virtual void SetVsync(bool on) = 0;
    // Anisotropic filtering of the LinearWrap sampler (what world textures use): 1 = off
    // (trilinear), up to MaxAnisotropy(). Call between frames.
    virtual void SetAnisotropy(float level) = 0;
    virtual float MaxAnisotropy() const = 0; // 1 when the GPU has none
    // The largest sample count colour and depth targets both support (1, 2, 4 or 8).
    virtual std::uint32_t MaxSamples() const = 0;
    // What the last finished frame recorded.
    struct FrameStats {
        std::uint32_t draws = 0;      // draw calls
        std::uint64_t primitives = 0; // triangles (vertices or indices / 3)
        std::uint32_t passes = 0;     // render passes, the swapchain's included
        std::uint32_t instances = 0;  // instances drawn (one per plain draw call)
        float gpuMs = -1.0f;          // GPU time of the frame's commands (-1: not measured)
        std::uint32_t uploads = 0;    // textures and buffers filled since the frame before
        std::uint64_t uploadBytes = 0;
        std::uint32_t streamed = 0;   // staging blocks this frame sent to the transfer queue
        std::uint64_t streamedBytes = 0;
        std::uint32_t dispatches = 0;    // compute dispatches
        std::uint32_t indirectDraws = 0; // indirect draw calls (each draws what the GPU listed)
    };
    virtual FrameStats LastFrameStats() const = 0;
    struct GpuTimer {
        const char* name = "";
        float ms = 0.0f;
    };
    // The timers of the last frame whose results are in, in recording order.
    virtual const std::vector<GpuTimer>& LastGpuTimers() const = 0;

    virtual std::uint32_t SwapchainWidth() const = 0;
    virtual std::uint32_t SwapchainHeight() const = 0;

    virtual TextureHandle CreateTexture(const TextureDesc& desc, const TextureInitData* init = nullptr) = 0;
    // Overwrites a rectangle of mip 0 with tightly packed pixels (uncompressed formats).
    virtual void UpdateTexture(TextureHandle texture, Rect region, std::span<const std::byte> pixels,
                               std::uint32_t rowPitch) = 0;
    virtual void DestroyTexture(TextureHandle texture) = 0;
    // Index into the global bindless texture array; 0 is a 1x1 white texture.
    virtual std::uint32_t BindlessIndex(TextureHandle texture) const = 0;

    virtual BufferHandle CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initialData = {}) = 0;
    virtual void DestroyBuffer(BufferHandle buffer) = 0;
    // A Storage buffer's index in the bindless storage array (binding 2: `RWByteAddressBuffer
    // gBuffers[]` in shaders, for atomics); 0 when it has none (slot 0 is a placeholder).
    virtual std::uint32_t StorageIndex(BufferHandle buffer) const = 0;
    // GPU address of a buffer's first byte (every buffer has one); 0 for a null handle.
    virtual std::uint64_t BufferAddress(BufferHandle buffer) const = 0;
    // A cpuVisible buffer's persistently mapped memory (nullptr otherwise). Writing it while the
    // GPU reads it is the caller's problem: rotate buffers by frame (CompletedFrame).
    virtual void* MappedMemory(BufferHandle buffer) const = 0;

    virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
    virtual void DestroyPipeline(PipelineHandle pipeline) = 0;

    virtual TransientAllocation AllocateTransient(std::size_t size, std::size_t alignment = 16) = 0;

    // Frames are numbered from 1 in submission order. Any thread may ask; WaitForFrame blocks
    // until the GPU has finished frame `frame` (immediately for 0 or a finished one).
    virtual std::uint64_t SubmittedFrame() const = 0;
    virtual std::uint64_t CompletedFrame() const = 0;
    virtual void WaitForFrame(std::uint64_t frame) = 0;
    // The frame BeginFrame() opened (or will open next): the number its submission will get.
    virtual std::uint64_t RecordingFrame() const = 0;

    // Streaming (see the top of this file). A block of `size` bytes from the staging ring, or an
    // empty one when the ring has no room now (try again in a later frame).
    virtual StagingBlock AllocateStaging(std::size_t size) = 0;
    // Queues the block's copies; the block must not be written afterwards.
    virtual UploadId SubmitStaging(const StagingBlock& block, std::span<const StagedBufferCopy> buffers,
                                   std::span<const StagedTextureCopy> textures) = 0;
    virtual void CancelStaging(const StagingBlock& block) = 0;
    // True once frames begun from now on may read what the upload wrote (0 is always uploaded).
    virtual bool IsUploaded(UploadId id) const = 0;
    struct StreamingInfo {
        bool dedicatedTransferQueue = false; // a transfer-only queue family (copy engine)
        std::size_t stagingCapacity = 0;
        std::size_t stagingUsed = 0;
        std::size_t textureReserve = 0;      // memory reserved for streamed textures (DeviceDesc, capped)
        UploadId submitted = 0, uploaded = 0;
    };
    virtual StreamingInfo GetStreamingInfo() const = 0;
    // Device-local memory in use and the budget the driver gives this process (both 0 if unknown).
    struct MemoryBudget {
        std::uint64_t usage = 0;
        std::uint64_t budget = 0;
    };
    virtual MemoryBudget GetMemoryBudget() const = 0;

    // Live resources (leak checks) and destructions waiting for the GPU.
    struct ResourceCounts {
        std::uint32_t textures = 0;
        std::uint32_t buffers = 0;
        std::uint32_t pipelines = 0;
        std::uint32_t pendingDeletions = 0;
    };
    virtual ResourceCounts GetResourceCounts() const = 0;

    virtual const std::string& AdapterName() const = 0;
    // Validation-layer warnings and errors seen so far (0 when validation is off).
    virtual std::uint32_t ValidationMessageCount() const = 0;
};

std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc, std::string& error);

} // namespace sr::rhi
