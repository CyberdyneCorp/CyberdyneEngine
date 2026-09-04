#pragma once
// The Vulkan device: the same interface the null backend implements, over a real driver.
// Tasks 2.3.1, 2.3.2 and 2.3.3.
//
// WHAT IS DIFFERENT FROM THE NULL BACKEND, AND WHAT DELIBERATELY IS NOT. The interface is
// identical; the null backend was written first so that it would be (design.md §1). What is
// different is everything the interface does not say: which queue families exist, how memory is
// suballocated, what a validation layer reports, and whether an ownership transfer is a real thing
// or a no-op.
//
// THE THREE THINGS THIS FILE GETS RIGHT THAT COST M3's SPIKE REAL TIME:
//
//   * Queue selection is by CAPABILITY, not by index. The async-compute queue is "has COMPUTE and
//     NOT GRAPHICS"; a device with no such family reports Capability::AsyncCompute false and the
//     render graph folds those passes onto graphics, which produces one submit from the same
//     declarations. `rhi-and-render-graph`: branch on the capability, never on the backend.
//
//   * One timeline semaphore per queue. A cross-queue dependency is a wait on the producer's
//     timeline; binary semaphores work identically and were tested, and timeline wins only because
//     it removes the "every signal must be waited exactly once" bookkeeping.
//
//   * Synchronisation validation is OFF by default even when the layers are on. Without
//     VkValidationFeaturesEXT chained into VkInstanceCreateInfo, none of the hazard checks fire and
//     the whole exercise is theatre. (Spike gotcha 6h.)

#include <cy/backends/rhi/validation.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/handle_pool.h>

#include "vulkan_translate.h"

// VMA is included here rather than in each source, because its header declares the types the object
// records below hold. VMA_IMPLEMENTATION is defined in exactly one translation unit
// (vulkan_memory.cpp); everywhere else this is a declaration.
#include <vk_mem_alloc.h>

#include <mutex>

namespace cy::rhi::vulkan {

/// A fixed-size copy of a resource's name, for the debug label the RHI requires every resource to
/// carry. The descriptions callers pass own nothing, exactly as in the null backend.
struct StoredName {
    char text[64] = {};
    void assign(const char* source) noexcept;
};

struct VulkanBuffer {
    BufferDescription desc{};
    StoredName name{};
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    bool transient = false;
    bool bound = true;
    GpuMemoryCategory category = GpuMemoryCategory::Persistent;
    u64 bytes = 0;
};

struct VulkanTexture {
    TextureDescription desc{};
    StoredName name{};
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    bool transient = false;
    bool bound = true;
    bool owned_by_swapchain = false;
    GpuMemoryCategory category = GpuMemoryCategory::Persistent;
    u64 bytes = 0;
};

struct VulkanTextureView {
    VkImageView view = VK_NULL_HANDLE;
    TextureHandle texture;
    SubresourceRange range{};
};

struct VulkanSampler {
    VkSampler sampler = VK_NULL_HANDLE;
};

struct VulkanShaderModule {
    VkShaderModule module = VK_NULL_HANDLE;
    ShaderStage stage = ShaderStage::None;
    StoredName entry_point{};
    u64 code_hash = 0;
};

struct VulkanDescriptorSetLayout {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    u32 binding_count = 0;
};

struct VulkanPipelineLayout {
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

struct VulkanDescriptorSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool per_frame = false;
    u32 frame_slot = 0;
};

struct VulkanPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    u64 state_hash = 0;
};

struct VulkanQueryPool {
    VkQueryPool pool = VK_NULL_HANDLE;
    QueryKind kind = QueryKind::Timestamp;
    u32 count = 0;
};

struct VulkanFence {
    VkFence fence = VK_NULL_HANDLE;
};

struct VulkanSemaphore {
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

class VulkanDevice;

/// The recording interface. Every method is a direct translation into one or two Vulkan calls; the
/// class exists so that a pass holds a reference rather than a handle it has to resolve.
class VulkanCommandBuffer final : public CommandBuffer {
public:
    VulkanCommandBuffer(VulkanDevice* device, VkCommandBuffer commands, QueueKind queue,
                        bool secondary, u32 frame_slot) noexcept
        : device_(device),
          commands_(commands),
          queue_(queue),
          secondary_(secondary),
          frame_slot_(frame_slot) {}

