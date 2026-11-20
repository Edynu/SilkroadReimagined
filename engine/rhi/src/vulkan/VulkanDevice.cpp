// Vulkan 1.3 backend for sr::rhi: dynamic rendering, synchronization2,
// bindless sampled images (descriptor indexing) and VMA for memory.
//
// Threads: resources may be created and destroyed from any thread;
// frames (BeginFrame .. EndFrame, Resize, SetVsync, SetAnisotropy, WaitIdle) belong to one
// thread, the render thread. Handle lookups take no lock (SlotPool). `resourceMutex_` guards
// uploads, deferred deletions and bindless descriptor writes; `queueMutex_` every use of the
// queue. Lock order: resourceMutex_, then queueMutex_.

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sr/rhi/Rhi.h"

namespace sr::rhi {

namespace {

constexpr std::uint32_t kFramesInFlight = 2;
constexpr std::uint32_t kMaxBindlessTextures = 16384;
// Storage buffers with a bindless slot (StorageIndex): buffers whose handle index is below this.
constexpr std::uint32_t kMaxBindlessBuffers = 4096;
constexpr std::uint32_t kPushConstantSize = 128;
constexpr std::size_t kTransientChunkSize = 4 * 1024 * 1024;
// Uploads recorded without a frame in between go out on their own past this many bytes.
constexpr std::size_t kUploadBatchLimit = 64 * 1024 * 1024;
// Streaming: staging blocks are aligned to this, and at most this many
// bytes go to the transfer queue per frame (a bigger burst spreads over frames).
constexpr std::size_t kStagingAlignment = 256;
constexpr std::size_t kStreamBytesPerFrame = 32 * 1024 * 1024;
// Transfer command buffers in rotation; one is submitted per frame at most.
constexpr std::size_t kTransferSlots = 8;
// Reserved texture memory comes in blocks of this size.
constexpr std::size_t kTextureBlockSize = 256ull << 20;
// GPU-only buffers this big are reservations: CreateBuffer makes them resident at once (blocking).
constexpr std::size_t kWarmBufferBytes = 16ull << 20;
// Deferred deletions run for at most this long a frame (the rest in the next ones).
constexpr std::chrono::microseconds kDeletionBudget{300};
// Timestamps per frame slot: the frame's start and end, then a pair per named timer.
constexpr std::uint32_t kMaxGpuTimers = 64;
constexpr std::uint32_t kQueriesPerFrame = 2 + 2 * kMaxGpuTimers;

#define SR_VK_CHECK(expr)                                                                                    \
    do {                                                                                                     \
        const VkResult srVkResult_ = (expr);                                                                 \
        if (srVkResult_ != VK_SUCCESS) {                                                                     \
            std::fprintf(stderr, "[rhi] %s failed: VkResult %d (%s:%d)\n", #expr, static_cast<int>(srVkResult_), \
                         __FILE__, __LINE__);                                                                \
        }                                                                                                    \
    } while (0)

VkFormat ToVk(Format format) {
    switch (format) {
    case Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BC1Unorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::BC2Unorm: return VK_FORMAT_BC2_UNORM_BLOCK;
    case Format::BC3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::D32Float: return VK_FORMAT_D32_SFLOAT;
    case Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::Unknown: break;
    }
    return VK_FORMAT_UNDEFINED;
}

VkFormat ToVk(VertexFormat format) {
    switch (format) {
    case VertexFormat::Float2: return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexFormat::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case VertexFormat::UByte4: return VK_FORMAT_R8G8B8A8_UINT;
    case VertexFormat::UInt4: return VK_FORMAT_R32G32B32A32_UINT;
    }
    return VK_FORMAT_UNDEFINED;
}

// Generational slot storage behind the public handles. Slot 0 is never handed
// out so that a zero handle is always null.
//
// Thread-safe: slots live in fixed blocks that never move, so Get
// takes no lock. A slot's value is written before its handle is published and stays intact
// after Retire() until Release(), which the device calls only once no frame can use it.
// Add, Retire, Release and ForEachAlive take the pool's lock.
template <typename T, typename Tag>
class SlotPool {
public:
    explicit SlotPool(std::uint32_t capacity = kMaxSlots) : capacity_(std::min(capacity, kMaxSlots)) {
        next_ = 1;
    }
    ~SlotPool() {
        for (auto& block : blocks_) delete[] block.load(std::memory_order_relaxed);
    }
    SlotPool(const SlotPool&) = delete;
    SlotPool& operator=(const SlotPool&) = delete;

    Handle<Tag> Add(T value) {
        std::lock_guard lock(mutex_);
        std::uint32_t index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            if (next_ >= capacity_) return {};
            index = next_++;
            auto& block = blocks_[index / kBlockSize];
            if (block.load(std::memory_order_relaxed) == nullptr) {
                block.store(new Slot[kBlockSize], std::memory_order_release);
            }
        }
        Slot& slot = SlotAt(index);
        slot.value = std::move(value);
        slot.alive.store(true, std::memory_order_release);
        ++alive_;
        return {index, slot.generation.load(std::memory_order_relaxed)};
    }

    T* Get(Handle<Tag> h) {
        if (h.index == 0 || h.index >= capacity_) return nullptr;
        Slot* block = blocks_[h.index / kBlockSize].load(std::memory_order_acquire);
        if (block == nullptr) return nullptr;
        Slot& slot = block[h.index % kBlockSize];
        return slot.alive.load(std::memory_order_acquire) && slot.generation.load(std::memory_order_relaxed) == h.generation
                   ? &slot.value
                   : nullptr;
    }
    const T* Get(Handle<Tag> h) const { return const_cast<SlotPool*>(this)->Get(h); }

    // Invalidates the handle and returns a copy of the entry. The slot index is NOT
    // reusable until Release(): for textures it is also a bindless descriptor
    // index that in-flight frames may still read.
    std::optional<T> Retire(Handle<Tag> h) {
        std::lock_guard lock(mutex_);
        T* value = Get(h);
        if (value == nullptr) return std::nullopt;
        Slot& slot = SlotAt(h.index);
        std::optional<T> out(*value);
        slot.alive.store(false, std::memory_order_release);
        slot.generation.fetch_add(1, std::memory_order_relaxed);
        --alive_;
        return out;
    }
    void Release(std::uint32_t index) {
        std::lock_guard lock(mutex_);
        SlotAt(index).value = T{};
        free_.push_back(index);
    }

    template <typename Fn>
    void ForEachAlive(Fn fn) {
        std::lock_guard lock(mutex_);
        for (std::uint32_t i = 1; i < next_; ++i) {
            Slot& slot = SlotAt(i);
            if (slot.alive.load(std::memory_order_relaxed)) fn(slot.value);
        }
    }
    std::uint32_t AliveCount() const {
        std::lock_guard lock(mutex_);
        return alive_;
    }

private:
    static constexpr std::uint32_t kBlockSize = 1024;
    static constexpr std::uint32_t kMaxBlocks = 256;
    static constexpr std::uint32_t kMaxSlots = kBlockSize * kMaxBlocks;
    struct Slot {
        T value{};
        std::atomic<std::uint32_t> generation{1};
        std::atomic<bool> alive{false};
    };
    Slot& SlotAt(std::uint32_t index) { return blocks_[index / kBlockSize].load(std::memory_order_relaxed)[index % kBlockSize]; }

    std::array<std::atomic<Slot*>, kMaxBlocks> blocks_{};
    std::vector<std::uint32_t> free_;
    std::uint32_t next_ = 1;
    std::uint32_t alive_ = 0;
    std::uint32_t capacity_;
    mutable std::mutex mutex_;
};

struct VkTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    Format format = Format::Unknown;
    TextureUsage usage = TextureUsage::Sampled;
    std::uint32_t width = 0, height = 0, mips = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool streamed = false; // filled through the staging ring (TextureDesc::streamed)
};

// Streaming. The staging ring hands out blocks in allocation order; a
// block's space returns once the transfer that read it finished (or it was cancelled), and the
// ring's tail moves over finished blocks only, so space is reused in order.
struct RingBlock {
    enum class State : std::uint8_t { Writing, Queued, InFlight, Done };
    std::uint64_t id = 0;
    std::size_t offset = 0, size = 0;
    State state = State::Writing;
    std::uint64_t value = 0; // the upload timeline value of its transfer (InFlight)
};

// A submitted block waiting for the next flush.
struct QueuedUpload {
    std::uint64_t block = 0;
    std::size_t offset = 0, size = 0; // the block's place in the ring
    UploadId id = 0;
    std::vector<StagedBufferCopy> buffers;
    std::vector<StagedTextureCopy> textures;
};

// One transfer submission: its command buffer, the timeline value it signals (frames wait for it
// before they use the results) and the textures it filled.
struct TransferSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::uint64_t value = 0;  // 0: free
    UploadId lastId = 0;      // the highest upload id in it
    bool visible = false;     // its results were made visible to the frames
    struct Image {
        TextureHandle handle;
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t mips = 1;
    };
    std::vector<Image> images; // the textures it filled: shown in their bindless slots once it is done
};

struct VkBufferRecord {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    std::size_t size = 0;
    VkDeviceAddress address = 0;
    bool storage = false; // has a bindless storage slot (StorageIndex)
};

// What a pipeline was made from, kept to build it again for another sample count.
struct PipelineSource {
    GraphicsPipelineDesc desc; // its shader spans point into the vectors below
    std::vector<std::byte> vertexCode, fragmentCode;
    std::string vertexEntry, fragmentEntry;
};

// A pipeline's variants (1, 2, 4, 8 samples), built on first use by whichever thread records
// the draw: read without a lock, built under `build`.
struct PipelineVariants {
    std::array<std::atomic<VkPipeline>, 4> pipelines{};
    std::mutex build;
};

struct VkPipelineRecord {
    std::shared_ptr<PipelineVariants> variants;
    std::shared_ptr<const PipelineSource> source;
    VkPipeline compute = VK_NULL_HANDLE; // a compute pipeline (no variants)
};

int SampleSlot(VkSampleCountFlagBits samples) {
    switch (samples) {
    case VK_SAMPLE_COUNT_2_BIT: return 1;
    case VK_SAMPLE_COUNT_4_BIT: return 2;
    case VK_SAMPLE_COUNT_8_BIT: return 3;
    default: return 0;
    }
}

VkSampleCountFlagBits SampleBits(std::uint32_t samples) {
    return samples >= 8 ? VK_SAMPLE_COUNT_8_BIT : samples >= 4 ? VK_SAMPLE_COUNT_4_BIT : samples >= 2 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
}

struct TransientChunk {
    BufferHandle handle;
    std::byte* mapped = nullptr;
    std::size_t size = 0;
    VkDeviceAddress address = 0;
};

// Secondary command buffers of one frame slot for parallel recording: one pool per chunk
// index, so each chunk's buffer is only ever touched by the one thread recording it.
struct ChunkRecorder {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};

struct FrameData {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore imageAcquired = VK_NULL_HANDLE;
    std::uint64_t number = 0; // frame number last submitted with this slot (1-based); the timeline's value
    bool timed = false;       // its commands wrote their timestamps
    std::vector<const char*> timers; // names of the timer pairs written after the frame's own
    bool timerOpen = false;
    std::vector<TransientChunk> chunks;
    std::size_t chunkIndex = 0;
    std::size_t chunkOffset = 0;
    std::vector<ChunkRecorder> recorders; // grown on demand by BeginParallel
    std::size_t recordersUsed = 0;
};

// A destruction that must wait until frame `safeAfter` has completed on the GPU.
struct PendingDeletion {
    std::uint64_t safeAfter = 0;
    std::function<void()> destroy;
};

class VulkanDevice;

// What a secondary command buffer recorded inside a pass must inherit (dynamic rendering).
struct PassFormats {
    VkFormat color = VK_FORMAT_UNDEFINED; // VK_FORMAT_UNDEFINED: no colour attachment
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkExtent2D extent{};
};

class VulkanCommandList final : public CommandList {
public:
    explicit VulkanCommandList(VulkanDevice& device) : device_(device) {}
    void Begin(VkCommandBuffer cmd) {
        cmd_ = cmd;
        secondary_ = false;
        parallelPass_ = false;
    }
    // Starts a secondary command buffer that continues an open pass (parallel recording).
    void BeginSecondary(VkCommandBuffer cmd, const PassFormats& pass, VkDescriptorSet set, VkPipelineLayout layout);
    VkCommandBuffer Buffer() const { return cmd_; }
    std::span<CommandList* const> BeginParallel(std::uint32_t count) override;
    void EndParallel() override;

    void BeginSwapchainPass(const float clearColor[4]) override;
    void BeginRenderPass(const RenderPassDesc& desc) override;
    void EndPass() override;
    void SetViewport(const Viewport& viewport) override;
    void SetScissor(const Rect& rect) override;
    void BindPipeline(PipelineHandle pipeline) override;
    void BindVertexBuffer(BufferHandle buffer, std::size_t offset) override;
    void BindIndexBuffer(BufferHandle buffer, std::size_t offset, bool index32) override;
    void PushConstants(const void* data, std::uint32_t size) override;
    void Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) override;
    void DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::int32_t vertexOffset) override;
    void DrawIndexedInstanced(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                              std::int32_t vertexOffset) override;
    void ClearDepth(const Rect& rect, float depth) override;
    void UpdateBuffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) override;
    void BindComputePipeline(PipelineHandle pipeline) override;
    void Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) override;
    void DispatchIndirect(BufferHandle buffer, std::size_t offset) override;
    void FillBuffer(BufferHandle buffer, std::size_t offset, std::size_t size, std::uint32_t value) override;
    void Barriers(std::span<const BufferBarrier> buffers) override;
    void DrawIndexedIndirectCount(BufferHandle commands, std::size_t offset, BufferHandle count, std::size_t countOffset,
                                  std::uint32_t maxDraws) override;
    void BeginTimer(const char* name) override;
    void EndTimer() override;

    Device::FrameStats stats;
    void ResetStats() { stats = {}; }

private:
    VulkanDevice& device_;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkImage offscreenColor_ = VK_NULL_HANDLE; // the image sampled after the open offscreen pass (the resolve, if any)
    VkImage shadowDepth_ = VK_NULL_HANDLE;    // the open pass's shadow map, sampled after it
    VkSampleCountFlagBits passSamples_ = VK_SAMPLE_COUNT_1_BIT;
    PassFormats pass_;          // the open pass
    bool parallelPass_ = false; // the open pass takes its contents from BeginParallel
    bool secondary_ = false;    // this list continues a pass (a chunk of a parallel pass)
    std::vector<std::unique_ptr<VulkanCommandList>> chunks_;
    std::vector<CommandList*> chunkPointers_;
    std::uint32_t openChunks_ = 0;
};

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

