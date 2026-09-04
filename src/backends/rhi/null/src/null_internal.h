#pragma once
// The null backend's concrete types. Private to this module.
//
// One header rather than two files that each declare half of NullDevice: the command buffer needs
// the device (it appends to its log and bumps its counters) and the device needs the command buffer
// (it hands them out), and splitting a mutual reference across two headers only produces two places
// to keep in step.

#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/handle_pool.h>

#include <mutex>

namespace cy::rhi::null {

/// A fixed-size copy of a resource's name.
///
/// The descriptions callers pass carry a `const char*` that this module does not own and that may
/// not outlive the create call. Every stored description therefore points at one of these, which is
/// why `buffer_description()` can hand a caller a pointer that stays valid for the resource's life.
struct StoredName {
    char text[64] = {};

    void assign(const char* source) noexcept;
};

struct NullBuffer {
    BufferDescription desc{};
    StoredName name{};
    /// Host storage for a mapped buffer, so that a readback test on the null backend reads its own
    /// zeroes rather than dereferencing null. Device-local buffers have none.
    void* storage = nullptr;
    u64 storage_bytes = 0;
    bool transient = false;
    bool bound = true;
    u64 transient_offset = 0;
    GpuMemoryCategory category = GpuMemoryCategory::Persistent;
};

struct NullTexture {
    TextureDescription desc{};
    StoredName name{};
    bool transient = false;
    bool bound = true;
    u64 transient_offset = 0;
    u64 byte_size = 0;
    GpuMemoryCategory category = GpuMemoryCategory::Persistent;
    /// Swapchain images are owned by the swapchain, not by the resource pool: destroying one
    /// through destroy_texture() would be a defect and this is what catches it.
    bool owned_by_swapchain = false;
};

struct NullTextureView {
    TextureViewDescription desc{};
    StoredName name{};
    SubresourceRange resolved{};
};

struct NullSampler {
    SamplerDescription desc{};
    StoredName name{};
};

struct NullShaderModule {
    StoredName name{};
    ShaderStage stage = ShaderStage::None;
    u64 word_count = 0;
    /// A hash of the SPIR-V, so the pipeline cache key is a function of the module's contents
    /// rather than of its handle — which is what makes a warm start a hit rather than a miss.
    u64 code_hash = 0;
};

struct NullDescriptorSetLayout {
    StoredName name{};
    u32 binding_count = 0;
    bool has_runtime_array = false;
};

struct NullPipelineLayout {
    StoredName name{};
    u32 set_count = 0;
    u32 push_constant_bytes = 0;
};

struct NullDescriptorSet {
    DescriptorSetLayoutHandle layout;
    bool per_frame = false;
    u32 frame_slot = 0;
    u32 write_count = 0;
};

struct NullPipeline {
    StoredName name{};
    u64 state_hash = 0;
    bool compute = false;
};

struct NullQueryPool {
    StoredName name{};
    QueryKind kind = QueryKind::Timestamp;
    u32 count = 0;
};

struct NullFence {
    bool signalled = false;
};

struct NullSemaphore {
    u64 value = 0;
};

struct NullSwapchain {
    StoredName name{};
    SwapchainInfo info{};
    Array<TextureHandle> textures;
    Array<TextureViewHandle> views;
    u32 next_image = 0;

    explicit NullSwapchain(Allocator& allocator) noexcept : textures(allocator), views(allocator) {}
};

class NullDevice;

/// The recording interface. Every call appends to the device's log and returns; nothing executes.
class NullCommandBuffer final : public CommandBuffer {
public:
    NullCommandBuffer(NullDevice* device, Allocator& allocator, QueueKind queue, bool secondary,
                      u32 frame_slot) noexcept
        : device_(device),
          log_(allocator),
          queue_(queue),
          secondary_(secondary),
          frame_slot_(frame_slot) {}

    [[nodiscard]] CommandBufferHandle handle() const noexcept override { return handle_; }
    /// The pool issues the handle after the object exists, so it is set once, immediately, by
    /// acquire_command_buffer(). Never null by the time a caller can see the object.
    void set_handle(CommandBufferHandle handle) noexcept { handle_ = handle; }
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

    /// Null: there is no backend object to escape to, which is the correct amount of friction for
    /// code that reached for the escape hatch. See CommandBuffer::native_handle().
    [[nodiscard]] void* native_handle() noexcept override { return nullptr; }

    /// THE COMMAND STREAM IS PER BUFFER, NOT PER DEVICE, and that is what makes parallel recording
    /// safe here and deterministic. Two workers recording into two command buffers touch two
    /// different arrays; the device's log is assembled from them in SUBMISSION order — a
    /// secondary's commands are spliced into its primary where it is executed, and a primary's are
    /// appended when it is submitted. So the stream a frame produces does not depend on which
    /// worker finished first, which is exactly what `rhi-and-render-graph` requires of parallel
    /// recording.
    void append(const RecordedCommand& command) noexcept;
    [[nodiscard]] Span<const RecordedCommand> log() const noexcept {
        return {log_.data(), log_.size()};
    }
    void take_log_into(Array<RecordedCommand>& target) noexcept;
    [[nodiscard]] Array<RecordedCommand>& mutable_log() noexcept { return log_; }

private:
    NullDevice* device_ = nullptr;
    Array<RecordedCommand> log_;
    CommandBufferHandle handle_;
    QueueKind queue_ = QueueKind::Graphics;
    bool secondary_ = false;
    bool recording_ = false;
    u32 frame_slot_ = 0;
};

/// The only object in this backend that can emit a barrier, and it can only be reached with a key
/// the render graph's executor alone constructs. See cy/backends/rhi/barrier.h.
class NullBarrierRecorder final : public BarrierRecorder {
public:
    explicit NullBarrierRecorder(NullDevice* device) noexcept : device_(device) {}