    [[nodiscard]] CommandBufferHandle handle() const noexcept override { return handle_; }
    void set_handle(CommandBufferHandle handle) noexcept { handle_ = handle; }
    [[nodiscard]] VkCommandBuffer raw() const noexcept { return commands_; }
    [[nodiscard]] QueueKind queue() const noexcept { return queue_; }
    [[nodiscard]] bool secondary() const noexcept { return secondary_; }
    [[nodiscard]] u32 frame_slot() const noexcept { return frame_slot_; }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    void set_recording(bool value) noexcept { recording_ = value; }

    void begin_rendering(const RenderingInfo& info) noexcept override;
    void end_rendering() noexcept override;
    void set_viewport(const Viewport& viewport) noexcept override;
    void set_scissor(const Rect2D& scissor) noexcept override;
    void bind_graphics_pipeline(GraphicsPipelineHandle pipeline) noexcept override;
    void bind_compute_pipeline(ComputePipelineHandle pipeline) noexcept override;
    void bind_descriptor_sets(PipelineLayoutHandle layout, u32 first_set,
                              Span<const DescriptorSetHandle> sets) noexcept override;
    void push_constants(PipelineLayoutHandle layout, ShaderStage stages, u32 offset,
                        Span<const u8> data) noexcept override;
    void bind_vertex_buffers(u32 first_binding, Span<const BufferHandle> buffers,
                             Span<const u64> offsets) noexcept override;
    void bind_index_buffer(BufferHandle buffer, u64 offset, bool wide) noexcept override;
    void draw(u32 vertex_count, u32 instance_count, u32 first_vertex,
              u32 first_instance) noexcept override;
    void draw_indexed(u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset,
                      u32 first_instance) noexcept override;
    void draw_indexed_indirect(BufferHandle arguments, u64 offset, u32 draw_count,
                               u32 stride) noexcept override;
    void dispatch(u32 groups_x, u32 groups_y, u32 groups_z) noexcept override;
    void dispatch_indirect(BufferHandle arguments, u64 offset) noexcept override;
    void copy_buffer(BufferHandle source, BufferHandle destination,
                     Span<const BufferCopy> regions) noexcept override;
    void copy_buffer_to_texture(BufferHandle source, TextureHandle destination,
                                Span<const BufferTextureCopy> regions) noexcept override;
    void copy_texture_to_buffer(TextureHandle source, BufferHandle destination,
                                Span<const BufferTextureCopy> regions) noexcept override;
    void begin_debug_label(const char* name) noexcept override;
    void end_debug_label() noexcept override;
    void insert_debug_label(const char* name) noexcept override;
    void write_timestamp(QueryPoolHandle pool, u32 index) noexcept override;
    void reset_queries(QueryPoolHandle pool, u32 first, u32 count) noexcept override;
    void write_breadcrumb(u32 slot, u32 value) noexcept override;

    /// The escape hatch `rhi-and-render-graph` permits: the VkCommandBuffer, documented as unsafe
    /// and excluded from the portability guarantees. It is not a way to emit a barrier — task
    /// 2.2.4's gate reads the source, not the type.
    [[nodiscard]] void* native_handle() noexcept override { return commands_; }

private:
    VulkanDevice* device_ = nullptr;
    VkCommandBuffer commands_ = VK_NULL_HANDLE;
    CommandBufferHandle handle_;
    QueueKind queue_ = QueueKind::Graphics;
    bool secondary_ = false;
    bool recording_ = false;
    u32 frame_slot_ = 0;
};

/// The only object in this backend that can emit a barrier, and reachable only with a key the
/// render graph's executor alone constructs. See cy/backends/rhi/barrier.h.
class VulkanBarrierRecorder final : public BarrierRecorder {
public:
    explicit VulkanBarrierRecorder(VulkanDevice* device) noexcept : device_(device) {}

