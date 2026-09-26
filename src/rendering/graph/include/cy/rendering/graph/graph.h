#pragma once
// The render graph: declarations in, a compiled plan out. Tasks 2.2.1, 2.2.2 and 2.2.3.
//
// --- WHAT A PASS AUTHOR CAN SAY ------------------------------------------------------------------
//
//     RenderGraph graph(allocator);
//     const ResourceId depth = graph.create_texture({.name = "depth", .format = Format::D32Sfloat,
//                                                    .width = 1920, .height = 1080});
//     graph.add_pass("depth prepass", QueueKind::Graphics)
//          .write(depth, Access::DepthStencilAttachmentWrite)
//          .read(instances, Access::VertexStorageRead)
//          .record(&record_prepass, &state);
//
// That is the whole vocabulary. There is no barrier call, no layout, no stage mask, no semaphore
// and no ownership transfer — not because they are discouraged, but because there is nowhere in
// this header for one to appear. Everything they would have expressed is DERIVED from the reads and
// writes above, and `rhi-and-render-graph` requires exactly that: "the required barrier SHALL be
// inserted by the graph, with no barrier code in the renderer".
//
// design.md §2 is why this lands with the first pass rather than the thirtieth: it is a property of
// the thirtieth pass, and the thirtieth pass obeys it because the first one did.
//
// --- THE SIX PHASES ------------------------------------------------------------------------------
//
// `rhi-and-render-graph` names them: build, cull, schedule, alias, synchronise, execute. The order
// this implementation runs them in has one wrinkle the specification does not mention and that M3's
// spike found the hard way:
//
//     cull -> schedule -> lifetimes -> place -> ADD ALIAS EDGES -> RE-SCHEDULE -> derive
//
// Memory aliasing creates dependencies the resource graph cannot see. The pass that first uses a
// transient must be ordered after the last use of every transient whose memory it reuses — and
// across queues that ordering can only be a semaphore, so the edges must exist before submits are
// cut. The spike reproduced the defect on the device: two independent chains, one per queue, whose
// transients the aliaser put on the same bytes, with no semaphore between them.
//
// The second pass terminates and is sound. Placement depends only on pass ORDER, which
// re-scheduling never changes — it only moves submit boundaries — and an alias edge always points
// backwards in pass order by construction, so it cannot close a cycle.
//
// --- THE DERIVATION TOUCHES NO DEVICE
// -------------------------------------------------------------
//
// compile() asks the caller exactly one question, through `CompileOptions::query_memory`: how much
// memory does this transient need. Everything else is arithmetic over the declarations. That is
// what lets the null backend run the identical code, what makes a graph test need no GPU, and what
// makes two independent processes produce byte-identical plans — which `plan_hash` below turns into
// an assertion rather than a hope.

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/barrier.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/resources.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>

