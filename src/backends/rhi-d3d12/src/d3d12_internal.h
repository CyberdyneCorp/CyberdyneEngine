#pragma once
// Direct3D 12 implementation records. Windows SDK types stay in this private header.

#include <cy/backends/rhi-d3d12/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/core/memory/handle_pool.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

// WinBase and WinSpool publish these generic names as preprocessor macros. They collide with the
// RHI's C++ types and would otherwise silently rewrite declarations in this private header.
#ifdef DeviceCapabilities
#    undef DeviceCapabilities
#endif
#ifdef MemoryBarrier
#    undef MemoryBarrier
#endif

#include <mutex>

namespace cy::rhi::d3d12 {

using Microsoft::WRL::ComPtr;

struct StoredName {
    char text[64] = {};
    void assign(const char* source) noexcept;
};

struct D3D12Buffer {
    BufferDescription desc{};
    StoredName name{};
    ComPtr<ID3D12Resource> resource;
    void* mapped = nullptr;
    bool transient = false;
    bool bound = true;
    u64 bytes = 0;
};

struct D3D12Texture {
    TextureDescription desc{};
    StoredName name{};
    ComPtr<ID3D12Resource> resource;
    bool transient = false;
    bool bound = true;
    bool swapchain_owned = false;
    u64 bytes = 0;
};

struct D3D12TextureView {
    TextureViewDescription desc{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    bool has_srv = false;
    bool has_rtv = false;
    bool has_dsv = false;
};

struct D3D12Sampler {
    SamplerDescription desc{};
};

struct D3D12ShaderModule {
    StoredName name{};
    StoredName entry_point{};
    ShaderStage stage = ShaderStage::None;
    Array<u8> bytecode;
    explicit D3D12ShaderModule(Allocator& allocator) noexcept : bytecode(allocator) {}
};

inline constexpr u32 kD3D12MaxLayoutBindings = 64;

struct D3D12LayoutBinding {
    u32 binding = 0;
    DescriptorKind kind = DescriptorKind::UniformBuffer;
    u32 count = 1;
    u32 resource_offset = 0;
    u32 sampler_offset = 0;
    u32 shader_register = 0;
    u32 sampler_register = 0;
};

struct D3D12DescriptorSetLayout {
    StoredName name{};
    D3D12LayoutBinding bindings[kD3D12MaxLayoutBindings]{};
    u32 binding_count = 0;
    u32 resource_count = 0;
    u32 sampler_count = 0;
};

struct D3D12PipelineLayout {
    StoredName name{};
    ComPtr<ID3D12RootSignature> root_signature;
    DescriptorSetLayoutHandle set_layouts[kMaxDescriptorSets]{};
    u32 resource_root[kMaxDescriptorSets]{};
    u32 sampler_root[kMaxDescriptorSets]{};
    u32 set_count = 0;
    u32 push_root = ~0U;
    u32 push_dwords = 0;
};

struct D3D12DescriptorSet {
    DescriptorSetLayoutHandle layout{};
    u32 resource_base = 0;
    u32 sampler_base = 0;
    bool per_frame = false;
    u32 frame_slot = 0;
};

struct D3D12GraphicsPipeline {
    ComPtr<ID3D12PipelineState> pipeline;
    PipelineLayoutHandle layout{};
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    u32 vertex_strides[kMaxVertexAttributes]{};
    u32 vertex_binding_count = 0;
};

struct D3D12ComputePipeline {
    ComPtr<ID3D12PipelineState> pipeline;
    PipelineLayoutHandle layout{};
};

struct D3D12QueryPool {
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    QueryKind kind = QueryKind::Timestamp;
    u32 count = 0;
};

struct D3D12Fence {
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    u64 value = 0;

    D3D12Fence() = default;
    D3D12Fence(const D3D12Fence&) = delete;
    D3D12Fence& operator=(const D3D12Fence&) = delete;
    D3D12Fence(D3D12Fence&& other) noexcept
        : fence(std::move(other.fence)), event(other.event), value(other.value) {
        other.event = nullptr;
    }
    ~D3D12Fence() {
        if (event != nullptr) {
            CloseHandle(event);
        }
    }
};

struct D3D12Semaphore {
    ComPtr<ID3D12Fence> fence;
    u64 next_signal = 1;
    u64 next_wait = 1;
};

struct D3D12Swapchain {
    ComPtr<IDXGISwapChain3> swapchain;
    SwapchainInfo info{};
    TextureHandle textures[kMaxFramesInFlight]{};
    TextureViewHandle views[kMaxFramesInFlight]{};
};

class D3D12Device;

class D3D12CommandBuffer final : public CommandBuffer {
public:
    D3D12CommandBuffer(D3D12Device* device, QueueKind queue, bool secondary,
                       u32 frame_slot) noexcept;