    void record_barriers(CommandBufferHandle command_buffer,
                         const BarrierBatch& batch) noexcept override;
    [[nodiscard]] u64 recorded_batch_count() const noexcept override { return batches_; }
    [[nodiscard]] u64 recorded_barrier_count() const noexcept override { return barriers_; }

private:
    VulkanDevice* device_ = nullptr;
    u64 batches_ = 0;
    u64 barriers_ = 0;
};

/// One frame in flight: its pools, its fence, and the resources retired while it was current.
///
/// ONE COMMAND POOL PER (QUEUE, RECORDING THREAD), not per queue. A VkCommandPool is externally
/// synchronised, and that covers recording into any command buffer allocated from it — so two
/// workers recording two passes of the same submit need two pools. Pools are created on first use,
/// because a frame that never records in parallel should not pay for sixteen of them.
struct FrameContext {
    VkCommandPool command_pools[kQueueKindCount][kMaxRecordingThreads] = {};
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    /// Timeline values this frame's submissions reached, per queue. `begin_frame` waits on these
    /// before it recycles the pools — which is `rhi-and-render-graph`'s "the CPU SHALL wait on the
    /// oldest frame's fence before reusing that frame's pools", expressed on a timeline.
    u64 timeline[kQueueKindCount] = {};
    bool used = false;
};

/// A resource waiting for the GPU to be finished with it.
///
/// `rhi-and-render-graph`: "its memory SHALL be released only after frame N's fence has signalled".
/// The retirement carries the frame index it was retired in and is released when that frame comes
/// round again, which is the same epoch mechanism the engine uses for CPU memory rather than a
/// GPU-specific deferral scheme.
struct Retirement {
    enum class Kind : u8 { Buffer, Image, ImageView, Sampler, Pipeline, ShaderModule, Framebuffer };
    Kind kind = Kind::Buffer;
    u64 frame = 0;
    /// The Vulkan object. Every handle retired here is non-dispatchable, which on every platform
    /// the engine targets is a pointer — carrying it as one rather than as a u64 keeps the round
    /// trip a pointer cast rather than an integer-to-pointer one.
    void* object = nullptr;
    VmaAllocation allocation = nullptr;
};

class VulkanDevice final : public Device {
public:
    VulkanDevice(Allocator& allocator, const DeviceDescription& desc) noexcept;
    ~VulkanDevice() override;

    /// The two-phase construction the RHI's Expected-returning factory needs: the constructor sets
    /// up the pools, and this does everything that can fail.
    [[nodiscard]] Status initialise(const DeviceDescription& desc) noexcept;

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    [[nodiscard]] DescriptorModel descriptor_model() const noexcept override { return model_; }
    [[nodiscard]] u32 frames_in_flight() const noexcept override { return frames_in_flight_; }
    [[nodiscard]] u32 queue_family(QueueKind queue) const noexcept override;
    [[nodiscard]] bool has_queue(QueueKind queue) const noexcept override;
    void set_validation_callback(ValidationCallback callback, void* user) noexcept override;

    Expected<BufferHandle, Error> create_buffer(const BufferDescription& desc) override;
    void destroy_buffer(BufferHandle handle) noexcept override;
    [[nodiscard]] bool is_valid(BufferHandle handle) const noexcept override;
    [[nodiscard]] void* buffer_mapped_pointer(BufferHandle handle) noexcept override;
    [[nodiscard]] const BufferDescription* buffer_description(
        BufferHandle handle) const noexcept override;

    Expected<TextureHandle, Error> create_texture(const TextureDescription& desc) override;
    void destroy_texture(TextureHandle handle) noexcept override;
    [[nodiscard]] bool is_valid(TextureHandle handle) const noexcept override;
    [[nodiscard]] const TextureDescription* texture_description(
        TextureHandle handle) const noexcept override;

    Expected<TextureViewHandle, Error> create_texture_view(
        const TextureViewDescription& desc) override;
    void destroy_texture_view(TextureViewHandle handle) noexcept override;
    [[nodiscard]] bool is_valid(TextureViewHandle handle) const noexcept override;

    Expected<SamplerHandle, Error> create_sampler(const SamplerDescription& desc) override;
    void destroy_sampler(SamplerHandle handle) noexcept override;

    Expected<QueryPoolHandle, Error> create_query_pool(const QueryPoolDescription& desc) override;
    void destroy_query_pool(QueryPoolHandle handle) noexcept override;
    Expected<u32, Error> read_query_results(QueryPoolHandle pool, u32 first, u32 count,
                                            Span<u64> out) override;

    Expected<TextureHandle, Error> create_transient_texture(
        const TextureDescription& desc) override;
    Expected<BufferHandle, Error> create_transient_buffer(const BufferDescription& desc) override;
    [[nodiscard]] Expected<MemoryRequirements, Error> texture_memory_requirements(
        TextureHandle handle) const override;
    [[nodiscard]] Expected<MemoryRequirements, Error> buffer_memory_requirements(
        BufferHandle handle) const override;
    Status reserve_transient_memory(u64 bytes, u32 memory_type_bits) override;
    Status bind_transient(TextureHandle handle, u64 offset) override;
    Status bind_transient(BufferHandle handle, u64 offset) override;
    void release_transient_resources() noexcept override;
    [[nodiscard]] u64 transient_pool_bytes() const noexcept override { return transient_bytes_; }

