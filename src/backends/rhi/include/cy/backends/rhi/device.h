#pragma once
// The device: the RHI's whole object lifetime, submission and memory surface. Tasks 2.1.1, 2.1.3,
// 2.2.6 and 2.2.7.
//
// One interface, implemented by the null backend first (design.md §1) and by Vulkan second. The
// order is the point: written first, the null backend forces this to be an interface rather than a
// thin wrapper over whichever Vulkan calls were convenient, because there is no Vulkan to lean on.
//
// WHAT A DEVICE OWNS
//   * every resource, addressed by a generational handle, so a stale handle fails validation
//     rather than aliasing whatever took the slot;
//   * `frames_in_flight` frames, each with its own command pools, descriptor pools and transient
//     memory, and the fence that says when frame N's resources may be reused;
//   * the retirement queue: destroying a resource frees it only after every frame that could
//     reference it has completed;
//   * one timeline semaphore per queue, which is what a cross-queue dependency becomes;
//   * the memory allocator and its report into the engine's memory domain tree.
//
// WHAT A DEVICE DOES NOT OWN. A window: `create_swapchain` is handed a native surface that
// DisplayServer produced, and the RHI never asks how. A render graph: the graph is layer 4 and sits
// above this. And a barrier: `barrier_recorder()` below needs a key only the graph's executor can
// construct.

#include <cy/backends/rhi/barrier.h>
#include <cy/backends/rhi/capabilities.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/handles.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/rhi/resources.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>