    void record_barriers(CommandBufferHandle command_buffer,
                         const BarrierBatch& batch) noexcept override;
    [[nodiscard]] u64 recorded_batch_count() const noexcept override { return batches_; }
    [[nodiscard]] u64 recorded_barrier_count() const noexcept override { return barriers_; }

private:
    NullDevice* device_ = nullptr;
    u64 batches_ = 0;
    u64 barriers_ = 0;
};

class NullDevice final : public Device {
public:
    NullDevice(Allocator& allocator, const DeviceDescription& desc) noexcept;
    ~NullDevice() override;

    // --- Introspection -------------------------------------------------------------------------

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    [[nodiscard]] DescriptorModel descriptor_model() const noexcept override {
        return DescriptorModel::Bindless;
    }
    [[nodiscard]] u32 frames_in_flight() const noexcept override { return frames_in_flight_; }
    [[nodiscard]] u32 queue_family(QueueKind queue) const noexcept override;
    [[nodiscard]] bool has_queue(QueueKind queue) const noexcept override;
    void set_validation_callback(ValidationCallback callback, void* user) noexcept override;

    // --- Resources -----------------------------------------------------------------------------

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

    // --- Transients ----------------------------------------------------------------------------

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

    // --- Shaders, descriptors, pipelines --------------------------------------------------------

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

    // --- Frames and submission
    // --------------------------------------------------------------------

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

    // --- Swapchain
    // --------------------------------------------------------------------------------

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

    // --- Reporting
    // ---------------------------------------------------------------------------------

    [[nodiscard]] GpuMemoryReport memory_report() const noexcept override { return memory_; }
    [[nodiscard]] const DeviceStatistics& statistics() const noexcept override { return stats_; }
    void reset_statistics() noexcept override { stats_ = DeviceStatistics{}; }
    void publish_memory_pressure() noexcept override;

    [[nodiscard]] BarrierRecorder& barrier_recorder(const GraphBarrierKey& key) noexcept override;
    [[nodiscard]] void* native_handle() noexcept override { return nullptr; }

    // --- This backend's own surface ------------------------------------------------------------

    /// The command buffer behind a handle, for the barrier recorder. Null when the handle is stale.
    [[nodiscard]] NullCommandBuffer* buffer_for(CommandBufferHandle handle) noexcept {
        return command_buffers_.resolve(handle);
    }
    [[nodiscard]] Span<const RecordedCommand> log() const noexcept;
    [[nodiscard]] u64 log_hash() const noexcept { return log_hash_; }
    void clear_log() noexcept;
    [[nodiscard]] DeviceStatistics& mutable_statistics() noexcept { return stats_; }

private:
    void absorb(NullCommandBuffer& buffer) noexcept;
    void charge(GpuMemoryCategory category, u64 bytes) noexcept;
    void discharge(GpuMemoryCategory category, u64 bytes) noexcept;
    void report_validation(ValidationSeverity severity, const char* message) noexcept;

    Allocator* allocator_ = nullptr;
    DeviceCapabilities capabilities_;
    DeviceStatistics stats_{};
    GpuMemoryReport memory_{};
    u32 frames_in_flight_ = kDefaultFramesInFlight;
    u64 frame_index_ = 0;
    u32 frame_slot_ = 0;
    bool frame_open_ = false;
    u64 timelines_[kQueueKindCount] = {};
    u32 queue_families_[kQueueKindCount] = {};

    HandlePool<NullBuffer, BufferTag> buffers_;
    HandlePool<NullTexture, TextureTag> textures_;
    HandlePool<NullTextureView, TextureViewTag> views_;
    HandlePool<NullSampler, SamplerTag> samplers_;
    HandlePool<NullShaderModule, ShaderModuleTag> shaders_;
    HandlePool<NullDescriptorSetLayout, DescriptorSetLayoutTag> set_layouts_;
    HandlePool<NullPipelineLayout, PipelineLayoutTag> pipeline_layouts_;
    HandlePool<NullDescriptorSet, DescriptorSetTag> descriptor_sets_;
    HandlePool<NullPipeline, GraphicsPipelineTag> graphics_pipelines_;
    HandlePool<NullPipeline, ComputePipelineTag> compute_pipelines_;
    HandlePool<NullQueryPool, QueryPoolTag> query_pools_;
    HandlePool<NullFence, FenceTag> fences_;
    HandlePool<NullSemaphore, SemaphoreTag> semaphores_;
    HandlePool<NullCommandBuffer, CommandBufferTag> command_buffers_;
    HandlePool<NullSwapchain, SwapchainTag> swapchains_;

    /// acquire_command_buffer() may be called from a job worker, so the bookkeeping around it is
    /// guarded. It is a handful of calls per frame, not a hot path.
    std::mutex acquire_mutex_;
    Array<CommandBufferHandle> live_command_buffers_;
    Array<TextureHandle> live_transient_textures_;
    Array<BufferHandle> live_transient_buffers_;
    Array<RecordedCommand> log_;
    u64 log_hash_ = 0;
    u64 transient_bytes_ = 0;
    u64 transient_high_water_ = 0;
    u32 bindless_next_ = 0;
    Array<BindlessIndex> bindless_free_;

    NullBarrierRecorder barriers_;
    ValidationCallback validation_callback_ = nullptr;
    void* validation_user_ = nullptr;
    bool validation_enabled_ = false;
};

}  // namespace cy::rhi::null