namespace cy::rendering {

using ResourceId = rhi::GraphResourceId;
inline constexpr ResourceId kInvalidResource = rhi::kInvalidGraphResource;

/// A pass's position in declaration order. Not its position in the schedule: the schedule is
/// derived, and a culled pass has no schedule position at all.
using PassId = u32;
inline constexpr PassId kInvalidPass = ~0U;

// --- Resource declarations -----------------------------------------------------------------------

/// A graph texture. `usage` is what the graph could not work out for itself: the accesses declared
/// against a resource already imply sampled, storage or attachment usage, and the graph unions
/// them. A caller adds a flag here only for something no declared access implies — a texture that
/// will be blitted from outside the graph, say.
struct TextureRequest {
    const char* name = "texture";
    rhi::Format format = rhi::Format::Undefined;
    u32 width = 0;
    u32 height = 0;
    u32 depth = 1;
    u16 mip_levels = 1;
    u16 array_layers = 1;
    u16 sample_count = 1;
    rhi::TextureDimension dimension = rhi::TextureDimension::Texture2D;
    rhi::TextureUsage extra_usage = rhi::TextureUsage::None;
};

struct BufferRequest {
    const char* name = "buffer";
    u64 size = 0;
    rhi::BufferUsage extra_usage = rhi::BufferUsage::None;
    rhi::MemoryUse memory = rhi::MemoryUse::DeviceLocal;
};

/// One resource's declaration, as the graph holds it. Public because the memory-query callback and
/// the dump both read it, and because a test that inspects a plan should not need a private header.
struct ResourceInfo {
    const char* name = "";
    bool is_texture = false;
    bool transient = false;  // graph-owned lifetime, and therefore eligible for memory aliasing
    bool imported = false;
    TextureRequest texture{};
    BufferRequest buffer{};
    /// Unioned from every declared access. What the resource is actually created with.
    rhi::TextureUsage texture_usage = rhi::TextureUsage::None;
    rhi::BufferUsage buffer_usage = rhi::BufferUsage::None;
    /// Imported resources only: what state the caller says the resource is already in.
    rhi::ImageUse initial_use = rhi::ImageUse::Undefined;
    rhi::QueueOwner initial_owner{};
    /// Imported resources only: a READ made outside the graph that the first write or layout
    /// transition must wait for, at the stage an external dependency makes it visible at. `None`
    /// for an ordinary import; `rhi::kPresentAcquireStage` for a swapchain image, whose outside
    /// reader is the presentation engine and whose ordering is the acquire semaphore's wait.
    rhi::Stage initial_read_stage = rhi::Stage::None;
    rhi::TextureHandle imported_texture;
    rhi::BufferHandle imported_buffer;
    /// MSAA through the attachment model. On a single-sample texture a multisampled pass rendered
    /// into: the graph-owned twin it actually rendered into. On that twin: the texture it resolves
    /// into. `kInvalidResource` everywhere else — which is every resource of a frame with no
    /// multisampled pass, so a 1x graph is the graph it was before this existed.
    ResourceId multisampled = kInvalidResource;
    ResourceId resolves_into = kInvalidResource;
};

/// One declared use of one resource by one pass.
struct Use {
    ResourceId resource = kInvalidResource;
    rhi::Access access = rhi::Access::ComputeStorageRead;
    /// Already resolved against the resource, so nothing downstream carries an "all remaining"
    /// sentinel. See rhi::SubresourceRange.
    rhi::SubresourceRange range{};
};

// --- Passes
// ---------------------------------------------------------------------------------------

class GraphExecutor;
class RenderGraph;

/// What a pass's record callback is handed.
///
/// Note again what is absent: no device, no barrier recorder, no queue. A pass records draws,
/// dispatches and copies into `commands`, and reaches its resources through the executor's
/// lookups — which hand back the view the graph created from the range the pass declared, so a
/// descriptor can never name a subresource the graph did not transition. (Spike gotcha 6d.)
struct PassContext {
    rhi::CommandBuffer* commands = nullptr;
    const GraphExecutor* executor = nullptr;
    PassId pass = kInvalidPass;
    /// The pass's index in the schedule, which is what a breadcrumb and a debug label carry.
    u32 schedule_index = 0;
    /// The graph the pass was declared in. What `attachment_view` reads the pass's sample count
    /// and the multisampled twins through.
    const RenderGraph* graph = nullptr;
    /// What the pass declared with `PassBuilder::multisample`. A pipeline the pass binds must be
    /// created with this sample count.
    u16 sample_count = 1;
    /// What the pass declared with `PassBuilder::views`, and how the executor chose to record it.
    ///
    /// WITH `Capability::Multiview` the callback runs ONCE, `view_mask` has one bit per view and a
    /// pipeline created with that mask renders every view in one pass. WITHOUT it the callback runs
    /// once PER VIEW, `view_mask` is zero, `view_index` names the view, and `attachment_view` hands
    /// back a single-layer view of that layer. The choice is the capability's and never the
    /// backend's name — `rhi-and-render-graph`'s "the renderer SHALL branch on capabilities".
    u32 view_count = 1;
    u32 view_mask = 0;
    u32 view_index = 0;