namespace cy::rhi {

// --- Submission -----------------------------------------------------------------------------

/// A wait on another queue's timeline. This is what a cross-queue dependency becomes: the render
/// graph never emits a pipeline barrier across queues, because a barrier synchronises nothing
/// between two command streams. (M3 spike, negative control "B-nosem": dropping this wait produces
/// SYNC-HAZARD-WRITE-RACING-WRITE, which is the harness proving it has teeth.)
struct TimelineWait {
    QueueKind queue = QueueKind::Graphics;
    u64 value = 0;
    Stage stage = Stage::AllCommands;
};

/// One submission. The render graph produces a sequence of these and the device executes them in
/// order; deterministic submission (design.md §6) means the same frame description produces the
/// same sequence, which is what makes a golden image reproduce and a replay render twice the same.
struct SubmitInfo {
    QueueKind queue = QueueKind::Graphics;
    Span<const CommandBufferHandle> command_buffers;
    Span<const TimelineWait> waits;
    /// Presentation's own synchronisation, which is binary rather than timeline because the
    /// presentation engine signals it. Null handles when a submit does not touch the swapchain.
    SemaphoreHandle wait_binary;
    Stage wait_binary_stage = Stage::ColorAttachmentOutput;
    SemaphoreHandle signal_binary;
    /// Signalled when this submission completes. The frame's own fence, usually.
    FenceHandle signal_fence;
};

// --- Memory ---------------------------------------------------------------------------------

/// What GPU memory is being spent on. `rhi-and-render-graph` requires GPU memory to be reported
/// "into the engine's memory domain and budget tree ... as the `GPU` domain with sub-domains for
/// persistent, streaming, upload and readback, and transient graph memory".
///
/// M1's cy::MemoryDomain has `Gpu` but no children for these five, so the RHI reports the total
/// against MemoryDomain::Gpu and carries this breakdown itself. Splitting MemoryDomain is an edit
/// to src/core/memory/, which is not this module's to make; the breakdown is here so the number
/// exists and the split is a mechanical change when it happens.
enum class GpuMemoryCategory : u8 {
    Persistent = 0,  // render targets and buffers that outlive a frame
    Streaming,       // resident asset data
    Upload,          // the staging ring
    Readback,
    Transient,  // the render graph's aliased pool
    Count,
};

inline constexpr u32 kGpuMemoryCategoryCount = static_cast<u32>(GpuMemoryCategory::Count);

[[nodiscard]] const char* gpu_memory_category_name(GpuMemoryCategory category) noexcept;

struct GpuMemoryReport {
    u64 live_bytes[kGpuMemoryCategoryCount] = {};
    u64 peak_bytes[kGpuMemoryCategoryCount] = {};
    u64 allocation_count = 0;
    /// What the device says about its own heap, where it will say. Zero when the device does not
    /// report a budget (Capability::MemoryBudgetReporting false), which is not the same as a budget
    /// of zero and a caller must not treat it as one.
    u64 device_heap_size = 0;
    u64 device_heap_used = 0;
    u64 device_heap_budget = 0;
};

/// What one resource needs from an allocator. Answered without binding anything, which is what
/// lets the render graph plan a whole frame's transient memory before a byte of it is reserved.
struct MemoryRequirements {
    u64 size = 0;
    u64 alignment = 1;
    /// A bitmask of the memory types the resource may live in. The graph intersects it over every
    /// transient it places, because one pool must be legal for all of them.
    u32 memory_type_bits = ~0U;
};

// --- Statistics -----------------------------------------------------------------------------

/// Counters `rhi-and-render-graph` requires to reach the shared trace, so that GPU behaviour
/// correlates with task, memory and streaming activity on one timeline.
struct DeviceStatistics {
    u64 frames_begun = 0;
    u64 frames_completed = 0;
    u64 submissions = 0;
    u64 command_buffers_recorded = 0;
    u64 draws = 0;
    u64 dispatches = 0;
    u64 barrier_batches = 0;
    u64 barriers = 0;
    u64 semaphore_waits = 0;
    u64 queue_ownership_transfers = 0;
    u64 pipeline_cache_hits = 0;
    u64 pipeline_cache_misses = 0;
    u64 resources_retired = 0;
    u64 resources_freed = 0;
    u64 validation_errors = 0;
    u64 validation_warnings = 0;
};

// --- Device creation --------------------------------------------------------------------------

struct DeviceDescription {
    const char* application_name = "CyberdyneEngine";
    u32 frames_in_flight = kDefaultFramesInFlight;
    /// Development builds turn the backend's validation layers on. `rhi-and-render-graph`: "A frame
    /// that renders but trips validation is not a frame that works." Off in Profile and Shipping,
    /// where the layers do not exist.
    bool enable_validation = false;
    /// Synchronisation validation is off even when the layers are on, and without it none of the
    /// negative controls fire. (Spike gotcha 6h.)
    bool enable_synchronisation_validation = false;
    /// Break into the debugger on a validation error rather than only logging it.
    bool break_on_validation_error = false;
    /// Ask for a dedicated async-compute queue. A device without one reports
    /// Capability::AsyncCompute false and the graph folds those passes onto graphics.
    bool request_async_compute = true;
    bool request_transfer_queue = true;
    /// Bytes reserved for the render graph's transient pool. Grown on demand; a starting size stops
    /// the first frame from reallocating five times.
    u64 transient_pool_bytes = 64ULL * 1024 * 1024;
    u64 upload_ring_bytes = 16ULL * 1024 * 1024;
};

/// Where a device sends validation messages. Installed by the caller so that
/// `rhi-and-render-graph`'s "log it with the pass name and, by configuration, break into the
/// debugger" is the host's policy rather than the backend's.
enum class ValidationSeverity : u8 { Info, Warning, Error };

using ValidationCallback = void (*)(ValidationSeverity severity, const char* message, void* user);

/// The abstract device. Every method that can fail returns Expected; everything else is noexcept
/// and cannot, which is a property the null backend is written to hold as strictly as Vulkan is.
class Device {
public:
    Device() = default;
    virtual ~Device() = default;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    [[nodiscard]] virtual const DeviceCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual DescriptorModel descriptor_model() const noexcept = 0;
    [[nodiscard]] virtual u32 frames_in_flight() const noexcept = 0;

    /// The backend's queue-family index for a queue kind, which is what an ownership transfer
    /// names. Two kinds may share a family — and on a device with no dedicated async compute they
    /// do, which is exactly what makes the transfer disappear rather than needing a special case.
    [[nodiscard]] virtual u32 queue_family(QueueKind queue) const noexcept = 0;
    [[nodiscard]] virtual bool has_queue(QueueKind queue) const noexcept = 0;

    virtual void set_validation_callback(ValidationCallback callback, void* user) noexcept = 0;

    // --- Resources ------------------------------------------------------------------------------
    //
    // Creation validates against the hard limits in types.h and fails naming the limit. Destruction
    // is deferred: `rhi-and-render-graph` requires a resource destroyed during frame N to be
    // released only after frame N's fence has signalled, and that deferral goes through the same
    // retirement mechanism the engine uses for CPU memory rather than a GPU-specific scheme.

