#pragma once
// The executor: realise, record, submit. Tasks 2.2.4, 2.2.5, 2.2.6 and 2.2.7.
//
// --- THIS CLASS IS THE ONE THE INVARIANT NAMES ---------------------------------------------------
//
// cy::rendering::GraphExecutor is the only type in the engine that can construct an
// rhi::GraphBarrierKey, and therefore the only one that can reach rhi::Device::barrier_recorder().
// Every barrier in every frame is recorded from execute() below, from a plan the graph derived. A
// pass's record callback is handed a command buffer and cannot emit one, because
// rhi::CommandBuffer has no such method (see cy/backends/rhi/command_buffer.h).
//
// Task 2.2.4 asks for that to be structural and for it to be PROVED by introducing a violation and
// watching the build fail. tools/layercheck/fixtures/barrier-outside-graph/ is that violation, kept
// as a fixture and run by `just quality-layers` so the proof is repeated on every pull request
// rather than performed once by hand.
//
// --- THE ORDER OF OPERATIONS, AND WHY IT IS THIS ORDER
// --------------------------------------------
//
//   1. realise    create every transient resource UNBOUND
//   2. compile    derive the plan, asking the device for each transient's memory requirements
//   3. reserve    one pool big enough for the plan's peak
//   4. bind       every transient at its planned offset
//   5. views      derived from the ranges the passes declared, never chosen by a pass author
//   6. record     barriers, debug labels, breadcrumbs and the passes' own commands
//   7. submit     one submission per plan submit, waits as timeline waits
//
// A view needs bound memory, so any order that creates one before step 4 fails on a device and
// silently does not on the null backend. M3's spike paid for that discovery; it is written down
// here so nobody pays for it twice.
//
// Step 5 is why the graph owns view creation. A combined-image-sampler descriptor's view must match
// the range the pass declared: binding a two-layer array view against a declaration of layer 0
// produced a validation error on the layer the graph never transitioned. Deriving the view from the
// declaration makes that unrepresentable.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/graph/graph.h>

namespace cy::jobs {
class JobSystem;
}  // namespace cy::jobs

namespace cy::rendering {

struct ExecuteOptions {
    /// The command buffer to record into, or a null handle to let the executor acquire one per
    /// submit from the device's current frame. A caller that is already inside a command buffer —
    /// an editor viewport inside the host's frame — passes its own.
    rhi::FenceHandle signal_fence;
    /// Presentation's binary semaphores. `rhi-and-render-graph`'s frame structure: the acquire
    /// semaphore is waited by the first submit that touches the swapchain image and the present
    /// semaphore is signalled by the last.
    rhi::SemaphoreHandle wait_acquire;
    rhi::SemaphoreHandle signal_present;
    /// Record independent passes into secondary command buffers on job workers, joined before
    /// submission. Off by default, because a graph test must not need a running job system and
    /// because sequential recording is the reference the parallel path is compared against.
    ///
    /// `rhi-and-render-graph` requires recording to be DETERMINISTIC: "the same frame description
    /// SHALL produce the same command stream regardless of thread scheduling". That holds here
    /// because the secondaries are executed in plan order, never in completion order — the workers
    /// decide when a pass is recorded, never where its commands end up.
    bool parallel_recording = false;
    /// Below this many passes in a submit, recording stays sequential: a secondary command buffer
    /// per pass costs more than it saves for a handful of passes.
    u32 parallel_pass_threshold = 4;
    jobs::JobSystem* job_system = nullptr;

    /// Write a breadcrumb per pass. `rhi-and-render-graph` requires it "so that they survive into a
    /// crash artefact when the trace tail does not". On in development builds, and cheap: one
    /// 32-bit write into a device-visible buffer.
    bool breadcrumbs = true;
    /// Emit a debug label per pass, which is what a RenderDoc or PIX capture is read by.
    bool debug_labels = true;
};

/// What one execution did, for the trace, the statistics and a test.
struct ExecutionResult {
    u32 submits = 0;
    u32 passes_recorded = 0;
    u32 passes_culled = 0;
    u32 secondary_command_buffers = 0;
    u32 barrier_batches = 0;
    u32 barriers = 0;
    u32 queue_ownership_transfers = 0;
    u64 transient_bytes = 0;
    u64 transient_bytes_without_aliasing = 0;
    /// The value each queue's timeline was left at, so a caller can wait on the frame.
    u64 timeline_signalled[rhi::kQueueKindCount] = {};
    u64 plan_hash = 0;
};

class GraphExecutor {
public:
    GraphExecutor(Allocator& allocator, rhi::Device& device) noexcept;
    ~GraphExecutor();

    GraphExecutor(const GraphExecutor&) = delete;
    GraphExecutor& operator=(const GraphExecutor&) = delete;
    GraphExecutor(GraphExecutor&&) = delete;
    GraphExecutor& operator=(GraphExecutor&&) = delete;

    /// Compile and run one frame's graph. `options.query_memory` is filled in from the device if
    /// the caller left it null, which is what makes the ordinary call site two arguments.
    Expected<ExecutionResult, Error> execute(RenderGraph& graph, CompileOptions compile_options,
                                             const ExecuteOptions& options) noexcept;

    /// Compile and realise without recording. For a test that wants to assert on the plan, and for
    /// a caller that wants to inspect a frame before running it.
    Expected<CompiledGraph, Error> compile_only(RenderGraph& graph,
                                                CompileOptions compile_options) noexcept;

    /// The backend handles behind a graph resource. Valid between realise and the next reset.
    [[nodiscard]] rhi::TextureHandle texture(ResourceId resource) const noexcept;
    [[nodiscard]] rhi::BufferHandle buffer(ResourceId resource) const noexcept;
    /// The view the graph created for a declared range. A null handle when no pass declared that
    /// range, which is the honest answer: the graph transitions what was declared and nothing else.
    [[nodiscard]] rhi::TextureViewHandle view(ResourceId resource,
                                              rhi::SubresourceRange range) const noexcept;
    /// The whole-resource view, which is what most passes want.
    [[nodiscard]] rhi::TextureViewHandle view(ResourceId resource) const noexcept;

    /// Destroy this frame's transient resources and views. Called at the end of execute(); exposed
    /// so a caller that used compile_only() can release what it realised.
    void release() noexcept;

    [[nodiscard]] rhi::Device& device() const noexcept { return *device_; }

private:
    struct ViewEntry {
        ResourceId resource = kInvalidResource;
        rhi::SubresourceRange range{};
        rhi::TextureViewHandle view;
    };

    Status realise_resources(RenderGraph& graph) noexcept;
    Status bind_and_view(RenderGraph& graph, const CompiledGraph& plan) noexcept;
    Status record_and_submit(RenderGraph& graph, CompiledGraph& plan, const ExecuteOptions& options,
                             ExecutionResult& result) noexcept;
    void patch_batch(RenderGraph& graph, rhi::BarrierBatch& batch) noexcept;
    Status record_pass(RenderGraph& graph, PassId pass, u32 schedule_index,
                       rhi::CommandBufferHandle command_buffer) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    Array<rhi::TextureHandle> textures_;
    Array<rhi::BufferHandle> buffers_;
    Array<ViewEntry> views_;
    /// The device timeline value each plan submit actually signalled. The plan counts from 1 per
    /// frame; a device timeline is monotonic for the device's life, so a wait has to be translated.
    Array<u64> submit_signals_;
    u32 breadcrumb_next_ = 0;
};

}  // namespace cy::rendering