    /// The view a pass renders `resource` through: the multisampled twin where the pass is
    /// multisampled, the single layer of this recording where multi-view is emulated, and the
    /// resource's own whole view otherwise. Defined in executor.cpp.
    [[nodiscard]] rhi::TextureViewHandle attachment_view(ResourceId resource) const noexcept;
};

/// A plain function pointer rather than a std::function: the engine has no exceptions and no
/// per-frame allocation on this path, and a captureless callback plus a `void*` says exactly what
/// the lifetime of the captured state is — the caller's.
using RecordFn = void (*)(const PassContext& context, void* user);

/// The declaration builder. Returned by RenderGraph::add_pass and used as a chain.
///
/// Failures accumulate on the graph rather than being returned from each call, so a declaration
/// reads as a declaration. RenderGraph::status() is checked once, before compiling; a graph that
/// failed to declare something refuses to compile rather than compiling a plan with a hole in it.
class PassBuilder {
public:
    PassBuilder(RenderGraph* graph, PassId pass) noexcept : graph_(graph), pass_(pass) {}

    /// Declare a read. Fails the graph if `access` is a writing intent — a mistyped declaration is
    /// exactly the thing that would produce a plausible-looking but wrong barrier.
    PassBuilder& read(ResourceId resource, rhi::Access access,
                      rhi::SubresourceRange range = {}) noexcept;
    PassBuilder& write(ResourceId resource, rhi::Access access,
                       rhi::SubresourceRange range = {}) noexcept;
    /// For the read-modify-write intents, where neither `read` nor `write` is the whole truth.
    PassBuilder& use(ResourceId resource, rhi::Access access,
                     rhi::SubresourceRange range = {}) noexcept;

    PassBuilder& record(RecordFn function, void* user) noexcept;

    /// Survive culling even with no consumed output. `rhi-and-render-graph`'s culling rule needs
    /// this: a pass whose whole purpose is a side effect the graph cannot see — a readback, a
    /// query, a debug overlay that is meant to run — says so here.
    PassBuilder& side_effect() noexcept;

    // --- MSAA through the attachment model -----------------------------------------------------
    //
    //     graph.add_pass("opaque", QueueKind::Graphics)
    //          .multisample(4)                                  // before the attachments
    //          .write(colour, Access::ColorAttachmentWrite)     // renders into colour's 4x twin
    //          .write(depth, Access::DepthStencilAttachmentWrite)
    //          .resolve(colour)                                 // 4x -> colour, after this pass
    //          .record(&record_opaque, &state);
    //
    // The pass names the single-sample texture it means; the graph allocates the multisampled twin
    // (transient, same format and extent) and redirects every ATTACHMENT use of this pass to it.
    // Sampled, storage and transfer uses are not redirected. `resolve` inserts a graph-owned pass
    // directly after this one that resolves the twin into the texture, and compile() REFUSES a
    // graph in which anything touches the texture while its twin holds rendering no resolve has
    // reached — a missing resolve, or one declared on a pass before the last multisampled write.
    //
    // A 1x pass is untouched: `multisample(1)` is the default, creates nothing and changes no
    // decision in the plan, so a frame with no multisampled pass compiles to the identical plan.

    /// The sample count this pass renders its attachments at: 1, 2, 4 or 8. Must precede every
    /// attachment use of the pass, because the redirection happens as each use is declared.
    PassBuilder& multisample(u16 samples) noexcept;
    /// Resolve `target`'s multisampled twin into `target` after this pass. Colour targets only: the
    /// RHI exposes no depth resolve mode yet, and a depth resolve is refused rather than guessed.
    PassBuilder& resolve(ResourceId target) noexcept;
    /// This pass cannot render multisampled, for the reason given. `multisample(n > 1)` on it then
    /// fails the graph with exactly that reason — the virtual-geometry stage is the first user,
    /// because a visibility payload is one surface a pixel.
    PassBuilder& single_sample(const char* reason) noexcept;

    // --- Multi-view ------------------------------------------------------------------------------

    /// Render `count` views into the leading `count` layers of every attachment. Recorded once with
    /// a view mask where the device reports `Capability::Multiview`, and once per view into a
    /// single-layer view where it does not — see `PassContext::view_count`. The plan is the same
    /// either way: the declarations, and therefore every barrier, do not depend on the choice.
    PassBuilder& views(u32 count) noexcept;