    virtual Expected<BufferHandle, Error> create_buffer(const BufferDescription& desc) = 0;
    virtual void destroy_buffer(BufferHandle handle) noexcept = 0;
    [[nodiscard]] virtual bool is_valid(BufferHandle handle) const noexcept = 0;
    /// The mapped pointer of a host-visible buffer, or null. Persistent: the RHI maps once at
    /// creation rather than around each write, because mapping is not free and a per-frame ring is
    /// written every frame.
    [[nodiscard]] virtual void* buffer_mapped_pointer(BufferHandle handle) noexcept = 0;
    [[nodiscard]] virtual const BufferDescription* buffer_description(
        BufferHandle handle) const noexcept = 0;

    virtual Expected<TextureHandle, Error> create_texture(const TextureDescription& desc) = 0;
    virtual void destroy_texture(TextureHandle handle) noexcept = 0;
    [[nodiscard]] virtual bool is_valid(TextureHandle handle) const noexcept = 0;
    [[nodiscard]] virtual const TextureDescription* texture_description(
        TextureHandle handle) const noexcept = 0;

    virtual Expected<TextureViewHandle, Error> create_texture_view(
        const TextureViewDescription& desc) = 0;
    virtual void destroy_texture_view(TextureViewHandle handle) noexcept = 0;
    [[nodiscard]] virtual bool is_valid(TextureViewHandle handle) const noexcept = 0;

    virtual Expected<SamplerHandle, Error> create_sampler(const SamplerDescription& desc) = 0;
    virtual void destroy_sampler(SamplerHandle handle) noexcept = 0;

    virtual Expected<QueryPoolHandle, Error> create_query_pool(
        const QueryPoolDescription& desc) = 0;
    virtual void destroy_query_pool(QueryPoolHandle handle) noexcept = 0;
    /// Results in the pool's own units — nanoseconds for a timestamp pool, already scaled by the
    /// device's timestamp period so a caller never multiplies by a backend constant.
    virtual Expected<u32, Error> read_query_results(QueryPoolHandle pool, u32 first, u32 count,
                                                    Span<u64> out) = 0;

    // --- Transient resources: the render graph's memory ---------------------------------------
    //
    // `rhi-and-render-graph`: "two intermediate render targets [with] non-overlapping lifetimes ...
    // SHALL share memory, reducing peak GPU memory". The graph decides who shares with whom; the
    // device owns the pool the sharing happens in, because only the device knows what its
    // allocator, its alignment rules and its memory types are.
    //
    // The order is fixed and the spike paid for discovering it (gotcha 6j): create the resources
    // UNBOUND, ask each one what memory it needs, reserve a pool big enough for the plan, bind each
    // at its planned offset, and only then create views. A view needs bound memory, so anything
    // that creates one earlier fails on a device and silently does not on a null backend.
    //
    // M3 measured this on the device: sixteen 4 MiB transients in a read-modify-write chain went
    // from a 64.00 MiB heap delta to 8.00 MiB, and the plan and VK_EXT_memory_budget agreed
    // exactly.

    virtual Expected<TextureHandle, Error> create_transient_texture(
        const TextureDescription& desc) = 0;
    virtual Expected<BufferHandle, Error> create_transient_buffer(
        const BufferDescription& desc) = 0;

    [[nodiscard]] virtual Expected<MemoryRequirements, Error> texture_memory_requirements(
        TextureHandle handle) const = 0;
    [[nodiscard]] virtual Expected<MemoryRequirements, Error> buffer_memory_requirements(
        BufferHandle handle) const = 0;

    /// Reserve the pool the plan needs. `memory_type_bits` is the intersection over every
    /// transient, which is what makes one pool legal for all of them.
    virtual Status reserve_transient_memory(u64 bytes, u32 memory_type_bits) = 0;
    virtual Status bind_transient(TextureHandle handle, u64 offset) = 0;
    virtual Status bind_transient(BufferHandle handle, u64 offset) = 0;
    /// Destroy every transient resource created since the last call. The pool itself is kept, so a
    /// steady-state frame reserves nothing.
    virtual void release_transient_resources() noexcept = 0;
    [[nodiscard]] virtual u64 transient_pool_bytes() const noexcept = 0;

    // --- Shaders, descriptors and pipelines
    // -------------------------------------------------------