class VulkanDevice final : public Device {
public:
    ~VulkanDevice() override;
    bool Init(const DeviceDesc& desc, std::string& error);

    CommandList* BeginFrame() override;
    void EndFrame(std::vector<std::uint8_t>* capture) override;
    void Resize(std::uint32_t width, std::uint32_t height) override;
    void WaitIdle() override {
        std::lock_guard lock(resourceMutex_);
        SubmitUploads();
        DeviceWaitIdle();
        ReclaimUploads(true);
    }
    void SetVsync(bool on) override {
        if (on == vsync_) return;
        vsync_ = on;
        swapchainDirty_ = true;
    }
    void SetAnisotropy(float level) override;
    float MaxAnisotropy() const override { return maxAnisotropy_; }
    std::uint32_t MaxSamples() const override { return maxSamples_; }
    FrameStats LastFrameStats() const override { return lastStats_; }
    const std::vector<GpuTimer>& LastGpuTimers() const override { return lastTimers_; }
    // Used by VulkanCommandList.
    void WriteTimer(VkCommandBuffer cmd, const char* name);
    std::uint32_t SwapchainWidth() const override { return extent_.width; }
    std::uint32_t SwapchainHeight() const override { return extent_.height; }

    TextureHandle CreateTexture(const TextureDesc& desc, const TextureInitData* init) override;
    void UpdateTexture(TextureHandle texture, Rect region, std::span<const std::byte> pixels,
                       std::uint32_t rowPitch) override;
    void DestroyTexture(TextureHandle texture) override;
    std::uint32_t BindlessIndex(TextureHandle texture) const override {
        return textures_.Get(texture) != nullptr ? texture.index : 0;
    }

    BufferHandle CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initialData) override;
    void DestroyBuffer(BufferHandle buffer) override;
    std::uint32_t StorageIndex(BufferHandle buffer) const override {
        const auto* b = buffers_.Get(buffer);
        return b != nullptr && b->storage && buffer.index < kMaxBindlessBuffers ? buffer.index : 0;
    }
    std::uint64_t BufferAddress(BufferHandle buffer) const override {
        const auto* b = buffers_.Get(buffer);
        return b != nullptr ? b->address : 0;
    }
    void* MappedMemory(BufferHandle buffer) const override {
        const auto* b = buffers_.Get(buffer);
        return b != nullptr ? b->mapped : nullptr;
    }
    std::uint64_t SubmittedFrame() const override { return submittedFrames_.load(std::memory_order_acquire); }
    std::uint64_t CompletedFrame() const override {
        std::uint64_t value = 0;
        if (frameTimeline_ != VK_NULL_HANDLE) vkGetSemaphoreCounterValue(device_, frameTimeline_, &value);
        return value;
    }
    void WaitForFrame(std::uint64_t frame) override {
        if (frame == 0 || frameTimeline_ == VK_NULL_HANDLE) return;
        VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait.semaphoreCount = 1;
        wait.pSemaphores = &frameTimeline_;
        wait.pValues = &frame;
        SR_VK_CHECK(vkWaitSemaphores(device_, &wait, UINT64_MAX));
    }
    std::uint64_t RecordingFrame() const override { return submittedFrames_.load(std::memory_order_acquire) + 1; }
    ResourceCounts GetResourceCounts() const override {
        ResourceCounts counts;
        counts.textures = textures_.AliveCount();
        counts.buffers = buffers_.AliveCount();
        counts.pipelines = pipelines_.AliveCount();
        std::lock_guard lock(resourceMutex_);
        counts.pendingDeletions = static_cast<std::uint32_t>(deletions_.size());
        return counts;
    }

    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override;
    void DestroyPipeline(PipelineHandle pipeline) override;

    TransientAllocation AllocateTransient(std::size_t size, std::size_t alignment) override;

    StagingBlock AllocateStaging(std::size_t size) override;
    UploadId SubmitStaging(const StagingBlock& block, std::span<const StagedBufferCopy> buffers,
                           std::span<const StagedTextureCopy> textures) override;
    void CancelStaging(const StagingBlock& block) override;
    bool IsUploaded(UploadId id) const override { return id <= visibleUpload_.load(std::memory_order_acquire); }
    StreamingInfo GetStreamingInfo() const override;
    MemoryBudget GetMemoryBudget() const override;

    const std::string& AdapterName() const override { return adapterName_; }
    std::uint32_t ValidationMessageCount() const override { return validationMessages_.load(); }

    // Used by VulkanCommandList.
    VkPipelineLayout PipelineLayout() const { return pipelineLayout_; }
    // The pipeline's variant for a pass of `samples` (built the first time it is needed).
    // Any recording thread may call this.
    VkPipeline GetPipeline(PipelineHandle h, VkSampleCountFlagBits samples) {
        auto* p = pipelines_.Get(h);
        if (p == nullptr || !p->variants) return VK_NULL_HANDLE;
        std::atomic<VkPipeline>& variant = p->variants->pipelines[static_cast<std::size_t>(SampleSlot(samples))];
        VkPipeline built = variant.load(std::memory_order_acquire);
        if (built != VK_NULL_HANDLE || !p->source) return built;
        std::lock_guard lock(p->variants->build);
        built = variant.load(std::memory_order_acquire);
        if (built == VK_NULL_HANDLE) {
            built = BuildPipeline(p->source->desc, samples);
            variant.store(built, std::memory_order_release);
        }
        return built;
    }
    VkPipeline GetComputePipeline(PipelineHandle h) const {
        const auto* p = pipelines_.Get(h);
        return p != nullptr ? p->compute : VK_NULL_HANDLE;
    }
    VkBuffer GetBuffer(BufferHandle h) const {
        const auto* b = buffers_.Get(h);
        return b != nullptr ? b->buffer : VK_NULL_HANDLE;
    }
    const VkTexture* GetTexture(TextureHandle h) const { return textures_.Get(h); }
    void BeginSwapchainPass(VkCommandBuffer cmd, const float clearColor[4]);
    // The secondary command buffer of chunk `index` in the frame being recorded (render thread).
    VkCommandBuffer AcquireRecorder(std::size_t index);
    VkDescriptorSet BindlessSet() const { return bindlessSet_; }

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                        VkDebugUtilsMessageTypeFlagsEXT types,
                                                        const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                        void* user);

    bool CreateInstance(bool validation, bool syncValidation, bool gpuValidation, std::string& error);
    bool PickPhysicalDevice(std::string& error);
    bool CreateLogicalDevice(std::string& error);
    bool CreateSwapchain(std::uint32_t width, std::uint32_t height, std::string& error);
    void DestroySwapchain();
    bool CreateBindlessLayout(std::string& error);
    bool CreateFrames(std::string& error);
    bool CreateStreaming(const DeviceDesc& desc, std::string& error);
    void WarmReservedMemory();
    // Render thread, in BeginFrame: makes finished transfers visible to the frame being recorded,
    // frees their staging space, then sends queued blocks to the transfer queue.
    void PumpStreaming();
    // Records and submits one transfer of queued blocks (returns false if nothing was sent).
    bool FlushStaging();
    // Every queue idle (both queue locks held).
    void DeviceWaitIdle() {
        std::lock_guard queue(queueMutex_);
        if (transferShared_) {
            vkDeviceWaitIdle(device_);
        } else {
            std::lock_guard transfer(transferMutex_);
            vkDeviceWaitIdle(device_);
        }
    }
    // Records upload commands into the open batch (opening one); `staging` (optional) is freed
    // once the batch has run. Batches go to the queue ahead of the next frame (SubmitUploads).
    void RecordUpload(const std::function<void(VkCommandBuffer)>& record, VkBuffer staging = VK_NULL_HANDLE,
                      VmaAllocation stagingAlloc = nullptr, std::size_t bytes = 0);
    void SubmitUploads();
    // Frees the staging memory of batches the GPU has finished (waiting for all with `wait`).
    void ReclaimUploads(bool wait);
    void WriteBindlessDescriptor(std::uint32_t slot, VkImageView view);
    void WriteStorageDescriptor(std::uint32_t slot, VkBuffer buffer);
    void SetName(VkObjectType type, std::uint64_t handle, const std::string& name);
    // Runs deletions whose frames have all completed (all of them with `everything`).
    void FlushDeletions(bool everything);
    // Any frame up to and including the one being recorded (or recorded next)
    // may still reference the resource.
    void DeferDeletion(std::function<void()> fn) {
        std::lock_guard lock(resourceMutex_);
        deletions_.push_back({submittedFrames_.load(std::memory_order_acquire) + 1, std::move(fn)});
    }
    void RecreateSwapchainIfNeeded();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queueFamily_ = 0;
    VmaAllocator allocator_ = nullptr;
    PFN_vkSetDebugUtilsObjectNameEXT setObjectName_ = nullptr;
    std::string adapterName_;
    std::atomic<std::uint32_t> validationMessages_{0}; // the layers may call back from any thread
    bool wireframeSupported_ = false;
    float maxAnisotropy_ = 1.0f; // 1: the GPU has no anisotropic filtering
    std::uint32_t maxSamples_ = 1;
    FrameStats lastStats_;
    VkPipeline BuildPipeline(const GraphicsPipelineDesc& desc, VkSampleCountFlagBits samples);
    float anisotropy_ = 1.0f;
    VkSampler MakeSampler(std::size_t index, float anisotropy) const;
    void WriteSamplers();

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    bool vsync_ = true;
    bool swapchainCanCopy_ = false;
    bool swapchainLogged_ = false; // the present modes are logged once
    bool swapchainDirty_ = false;
    std::uint32_t requestedWidth_ = 0, requestedHeight_ = 0;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<VkSemaphore> renderFinished_; // one per swapchain image
    std::uint32_t imageIndex_ = 0;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet bindlessSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    std::array<VkSampler, static_cast<std::size_t>(SamplerIndex::Count)> samplers_{};

    std::array<FrameData, kFramesInFlight> frames_{};
    std::uint32_t frameIndex_ = 0;
    bool frameActive_ = false;
    VkSemaphore frameTimeline_ = VK_NULL_HANDLE; // value = number of the last frame the GPU finished
    std::atomic<std::uint64_t> submittedFrames_{0};
    std::uint64_t completedFrames_ = 0;
    std::vector<PendingDeletion> deletions_;
    VulkanCommandList commandList_{*this};
    mutable std::mutex resourceMutex_;  // uploads, deletions, bindless descriptor writes
    std::mutex queueMutex_;             // every use of queue_ (and vkDeviceWaitIdle)
    std::mutex transientMutex_;         // AllocateTransient from parallel recording

    VkCommandPool uploadPool_ = VK_NULL_HANDLE;
    struct UploadBatch {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool inFlight = false;
        std::vector<std::pair<VkBuffer, VmaAllocation>> staging;
        std::size_t bytes = 0;
    };
    std::vector<UploadBatch> uploads_;
    int openUpload_ = -1; // the batch being recorded, or -1
    std::uint32_t uploadCount_ = 0;
    std::uint64_t uploadBytes_ = 0;
    // Two timestamps per frame slot (start, end of its commands); null when the queue has none.
    VkQueryPool timestamps_ = VK_NULL_HANDLE;
    double timestampPeriodNs_ = 0.0;
    std::uint64_t timestampMask_ = 0;
    float lastGpuMs_ = -1.0f;
    std::vector<GpuTimer> lastTimers_;

    SlotPool<VkTexture, TextureTag> textures_{kMaxBindlessTextures};
    SlotPool<VkBufferRecord, BufferTag> buffers_;
    SlotPool<VkPipelineRecord, PipelineTag> pipelines_;
    VkTexture whiteTexture_{};
    VkBufferRecord nullStorage_{}; // what unused storage slots point at

    // Streaming. The transfer queue is a dedicated transfer family's when
    // there is one (resources it writes are shared by both families, no ownership transfers),
    // else a second queue of the graphics family, else the graphics queue itself (`transferShared_`,
    // then guarded by queueMutex_). Only the render thread submits to it.
    std::uint32_t transferFamily_ = 0;
    std::uint32_t transferQueueIndex_ = 0;
    VkQueue transferQueue_ = VK_NULL_HANDLE;
    bool transferShared_ = true;     // the same VkQueue as the frames
    bool ownershipTransfer_ = false; // the transfer family differs from the graphics family (share resources)
    std::mutex transferMutex_;       // the transfer queue, when it is not the graphics queue
    bool memoryBudget_ = false;      // VK_EXT_memory_budget enabled
    VmaPool texturePool_ = nullptr;  // reserved memory for streamed textures
    std::size_t textureReserve_ = 0; // its size
    VkSemaphore uploadTimeline_ = VK_NULL_HANDLE; // value = the last transfer submission finished
    std::uint64_t uploadValue_ = 0;               // the last value a transfer submission signals
    std::uint64_t frameUploadWait_ = 0;           // the frame being recorded waits for this value
    std::array<TransferSlot, kTransferSlots> transfers_{};
    // The staging ring; blocks and queued uploads are guarded by stagingMutex_.
    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc_ = nullptr;
    std::byte* stagingCpu_ = nullptr;
    std::size_t stagingCapacity_ = 0;
    std::size_t stagingHead_ = 0; // where the next block goes
    std::deque<RingBlock> ringBlocks_; // allocation order (ids ascending)
    std::vector<QueuedUpload> queuedUploads_; // submission order
    std::uint64_t nextBlockId_ = 1;
    UploadId nextUploadId_ = 1;
    mutable std::mutex stagingMutex_;
    std::atomic<UploadId> visibleUpload_{0};
    std::uint32_t streamedCount_ = 0;
    std::uint64_t streamedBytes_ = 0;
};

// ---------------------------------------------------------------- setup

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                           VkDebugUtilsMessageTypeFlagsEXT types,
                                                           const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                           void* user) {
    auto* self = static_cast<VulkanDevice*>(user);
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        return VK_FALSE;
    }
    // "General" messages come from the loader (e.g. third-party overlay layers
    // installed on the machine), not from our API usage: report, don't count.
    const bool ours = (types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0;
    if (ours) {
        ++self->validationMessages_;
    }
    std::fprintf(stderr, "[vulkan %s] %s\n",
                 !ours ? "info" : severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
                 data->pMessage);
    return VK_FALSE;
}