    [[nodiscard]] CommandBufferHandle handle() const noexcept override { return handle_; }
    void set_handle(CommandBufferHandle handle) noexcept { handle_ = handle; }
    [[nodiscard]] QueueKind queue() const noexcept { return queue_; }
    [[nodiscard]] bool secondary() const noexcept { return secondary_; }
    [[nodiscard]] bool recording() const noexcept { return recording_; }
    void set_recording(bool recording) noexcept { recording_ = recording; }
    [[nodiscard]] ID3D12GraphicsCommandList* raw() const noexcept { return list.Get(); }

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
    [[nodiscard]] void* native_handle() noexcept override { return list.Get(); }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;

private:
    D3D12Device* device_ = nullptr;
    CommandBufferHandle handle_{};
    QueueKind queue_ = QueueKind::Graphics;
    bool secondary_ = false;
    bool recording_ = false;
    bool compute_bound_ = false;
    u32 frame_slot_ = 0;
    GraphicsPipelineHandle graphics_pipeline_{};
};

class D3D12BarrierRecorder final : public BarrierRecorder {
public:
    explicit D3D12BarrierRecorder(D3D12Device* device) noexcept : device_(device) {}
    void record_barriers(CommandBufferHandle command_buffer,
                         const BarrierBatch& batch) noexcept override;
    [[nodiscard]] u64 recorded_batch_count() const noexcept override { return batches_; }
    [[nodiscard]] u64 recorded_barrier_count() const noexcept override { return barriers_; }

private:
    D3D12Device* device_ = nullptr;
    u64 batches_ = 0;
    u64 barriers_ = 0;
};

class D3D12Device final : public Device {
public:
    D3D12Device(Allocator& allocator, const DeviceDescription& desc) noexcept;
    ~D3D12Device() override;
    [[nodiscard]] Status initialize() noexcept;

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    [[nodiscard]] DescriptorModel descriptor_model() const noexcept override {
        return descriptor_model_;
    }
    [[nodiscard]] u32 frames_in_flight() const noexcept override { return frames_in_flight_; }
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
    Status reserve_transient_memory(u64 bytes, MemoryPoolClass pool_class) override;
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
    [[nodiscard]] DescriptorSetLayoutHandle global_texture_table_layout() const noexcept override {
        return bindless_layout_;
    }
    [[nodiscard]] DescriptorSetHandle global_texture_table() const noexcept override {
        return bindless_set_;
    }
    Status set_global_sampler(SamplerHandle sampler) noexcept override;
    Expected<GraphicsPipelineHandle, Error> create_graphics_pipeline(
        const GraphicsPipelineDescription& desc) override;
    void destroy_graphics_pipeline(GraphicsPipelineHandle handle) noexcept override;
    Expected<ComputePipelineHandle, Error> create_compute_pipeline(
        const ComputePipelineDescription& desc) override;
    void destroy_compute_pipeline(ComputePipelineHandle handle) noexcept override;
    Status save_pipeline_cache(const char* path) override;
    Status load_pipeline_cache(const char* path) override;

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

    [[nodiscard]] GpuMemoryReport memory_report() const noexcept override { return memory_; }
    [[nodiscard]] const DeviceStatistics& statistics() const noexcept override { return stats_; }
    void reset_statistics() noexcept override { stats_ = DeviceStatistics{}; }
    void publish_memory_pressure() noexcept override;
    [[nodiscard]] BarrierRecorder& barrier_recorder(const GraphBarrierKey&) noexcept override {
        return barriers_;
    }
    [[nodiscard]] void* native_handle() noexcept override { return device_.Get(); }

    [[nodiscard]] D3D12Buffer* buffer(BufferHandle handle) noexcept {
        return buffers_.resolve(handle);
    }
    [[nodiscard]] D3D12Texture* texture(TextureHandle handle) noexcept {
        return textures_.resolve(handle);
    }
    [[nodiscard]] D3D12TextureView* view(TextureViewHandle handle) noexcept {
        return views_.resolve(handle);
    }
    [[nodiscard]] D3D12Sampler* sampler(SamplerHandle handle) noexcept {
        return samplers_.resolve(handle);
    }
    [[nodiscard]] D3D12PipelineLayout* pipeline_layout(PipelineLayoutHandle handle) noexcept {
        return pipeline_layouts_.resolve(handle);
    }
    [[nodiscard]] D3D12DescriptorSet* descriptor_set(DescriptorSetHandle handle) noexcept {
        return descriptor_sets_.resolve(handle);
    }
    [[nodiscard]] D3D12GraphicsPipeline* graphics_pipeline(GraphicsPipelineHandle handle) noexcept {
        return graphics_pipelines_.resolve(handle);
    }
    [[nodiscard]] D3D12ComputePipeline* compute_pipeline(ComputePipelineHandle handle) noexcept {
        return compute_pipelines_.resolve(handle);
    }
    [[nodiscard]] D3D12QueryPool* query_pool(QueryPoolHandle handle) noexcept {
        return query_pools_.resolve(handle);
    }
    [[nodiscard]] D3D12CommandBuffer* commands(CommandBufferHandle handle) noexcept {
        return command_buffers_.resolve(handle);
    }
    [[nodiscard]] ID3D12Device* raw_device() noexcept { return device_.Get(); }
    [[nodiscard]] ID3D12DescriptorHeap* resource_heap() noexcept { return resource_heap_.Get(); }
    [[nodiscard]] ID3D12DescriptorHeap* sampler_heap() noexcept { return sampler_heap_.Get(); }
    [[nodiscard]] ID3D12CommandSignature* draw_indexed_signature() noexcept {
        return draw_indexed_signature_.Get();
    }
    [[nodiscard]] ID3D12CommandSignature* dispatch_signature() noexcept {
        return dispatch_signature_.Get();
    }
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE resource_gpu(u32 index) const noexcept;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu(u32 index) const noexcept;
    void add_draw() noexcept { ++stats_.draws; }
    void add_dispatch() noexcept { ++stats_.dispatches; }
    void report_validation(ValidationSeverity severity, const char* message) noexcept;

private:
    void configure_capabilities(const AdapterIdentity& identity) noexcept;
    void probe_format_features() noexcept;
    [[nodiscard]] Status create_descriptor_heaps() noexcept;
    [[nodiscard]] Status create_command_signatures() noexcept;
    [[nodiscard]] Status create_queues() noexcept;
    [[nodiscard]] Status create_bindless_table() noexcept;
    [[nodiscard]] u32 allocate_resource_descriptors(u32 count) noexcept;
    [[nodiscard]] u32 allocate_sampler_descriptors(u32 count) noexcept;
    void charge(GpuMemoryCategory category, u64 bytes) noexcept;
    void discharge(GpuMemoryCategory category, u64 bytes) noexcept;
    [[nodiscard]] bool drain_validation_messages() noexcept;