    virtual Expected<ShaderModuleHandle, Error> create_shader_module(
        const ShaderModuleDescription& desc) = 0;
    virtual void destroy_shader_module(ShaderModuleHandle handle) noexcept = 0;

    virtual Expected<DescriptorSetLayoutHandle, Error> create_descriptor_set_layout(
        const DescriptorSetLayoutDescription& desc) = 0;
    virtual void destroy_descriptor_set_layout(DescriptorSetLayoutHandle handle) noexcept = 0;

    virtual Expected<PipelineLayoutHandle, Error> create_pipeline_layout(
        const PipelineLayoutDescription& desc) = 0;
    virtual void destroy_pipeline_layout(PipelineLayoutHandle handle) noexcept = 0;

    /// Allocated from the current frame's pool when `per_frame` is set, and freed wholesale when
    /// that frame comes round again; from the persistent pool otherwise. A bindless global table is
    /// persistent, a per-view set is per-frame, and getting that wrong is a leak rather than a
    /// crash, which is why it is a parameter and not a guess.
    virtual Expected<DescriptorSetHandle, Error> allocate_descriptor_set(
        DescriptorSetLayoutHandle layout, bool per_frame) = 0;
    virtual Status update_descriptor_set(DescriptorSetHandle set,
                                         Span<const DescriptorWrite> writes) = 0;

    /// Reserve a slot in the global bindless table for a view or a buffer. kInvalidBindlessIndex
    /// when the device is on the compatibility path or the table is full.
    virtual BindlessIndex bind_texture_globally(TextureViewHandle view,
                                                SamplerHandle sampler) noexcept = 0;
    virtual void release_bindless_index(BindlessIndex index) noexcept = 0;

    /// Pipelines are cached by a hash of their full state and the cache is persisted across runs,
    /// so a warm start compiles nothing. `pipeline_cache_hits` in DeviceStatistics is how a test
    /// tells a warm start from a cold one rather than timing it.
    virtual Expected<GraphicsPipelineHandle, Error> create_graphics_pipeline(
        const GraphicsPipelineDescription& desc) = 0;
    virtual void destroy_graphics_pipeline(GraphicsPipelineHandle handle) noexcept = 0;

    virtual Expected<ComputePipelineHandle, Error> create_compute_pipeline(
        const ComputePipelineDescription& desc) = 0;
    virtual void destroy_compute_pipeline(ComputePipelineHandle handle) noexcept = 0;

    /// The serialised pipeline cache, to be written to disk and handed back through
    /// `load_pipeline_cache` next run. Empty when the backend has none.
    virtual Expected<u64, Error> save_pipeline_cache(Span<u8> out) = 0;
    virtual Status load_pipeline_cache(Span<const u8> data) = 0;

    // --- Frames ---------------------------------------------------------------------------------

    /// Waits on the oldest in-flight frame's fence, recycles that frame's pools, and runs the
    /// retirement queue for it. Returns the frame's index modulo frames_in_flight.
    virtual Expected<u32, Error> begin_frame() = 0;
    virtual Status end_frame() = 0;
    [[nodiscard]] virtual u64 frame_index() const noexcept = 0;
    [[nodiscard]] virtual u32 frame_slot() const noexcept = 0;

    // --- Command buffers and submission
    // -----------------------------------------------------------

    /// A command buffer from this frame's pool for `queue`. `secondary` allocates one that is
    /// recorded on a worker and executed inside a primary — task 2.2.5's parallel recording.
    ///
    /// MAY BE CALLED FROM ANY THREAD, and the buffer it returns MUST be recorded on the thread that
    /// asked for it. The command pool it comes from is that thread's (see kMaxRecordingThreads):
    /// a backend's command allocator is externally synchronised, so two threads recording into
    /// buffers from one pool is a data race whose symptom is a corrupted command stream. Fails with
    /// ErrorCode::OutOfRange when more than kMaxRecordingThreads threads have recorded.
    virtual Expected<CommandBufferHandle, Error> acquire_command_buffer(QueueKind queue,
                                                                        bool secondary) = 0;
    /// The recording interface for a handle, or null when the handle is stale.
    [[nodiscard]] virtual CommandBuffer* command_buffer(CommandBufferHandle handle) noexcept = 0;
    virtual Status begin_command_buffer(CommandBufferHandle handle) = 0;
    virtual Status end_command_buffer(CommandBufferHandle handle) = 0;
    /// Execute secondaries inside a primary, in the order given. Deterministic by construction:
    /// the order is the graph's, never the order the workers happened to finish in.
    virtual Status execute_secondary(CommandBufferHandle primary,
                                     Span<const CommandBufferHandle> secondaries) = 0;