bool VulkanDevice::Init(const DeviceDesc& desc, std::string& error) {
    vsync_ = desc.vsync;
    if (!CreateInstance(desc.validation, desc.syncValidation, desc.gpuValidation, error)) return false;

#ifdef VK_USE_PLATFORM_WIN32_KHR
    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = static_cast<HINSTANCE>(desc.window.hinstance);
    surfaceInfo.hwnd = static_cast<HWND>(desc.window.hwnd);
    if (vkCreateWin32SurfaceKHR(instance_, &surfaceInfo, nullptr, &surface_) != VK_SUCCESS) {
        error = "vkCreateWin32SurfaceKHR failed";
        return false;
    }
#else
    error = "no Vulkan surface support for this platform yet";
    return false;
#endif

    if (!PickPhysicalDevice(error) || !CreateLogicalDevice(error)) return false;

    VmaAllocatorCreateInfo allocInfo{};
    allocInfo.instance = instance_;
    allocInfo.physicalDevice = physical_;
    allocInfo.device = device_;
    allocInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
                      (memoryBudget_ ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    if (vmaCreateAllocator(&allocInfo, &allocator_) != VK_SUCCESS) {
        error = "vmaCreateAllocator failed";
        return false;
    }

    if (!CreateBindlessLayout(error) || !CreateFrames(error) || !CreateStreaming(desc, error)) return false;
    if (!CreateSwapchain(desc.width, desc.height, error)) return false;

    // Bindless slot 0: an opaque white 1x1 texture, the fallback for any index.
    const std::array<std::uint8_t, 4> white = {255, 255, 255, 255};
    const std::span<const std::byte> whiteMip(reinterpret_cast<const std::byte*>(white.data()), 4);
    const TextureInitData init{std::span(&whiteMip, 1)};
    const TextureHandle tmp = CreateTexture({1, 1, 1, Format::RGBA8Unorm, "white"}, &init);
    if (!tmp) {
        error = "could not create the fallback texture";
        return false;
    }
    whiteTexture_ = *textures_.Retire(tmp); // re-home it into the reserved slot 0
    textures_.Release(tmp.index);
    std::lock_guard lock(resourceMutex_);
    WriteBindlessDescriptor(0, whiteTexture_.view);
    // Storage slot 0 and freed slots: a small placeholder buffer.
    {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = 256;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateBuffer(allocator_, &info, &alloc, &nullStorage_.buffer, &nullStorage_.allocation, nullptr) != VK_SUCCESS) {
            error = "cannot create the placeholder storage buffer";
            return false;
        }
        WriteStorageDescriptor(0, nullStorage_.buffer);
    }
    return true;
}

bool VulkanDevice::CreateInstance(bool validation, bool syncValidation, bool gpuValidation, std::string& error) {
    std::uint32_t version = 0;
    vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3) {
        error = "Vulkan 1.3 is required (update the GPU driver)";
        return false;
    }

    std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
#ifdef VK_USE_PLATFORM_WIN32_KHR
    extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
    std::vector<const char*> layers;
    if (validation) {
        std::uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        const bool found = std::any_of(available.begin(), available.end(), [](const VkLayerProperties& p) {
            return std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (found) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            std::fprintf(stderr, "[rhi] validation requested but VK_LAYER_KHRONOS_validation is not installed\n");
            validation = false;
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "SroReimagined";
    app.pEngineName = "SroReimagined";
    app.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messengerInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messengerInfo.pfnUserCallback = &VulkanDevice::DebugCallback;
    messengerInfo.pUserData = this;

    // Synchronisation validation (hazards between commands, queues and the host), on request.
    // GPU-assisted validation (shaders' out-of-bounds reads and writes, buffer device addresses
    // included), on request.
    std::vector<VkValidationFeatureEnableEXT> features;
    if (syncValidation) features.push_back(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
    if (gpuValidation) features.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = static_cast<std::uint32_t>(features.size());
    validationFeatures.pEnabledValidationFeatures = features.data();
    if (!features.empty()) messengerInfo.pNext = &validationFeatures;

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    if (validation) info.pNext = &messengerInfo;

    if (vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS) {
        error = "vkCreateInstance failed";
        return false;
    }
    if (validation) {
        messengerInfo.pNext = nullptr; // the validation features belong to instance creation only
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create != nullptr) create(instance_, &messengerInfo, nullptr, &messenger_);
        setObjectName_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(instance_, "vkSetDebugUtilsObjectNameEXT"));
    }
    return true;
}

bool VulkanDevice::PickPhysicalDevice(std::string& error) {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    int bestScore = -1;
    std::string rejected;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);

        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f11.pNext = &f12;
        f12.pNext = &f13;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &f11;
        vkGetPhysicalDeviceFeatures2(pd, &features);

        const bool featuresOk = props.apiVersion >= VK_API_VERSION_1_3 && f13.dynamicRendering &&
                                f13.synchronization2 && f13.shaderDemoteToHelperInvocation &&
                                f12.descriptorIndexing && f12.runtimeDescriptorArray &&
                                f12.descriptorBindingPartiallyBound &&
                                f12.descriptorBindingSampledImageUpdateAfterBind &&
                                f12.descriptorBindingStorageBufferUpdateAfterBind &&
                                f12.descriptorBindingUpdateUnusedWhilePending &&
                                f12.shaderSampledImageArrayNonUniformIndexing && f12.bufferDeviceAddress &&
                                f12.timelineSemaphore && f12.drawIndirectCount && f11.shaderDrawParameters &&
                                features.features.multiDrawIndirect &&
                                features.features.textureCompressionBC && features.features.shaderInt64;

        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, families.data());
        std::optional<std::uint32_t> family;
        for (std::uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                family = i;
                break;
            }
        }

        if (!featuresOk || !family) {
            rejected += std::string(" ") + props.deviceName + (featuresOk ? " (no present queue)" : " (missing features)");
            continue;
        }
        const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                          : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                         : 0;
        if (score > bestScore) {
            bestScore = score;
            physical_ = pd;
            queueFamily_ = *family;
            adapterName_ = props.deviceName;
            // Streaming copies: a transfer-only family (the copy engines) if there is one, else a
            // second queue of the graphics family, else the graphics queue itself.
            transferFamily_ = *family;
            transferQueueIndex_ = families[*family].queueCount > 1 ? 1 : 0;
            for (std::uint32_t i = 0; i < familyCount; ++i) {
                const VkQueueFlags flags = families[i].queueFlags;
                if ((flags & VK_QUEUE_TRANSFER_BIT) && !(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
                    transferFamily_ = i;
                    transferQueueIndex_ = 0;
                    break;
                }
            }
        }
    }
    if (physical_ == VK_NULL_HANDLE) {
        error = "no GPU supports Vulkan 1.3 with bindless textures and BC compression;" + rejected;
        return false;
    }
    return true;
}

bool VulkanDevice::CreateLogicalDevice(std::string& error) {
    const float priorities[2] = {1.0f, 0.5f};
    std::array<VkDeviceQueueCreateInfo, 2> queueInfos{};
    std::uint32_t queueInfoCount = 1;
    queueInfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfos[0].queueFamilyIndex = queueFamily_;
    queueInfos[0].queueCount = transferFamily_ == queueFamily_ ? transferQueueIndex_ + 1 : 1;
    queueInfos[0].pQueuePriorities = priorities;
    if (transferFamily_ != queueFamily_) {
        queueInfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfos[1].queueFamilyIndex = transferFamily_;
        queueInfos[1].queueCount = 1;
        queueInfos[1].pQueuePriorities = &priorities[1];
        queueInfoCount = 2;
    }

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE; // DXC compiles HLSL `discard` to OpDemoteToHelperInvocation
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.drawIndirectCount = VK_TRUE; // GPU-built draw lists
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE; // shaders read buffers through 64-bit pointers (vk::RawBufferLoad)
    f12.timelineSemaphore = VK_TRUE;   // frames in flight
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    f11.shaderDrawParameters = VK_TRUE; // the indirect draw's first instance in shaders (BaseInstance)
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f11;
    features.features.textureCompressionBC = VK_TRUE;
    features.features.shaderInt64 = VK_TRUE; // 64-bit buffer addresses in shaders
    features.features.multiDrawIndirect = VK_TRUE;
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_, &supported);
    features.features.samplerAnisotropy = supported.samplerAnisotropy;
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_, &props);
        if (supported.samplerAnisotropy) maxAnisotropy_ = std::max(1.0f, props.limits.maxSamplerAnisotropy);
        const VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
        for (const std::uint32_t n : {8u, 4u, 2u}) {
            if (counts & n) {
                maxSamples_ = n;
                break;
            }
        }
    }
    wireframeSupported_ = supported.fillModeNonSolid == VK_TRUE;
    features.features.fillModeNonSolid = supported.fillModeNonSolid;

    // The memory budget extension, when the driver has it (VMA reads the budget through it).
    std::uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount, available.data());
    for (const VkExtensionProperties& e : available) {
        if (std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) memoryBudget_ = true;
    }
    std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (memoryBudget_) extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &features;
    info.queueCreateInfoCount = queueInfoCount;
    info.pQueueCreateInfos = queueInfos.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    if (vkCreateDevice(physical_, &info, nullptr, &device_) != VK_SUCCESS) {
        error = "vkCreateDevice failed";
        return false;
    }
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    vkGetDeviceQueue(device_, transferFamily_, transferQueueIndex_, &transferQueue_);
    transferShared_ = transferQueue_ == queue_;
    ownershipTransfer_ = transferFamily_ != queueFamily_;
    return true;
}

namespace {
// The fixed sampler table (SamplerIndex order).
const struct {
    VkFilter filter;
    VkSamplerAddressMode address;
} kSamplerSpecs[] = {
    {VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT},
    {VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE},
    {VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT},
    {VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE},
};
static_assert(std::size(kSamplerSpecs) == static_cast<std::size_t>(SamplerIndex::Count));
} // namespace

VkSampler VulkanDevice::MakeSampler(std::size_t index, float anisotropy) const {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = info.minFilter = kSamplerSpecs[index].filter;
    info.mipmapMode = kSamplerSpecs[index].filter == VK_FILTER_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = info.addressModeV = info.addressModeW = kSamplerSpecs[index].address;
    info.maxLod = VK_LOD_CLAMP_NONE;
    if (anisotropy > 1.0f) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(anisotropy, maxAnisotropy_);
    }
    VkSampler sampler = VK_NULL_HANDLE;
    SR_VK_CHECK(vkCreateSampler(device_, &info, nullptr, &sampler));
    return sampler;
}

void VulkanDevice::WriteSamplers() {
    std::array<VkDescriptorImageInfo, static_cast<std::size_t>(SamplerIndex::Count)> infos{};
    for (std::size_t i = 0; i < infos.size(); ++i) infos[i].sampler = samplers_[i];
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindlessSet_;
    write.dstBinding = 1;
    write.descriptorCount = static_cast<std::uint32_t>(infos.size());
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = infos.data();
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanDevice::SetAnisotropy(float level) {
    level = std::clamp(level, 1.0f, maxAnisotropy_);
    if (level == anisotropy_ || bindlessSet_ == VK_NULL_HANDLE) return;
    // The sampler binding is not update-after-bind: nothing may be in flight.
    DeviceWaitIdle();
    anisotropy_ = level;
    const auto wrap = static_cast<std::size_t>(SamplerIndex::LinearWrap);
    const VkSampler old = samplers_[wrap];
    samplers_[wrap] = MakeSampler(wrap, level);
    WriteSamplers();
    vkDestroySampler(device_, old, nullptr);
}

bool VulkanDevice::CreateBindlessLayout(std::string& error) {
    for (std::size_t i = 0; i < samplers_.size(); ++i) samplers_[i] = MakeSampler(i, 1.0f);

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = kMaxBindlessTextures;
    bindings[0].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = static_cast<std::uint32_t>(samplers_.size());
    bindings[1].stageFlags = VK_SHADER_STAGE_ALL;
    // Not immutable: the anisotropy option replaces LinearWrap (SetAnisotropy).
    // Storage buffers, indexed by their handle's slot (StorageIndex): what compute shaders need
    // atomics on (GPU-built draw lists); plain reads go by buffer address.
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = kMaxBindlessBuffers;
    bindings[2].stageFlags = VK_SHADER_STAGE_ALL;

    const VkDescriptorBindingFlags bindless = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                              VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                              VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const VkDescriptorBindingFlags bindingFlags[3] = {bindless, 0, bindless};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = 3;
    flagsInfo.pBindingFlags = bindingFlags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout_) != VK_SUCCESS) {
        error = "vkCreateDescriptorSetLayout (bindless) failed";
        return false;
    }

    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxBindlessTextures},
                                           {VK_DESCRIPTOR_TYPE_SAMPLER, static_cast<std::uint32_t>(samplers_.size())},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxBindlessBuffers}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        error = "vkCreateDescriptorPool failed";
        return false;
    }

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &setLayout_;
    if (vkAllocateDescriptorSets(device_, &allocInfo, &bindlessSet_) != VK_SUCCESS) {
        error = "vkAllocateDescriptorSets failed";
        return false;
    }
    WriteSamplers();

    VkPushConstantRange range{VK_SHADER_STAGE_ALL, 0, kPushConstantSize};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        error = "vkCreatePipelineLayout failed";
        return false;
    }
    return true;
}