    Allocator* allocator_ = nullptr;
    DeviceDescription description_{};
    DeviceCapabilities capabilities_{};
    DescriptorModel descriptor_model_ = DescriptorModel::Compatibility;
    DeviceStatistics stats_{};
    GpuMemoryReport memory_{};
    u64 reported_gpu_bytes_ = 0;
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12InfoQueue> info_queue_;
    ComPtr<ID3D12CommandQueue> queues_[kQueueKindCount];
    ComPtr<ID3D12Fence> timelines_[kQueueKindCount];
    HANDLE timeline_events_[kQueueKindCount]{};
    u64 timeline_values_[kQueueKindCount]{};
    D3D12_RESOURCE_HEAP_TIER heap_tier_ = D3D12_RESOURCE_HEAP_TIER_1;
    u32 frames_in_flight_ = kDefaultFramesInFlight;
    u64 frame_index_ = 0;
    u32 frame_slot_ = 0;
    bool frame_open_ = false;

    ComPtr<ID3D12DescriptorHeap> resource_heap_;
    ComPtr<ID3D12DescriptorHeap> sampler_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12CommandSignature> draw_indexed_signature_;
    ComPtr<ID3D12CommandSignature> dispatch_signature_;
    u32 resource_stride_ = 0;
    u32 sampler_stride_ = 0;
    u32 rtv_stride_ = 0;
    u32 dsv_stride_ = 0;
    u32 next_resource_descriptor_ = 0;
    u32 next_sampler_descriptor_ = 0;
    u32 next_rtv_descriptor_ = 0;
    u32 next_dsv_descriptor_ = 0;

    ComPtr<ID3D12Heap> transient_heap_;
    u64 transient_bytes_ = 0;
    D3D12_HEAP_FLAGS transient_flags_ = D3D12_HEAP_FLAG_NONE;
    Array<TextureHandle> transient_textures_;
    Array<BufferHandle> transient_buffers_;
    Array<CommandBufferHandle> live_commands_;
    std::mutex acquire_mutex_;

    HandlePool<D3D12Buffer, BufferTag> buffers_;
    HandlePool<D3D12Texture, TextureTag> textures_;
    HandlePool<D3D12TextureView, TextureViewTag> views_;
    HandlePool<D3D12Sampler, SamplerTag> samplers_;
    HandlePool<D3D12ShaderModule, ShaderModuleTag> shaders_;
    HandlePool<D3D12DescriptorSetLayout, DescriptorSetLayoutTag> set_layouts_;
    HandlePool<D3D12PipelineLayout, PipelineLayoutTag> pipeline_layouts_;
    HandlePool<D3D12DescriptorSet, DescriptorSetTag> descriptor_sets_;
    HandlePool<D3D12GraphicsPipeline, GraphicsPipelineTag> graphics_pipelines_;
    HandlePool<D3D12ComputePipeline, ComputePipelineTag> compute_pipelines_;
    HandlePool<D3D12QueryPool, QueryPoolTag> query_pools_;
    HandlePool<D3D12Fence, FenceTag> fences_;
    HandlePool<D3D12Semaphore, SemaphoreTag> semaphores_;
    HandlePool<D3D12CommandBuffer, CommandBufferTag> command_buffers_;
    HandlePool<D3D12Swapchain, SwapchainTag> swapchains_;

    DescriptorSetLayoutHandle bindless_layout_{};
    DescriptorSetHandle bindless_set_{};
    SamplerHandle bindless_sampler_{};
    Array<BindlessIndex> bindless_free_;
    u32 bindless_next_ = 0;
    D3D12BarrierRecorder barriers_;
    ValidationCallback validation_callback_ = nullptr;
    void* validation_user_ = nullptr;
};

}  // namespace cy::rhi::d3d12