    [[nodiscard]] PassId id() const noexcept { return pass_; }

private:
    RenderGraph* graph_ = nullptr;
    PassId pass_ = kInvalidPass;
};

// --- The compiled plan
// ------------------------------------------------------------------------------

struct SemaphoreWait {
    rhi::QueueKind queue = rhi::QueueKind::Graphics;
    u64 value = 0;
    rhi::Stage stage = rhi::Stage::AllCommands;
};

/// One pass, in schedule order, with the barriers that must be recorded immediately before it.
struct ScheduledPass {
    PassId pass = kInvalidPass;
    rhi::BarrierBatch pre;
};

/// One submission. A cross-queue dependency becomes a wait here, never a barrier: a pipeline
/// barrier synchronises nothing between two command streams, and the spike's negative control
/// proved it — dropping the wait produced SYNC-HAZARD-WRITE-RACING-WRITE on the device.
struct Submit {
    rhi::QueueKind queue = rhi::QueueKind::Graphics;
    u64 signal_value = 0;
    Array<SemaphoreWait> waits;
    Array<ScheduledPass> passes;
    /// Queue-family ownership releases, recorded at the END of this submit's command buffer. Their
    /// acquire halves are in the consuming pass's `pre` batch, and the semaphore between the two
    /// submits is what orders them — the barriers alone do nothing.
    rhi::BarrierBatch release;

    explicit Submit(Allocator& allocator) noexcept : waits(allocator), passes(allocator) {}
};

/// Where one transient lives in the pool, and for how long.
struct Placement {
    ResourceId resource = kInvalidResource;
    u64 offset = 0;
    u64 size = 0;
    u32 first_pass = 0;  // inclusive, in schedule order
    u32 last_pass = 0;   // inclusive
};

struct MemoryPlan {
    /// The peak the plan needs, with aliasing.
    u64 heap_bytes = 0;
    /// The sum of every transient, which is what the same frame would cost without aliasing. Both
    /// are reported so that task 7.3's "aliasing measurably reduces peak GPU memory" is a number
    /// the plan itself carries rather than a second build.
    u64 naive_bytes = 0;
    rhi::MemoryPoolClass pool_class;
    Array<Placement> placements;

    explicit MemoryPlan(Allocator& allocator) noexcept : placements(allocator) {}
};

struct GraphStatistics {
    u32 passes_declared = 0;
    u32 passes_culled = 0;
    u32 submits = 0;
    u32 semaphore_waits = 0;
    u32 image_barriers = 0;
    u32 buffer_barriers = 0;
    u32 memory_barriers = 0;
    u32 queue_ownership_transfers = 0;
    u32 alias_barriers = 0;
    u32 alias_edges = 0;
};

/// What compile() produced. Owns its arrays; move-only, like everything else in the engine that
/// owns memory.
struct CompiledGraph {
    Array<Submit> submits;
    MemoryPlan memory;
    Array<PassId> culled;
    GraphStatistics stats{};
    /// A hash of every decision in the plan: the submit boundaries, the waits, every barrier field,
    /// every placement. design.md §6 requires deterministic submission; this is how a test asserts
    /// it — two runs, one number — rather than eyeballing a dump.
    u64 plan_hash = 0;

    explicit CompiledGraph(Allocator& allocator) noexcept
        : submits(allocator), memory(allocator), culled(allocator) {}
};

// --- Compilation options
// ------------------------------------------------------------------------------

/// Answers "how much memory does this transient need". Returns false when the resource has no
/// memory requirement the caller can answer, in which case the graph does not place it.
///
/// A callback rather than a device reference, because the derivation must stay device-free: the
/// null backend answers it synthetically, a Vulkan device answers it from VkMemoryRequirements, and
/// a unit test answers it with arithmetic. All three run the same code below.
using MemoryQueryFn = bool (*)(ResourceId resource, const ResourceInfo& info,
                               rhi::MemoryRequirements& out, void* user);

struct CompileOptions {
    /// Transient memory aliasing. Off, the plan places every transient at its own offset — which is
    /// what the no-aliasing half of the measurement in task 7.3 is.
    bool enable_aliasing = true;

    /// With async compute off, every pass folds onto the graphics queue: one submit, no semaphores,
    /// no ownership transfers, from exactly the same declarations. That is the null backend's and
    /// continuous integration's normal path, and it is not a special case anywhere in the code.
    bool enable_async_compute = true;

    /// Which queues the device actually has. A queue the device does not have is folded onto
    /// graphics.
    bool queue_available[rhi::kQueueKindCount] = {true, false, false};