    Expected<ShaderModuleHandle, Error> create_shader_module(
        const ShaderModuleDescription& desc) override;
    void destroy_shader_module(ShaderModuleHandle handle) noexcept override;
    Expected<DescriptorSetLayoutHandle, Error> create_descriptor_set_layout(
        const DescriptorSetLayoutDescription& desc) override;
    void destroy_descriptor_set_layout(DescriptorSetLayoutHandle handle) noexcept override;
    Expected<PipelineLayoutHandle, Error> create_pipeline_layout(
        const PipelineLayoutDescription& desc) override;
    void destroy_pipeline_layout(PipelineLayoutHandle handle) noexcept override;
    Expected<DescriptorSetHandle, Error> allocate_descriptor_set(DescriptorSetLayoutHandle layout,
                                                                 bool per_frame) override;
    Status update_descriptor_set(DescriptorSetHandle set,
                                 Span<const DescriptorWrite> writes) override;
    BindlessIndex bind_texture_globally(TextureViewHandle view,
                                        SamplerHandle sampler) noexcept override;
    void release_bindless_index(BindlessIndex index) noexcept override;
    Expected<GraphicsPipelineHandle, Error> create_graphics_pipeline(
        const GraphicsPipelineDescription& desc) override;
    void destroy_graphics_pipeline(GraphicsPipelineHandle handle) noexcept override;
    Expected<ComputePipelineHandle, Error> create_compute_pipeline(
        const ComputePipelineDescription& desc) override;
    void destroy_compute_pipeline(ComputePipelineHandle handle) noexcept override;
    Expected<u64, Error> save_pipeline_cache(Span<u8> out) override;
    Status load_pipeline_cache(Span<const u8> data) override;

    Expected<u32, Error> begin_frame() override;
    Status end_frame() override;
    [[nodiscard]] u64 frame_index() const noexcept override { return frame_index_; }
    [[nodiscard]] u32 frame_slot() const noexcept override { return frame_slot_; }

    Expected<CommandBufferHandle, Error> acquire_command_buffer(QueueKind queue,
                                                                bool secondary) override;
    [[nodiscard]] CommandBuffer* command_buffer(CommandBufferHandle handle) noexcept override;
    Status begin_command_buffer(CommandBufferHandle handle) override;
    Status end_command_buffer(CommandBufferHandle handle) override;
    Status execute_secondary(CommandBufferHandle primary,
                             Span<const CommandBufferHandle> secondaries) override;
    Expected<u64, Error> submit(const SubmitInfo& info) override;
    [[nodiscard]] u64 timeline_value(QueueKind queue) const noexcept override;
    Status wait_timeline(QueueKind queue, u64 value, u64 timeout_ns) override;
    Status wait_idle() override;

    Expected<FenceHandle, Error> create_fence(bool signalled) override;
    void destroy_fence(FenceHandle handle) noexcept override;
    Status wait_fence(FenceHandle handle, u64 timeout_ns) override;
    Status reset_fence(FenceHandle handle) override;
    [[nodiscard]] bool fence_signalled(FenceHandle handle) const noexcept override;
    Expected<SemaphoreHandle, Error> create_semaphore() override;
    void destroy_semaphore(SemaphoreHandle handle) noexcept override;

    Expected<SwapchainHandle, Error> create_swapchain(const SwapchainDescription& desc) override;
    void destroy_swapchain(SwapchainHandle handle) noexcept override;
    Status resize_swapchain(SwapchainHandle handle, Extent2D extent) override;
    [[nodiscard]] SwapchainInfo swapchain_info(SwapchainHandle handle) const noexcept override;
    Expected<u32, Error> acquire_next_image(SwapchainHandle handle, SemaphoreHandle signal,
                                            u64 timeout_ns) override;
    [[nodiscard]] TextureHandle swapchain_texture(SwapchainHandle handle,
                                                  u32 index) const noexcept override;
    [[nodiscard]] TextureViewHandle swapchain_view(SwapchainHandle handle,
                                                   u32 index) const noexcept override;
    Status present(SwapchainHandle handle, u32 image_index, SemaphoreHandle wait) override;

    [[nodiscard]] GpuMemoryReport memory_report() const noexcept override;
    [[nodiscard]] const DeviceStatistics& statistics() const noexcept override { return stats_; }
    void reset_statistics() noexcept override { stats_ = DeviceStatistics{}; }
    void publish_memory_pressure() noexcept override;