bool VulkanDevice::CreateFrames(std::string& error) {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = queueFamily_;
    VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    // Frames in flight are tracked by one timeline semaphore: frame N signals the value N.
    VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineType.initialValue = 0;
    VkSemaphoreCreateInfo timelineInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    timelineInfo.pNext = &timelineType;
    if (vkCreateSemaphore(device_, &timelineInfo, nullptr, &frameTimeline_) != VK_SUCCESS) {
        error = "cannot create the frame timeline semaphore";
        return false;
    }
    SetName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<std::uint64_t>(frameTimeline_), "frame timeline");

    for (FrameData& frame : frames_) {
        if (vkCreateCommandPool(device_, &poolInfo, nullptr, &frame.pool) != VK_SUCCESS) {
            error = "vkCreateCommandPool failed";
            return false;
        }
        cmdInfo.commandPool = frame.pool;
        SR_VK_CHECK(vkAllocateCommandBuffers(device_, &cmdInfo, &frame.cmd));
        SR_VK_CHECK(vkCreateSemaphore(device_, &semInfo, nullptr, &frame.imageAcquired));
    }

    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    SR_VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &uploadPool_));

    // GPU frame time: a timestamp at the start and at the end of each frame's commands.
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &familyCount, families.data());
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    const std::uint32_t validBits = queueFamily_ < familyCount ? families[queueFamily_].timestampValidBits : 0;
    if (validBits > 0 && props.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = kQueriesPerFrame * kFramesInFlight;
        if (vkCreateQueryPool(device_, &queryInfo, nullptr, &timestamps_) == VK_SUCCESS) {
            timestampPeriodNs_ = props.limits.timestampPeriod;
            timestampMask_ = validBits >= 64 ? ~0ull : (1ull << validBits) - 1;
        }
    }
    return true;
}

bool VulkanDevice::CreateStreaming(const DeviceDesc& desc, std::string& error) {
    VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo timelineInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    timelineInfo.pNext = &timelineType;
    if (vkCreateSemaphore(device_, &timelineInfo, nullptr, &uploadTimeline_) != VK_SUCCESS) {
        error = "cannot create the upload timeline semaphore";
        return false;
    }
    SetName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<std::uint64_t>(uploadTimeline_), "upload timeline");

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = transferFamily_;
    for (TransferSlot& slot : transfers_) {
        SR_VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &slot.pool));
        VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdInfo.commandPool = slot.pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdInfo.commandBufferCount = 1;
        SR_VK_CHECK(vkAllocateCommandBuffers(device_, &cmdInfo, &slot.cmd));
    }

    stagingCapacity_ = std::max<std::size_t>(desc.stagingBytes, 1u << 20) / kStagingAlignment * kStagingAlignment;
    VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = stagingCapacity_;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT; // DST: the start-up touch
    VmaAllocationCreateInfo stagingInfo{};
    stagingInfo.usage = VMA_MEMORY_USAGE_AUTO;
    stagingInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(allocator_, &bufInfo, &stagingInfo, &stagingBuffer_, &stagingAlloc_, &mapped) != VK_SUCCESS) {
        error = "cannot create the staging ring";
        return false;
    }
    stagingCpu_ = static_cast<std::byte*>(mapped.pMappedData);
    SetName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(stagingBuffer_), "staging ring");

    // Streamed textures come from blocks reserved now: no device allocation during play. The
    // memory type is the one a typical streamed texture (BC1, mips) wants.
    if (desc.textureReserveBytes > 0) {
        VkImageCreateInfo probe{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        probe.imageType = VK_IMAGE_TYPE_2D;
        probe.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        probe.extent = {256, 256, 1};
        probe.mipLevels = 9;
        probe.arrayLayers = 1;
        probe.samples = VK_SAMPLE_COUNT_1_BIT;
        probe.tiling = VK_IMAGE_TILING_OPTIMAL;
        probe.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo probeAlloc{};
        probeAlloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        std::uint32_t memoryType = 0;
        if (vmaFindMemoryTypeIndexForImageInfo(allocator_, &probe, &probeAlloc, &memoryType) == VK_SUCCESS) {
            VmaPoolCreateInfo pool{};
            pool.memoryTypeIndex = memoryType;
            pool.blockSize = kTextureBlockSize;
            // At most 40% of the device-local memory the driver budgets for us (small GPUs).
            std::size_t reserve = desc.textureReserveBytes;
            const MemoryBudget budget = GetMemoryBudget();
            if (budget.budget > 0) reserve = std::min<std::size_t>(reserve, budget.budget * 2 / 5);
            pool.minBlockCount = std::max<std::size_t>(1, reserve / kTextureBlockSize);
            textureReserve_ = pool.minBlockCount * kTextureBlockSize;
            if (vmaCreatePool(allocator_, &pool, &texturePool_) != VK_SUCCESS) texturePool_ = nullptr;
        }
        if (texturePool_ == nullptr) std::fprintf(stderr, "[rhi] no reserved texture memory; streamed textures use the default heap\n");
    }
    // Each frame slot's first transient chunk (UpdateBuffer, palettes, UI) exists from the start:
    // created on first use, it cost the first frame that drew a scene 6 ms.
    for (FrameData& frame : frames_) {
        const BufferHandle handle = CreateBuffer(
            {kTransientChunkSize, BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Uniform | BufferUsage::Storage, "transient"}, {});
        if (!handle) continue;
        frame.chunks.push_back({handle, static_cast<std::byte*>(buffers_.Get(handle)->mapped), kTransientChunkSize,
                                buffers_.Get(handle)->address});
    }
    WarmReservedMemory();
    return true;
}

void VulkanDevice::WarmReservedMemory() {
    // Windows makes memory resident when the GPU first uses it, and every queue waits meanwhile
    // (measured: about 11 ms per 256 MiB block, both queues). Touch what is
    // reserved now, at start: the texture blocks, the staging ring and the transient chunks.
    std::vector<std::pair<VkBuffer, VmaAllocation>> touched; // temporary buffers over the texture blocks
    if (texturePool_ != nullptr) {
        VmaDetailedStatistics stats{};
        vmaCalculatePoolStatistics(allocator_, texturePool_, &stats);
        for (std::uint32_t i = 0; i < stats.statistics.blockCount; ++i) {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = kTextureBlockSize;
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.pool = texturePool_;
            alloc.flags = VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            if (vmaCreateBuffer(allocator_, &info, &alloc, &buffer, &allocation, nullptr) != VK_SUCCESS) break; // type not for buffers
            touched.emplace_back(buffer, allocation);
        }
    }
    RecordUpload([&](VkCommandBuffer cmd) {
        for (const auto& [buffer, allocation] : touched) vkCmdFillBuffer(cmd, buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, stagingBuffer_, 0, VK_WHOLE_SIZE, 0);
        for (const FrameData& frame : frames_) {
            for (const TransientChunk& chunk : frame.chunks) vkCmdFillBuffer(cmd, GetBuffer(chunk.handle), 0, VK_WHOLE_SIZE, 0);
        }
    });
    {
        std::lock_guard lock(resourceMutex_);
        SubmitUploads();
    }
    DeviceWaitIdle();
    {
        std::lock_guard lock(resourceMutex_);
        ReclaimUploads(true);
    }
    for (const auto& [buffer, allocation] : touched) vmaDestroyBuffer(allocator_, buffer, allocation); // the pool keeps its blocks
}

bool VulkanDevice::CreateSwapchain(std::uint32_t width, std::uint32_t height, std::string& error) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);
    if (caps.currentExtent.width != UINT32_MAX) {
        extent_ = caps.currentExtent;
    } else {
        extent_.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent_.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent_.width == 0 || extent_.height == 0) {
        return true; // minimized; created later by Resize()
    }

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &formatCount, formats.data());
    // UNORM on purpose: the original game rendered without sRGB conversion.
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{} : formats[0];
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    if (swapchainFormat_ != VK_FORMAT_UNDEFINED && chosen.format != swapchainFormat_) {
        error = "swapchain format changed; pipelines would need rebuilding";
        return false;
    }
    swapchainFormat_ = chosen.format;

    std::uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &modeCount, modes.data());
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        for (VkPresentModeKHR preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
                presentMode = preferred;
                break;
            }
        }
    }
    if (!swapchainLogged_) {
        swapchainLogged_ = true;
        std::string offered;
        for (VkPresentModeKHR m : modes) offered += std::to_string(int(m)) + " ";
        std::fprintf(stderr, "[rhi] present modes offered: %s(0 immediate, 1 mailbox, 2 fifo, 3 relaxed); using %d\n", offered.c_str(), int(presentMode));
    }

    swapchainCanCopy_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount != 0) info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (swapchainCanCopy_ ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) {
        error = "vkCreateSwapchainKHR failed";
        return false;
    }

    std::uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());
    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkImage image : swapchainImages_) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        SR_VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &view));
        swapchainViews_.push_back(view);
        VkSemaphore sem = VK_NULL_HANDLE;
        SR_VK_CHECK(vkCreateSemaphore(device_, &semInfo, nullptr, &sem));
        renderFinished_.push_back(sem);
    }
    return true;
}

void VulkanDevice::DestroySwapchain() {
    for (VkImageView view : swapchainViews_) vkDestroyImageView(device_, view, nullptr);
    for (VkSemaphore sem : renderFinished_) vkDestroySemaphore(device_, sem, nullptr);
    swapchainViews_.clear();
    renderFinished_.clear();
    swapchainImages_.clear();
    if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanDevice::Resize(std::uint32_t width, std::uint32_t height) {
    requestedWidth_ = width;
    requestedHeight_ = height;
    swapchainDirty_ = true;
}

void VulkanDevice::RecreateSwapchainIfNeeded() {
    if (!swapchainDirty_ && swapchain_ != VK_NULL_HANDLE) return;
    DeviceWaitIdle();
    DestroySwapchain();
    std::string error;
    if (!CreateSwapchain(requestedWidth_, requestedHeight_, error)) {
        std::fprintf(stderr, "[rhi] swapchain: %s\n", error.c_str());
    }
    swapchainDirty_ = false;
}

VulkanDevice::~VulkanDevice() {
    if (device_ == VK_NULL_HANDLE) {
        if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
        return;
    }
    DeviceWaitIdle();
    FlushDeletions(true);
    for (UploadBatch& b : uploads_) {
        for (const auto& [buffer, allocation] : b.staging) vmaDestroyBuffer(allocator_, buffer, allocation);
        vkDestroyFence(device_, b.fence, nullptr);
    }
    uploads_.clear();
    if (timestamps_ != VK_NULL_HANDLE) vkDestroyQueryPool(device_, timestamps_, nullptr);

    textures_.ForEachAlive([this](VkTexture& t) {
        vkDestroyImageView(device_, t.view, nullptr);
        vmaDestroyImage(allocator_, t.image, t.allocation);
    });
    if (whiteTexture_.image != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, whiteTexture_.view, nullptr);
        vmaDestroyImage(allocator_, whiteTexture_.image, whiteTexture_.allocation);
    }
    if (texturePool_ != nullptr) vmaDestroyPool(allocator_, texturePool_); // after every texture in it
    if (nullStorage_.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(allocator_, nullStorage_.buffer, nullStorage_.allocation);
    for (TransferSlot& slot : transfers_) {
        if (slot.pool != VK_NULL_HANDLE) vkDestroyCommandPool(device_, slot.pool, nullptr);
    }
    if (uploadTimeline_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, uploadTimeline_, nullptr);
    if (stagingBuffer_ != VK_NULL_HANDLE) vmaDestroyBuffer(allocator_, stagingBuffer_, stagingAlloc_);
    buffers_.ForEachAlive([this](VkBufferRecord& b) { vmaDestroyBuffer(allocator_, b.buffer, b.allocation); });
    pipelines_.ForEachAlive([this](VkPipelineRecord& p) {
        if (p.compute != VK_NULL_HANDLE) vkDestroyPipeline(device_, p.compute, nullptr);
        if (!p.variants) return;
        for (auto& v : p.variants->pipelines) {
            const VkPipeline built = v.exchange(VK_NULL_HANDLE);
            if (built != VK_NULL_HANDLE) vkDestroyPipeline(device_, built, nullptr);
        }
    });

    for (FrameData& frame : frames_) {
        vkDestroyCommandPool(device_, frame.pool, nullptr);
        vkDestroySemaphore(device_, frame.imageAcquired, nullptr);
        for (ChunkRecorder& r : frame.recorders) vkDestroyCommandPool(device_, r.pool, nullptr);
    }
    if (frameTimeline_ != VK_NULL_HANDLE) vkDestroySemaphore(device_, frameTimeline_, nullptr);
    vkDestroyCommandPool(device_, uploadPool_, nullptr);
    DestroySwapchain();
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    for (VkSampler s : samplers_) vkDestroySampler(device_, s, nullptr);
    if (allocator_ != nullptr) vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
    if (surface_ != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_ != VK_NULL_HANDLE) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy != nullptr) destroy(instance_, messenger_, nullptr);
    }
    vkDestroyInstance(instance_, nullptr);
}

// ---------------------------------------------------------------- helpers

void VulkanDevice::SetName(VkObjectType type, std::uint64_t handle, const std::string& name) {
    if (setObjectName_ == nullptr || name.empty()) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name.c_str();
    setObjectName_(device_, &info);
}