    /// WHETHER A RESOURCE CHANGING QUEUES NEEDS AN EXPLICIT TRANSFER, and which queues share an
    /// ownership domain — `DeviceCapabilities::needs_queue_ownership_transfer()` and
    /// `queue_ownership_domain()`, copied in by the executor. Metal gap 4: this used to be a
    /// `u32 queue_family[3]` read straight off `Device::queue_family()`, which is a Vulkan family
    /// index in an engine-owned plan. The graph only ever COMPARES these, so an opaque domain is
    /// all it needed, and a backend with no ownership concept turns the whole derivation off with
    /// the flag rather than inventing indices that differ.
    bool queue_ownership_transfers = true;
    u8 queue_ownership_domain[rhi::kQueueKindCount] = {0, 0, 0};

    /// A DELIBERATE POLICY KNOB, not a defect switch. Aliasing buys memory and spends parallelism:
    /// an alias edge serialises work that shares nothing but bytes. On a frame where the async
    /// overlap is worth more than the megabytes, turn this off and the aliaser will not place two
    /// transients from different queues on the same memory.
    bool alias_across_queues = true;

    MemoryQueryFn query_memory = nullptr;
    void* query_user = nullptr;
};

/// The synthetic memory query: sizes computed from the declarations, with a plausible alignment.
/// What a graph test uses when it has no device, and what makes the aliasing arithmetic assertable
/// without one.
bool synthetic_memory_query(ResourceId resource, const ResourceInfo& info,
                            rhi::MemoryRequirements& out, void* user) noexcept;

// --- The graph
// -------------------------------------------------------------------------------------

class RenderGraph {
public:
    explicit RenderGraph(Allocator& allocator) noexcept;

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&) = delete;
    RenderGraph& operator=(RenderGraph&&) = delete;

    /// A resource the graph owns for the frame. Eligible for memory aliasing, and culled with the
    /// last pass that consumes it.
    ResourceId create_texture(const TextureRequest& request) noexcept;
    ResourceId create_buffer(const BufferRequest& request) noexcept;

    /// A resource that outlives the frame: the swapchain image, the GPU scene, a persistent target.
    /// `current_use` and `owner` are what the caller promises the resource is already in, and
    /// getting them wrong is how a first barrier transitions from the wrong state.
    ///
    /// A write to an imported resource is a CULLING ROOT. That falls out of the definition — the
    /// graph cannot see who reads it afterwards — and it is worth knowing, because it means a
    /// culling test only means something when the shared resource is graph-owned.
    ResourceId import_texture(const TextureRequest& request, rhi::TextureHandle texture,
                              rhi::ImageUse current_use, rhi::QueueOwner owner = {}) noexcept;
    ResourceId import_buffer(const BufferRequest& request, rhi::BufferHandle buffer,
                             rhi::QueueOwner owner = {}) noexcept;

    /// The image `Device::acquire_next_image` just returned. Its contents are undefined, and the
    /// presentation engine may still be READING it until the acquire semaphore is signalled — which
    /// is what an ordinary `import_texture(..., ImageUse::Undefined)` cannot say. The graph records
    /// that read at `rhi::kPresentAcquireStage`, the stage the executor waits on `wait_acquire` at,
    /// so the first transition's source stage chains to the wait instead of being ordered after
    /// nothing (SYNC-HAZARD-WRITE-AFTER-READ against PRESENT_ACQUIRE_READ, M11.d).
    ResourceId import_swapchain_texture(const TextureRequest& request,
                                        rhi::TextureHandle texture) noexcept;

    /// Declare a pass. Passes must be declared in dependency order: the scheduler treats
    /// declaration order as a topological order and asserts it, which is true whenever an author
    /// declares a pass after the passes it reads from. A real topological sort is the change to
    /// make when that stops being true, and the alias-edge re-schedule step must then run after it.
    [[nodiscard]] PassBuilder add_pass(const char* name, rhi::QueueKind queue) noexcept;

    /// Derive the whole plan. Makes no device calls beyond `options.query_memory`.
    [[nodiscard]] Expected<CompiledGraph, Error> compile(const CompileOptions& options) noexcept;

    // --- Inspection ------------------------------------------------------------------------------