    /// Submit and return the value signalled on `info.queue`'s timeline.
    virtual Expected<u64, Error> submit(const SubmitInfo& info) = 0;
    [[nodiscard]] virtual u64 timeline_value(QueueKind queue) const noexcept = 0;
    virtual Status wait_timeline(QueueKind queue, u64 value, u64 timeout_ns) = 0;
    virtual Status wait_idle() = 0;

    virtual Expected<FenceHandle, Error> create_fence(bool signalled) = 0;
    virtual void destroy_fence(FenceHandle handle) noexcept = 0;
    virtual Status wait_fence(FenceHandle handle, u64 timeout_ns) = 0;
    virtual Status reset_fence(FenceHandle handle) = 0;
    [[nodiscard]] virtual bool fence_signalled(FenceHandle handle) const noexcept = 0;

    virtual Expected<SemaphoreHandle, Error> create_semaphore() = 0;
    virtual void destroy_semaphore(SemaphoreHandle handle) noexcept = 0;

    // --- Swapchain
    // --------------------------------------------------------------------------------

    /// `desc.native_surface` is what DisplayServer::create_surface() produced for this backend's
    /// GraphicsApi. The RHI never speaks to a window system — design.md §4's rule, three milestones
    /// old, and this is its first consumer.
    virtual Expected<SwapchainHandle, Error> create_swapchain(const SwapchainDescription& desc) = 0;
    virtual void destroy_swapchain(SwapchainHandle handle) noexcept = 0;
    virtual Status resize_swapchain(SwapchainHandle handle, Extent2D extent) = 0;
    [[nodiscard]] virtual SwapchainInfo swapchain_info(SwapchainHandle handle) const noexcept = 0;
    /// The acquired image's index, with `signal` raised when it is safe to render into. An out-of-
    /// date swapchain is ErrorCode::Unavailable, which the caller answers by resizing rather than
    /// by failing the frame.
    virtual Expected<u32, Error> acquire_next_image(SwapchainHandle handle, SemaphoreHandle signal,
                                                    u64 timeout_ns) = 0;
    [[nodiscard]] virtual TextureHandle swapchain_texture(SwapchainHandle handle,
                                                          u32 index) const noexcept = 0;
    [[nodiscard]] virtual TextureViewHandle swapchain_view(SwapchainHandle handle,
                                                           u32 index) const noexcept = 0;
    virtual Status present(SwapchainHandle handle, u32 image_index, SemaphoreHandle wait) = 0;

    // --- Memory and reporting
    // -----------------------------------------------------------------------

    [[nodiscard]] virtual GpuMemoryReport memory_report() const noexcept = 0;
    [[nodiscard]] virtual const DeviceStatistics& statistics() const noexcept = 0;
    virtual void reset_statistics() noexcept = 0;

    /// Publish the GPU figures into the engine's budget tree and let the pressure monitor react.
    /// `rhi-and-render-graph`: GPU memory pressure raises the engine's pressure level "so that
    /// streaming and residency systems respond through the same mechanism they use for CPU memory,
    /// rather than each polling the device budget". Called once a frame by the host.
    virtual void publish_memory_pressure() noexcept = 0;

    // --- The render graph's private door
    // ----------------------------------------------------------

    /// The barrier-emitting interface. `GraphBarrierKey` cannot be constructed outside
    /// cy::rendering::GraphExecutor, so this method has exactly one caller in the engine — which is
    /// the M3 invariant expressed in the type system. See barrier.h.
    /// By const reference because the key is deliberately non-copyable: a key obtained legitimately
    /// cannot be stored and handed to somebody else.
    [[nodiscard]] virtual BarrierRecorder& barrier_recorder(
        const GraphBarrierKey& key) noexcept = 0;

    /// UNSAFE, documented, and excluded from the portability guarantees: the backend's own device
    /// object. Null on the null backend.
    [[nodiscard]] virtual void* native_handle() noexcept = 0;
};

}  // namespace cy::rhi