void VulkanDevice::RecordUpload(const std::function<void(VkCommandBuffer)>& record, VkBuffer staging,
                                VmaAllocation stagingAlloc, std::size_t bytes) {
    std::lock_guard lock(resourceMutex_);
    if (openUpload_ < 0) {
        ReclaimUploads(false);
        int free = -1;
        for (std::size_t i = 0; i < uploads_.size() && free < 0; ++i) {
            if (!uploads_[i].inFlight) free = static_cast<int>(i);
        }
        if (free < 0) {
            UploadBatch batch;
            VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cmdInfo.commandPool = uploadPool_;
            cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cmdInfo.commandBufferCount = 1;
            SR_VK_CHECK(vkAllocateCommandBuffers(device_, &cmdInfo, &batch.cmd));
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            SR_VK_CHECK(vkCreateFence(device_, &fenceInfo, nullptr, &batch.fence));
            uploads_.push_back(std::move(batch));
            free = static_cast<int>(uploads_.size()) - 1;
        }
        UploadBatch& batch = uploads_[static_cast<std::size_t>(free)];
        SR_VK_CHECK(vkResetCommandBuffer(batch.cmd, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        SR_VK_CHECK(vkBeginCommandBuffer(batch.cmd, &begin));
        openUpload_ = free;
    }
    UploadBatch& batch = uploads_[static_cast<std::size_t>(openUpload_)];
    record(batch.cmd);
    if (staging != VK_NULL_HANDLE) batch.staging.emplace_back(staging, stagingAlloc);
    batch.bytes += bytes;
    ++uploadCount_;
    uploadBytes_ += bytes;
    // Loading with no frames in between (minimized, a loading screen) must not hold
    // unbounded staging memory.
    if (batch.bytes >= kUploadBatchLimit) SubmitUploads();
}

void VulkanDevice::SubmitUploads() {
    if (openUpload_ < 0) return;
    UploadBatch& batch = uploads_[static_cast<std::size_t>(openUpload_)];
    openUpload_ = -1;
    SR_VK_CHECK(vkEndCommandBuffer(batch.cmd));
    // Same queue as the frames: the batch's barriers (ALL_COMMANDS scopes) order its copies
    // before every frame submitted after it.
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = batch.cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    {
        std::lock_guard queue(queueMutex_);
        SR_VK_CHECK(vkQueueSubmit2(queue_, 1, &submit, batch.fence));
    }
    batch.inFlight = true;
    batch.bytes = 0;
}

void VulkanDevice::ReclaimUploads(bool wait) {
    for (UploadBatch& batch : uploads_) {
        if (!batch.inFlight) continue;
        if (wait) {
            SR_VK_CHECK(vkWaitForFences(device_, 1, &batch.fence, VK_TRUE, UINT64_MAX));
        } else if (vkGetFenceStatus(device_, batch.fence) != VK_SUCCESS) {
            continue;
        }
        SR_VK_CHECK(vkResetFences(device_, 1, &batch.fence));
        for (const auto& [buffer, allocation] : batch.staging) vmaDestroyBuffer(allocator_, buffer, allocation);
        batch.staging.clear();
        batch.inFlight = false;
    }
}

void VulkanDevice::WriteStorageDescriptor(std::uint32_t slot, VkBuffer buffer) {
    VkDescriptorBufferInfo bufferInfo{buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindlessSet_;
    write.dstBinding = 2;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanDevice::WriteBindlessDescriptor(std::uint32_t slot, VkImageView view) {
    VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = bindlessSet_;
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanDevice::FlushDeletions(bool everything) {
    // Due deletions run outside the lock (workers create resources meanwhile), and at most for a
    // short time a frame: streaming evicts hundreds of textures in a burst, and destroying them
    // all at once cost a 6 ms frame. The rest wait for the next frame.
    std::vector<PendingDeletion> due;
    {
        std::lock_guard lock(resourceMutex_);
        std::size_t kept = 0;
        for (PendingDeletion& d : deletions_) {
            if (everything || d.safeAfter <= completedFrames_) {
                due.push_back(std::move(d));
            } else {
                deletions_[kept++] = std::move(d);
            }
        }
        deletions_.resize(kept);
    }
    const auto start = std::chrono::steady_clock::now();
    std::size_t done = 0;
    for (; done < due.size(); ++done) {
        if (!everything && done % 8 == 0 && std::chrono::steady_clock::now() - start > kDeletionBudget) break;
        due[done].destroy();
    }
    if (done < due.size()) {
        std::lock_guard lock(resourceMutex_);
        deletions_.insert(deletions_.end(), std::make_move_iterator(due.begin() + static_cast<std::ptrdiff_t>(done)),
                          std::make_move_iterator(due.end()));
    }
}

// ---------------------------------------------------------------- resources

TextureHandle VulkanDevice::CreateTexture(const TextureDesc& desc, const TextureInitData* init) {
    VkTexture tex;
    tex.format = desc.format;
    tex.usage = desc.usage;
    tex.width = desc.width;
    tex.height = desc.height;
    tex.mips = desc.usage == TextureUsage::Sampled ? std::max(desc.mipLevels, 1u) : 1;
    const bool isDepth = desc.usage == TextureUsage::DepthStencil || desc.usage == TextureUsage::ShadowMap;
    tex.samples = desc.usage == TextureUsage::Sampled ? VK_SAMPLE_COUNT_1_BIT : SampleBits(std::min(desc.samples, maxSamples_));
    const bool multisampled = tex.samples != VK_SAMPLE_COUNT_1_BIT;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = ToVk(desc.format);
    imageInfo.extent = {desc.width, desc.height, 1};
    imageInfo.mipLevels = tex.mips;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = tex.samples;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    switch (desc.usage) {
    case TextureUsage::Sampled:
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        break;
    case TextureUsage::RenderTarget:
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (multisampled ? 0 : VK_IMAGE_USAGE_SAMPLED_BIT);
        break;
    case TextureUsage::DepthStencil:
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        break;
    case TextureUsage::ShadowMap:
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        break;
    }
    tex.streamed = desc.streamed && desc.usage == TextureUsage::Sampled;
    // Streamed textures are shared by the graphics and transfer families:
    // the transfer queue leaves them ready to sample, so frames record nothing per arriving image
    // (thousands a second in a burst) and no release/acquire pair can go out of step. The price,
    // drivers skipping colour compression on shared images, does not apply to the block-compressed
    // formats most game textures use.
    const std::uint32_t sharedFamilies[2] = {queueFamily_, transferFamily_};
    if (tex.streamed && ownershipTransfer_) {
        imageInfo.sharingMode = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = 2;
        imageInfo.pQueueFamilyIndices = sharedFamilies;
    }
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkResult result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (tex.streamed && texturePool_ != nullptr) {
        // From the reserved blocks; never grows them (a full pool falls back to the default heap).
        VmaAllocationCreateInfo poolAlloc{};
        poolAlloc.pool = texturePool_;
        poolAlloc.flags = VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
        result = vmaCreateImage(allocator_, &imageInfo, &poolAlloc, &tex.image, &tex.allocation, nullptr);
    }
    if (result != VK_SUCCESS) result = vmaCreateImage(allocator_, &imageInfo, &allocInfo, &tex.image, &tex.allocation, nullptr);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[rhi] vmaCreateImage failed for '%s'\n", desc.debugName.c_str());
        return {};
    }
    SetName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(tex.image), desc.debugName);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageInfo.format;
    const VkImageAspectFlags aspect = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange = {aspect, 0, tex.mips, 0, 1};
    SR_VK_CHECK(vkCreateImageView(device_, &viewInfo, nullptr, &tex.view));

    if ((isDepth && desc.usage != TextureUsage::ShadowMap) || multisampled) {
        // Depth and multisampled targets are never sampled (no bindless slot); a pass starts
        // them from UNDEFINED (or keeps a multisampled colour it loads).
        const VkTexture created = tex;
        const TextureHandle handle = textures_.Add(std::move(tex));
        if (!handle) {
            vkDestroyImageView(device_, created.view, nullptr);
            vmaDestroyImage(allocator_, created.image, created.allocation);
        }
        return handle;
    }

    if (tex.streamed) {
        // The transfer that fills it also moves it to SHADER_READ_ONLY (SubmitStaging). Until then
        // its bindless slot shows the white texture: nothing may sample it before IsUploaded, and
        // a stray read must not see an image in the wrong layout.
        const VkTexture done = tex;
        const TextureHandle handle = textures_.Add(std::move(tex));
        if (!handle) {
            std::fprintf(stderr, "[rhi] bindless texture table is full (%u)\n", kMaxBindlessTextures);
            vkDestroyImageView(device_, done.view, nullptr);
            vmaDestroyImage(allocator_, done.image, done.allocation);
            return {};
        }
        std::lock_guard lock(resourceMutex_);
        WriteBindlessDescriptor(handle.index, whiteTexture_.view);
        return handle;
    }

    // Upload through a staging buffer: all mips back to back. Mip sizes are
    // multiples of 4 (RGBA8) or of the block size (BC), as Vulkan requires.
    std::size_t stagingSize = 0;
    const std::uint32_t initMips = init != nullptr ? std::min<std::uint32_t>(tex.mips, static_cast<std::uint32_t>(init->mips.size())) : 0;
    for (std::uint32_t m = 0; m < initMips; ++m) stagingSize += init->mips[m].size();

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    if (stagingSize > 0) {
        VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = stagingSize;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingInfo{};
        stagingInfo.usage = VMA_MEMORY_USAGE_AUTO;
        stagingInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mapped{};
        SR_VK_CHECK(vmaCreateBuffer(allocator_, &bufInfo, &stagingInfo, &staging, &stagingAlloc, &mapped));
        std::size_t off = 0;
        for (std::uint32_t m = 0; m < initMips; ++m) {
            std::memcpy(static_cast<std::byte*>(mapped.pMappedData) + off, init->mips[m].data(), init->mips[m].size());
            off += init->mips[m].size();
        }
    }

    RecordUpload([&](VkCommandBuffer cmd) {
        if (staging == VK_NULL_HANDLE) {
            ImageBarrier(cmd, tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, aspect);
            return;
        }
        ImageBarrier(cmd, tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        std::vector<VkBufferImageCopy> regions;
        std::size_t off = 0;
        for (std::uint32_t m = 0; m < initMips; ++m) {
            VkBufferImageCopy region{};
            region.bufferOffset = off;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            region.imageExtent = {std::max(desc.width >> m, 1u), std::max(desc.height >> m, 1u), 1};
            regions.push_back(region);
            off += init->mips[m].size();
        }
        vkCmdCopyBufferToImage(cmd, staging, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(regions.size()), regions.data());
        ImageBarrier(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }, staging, stagingAlloc, stagingSize);

    const VkTexture created = tex;
    const TextureHandle handle = textures_.Add(std::move(tex));
    if (!handle) {
        std::fprintf(stderr, "[rhi] bindless texture table is full (%u)\n", kMaxBindlessTextures);
        vkDestroyImageView(device_, created.view, nullptr);
        vmaDestroyImage(allocator_, created.image, created.allocation);
        return {};
    }
    {
        std::lock_guard lock(resourceMutex_);
        WriteBindlessDescriptor(handle.index, created.view);
    }
    return handle;
}

void VulkanDevice::UpdateTexture(TextureHandle texture, Rect region, std::span<const std::byte> pixels,
                                 std::uint32_t rowPitch) {
    const VkTexture* tex = textures_.Get(texture);
    if (tex == nullptr || tex->usage != TextureUsage::Sampled || IsBlockCompressed(tex->format) ||
        region.width == 0 || region.height == 0) {
        return;
    }
    const std::uint32_t bpp = 4;

    VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufInfo.size = pixels.size();
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stagingInfo{};
    stagingInfo.usage = VMA_MEMORY_USAGE_AUTO;
    stagingInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAlloc = nullptr;
    VmaAllocationInfo mapped{};
    SR_VK_CHECK(vmaCreateBuffer(allocator_, &bufInfo, &stagingInfo, &staging, &stagingAlloc, &mapped));
    std::memcpy(mapped.pMappedData, pixels.data(), pixels.size());

    // The barriers' ALL_COMMANDS scopes order this against frames already
    // submitted and frames submitted later on the same queue.
    RecordUpload([&](VkCommandBuffer cmd) {
        ImageBarrier(cmd, tex->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.bufferRowLength = rowPitch / bpp;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {region.x, region.y, 0};
        copy.imageExtent = {region.width, region.height, 1};
        vkCmdCopyBufferToImage(cmd, staging, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        ImageBarrier(cmd, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }, staging, stagingAlloc, pixels.size());
}

void VulkanDevice::DestroyTexture(TextureHandle texture) {
    std::optional<VkTexture> tex = textures_.Retire(texture);
    if (!tex) return;
    DeferDeletion([this, t = *tex, slot = texture.index] {
        // No pending frame uses the slot any more: point it at the white
        // texture (so a stale index never reads freed memory), then recycle it.
        {
            std::lock_guard lock(resourceMutex_);
            WriteBindlessDescriptor(slot, whiteTexture_.view);
        }
        textures_.Release(slot);
        vkDestroyImageView(device_, t.view, nullptr);
        vmaDestroyImage(allocator_, t.image, t.allocation);
    });
}

BufferHandle VulkanDevice::CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initialData) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<std::size_t>(desc.size, 4);
    const auto usage = static_cast<std::uint32_t>(desc.usage);
    if (usage & static_cast<std::uint32_t>(BufferUsage::Vertex)) info.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (usage & static_cast<std::uint32_t>(BufferUsage::Index)) info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (usage & static_cast<std::uint32_t>(BufferUsage::Uniform)) info.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (usage & static_cast<std::uint32_t>(BufferUsage::Storage)) info.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (usage & static_cast<std::uint32_t>(BufferUsage::Indirect)) info.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VmaAllocationCreateInfo allocInfo{};
    if (desc.cpuVisible) {
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        // SRC: CommandList::UpdateBuffer copies out of transient memory; DST: the start-up touch.
        info.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    } else {
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        info.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    // GPU-only buffers may be filled by the transfer queue (streaming): shared by both families,
    // so no ownership transfer is needed (for buffers it costs nothing).
    const std::uint32_t families[2] = {queueFamily_, transferFamily_};
    if (!desc.cpuVisible && ownershipTransfer_) {
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    }

    VkBufferRecord record;
    record.size = info.size;
    VmaAllocationInfo mapped{};
    if (vmaCreateBuffer(allocator_, &info, &allocInfo, &record.buffer, &record.allocation, &mapped) != VK_SUCCESS) {
        std::fprintf(stderr, "[rhi] vmaCreateBuffer failed for '%s'\n", desc.debugName.c_str());
        return {};
    }
    record.mapped = desc.cpuVisible ? mapped.pMappedData : nullptr;
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = record.buffer;
    record.address = vkGetBufferDeviceAddress(device_, &addressInfo);
    const std::size_t copySize = std::min(initialData.size(), record.size);
    if (copySize > 0 && desc.cpuVisible) {
        std::memcpy(record.mapped, initialData.data(), copySize);
    } else if (copySize > 0) {
        VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        stagingInfo.size = copySize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingAllocInfo{};
        stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAllocInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = nullptr;
        VmaAllocationInfo stagingMapped{};
        SR_VK_CHECK(vmaCreateBuffer(allocator_, &stagingInfo, &stagingAllocInfo, &staging, &stagingAlloc, &stagingMapped));
        std::memcpy(stagingMapped.pMappedData, initialData.data(), copySize);
        const VkBuffer target = record.buffer;
        RecordUpload([&](VkCommandBuffer cmd) {
            const VkBufferCopy region{0, 0, copySize};
            vkCmdCopyBuffer(cmd, staging, target, 1, &region);
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dep);
        }, staging, stagingAlloc, copySize);
    } else if (!desc.cpuVisible && record.size >= kWarmBufferBytes) {
        // A reservation (the geometry pool): made resident now, synchronously, so the GPU does
        // not stall the first time a frame or a transfer touches it (see WarmReservedMemory).
        const VkBuffer target = record.buffer;
        RecordUpload([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, target, 0, VK_WHOLE_SIZE, 0); });
        {
            std::lock_guard lock(resourceMutex_);
            SubmitUploads();
        }
        DeviceWaitIdle();
    }
    SetName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<std::uint64_t>(record.buffer), desc.debugName);
    record.storage = (usage & static_cast<std::uint32_t>(BufferUsage::Storage)) != 0;
    const BufferHandle handle = buffers_.Add(record);
    if (handle && record.storage && handle.index < kMaxBindlessBuffers) {
        std::lock_guard lock(resourceMutex_);
        WriteStorageDescriptor(handle.index, record.buffer);
    }
    return handle;
}

void VulkanDevice::DestroyBuffer(BufferHandle buffer) {
    std::optional<VkBufferRecord> record = buffers_.Retire(buffer);
    if (!record) return;
    DeferDeletion([this, b = *record, slot = buffer.index] {
        if (b.storage && slot < kMaxBindlessBuffers) {
            // No frame uses the slot any more: a stale index reads the placeholder, not freed memory.
            std::lock_guard lock(resourceMutex_);
            WriteStorageDescriptor(slot, nullStorage_.buffer);
        }
        buffers_.Release(slot);
        vmaDestroyBuffer(allocator_, b.buffer, b.allocation);
    });
}

PipelineHandle VulkanDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    auto source = std::make_shared<PipelineSource>();
    source->vertexCode.assign(desc.vertexShader.begin(), desc.vertexShader.end());
    source->fragmentCode.assign(desc.fragmentShader.begin(), desc.fragmentShader.end());
    source->vertexEntry = desc.vertexEntry;
    source->fragmentEntry = desc.fragmentEntry;
    source->desc = desc;
    source->desc.vertexShader = source->vertexCode;
    source->desc.fragmentShader = source->fragmentCode;
    source->desc.vertexEntry = source->vertexEntry.c_str();
    source->desc.fragmentEntry = source->fragmentEntry.c_str();
    VkPipelineRecord record;
    record.variants = std::make_shared<PipelineVariants>();
    const VkPipeline single = BuildPipeline(source->desc, VK_SAMPLE_COUNT_1_BIT);
    if (single == VK_NULL_HANDLE) return {};
    record.variants->pipelines[0].store(single, std::memory_order_release);
    record.source = std::move(source);
    return pipelines_.Add(std::move(record));
}

PipelineHandle VulkanDevice::CreateComputePipeline(const ComputePipelineDesc& desc) {
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = desc.shader.size();
    moduleInfo.pCode = reinterpret_cast<const std::uint32_t*>(desc.shader.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &moduleInfo, nullptr, &module) != VK_SUCCESS) return {};
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = desc.entry;
    info.layout = pipelineLayout_;
    VkPipelineRecord record;
    const VkResult result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &record.compute);
    vkDestroyShaderModule(device_, module, nullptr);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[rhi] compute pipeline '%s' failed: VkResult %d\n", desc.debugName.c_str(), result);
        return {};
    }
    SetName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(record.compute), desc.debugName);
    return pipelines_.Add(std::move(record));
}