    [[nodiscard]] usize resource_count() const noexcept { return resources_.size(); }
    [[nodiscard]] usize pass_count() const noexcept { return passes_.size(); }
    [[nodiscard]] const ResourceInfo& resource(ResourceId id) const noexcept;
    [[nodiscard]] const char* pass_name(PassId pass) const noexcept;
    [[nodiscard]] rhi::QueueKind pass_queue(PassId pass) const noexcept;
    [[nodiscard]] Span<const Use> pass_uses(PassId pass) const noexcept;
    [[nodiscard]] bool pass_has_side_effect(PassId pass) const noexcept;
    [[nodiscard]] RecordFn pass_record_function(PassId pass) const noexcept;
    [[nodiscard]] void* pass_record_user(PassId pass) const noexcept;
    /// What `PassBuilder::multisample` and `PassBuilder::views` declared; 1 when nothing did.
    [[nodiscard]] u16 pass_sample_count(PassId pass) const noexcept;
    [[nodiscard]] u32 pass_view_count(PassId pass) const noexcept;
    /// For a pass the GRAPH inserted to resolve a multisampled twin: the twin and the texture it
    /// resolves into. `kInvalidResource` for both on every pass an author declared.
    [[nodiscard]] ResourceId pass_resolve_source(PassId pass) const noexcept;
    [[nodiscard]] ResourceId pass_resolve_target(PassId pass) const noexcept;

    /// The first declaration failure, or success. Checked once before compiling.
    [[nodiscard]] Status status() const noexcept { return status_; }

    /// Forget every declaration but keep the allocations, which is what a per-frame graph wants.
    void reset() noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }

private:
    friend class PassBuilder;

    struct Pass {
        const char* name = "";
        rhi::QueueKind queue = rhi::QueueKind::Graphics;
        bool side_effect = false;
        RecordFn record = nullptr;
        void* user = nullptr;
        usize first_use = 0;  // index into uses_
        usize use_count = 0;
        u16 samples = 1;
        u32 views = 1;
        const char* single_sample_reason = nullptr;
        /// Set on a graph-inserted resolve pass only.
        ResourceId resolve_source = kInvalidResource;
        ResourceId resolve_target = kInvalidResource;
    };

    /// A `resolve()` waiting for its pass to finish being declared: the pass is still the one
    /// uses are appended to, so the resolve pass is inserted when the NEXT pass is added, or at
    /// compile(). Either way it lands directly after the pass that declared it.
    struct PendingResolve {
        PassId pass = kInvalidPass;
        ResourceId target = kInvalidResource;
    };

    // multisample.cpp — MSAA and multi-view, kept out of the declaration core above.
    void set_samples(PassId pass, u16 samples) noexcept;
    void set_single_sample(PassId pass, const char* reason) noexcept;
    void set_views(PassId pass, u32 views) noexcept;
    void request_resolve(PassId pass, ResourceId target) noexcept;
    /// The twin `resource` renders into from `pass`, creating it on first use; `resource` itself
    /// when the pass is single-sample or the access is not an attachment.
    [[nodiscard]] ResourceId attachment_target(PassId pass, ResourceId resource,
                                               rhi::Access access) noexcept;
    void flush_resolves() noexcept;
    /// Refuses a graph whose resolves are missing or misplaced, or whose multi-view pass renders
    /// into an attachment with fewer layers than views. Run at the start of compile().
    [[nodiscard]] Status validate_multisampling() const noexcept;

    /// Append a use to the pass currently being built. Fails the graph when the pass is not the
    /// last one declared, because uses are stored in one flat array and interleaving two passes'
    /// declarations would silently attribute a use to the wrong pass.
    void add_use(PassId pass, ResourceId resource, rhi::Access access,
                 rhi::SubresourceRange range) noexcept;
    void set_record(PassId pass, RecordFn function, void* user) noexcept;
    void set_side_effect(PassId pass) noexcept;
    void note_usage(ResourceId resource, rhi::Access access) noexcept;
    void set_failure(ErrorCode code, const char* message) noexcept;

    Allocator* allocator_ = nullptr;
    Array<ResourceInfo> resources_;
    Array<Pass> passes_;
    Array<Use> uses_;
    Array<PendingResolve> pending_resolves_;
    Status status_;

    friend struct Compiler;
};

}  // namespace cy::rendering