    [[nodiscard]] BarrierRecorder& barrier_recorder(const GraphBarrierKey& key) noexcept override;
    [[nodiscard]] void* native_handle() noexcept override { return device_; }

    // --- This backend's own surface, used across its translation units ---------------------------

    [[nodiscard]] VkDevice vk_device() const noexcept { return device_; }
    [[nodiscard]] VkPhysicalDevice vk_physical_device() const noexcept { return physical_; }
    [[nodiscard]] VkInstance vk_instance() const noexcept { return instance_; }
    [[nodiscard]] VmaAllocator vk_allocator() const noexcept { return vma_; }
    [[nodiscard]] DeviceStatistics& mutable_statistics() noexcept { return stats_; }
    [[nodiscard]] bool debug_markers() const noexcept { return debug_markers_; }

    [[nodiscard]] VulkanBuffer* buffer(BufferHandle handle) noexcept {
        return buffers_.resolve(handle);
    }
    [[nodiscard]] VulkanTexture* texture(TextureHandle handle) noexcept {
        return textures_.resolve(handle);
    }
    [[nodiscard]] VulkanTextureView* view(TextureViewHandle handle) noexcept {
        return views_.resolve(handle);
    }
    [[nodiscard]] VulkanPipeline* graphics_pipeline(GraphicsPipelineHandle handle) noexcept {
        return graphics_pipelines_.resolve(handle);
    }
    [[nodiscard]] VulkanPipeline* compute_pipeline(ComputePipelineHandle handle) noexcept {
        return compute_pipelines_.resolve(handle);
    }
    [[nodiscard]] VulkanPipelineLayout* pipeline_layout(PipelineLayoutHandle handle) noexcept {
        return pipeline_layouts_.resolve(handle);
    }
    [[nodiscard]] VulkanDescriptorSet* descriptor_set(DescriptorSetHandle handle) noexcept {
        return descriptor_sets_.resolve(handle);
    }
    [[nodiscard]] VulkanQueryPool* query_pool(QueryPoolHandle handle) noexcept {
        return query_pools_.resolve(handle);
    }
    [[nodiscard]] VulkanCommandBuffer* vulkan_command_buffer(CommandBufferHandle handle) noexcept {
        return command_buffers_.resolve(handle);
    }
    [[nodiscard]] VkBuffer breadcrumb_buffer() const noexcept { return breadcrumb_buffer_; }
    [[nodiscard]] static u32 breadcrumb_slots() noexcept { return kBreadcrumbSlots; }

    void report_validation(ValidationSeverity severity, const char* message) noexcept;
    void name_object(u64 handle, VkObjectType type, const char* name) noexcept;

private:
    static constexpr u32 kBindlessCapacity = 16384;
    static constexpr u32 kBreadcrumbSlots = 1024;

    Status create_instance(const DeviceDescription& desc) noexcept;
    Status select_physical_device() noexcept;
    Status create_logical_device(const DeviceDescription& desc) noexcept;
    Status create_allocator() noexcept;
    Status create_frames(const DeviceDescription& desc) noexcept;
    Status create_bindless_table() noexcept;
    Status create_breadcrumbs() noexcept;
    void fill_capabilities() noexcept;
    /// The command pool for `queue` in the current frame, for the calling thread. Created on first
    /// use, under the acquire lock.
    [[nodiscard]] Expected<VkCommandPool, Error> pool_for_this_thread(QueueKind queue) noexcept;
    void retire(Retirement::Kind kind, void* object, VmaAllocation allocation) noexcept;
    void release_retirements(u64 up_to_frame) noexcept;
    void charge(GpuMemoryCategory category, u64 bytes) noexcept;
    void discharge(GpuMemoryCategory category, u64 bytes) noexcept;

    Allocator* allocator_ = nullptr;
    DeviceCapabilities capabilities_;
    DeviceStatistics stats_{};
    DescriptorModel model_ = DescriptorModel::Bindless;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator vma_ = nullptr;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;

    VkQueue queues_[kQueueKindCount] = {};
    u32 queue_families_[kQueueKindCount] = {};
    bool queue_present_[kQueueKindCount] = {};
    VkSemaphore timelines_[kQueueKindCount] = {};
    u64 timeline_values_[kQueueKindCount] = {};

    u32 frames_in_flight_ = kDefaultFramesInFlight;
    u64 frame_index_ = 0;
    u32 frame_slot_ = 0;
    bool frame_open_ = false;
    FrameContext frames_[kMaxFramesInFlight] = {};