VkPipeline VulkanDevice::BuildPipeline(const GraphicsPipelineDesc& desc, VkSampleCountFlagBits samples) {
    auto makeModule = [this](std::span<const std::byte> code) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = code.size();
        info.pCode = reinterpret_cast<const std::uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        SR_VK_CHECK(vkCreateShaderModule(device_, &info, nullptr, &module));
        return module;
    };
    const VkShaderModule vs = makeModule(desc.vertexShader);
    // No fragment shader: a depth-only pipeline (desc.depthOnly) keeps the GPU's early depth test.
    const VkShaderModule fs = desc.fragmentShader.empty() ? VK_NULL_HANDLE : makeModule(desc.fragmentShader);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = desc.vertexEntry;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = desc.fragmentEntry;

    VkVertexInputBindingDescription binding{0, desc.vertexStride, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const VertexAttribute& a : desc.attributes) {
        attributes.push_back({a.location, 0, ToVk(a.format), a.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (desc.vertexStride > 0) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = desc.wireframe && wireframeSupported_ ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    raster.cullMode = desc.cull == CullMode::Back    ? VK_CULL_MODE_BACK_BIT
                      : desc.cull == CullMode::Front ? VK_CULL_MODE_FRONT_BIT
                                                     : VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE; // Direct3D convention, like the original assets
    raster.lineWidth = 1.0f;
    if (desc.depthBiasConstant != 0.0f || desc.depthBiasSlope != 0.0f) {
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = desc.depthBiasConstant;
        raster.depthBiasSlopeFactor = desc.depthBiasSlope;
    }

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = samples;

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = desc.depthTest;
    depth.depthWriteEnable = desc.depthWrite;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    switch (desc.blend) {
    case BlendMode::Opaque:
        break;
    case BlendMode::AlphaBlend:
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::PremultipliedAlpha:
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case BlendMode::AddModulate:
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    }
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = desc.depthOnly ? 0 : 1;
    colorBlend.pAttachments = &blend;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    const VkFormat colorFormat = desc.colorFormat == Format::Unknown ? swapchainFormat_ : ToVk(desc.colorFormat);
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = desc.depthOnly ? 0 : 1;
    rendering.pColorAttachmentFormats = desc.depthOnly ? nullptr : &colorFormat;
    rendering.depthAttachmentFormat = ToVk(desc.depthFormat);

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = fs != VK_NULL_HANDLE ? 2 : 1;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    vkDestroyShaderModule(device_, vs, nullptr);
    if (fs != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fs, nullptr);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[rhi] pipeline '%s' (%dx) failed: VkResult %d\n", desc.debugName.c_str(), static_cast<int>(samples), result);
        return VK_NULL_HANDLE;
    }
    SetName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(pipeline), desc.debugName);
    return pipeline;
}

void VulkanDevice::DestroyPipeline(PipelineHandle pipeline) {
    std::optional<VkPipelineRecord> record = pipelines_.Retire(pipeline);
    if (!record) return;
    DeferDeletion([this, p = *record, slot = pipeline.index] {
        pipelines_.Release(slot);
        if (p.compute != VK_NULL_HANDLE) vkDestroyPipeline(device_, p.compute, nullptr);
        if (!p.variants) return;
        for (auto& v : p.variants->pipelines) {
            const VkPipeline built = v.exchange(VK_NULL_HANDLE);
            if (built != VK_NULL_HANDLE) vkDestroyPipeline(device_, built, nullptr);
        }
    });
}

TransientAllocation VulkanDevice::AllocateTransient(std::size_t size, std::size_t alignment) {
    // Only valid while recording: the slot's memory is recycled in BeginFrame().
    if (!frameActive_) {
        std::fprintf(stderr, "[rhi] AllocateTransient called outside BeginFrame/EndFrame\n");
        return {};
    }
    std::lock_guard lock(transientMutex_);
    FrameData& frame = frames_[frameIndex_];
    for (;;) {
        if (frame.chunkIndex < frame.chunks.size()) {
            TransientChunk& chunk = frame.chunks[frame.chunkIndex];
            const std::size_t offset = (frame.chunkOffset + alignment - 1) / alignment * alignment;
            if (offset + size <= chunk.size) {
                frame.chunkOffset = offset + size;
                return {chunk.mapped + offset, chunk.handle, offset, chunk.address + offset};
            }
            if (frame.chunkOffset > 0 || size <= chunk.size) {
                ++frame.chunkIndex; // this chunk is full; try the next one
                frame.chunkOffset = 0;
                continue;
            }
        }
        const std::size_t chunkSize = std::max(kTransientChunkSize, size);
        const BufferHandle handle = CreateBuffer(
            {chunkSize, BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Uniform | BufferUsage::Storage,
             "transient"},
            {});
        if (!handle) return {};
        frame.chunks.insert(frame.chunks.begin() + static_cast<std::ptrdiff_t>(frame.chunkIndex),
                            {handle, static_cast<std::byte*>(buffers_.Get(handle)->mapped), chunkSize,
                             buffers_.Get(handle)->address});
        frame.chunkOffset = 0;
    }
}

// ---------------------------------------------------------------- streaming

StagingBlock VulkanDevice::AllocateStaging(std::size_t size) {
    if (size == 0) return {};
    const std::size_t aligned = (size + kStagingAlignment - 1) / kStagingAlignment * kStagingAlignment;
    if (aligned > stagingCapacity_) {
        std::fprintf(stderr, "[rhi] a staging block of %zu bytes is larger than the ring\n", size);
        return {};
    }
    std::lock_guard lock(stagingMutex_);
    std::size_t offset = 0;
    if (ringBlocks_.empty()) {
        stagingHead_ = 0;
        offset = 0;
    } else {
        const std::size_t tail = ringBlocks_.front().offset;
        if (stagingHead_ > tail) {
            // Free: [head, end) and [0, tail).
            if (stagingCapacity_ - stagingHead_ >= aligned) {
                offset = stagingHead_;
            } else if (tail > aligned) {
                // Skip the end of the ring: a finished block holds it until the tail passes it.
                RingBlock pad;
                pad.id = nextBlockId_++;
                pad.offset = stagingHead_;
                pad.size = stagingCapacity_ - stagingHead_;
                pad.state = RingBlock::State::Done;
                ringBlocks_.push_back(pad);
                offset = 0;
            } else {
                return {};
            }
        } else if (stagingHead_ < tail && tail - stagingHead_ > aligned) {
            offset = stagingHead_; // free: [head, tail), kept from touching the tail
        } else {
            return {}; // head == tail with blocks: the ring is full
        }
    }
    RingBlock block;
    block.id = nextBlockId_++;
    block.offset = offset;
    block.size = aligned;
    ringBlocks_.push_back(block);
    stagingHead_ = offset + aligned;
    if (stagingHead_ == stagingCapacity_) stagingHead_ = 0;
    return {stagingCpu_ + offset, size, block.id};
}

namespace {
RingBlock* FindBlock(std::deque<RingBlock>& blocks, std::uint64_t id) {
    const auto it = std::lower_bound(blocks.begin(), blocks.end(), id,
                                     [](const RingBlock& b, std::uint64_t v) { return b.id < v; });
    return it != blocks.end() && it->id == id ? &*it : nullptr;
}
} // namespace

UploadId VulkanDevice::SubmitStaging(const StagingBlock& block, std::span<const StagedBufferCopy> buffers,
                                     std::span<const StagedTextureCopy> textures) {
    std::lock_guard lock(stagingMutex_);
    RingBlock* ring = FindBlock(ringBlocks_, block.id);
    if (ring == nullptr || ring->state != RingBlock::State::Writing) {
        std::fprintf(stderr, "[rhi] SubmitStaging: not an open staging block\n");
        return 0;
    }
    ring->state = RingBlock::State::Queued;
    QueuedUpload upload;
    upload.block = block.id;
    upload.offset = ring->offset;
    upload.size = ring->size;
    upload.id = nextUploadId_++;
    upload.buffers.assign(buffers.begin(), buffers.end());
    upload.textures.assign(textures.begin(), textures.end());
    const UploadId id = upload.id;
    queuedUploads_.push_back(std::move(upload));
    return id;
}

void VulkanDevice::CancelStaging(const StagingBlock& block) {
    std::lock_guard lock(stagingMutex_);
    RingBlock* ring = FindBlock(ringBlocks_, block.id);
    if (ring == nullptr || ring->state != RingBlock::State::Writing) return;
    ring->state = RingBlock::State::Done;
    while (!ringBlocks_.empty() && ringBlocks_.front().state == RingBlock::State::Done) ringBlocks_.pop_front();
}

Device::StreamingInfo VulkanDevice::GetStreamingInfo() const {
    StreamingInfo info;
    info.dedicatedTransferQueue = ownershipTransfer_;
    info.stagingCapacity = stagingCapacity_;
    info.textureReserve = textureReserve_;
    std::lock_guard lock(stagingMutex_);
    for (const RingBlock& b : ringBlocks_) info.stagingUsed += b.size;
    info.submitted = nextUploadId_ - 1;
    info.uploaded = visibleUpload_.load(std::memory_order_acquire);
    return info;
}

Device::MemoryBudget VulkanDevice::GetMemoryBudget() const {
    MemoryBudget out;
    if (!memoryBudget_) return out;
    const VkPhysicalDeviceMemoryProperties* props = nullptr;
    vmaGetMemoryProperties(allocator_, &props);
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator_, budgets.data());
    for (std::uint32_t h = 0; h < props->memoryHeapCount; ++h) {
        if ((props->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
        out.usage += budgets[h].usage;
        out.budget += budgets[h].budget;
    }
    return out;
}

void VulkanDevice::PumpStreaming() {
    std::uint64_t done = 0;
    vkGetSemaphoreCounterValue(device_, uploadTimeline_, &done);

    // Finished transfers become visible to this frame: their textures' bindless slots start showing
    // them, and the frame's submission waits for their timeline value (reached already; the wait
    // orders the memory). No barrier is needed: streamed images and the geometry are shared by
    // both queue families and left ready by the transfer.
    UploadId visible = visibleUpload_.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(resourceMutex_);
        for (TransferSlot& slot : transfers_) {
            if (slot.value == 0 || slot.visible || slot.value > done) continue;
            for (const TransferSlot::Image& t : slot.images) {
                if (textures_.Get(t.handle) != nullptr) WriteBindlessDescriptor(t.handle.index, t.view); // else destroyed meanwhile
            }
            slot.images.clear();
            slot.visible = true;
            frameUploadWait_ = std::max(frameUploadWait_, slot.value);
            visible = std::max(visible, slot.lastId);
        }
    }
    visibleUpload_.store(visible, std::memory_order_release);

    // Staging space of finished transfers returns to the ring.
    {
        std::lock_guard lock(stagingMutex_);
        for (RingBlock& b : ringBlocks_) {
            if (b.state == RingBlock::State::InFlight && b.value <= done) b.state = RingBlock::State::Done;
        }
        while (!ringBlocks_.empty() && ringBlocks_.front().state == RingBlock::State::Done) ringBlocks_.pop_front();
    }
    for (TransferSlot& slot : transfers_) {
        if (slot.value != 0 && slot.visible && slot.value <= done) slot.value = 0; // free again
    }

    FlushStaging();
}

bool VulkanDevice::FlushStaging() {
    TransferSlot* slot = nullptr;
    for (TransferSlot& s : transfers_) {
        if (s.value == 0) {
            slot = &s;
            break;
        }
    }
    if (slot == nullptr) return false; // every transfer still running: try next frame

    std::vector<QueuedUpload> batch;
    {
        std::lock_guard lock(stagingMutex_);
        std::size_t bytes = 0, taken = 0;
        while (taken < queuedUploads_.size() && (taken == 0 || bytes + queuedUploads_[taken].size <= kStreamBytesPerFrame)) {
            bytes += queuedUploads_[taken].size;
            ++taken;
        }
        if (taken == 0) return false;
        batch.assign(std::make_move_iterator(queuedUploads_.begin()),
                     std::make_move_iterator(queuedUploads_.begin() + static_cast<std::ptrdiff_t>(taken)));
        queuedUploads_.erase(queuedUploads_.begin(), queuedUploads_.begin() + static_cast<std::ptrdiff_t>(taken));
        const std::uint64_t value = uploadValue_ + 1;
        for (const QueuedUpload& u : batch) {
            if (RingBlock* b = FindBlock(ringBlocks_, u.block)) {
                b->state = RingBlock::State::InFlight;
                b->value = value;
            }
        }
    }

    SR_VK_CHECK(vkResetCommandPool(device_, slot->pool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    SR_VK_CHECK(vkBeginCommandBuffer(slot->cmd, &begin));
    slot->images.clear();
    std::vector<VkImageMemoryBarrier2> barriers;
    std::vector<VkBufferImageCopy> regions;
    std::uint32_t blocks = 0;
    std::uint64_t bytes = 0;
    for (const QueuedUpload& u : batch) {
        ++blocks;
        bytes += u.size;
        for (const StagedBufferCopy& c : u.buffers) {
            const VkBuffer dst = GetBuffer(c.buffer);
            if (dst == VK_NULL_HANDLE || c.size == 0) continue;
            const VkBufferCopy region{u.offset + c.srcOffset, c.dstOffset, c.size};
            vkCmdCopyBuffer(slot->cmd, stagingBuffer_, dst, 1, &region);
        }
        for (const StagedTextureCopy& c : u.textures) {
            const VkTexture* tex = textures_.Get(c.texture);
            if (tex == nullptr || !tex->streamed) continue;
            ImageBarrier(slot->cmd, tex->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            regions.clear();
            std::size_t off = u.offset + c.srcOffset;
            const std::uint32_t last = std::min(c.firstMip + c.mipCount, tex->mips);
            for (std::uint32_t m = c.firstMip; m < last; ++m) {
                const std::uint32_t w = std::max(tex->width >> m, 1u), h = std::max(tex->height >> m, 1u);
                VkBufferImageCopy region{};
                region.bufferOffset = off;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
                region.imageExtent = {w, h, 1};
                regions.push_back(region);
                off += MipSizeInBytes(tex->format, w, h);
            }
            if (!regions.empty()) {
                vkCmdCopyBufferToImage(slot->cmd, stagingBuffer_, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       static_cast<std::uint32_t>(regions.size()), regions.data());
            }
            // Ready to sample: the image is shared by both families (concurrent), so the layout
            // changes here and the timeline semaphore orders it before the frames that use it.
            VkImageMemoryBarrier2 release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            release.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            release.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            release.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            release.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            release.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            release.image = tex->image;
            release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, tex->mips, 0, 1};
            barriers.push_back(release);
            slot->images.push_back({c.texture, tex->image, tex->view, tex->mips});
        }
    }
    if (!barriers.empty()) {
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
        dep.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(slot->cmd, &dep);
    }
    SR_VK_CHECK(vkEndCommandBuffer(slot->cmd));

    slot->value = ++uploadValue_;
    slot->lastId = batch.back().id;
    slot->visible = false;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = uploadTimeline_;
    signal.value = slot->value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // After the frames that are already finished (reached, so free): geometry ranges are reused
    // only once the frames that read them are done, and this makes that order explicit.
    VkSemaphoreSubmitInfo afterFrames{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    afterFrames.semaphore = frameTimeline_;
    afterFrames.value = completedFrames_;
    afterFrames.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = slot->cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = completedFrames_ > 0 ? 1 : 0;
    submit.pWaitSemaphoreInfos = &afterFrames;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    {
        std::lock_guard queue(transferShared_ ? queueMutex_ : transferMutex_);
        SR_VK_CHECK(vkQueueSubmit2(transferQueue_, 1, &submit, VK_NULL_HANDLE));
    }
    streamedCount_ += blocks;
    streamedBytes_ += bytes;
    return true;
}

// ---------------------------------------------------------------- frames

CommandList* VulkanDevice::BeginFrame() {
    RecreateSwapchainIfNeeded();
    if (swapchain_ == VK_NULL_HANDLE) return nullptr;

    FrameData& frame = frames_[frameIndex_];
    // The slot's previous frame must be finished before its command buffers are reused.
    WaitForFrame(frame.number);
    completedFrames_ = std::max(completedFrames_, CompletedFrame());
    FlushDeletions(false);
    {
        std::lock_guard lock(resourceMutex_);
        ReclaimUploads(false);
    }
    if (frame.timed) {
        const std::uint32_t count = 2 + 2 * static_cast<std::uint32_t>(frame.timers.size());
        std::uint64_t ticks[kQueriesPerFrame] = {};
        if (vkGetQueryPoolResults(device_, timestamps_, frameIndex_ * kQueriesPerFrame, count, sizeof(std::uint64_t) * count, ticks,
                                  sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const auto ms = [&](std::uint32_t a) {
                return static_cast<float>(static_cast<double>((ticks[a + 1] - ticks[a]) & timestampMask_) * timestampPeriodNs_ / 1.0e6);
            };
            lastGpuMs_ = ms(0);
            lastTimers_.clear();
            for (std::size_t t = 0; t < frame.timers.size(); ++t) lastTimers_.push_back({frame.timers[t], ms(2 + 2 * static_cast<std::uint32_t>(t))});
        }
        frame.timed = false;
    }
    frame.timers.clear();
    frame.timerOpen = false;
    frame.chunkIndex = 0;
    frame.chunkOffset = 0;
    for (std::size_t i = 0; i < frame.recordersUsed; ++i) SR_VK_CHECK(vkResetCommandPool(device_, frame.recorders[i].pool, 0));
    frame.recordersUsed = 0;

    const VkResult acquire =
        vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, frame.imageAcquired, VK_NULL_HANDLE, &imageIndex_);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainDirty_ = true;
        return nullptr;
    }
    if (acquire == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true; // still usable this frame

    SR_VK_CHECK(vkResetCommandPool(device_, frame.pool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    SR_VK_CHECK(vkBeginCommandBuffer(frame.cmd, &begin));
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &bindlessSet_, 0,
                            nullptr);
    vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &bindlessSet_, 0,
                            nullptr);
    if (timestamps_ != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(frame.cmd, timestamps_, frameIndex_ * kQueriesPerFrame, kQueriesPerFrame);
        vkCmdWriteTimestamp2(frame.cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps_, frameIndex_ * kQueriesPerFrame);
    }
    PumpStreaming();
    frameActive_ = true;
    commandList_.Begin(frame.cmd);
    commandList_.ResetStats();
    return &commandList_;
}

void VulkanDevice::BeginSwapchainPass(VkCommandBuffer cmd, const float clearColor[4]) {
    ImageBarrier(cmd, swapchainImages_[imageIndex_], VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = swapchainViews_[imageIndex_];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    std::memcpy(color.clearValue.color.float32, clearColor, sizeof(float) * 4);
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, extent_};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &info);
    // Every pass starts with the whole target as viewport and scissor.
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void VulkanDevice::EndFrame(std::vector<std::uint8_t>* capture) {
    if (!frameActive_) return;
    frameActive_ = false;
    lastStats_ = commandList_.stats;
    lastStats_.gpuMs = lastGpuMs_;
    FrameData& frame = frames_[frameIndex_];
    VkImage image = swapchainImages_[imageIndex_];

    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readbackAlloc = nullptr;
    VmaAllocationInfo readbackInfo{};
    const bool doCapture = capture != nullptr && swapchainCanCopy_;
    if (doCapture) {
        VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = std::size_t{extent_.width} * extent_.height * 4;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        SR_VK_CHECK(vmaCreateBuffer(allocator_, &bufInfo, &allocInfo, &readback, &readbackAlloc, &readbackInfo));

        ImageBarrier(frame.cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(frame.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
        ImageBarrier(frame.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0);
    } else {
        ImageBarrier(frame.cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_NONE, 0);
    }
    if (timestamps_ != VK_NULL_HANDLE) {
        if (frame.timerOpen) WriteTimer(frame.cmd, nullptr); // an unended timer ends with the frame
        vkCmdWriteTimestamp2(frame.cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps_, frameIndex_ * kQueriesPerFrame + 1);
        frame.timed = true;
    }
    SR_VK_CHECK(vkEndCommandBuffer(frame.cmd));
    {
        // This frame's new textures and buffers are copied before it runs.
        std::lock_guard lock(resourceMutex_);
        SubmitUploads();
        lastStats_.uploads = uploadCount_;
        lastStats_.uploadBytes = uploadBytes_;
        uploadCount_ = 0;
        uploadBytes_ = 0;
    }
    lastStats_.streamed = streamedCount_;
    lastStats_.streamedBytes = streamedBytes_;
    streamedCount_ = 0;
    streamedBytes_ = 0;

    const std::uint64_t number = submittedFrames_.load(std::memory_order_relaxed) + 1;
    // The swapchain image, and the streamed data this frame may use.
    std::array<VkSemaphoreSubmitInfo, 2> waits{};
    VkSemaphoreSubmitInfo& wait = waits[0];
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = frame.imageAcquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    waits[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[1].semaphore = uploadTimeline_;
    waits[1].value = frameUploadWait_;
    waits[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    const std::uint32_t waitCount = frameUploadWait_ > 0 ? 2 : 1;
    std::array<VkSemaphoreSubmitInfo, 2> signals{};
    signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[0].semaphore = renderFinished_[imageIndex_];
    signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[1].semaphore = frameTimeline_;
    signals[1].value = number;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = frame.cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = waitCount;
    submit.pWaitSemaphoreInfos = waits.data();
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size());
    submit.pSignalSemaphoreInfos = signals.data();

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[imageIndex_];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex_;
    VkResult result = VK_SUCCESS;
    {
        std::lock_guard queue(queueMutex_);
        SR_VK_CHECK(vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE));
        frame.number = number;
        submittedFrames_.store(number, std::memory_order_release);
        result = vkQueuePresentKHR(queue_, &present);
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;

    if (doCapture) {
        WaitForFrame(frame.number);
        const auto* src = static_cast<const std::uint8_t*>(readbackInfo.pMappedData);
        capture->assign(src, src + std::size_t{extent_.width} * extent_.height * 4);
        if (swapchainFormat_ == VK_FORMAT_B8G8R8A8_UNORM) {
            for (std::size_t i = 0; i < capture->size(); i += 4) std::swap((*capture)[i], (*capture)[i + 2]);
        }
        vmaDestroyBuffer(allocator_, readback, readbackAlloc);
    } else if (capture != nullptr) {
        capture->clear();
    }

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
}

VkCommandBuffer VulkanDevice::AcquireRecorder(std::size_t index) {
    FrameData& frame = frames_[frameIndex_];
    while (frame.recorders.size() <= index) {
        ChunkRecorder recorder;
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = queueFamily_;
        SR_VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &recorder.pool));
        VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdInfo.commandPool = recorder.pool;
        cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        cmdInfo.commandBufferCount = 1;
        SR_VK_CHECK(vkAllocateCommandBuffers(device_, &cmdInfo, &recorder.cmd));
        frame.recorders.push_back(recorder);
    }
    frame.recordersUsed = std::max(frame.recordersUsed, index + 1);
    return frame.recorders[index].cmd;
}

// ---------------------------------------------------------------- command list

void VulkanCommandList::BeginSecondary(VkCommandBuffer cmd, const PassFormats& pass, VkDescriptorSet set,
                                       VkPipelineLayout layout) {
    cmd_ = cmd;
    secondary_ = true;
    parallelPass_ = false;
    pass_ = pass;
    passSamples_ = pass.samples;
    ResetStats();
    VkCommandBufferInheritanceRenderingInfo rendering{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
    rendering.colorAttachmentCount = pass.color != VK_FORMAT_UNDEFINED ? 1 : 0;
    rendering.pColorAttachmentFormats = pass.color != VK_FORMAT_UNDEFINED ? &pass_.color : nullptr;
    rendering.depthAttachmentFormat = pass.depth;
    rendering.rasterizationSamples = pass.samples;
    VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    inherit.pNext = &rendering;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
    begin.pInheritanceInfo = &inherit;
    SR_VK_CHECK(vkBeginCommandBuffer(cmd_, &begin));
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    // Dynamic state is not inherited: start with the whole pass.
    const VkViewport vp{0.0f, 0.0f, static_cast<float>(pass.extent.width), static_cast<float>(pass.extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    const VkRect2D scissor{{0, 0}, pass.extent};
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
}

std::span<CommandList* const> VulkanCommandList::BeginParallel(std::uint32_t count) {
    if (!parallelPass_ || secondary_ || openChunks_ != 0) {
        std::fprintf(stderr, "[rhi] BeginParallel needs an open pass begun with RenderPassDesc::parallel\n");
        return {};
    }
    while (chunks_.size() < count) chunks_.push_back(std::make_unique<VulkanCommandList>(device_));
    chunkPointers_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        chunks_[i]->BeginSecondary(device_.AcquireRecorder(i), pass_, device_.BindlessSet(), device_.PipelineLayout());
        chunkPointers_[i] = chunks_[i].get();
    }
    openChunks_ = count;
    return {chunkPointers_.data(), count};
}

void VulkanCommandList::EndParallel() {
    if (openChunks_ == 0) return;
    std::vector<VkCommandBuffer> buffers(openChunks_);
    for (std::uint32_t i = 0; i < openChunks_; ++i) {
        VulkanCommandList& chunk = *chunks_[i];
        SR_VK_CHECK(vkEndCommandBuffer(chunk.cmd_));
        buffers[i] = chunk.cmd_;
        stats.draws += chunk.stats.draws;
        stats.primitives += chunk.stats.primitives;
        stats.instances += chunk.stats.instances;
        stats.indirectDraws += chunk.stats.indirectDraws;
    }
    vkCmdExecuteCommands(cmd_, openChunks_, buffers.data());
    openChunks_ = 0;
}

void VulkanCommandList::BeginSwapchainPass(const float clearColor[4]) {
    if (secondary_) return;
    passSamples_ = VK_SAMPLE_COUNT_1_BIT;
    parallelPass_ = false;
    ++stats.passes;
    device_.BeginSwapchainPass(cmd_, clearColor);
}

void VulkanCommandList::BeginRenderPass(const RenderPassDesc& desc) {
    if (secondary_) {
        std::fprintf(stderr, "[rhi] a parallel chunk cannot begin a pass\n");
        return;
    }
    const VkTexture* color = device_.GetTexture(desc.color);
    const VkTexture* depth = device_.GetTexture(desc.depth);
    if (color == nullptr && depth == nullptr) {
        std::fprintf(stderr, "[rhi] BeginRenderPass needs a colour or a depth target\n");
        return;
    }
    if (color != nullptr && color->usage != TextureUsage::RenderTarget) {
        std::fprintf(stderr, "[rhi] BeginRenderPass needs a RenderTarget colour texture\n");
        return;
    }
    const bool multisampled = color != nullptr && color->samples != VK_SAMPLE_COUNT_1_BIT;
    const VkTexture* resolve = multisampled ? device_.GetTexture(desc.resolve) : nullptr;
    if (multisampled && (resolve == nullptr || resolve->samples != VK_SAMPLE_COUNT_1_BIT || resolve->usage != TextureUsage::RenderTarget)) {
        std::fprintf(stderr, "[rhi] a multisampled pass needs a single-sample RenderTarget to resolve into\n");
        return;
    }

    // A cleared colour target discards its previous contents, so it starts from
    // UNDEFINED. A loaded one was left in SHADER_READ_ONLY by the EndPass of the
    // pass that rendered it (a multisampled one stays a colour attachment). Either
    // barrier also waits for earlier sampling of the target (e.g. last frame's
    // ImGui::Image) and, when loading, for the earlier pass's writes.
    const VkPipelineStageFlags2 colorOut = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (color != nullptr && desc.loadColor) {
        ImageBarrier(cmd_, color->image, multisampled ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, colorOut | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, colorOut,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    } else if (color != nullptr) {
        ImageBarrier(cmd_, color->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | colorOut, 0, colorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
    if (resolve != nullptr) {
        // Wholly written by the resolve.
        ImageBarrier(cmd_, resolve->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, colorOut, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
    const bool keepDepth = desc.loadDepth && depth != nullptr && depth->usage == TextureUsage::ShadowMap;
    if (depth != nullptr) {
        // Cleared: from UNDEFINED, after earlier depth tests and (a shadow map) earlier sampling.
        // Kept (a shadow map): from where its last pass left it, sampled.
        const VkPipelineStageFlags2 tests =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        ImageBarrier(cmd_, depth->image, keepDepth ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     tests | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, tests,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    VkRenderingAttachmentInfo colorInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (color != nullptr) {
        colorInfo.imageView = color->view;
        colorInfo.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorInfo.loadOp = desc.loadColor ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorInfo.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        std::memcpy(colorInfo.clearValue.color.float32, desc.clearColor, sizeof(float) * 4);
        if (resolve != nullptr) {
            colorInfo.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            colorInfo.resolveImageView = resolve->view;
            colorInfo.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    const bool sampledDepth = depth != nullptr && depth->usage == TextureUsage::ShadowMap;
    VkRenderingAttachmentInfo depthInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depth != nullptr) {
        depthInfo.imageView = depth->view;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthInfo.loadOp = keepDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthInfo.storeOp = sampledDepth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthInfo.clearValue.depthStencil.depth = desc.clearDepth;
    }

    const VkTexture* area = color != nullptr ? color : depth;
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {area->width, area->height}};
    info.layerCount = 1;
    info.colorAttachmentCount = color != nullptr ? 1 : 0;
    info.pColorAttachments = color != nullptr ? &colorInfo : nullptr;
    info.pDepthAttachment = depth != nullptr ? &depthInfo : nullptr;
    if (desc.parallel) info.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    vkCmdBeginRendering(cmd_, &info);
    if (!desc.parallel) {
        // Every pass starts with the whole target as viewport and scissor (parallel chunks set
        // theirs in BeginSecondary: a pass recorded in chunks takes no commands of its own).
        const VkViewport vp{0.0f, 0.0f, static_cast<float>(area->width), static_cast<float>(area->height), 0.0f, 1.0f};
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &info.renderArea);
    }
    offscreenColor_ = color == nullptr ? VK_NULL_HANDLE : resolve != nullptr ? resolve->image : color->image;
    shadowDepth_ = sampledDepth ? depth->image : VK_NULL_HANDLE;
    passSamples_ = area->samples;
    parallelPass_ = desc.parallel;
    pass_ = PassFormats{color != nullptr ? ToVk(color->format) : VK_FORMAT_UNDEFINED,
                        depth != nullptr ? ToVk(depth->format) : VK_FORMAT_UNDEFINED, area->samples,
                        {area->width, area->height}};
    ++stats.passes;
}

void VulkanCommandList::EndPass() {
    if (secondary_) return;
    if (openChunks_ != 0) EndParallel(); // forgiving: the chunks' commands still run
    parallelPass_ = false;
    vkCmdEndRendering(cmd_);
    if (offscreenColor_ != VK_NULL_HANDLE) {
        ImageBarrier(cmd_, offscreenColor_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        offscreenColor_ = VK_NULL_HANDLE;
    }
    if (shadowDepth_ != VK_NULL_HANDLE) {
        // Sampled by later passes' fragment shaders, and by a compute shader (the scene's depth read under the mouse).
        ImageBarrier(cmd_, shadowDepth_, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        shadowDepth_ = VK_NULL_HANDLE;
    }
    passSamples_ = VK_SAMPLE_COUNT_1_BIT;
}

void VulkanCommandList::SetViewport(const Viewport& v) {
    const VkViewport vp{v.x, v.y, v.width, v.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
}

void VulkanCommandList::SetScissor(const Rect& r) {
    const VkRect2D rect{{r.x, r.y}, {r.width, r.height}};
    vkCmdSetScissor(cmd_, 0, 1, &rect);
}

void VulkanCommandList::BindPipeline(PipelineHandle pipeline) {
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, device_.GetPipeline(pipeline, passSamples_));
}

void VulkanCommandList::BindVertexBuffer(BufferHandle buffer, std::size_t offset) {
    const VkBuffer vb = device_.GetBuffer(buffer);
    const VkDeviceSize off = offset;
    vkCmdBindVertexBuffers(cmd_, 0, 1, &vb, &off);
}

void VulkanCommandList::BindIndexBuffer(BufferHandle buffer, std::size_t offset, bool index32) {
    vkCmdBindIndexBuffer(cmd_, device_.GetBuffer(buffer), offset, index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
}

void VulkanCommandList::PushConstants(const void* data, std::uint32_t size) {
    vkCmdPushConstants(cmd_, device_.PipelineLayout(), VK_SHADER_STAGE_ALL, 0, std::min(size, kPushConstantSize), data);
}

void VulkanCommandList::Draw(std::uint32_t vertexCount, std::uint32_t firstVertex) {
    vkCmdDraw(cmd_, vertexCount, 1, firstVertex, 0);
    ++stats.draws;
    ++stats.instances;
    stats.primitives += vertexCount / 3;
}

void VulkanCommandList::DrawIndexed(std::uint32_t indexCount, std::uint32_t firstIndex, std::int32_t vertexOffset) {
    vkCmdDrawIndexed(cmd_, indexCount, 1, firstIndex, vertexOffset, 0);
    ++stats.draws;
    ++stats.instances;
    stats.primitives += indexCount / 3;
}

void VulkanDevice::WriteTimer(VkCommandBuffer cmd, const char* name) {
    if (timestamps_ == VK_NULL_HANDLE || !frameActive_) return;
    FrameData& frame = frames_[frameIndex_];
    const std::uint32_t base = frameIndex_ * kQueriesPerFrame + 2;
    if (frame.timerOpen) {
        // The end of the open timer.
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps_,
                             base + 2 * static_cast<std::uint32_t>(frame.timers.size() - 1) + 1);
        frame.timerOpen = false;
    }
    if (name == nullptr || frame.timers.size() >= kMaxGpuTimers) return;
    frame.timers.push_back(name);
    frame.timerOpen = true;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps_, base + 2 * static_cast<std::uint32_t>(frame.timers.size() - 1));
}

void VulkanCommandList::ClearDepth(const Rect& r, float depth) {
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    clear.clearValue.depthStencil.depth = depth;
    VkClearRect rect{};
    rect.rect = {{r.x, r.y}, {r.width, r.height}};
    rect.layerCount = 1;
    vkCmdClearAttachments(cmd_, 1, &clear, 1, &rect);
}

void VulkanCommandList::UpdateBuffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) {
    if (secondary_ || data.empty()) return;
    const VkBuffer dst = device_.GetBuffer(buffer);
    if (dst == VK_NULL_HANDLE) return;
    const TransientAllocation src = device_.AllocateTransient(data.size(), 16);
    if (src.cpu == nullptr) return;
    std::memcpy(src.cpu, data.data(), data.size());
    // After every earlier use in the queue (reads: an execution dependency; earlier updates:
    // their writes), and before every later use.
    VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd_, &dep);
    const VkBufferCopy region{src.offset, offset, data.size()};
    vkCmdCopyBuffer(cmd_, device_.GetBuffer(src.buffer), dst, 1, &region);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    vkCmdPipelineBarrier2(cmd_, &dep);
}

namespace {
// The stages and accesses of a BufferAccess (synchronisation 2).
void ToVk(BufferAccess access, VkPipelineStageFlags2& stage, VkAccessFlags2& flags) {
    switch (access) {
    case BufferAccess::None: stage = VK_PIPELINE_STAGE_2_NONE; flags = 0; return;
    case BufferAccess::IndirectRead: stage = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT; flags = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT; return;
    case BufferAccess::IndexRead: stage = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT; flags = VK_ACCESS_2_INDEX_READ_BIT; return;
    case BufferAccess::VertexShaderRead: stage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT; flags = VK_ACCESS_2_SHADER_READ_BIT; return;
    case BufferAccess::FragmentRead: stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; flags = VK_ACCESS_2_SHADER_READ_BIT; return;
    case BufferAccess::ComputeRead: stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; flags = VK_ACCESS_2_SHADER_READ_BIT; return;
    case BufferAccess::ComputeWrite:
        stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        flags = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        return;
    case BufferAccess::TransferWrite: stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; flags = VK_ACCESS_2_TRANSFER_WRITE_BIT; return;
    case BufferAccess::TransferRead: stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; flags = VK_ACCESS_2_TRANSFER_READ_BIT; return;
    case BufferAccess::Any: break;
    }
    stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    flags = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
}
} // namespace

void VulkanCommandList::BindComputePipeline(PipelineHandle pipeline) {
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, device_.GetComputePipeline(pipeline));
}

void VulkanCommandList::Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) {
    if (groupsX == 0 || groupsY == 0 || groupsZ == 0) return;
    vkCmdDispatch(cmd_, groupsX, groupsY, groupsZ);
    ++stats.dispatches;
}

void VulkanCommandList::DispatchIndirect(BufferHandle buffer, std::size_t offset) {
    const VkBuffer args = device_.GetBuffer(buffer);
    if (args == VK_NULL_HANDLE) return;
    vkCmdDispatchIndirect(cmd_, args, offset);
    ++stats.dispatches;
}

void VulkanCommandList::FillBuffer(BufferHandle buffer, std::size_t offset, std::size_t size, std::uint32_t value) {
    const VkBuffer target = device_.GetBuffer(buffer);
    if (target == VK_NULL_HANDLE) return;
    vkCmdFillBuffer(cmd_, target, offset, size == 0 ? VK_WHOLE_SIZE : size, value);
}

void VulkanCommandList::Barriers(std::span<const BufferBarrier> buffers) {
    std::array<VkBufferMemoryBarrier2, 32> barriers{};
    std::uint32_t count = 0;
    const auto flush = [&] {
        if (count == 0) return;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount = count;
        dep.pBufferMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd_, &dep);
        count = 0;
    };
    for (const BufferBarrier& b : buffers) {
        const VkBuffer buffer = device_.GetBuffer(b.buffer);
        if (buffer == VK_NULL_HANDLE) continue;
        VkBufferMemoryBarrier2& out = barriers[count++];
        out = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        ToVk(b.from, out.srcStageMask, out.srcAccessMask);
        ToVk(b.to, out.dstStageMask, out.dstAccessMask);
        out.srcQueueFamilyIndex = out.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        out.buffer = buffer;
        out.offset = 0;
        out.size = VK_WHOLE_SIZE;
        if (count == barriers.size()) flush();
    }
    flush();
}

void VulkanCommandList::DrawIndexedIndirectCount(BufferHandle commands, std::size_t offset, BufferHandle count,
                                                 std::size_t countOffset, std::uint32_t maxDraws) {
    const VkBuffer commandBuffer = device_.GetBuffer(commands);
    const VkBuffer countBuffer = device_.GetBuffer(count);
    if (commandBuffer == VK_NULL_HANDLE || countBuffer == VK_NULL_HANDLE || maxDraws == 0) return;
    vkCmdDrawIndexedIndirectCount(cmd_, commandBuffer, offset, countBuffer, countOffset, maxDraws,
                                  sizeof(DrawIndexedIndirectCommand));
    ++stats.indirectDraws;
}

void VulkanCommandList::BeginTimer(const char* name) {
    if (!secondary_) device_.WriteTimer(cmd_, name);
}

void VulkanCommandList::EndTimer() {
    if (!secondary_) device_.WriteTimer(cmd_, nullptr);
}

void VulkanCommandList::DrawIndexedInstanced(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex,
                                             std::int32_t vertexOffset) {
    if (instanceCount == 0) return;
    vkCmdDrawIndexed(cmd_, indexCount, instanceCount, firstIndex, vertexOffset, 0);
    ++stats.draws;
    stats.instances += instanceCount;
    stats.primitives += std::uint64_t{indexCount / 3} * instanceCount;
}

} // namespace

std::unique_ptr<Device> CreateVulkanDevice(const DeviceDesc& desc, std::string& error) {
    auto device = std::make_unique<VulkanDevice>();
    if (!device->Init(desc, error)) return nullptr;
    return device;
}

} // namespace sr::rhi