    // THE POOL THAT IS NEVER RESET, which is what makes `allocate_descriptor_set(layout, false)`
    // mean what its documentation says. Every FrameContext's pool is reset when its slot comes
    // round, so a set allocated from one survives exactly `frames_in_flight_` frames; a persistent
    // set has to come from somewhere else, and this is it.
    //
    // It was missing, and the defect was not theoretical: the milestone's own artefact allocates
    // its constants set with `per_frame = false`, and from frame 3 onward every draw bound a
    // descriptor the driver had already recycled — 24 validation errors a frame, on a sample that
    // still exited 0. No device suite caught it because none of them renders a third frame.
    VkDescriptorPool persistent_descriptor_pool_ = VK_NULL_HANDLE;

    // Bindless: one global set with a runtime-sized combined-image-sampler array.
    VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bindless_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
    u32 bindless_next_ = 0;

    // The transient pool the render graph places into. One VkDeviceMemory, bound at explicit
    // offsets; VMA owns everything else.
    VkDeviceMemory transient_memory_ = VK_NULL_HANDLE;
    u64 transient_bytes_ = 0;
    u32 transient_memory_type_ = ~0U;

    // Breadcrumbs: a host-visible buffer the GPU writes a value into per pass, read back after a
    // device loss to name the pass the GPU reached.
    VkBuffer breadcrumb_buffer_ = VK_NULL_HANDLE;
    VmaAllocation breadcrumb_allocation_ = nullptr;
    u32* breadcrumb_mapped_ = nullptr;

    HandlePool<VulkanBuffer, BufferTag> buffers_;
    HandlePool<VulkanTexture, TextureTag> textures_;
    HandlePool<VulkanTextureView, TextureViewTag> views_;
    HandlePool<VulkanSampler, SamplerTag> samplers_;
    HandlePool<VulkanShaderModule, ShaderModuleTag> shaders_;
    HandlePool<VulkanDescriptorSetLayout, DescriptorSetLayoutTag> set_layouts_;
    HandlePool<VulkanPipelineLayout, PipelineLayoutTag> pipeline_layouts_;
    HandlePool<VulkanDescriptorSet, DescriptorSetTag> descriptor_sets_;
    HandlePool<VulkanPipeline, GraphicsPipelineTag> graphics_pipelines_;
    HandlePool<VulkanPipeline, ComputePipelineTag> compute_pipelines_;
    HandlePool<VulkanQueryPool, QueryPoolTag> query_pools_;
    HandlePool<VulkanFence, FenceTag> fences_;
    HandlePool<VulkanSemaphore, SemaphoreTag> semaphores_;
    HandlePool<VulkanCommandBuffer, CommandBufferTag> command_buffers_;

    struct SwapchainRecord {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        SwapchainInfo info{};
        Array<TextureHandle> textures;
        Array<TextureViewHandle> views;
        PresentMode requested = PresentMode::Fifo;

        explicit SwapchainRecord(Allocator& allocator) noexcept
            : textures(allocator), views(allocator) {}
    };
    HandlePool<SwapchainRecord, SwapchainTag> swapchains_;

    /// acquire_command_buffer() is called from job workers during parallel recording, so the
    /// bookkeeping around it — the lazily created pools and the live-buffer list — is guarded. It
    /// is a handful of calls per frame, not a hot path.
    std::mutex acquire_mutex_;
    Array<CommandBufferHandle> live_command_buffers_;
    Array<TextureHandle> live_transient_textures_;
    Array<BufferHandle> live_transient_buffers_;
    Array<Retirement> retirements_;
    Array<BindlessIndex> bindless_free_;

    u64 live_bytes_[kGpuMemoryCategoryCount] = {};
    u64 peak_bytes_[kGpuMemoryCategoryCount] = {};
    u64 allocation_count_ = 0;

    VulkanBarrierRecorder barriers_;
    ValidationCallback validation_callback_ = nullptr;
    void* validation_user_ = nullptr;
    bool break_on_validation_error_ = false;
    bool debug_markers_ = false;
    bool memory_budget_ = false;
};

/// Set up the swapchain half of the device. Declared here because it lives in its own translation
/// unit and needs the class.
Expected<SwapchainHandle, Error> create_swapchain_impl(VulkanDevice& device,
                                                       const SwapchainDescription& desc) noexcept;

}  // namespace cy::rhi::vulkan
