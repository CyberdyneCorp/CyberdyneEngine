// Running a compiled graph through the null backend, and the determinism that makes it comparable.
// Tasks 2.2.4, 2.2.5, 2.2.6, 2.2.8 and design.md §6.
//
// `rhi-and-render-graph`, "Headless CI": the null backend "SHALL allow render graph construction,
// culling, and scheduling to be tested without device access". This is that test, and it is the one
// task 6.4 — the same frame through the null backend with no GPU — is built on.

#include <cy/test/test.h>

#include "fixtures.h"

#include <cy/core/jobs/job_system.h>

#include <cstring>

using cy::rendering::CompiledGraph;
using cy::rendering::GraphExecutor;
using cy::rendering::PassContext;
using cy::rendering::RenderGraph;
using cy::rendering::ResourceId;
using cy::rhi::Access;
using namespace cy::rendering::test;

namespace {

struct RecordCounter {
    cy::u32 calls = 0;
    cy::u32 last_schedule_index = 0;
    cy::rhi::TextureViewHandle seen_view;
};

void record_draw(const PassContext& context, void* user) noexcept {
    auto* counter = static_cast<RecordCounter*>(user);
    ++counter->calls;
    counter->last_schedule_index = context.schedule_index;
    context.commands->draw(3, 1, 0, 0);
}

void record_dispatch(const PassContext& context, void* user) noexcept {
    auto* counter = static_cast<RecordCounter*>(user);
    ++counter->calls;
    context.commands->dispatch(4, 4, 1);
}

/// One frame's declarations, used by several cases. A transient written by a compute pass, sampled
/// by a graphics pass that writes the swapchain image, and a debug pass that nothing consumes.
struct Frame {
    RenderGraph graph;
    ResourceId scratch = cy::rendering::kInvalidResource;
    ResourceId target = cy::rendering::kInvalidResource;
    cy::rendering::PassId debug_pass = cy::rendering::kInvalidPass;
    RecordCounter compute_counter;
    RecordCounter shade_counter;
    RecordCounter debug_counter;

    explicit Frame(cy::Allocator& allocator, cy::rhi::TextureHandle swapchain_image) noexcept
        : graph(allocator) {
        scratch = graph.create_texture(storage_image("scratch", 64));
        target = graph.import_texture(colour_target("swapchain", 64), swapchain_image,
                                      cy::rhi::ImageLayout::Undefined);

        graph.add_pass("compute", cy::rhi::QueueKind::Graphics)
            .write(scratch, Access::ComputeStorageWrite)
            .record(&record_dispatch, &compute_counter);
        graph.add_pass("shade", cy::rhi::QueueKind::Graphics)
            .read(scratch, Access::FragmentSampledRead)
            .write(target, Access::ColorAttachmentWrite)
            .record(&record_draw, &shade_counter);
        debug_pass = graph.add_pass("debug overlay", cy::rhi::QueueKind::Graphics)
                         .write(graph.create_texture(storage_image("overlay", 64)),
                                Access::ComputeStorageWrite)
                         .record(&record_draw, &debug_counter)
                         .id();
    }
};

cy::rhi::TextureDescription swapchain_description() noexcept {
    cy::rhi::TextureDescription description;
    description.name = "swapchain";
    description.format = cy::rhi::Format::Rgba8Unorm;
    description.extent = cy::rhi::Extent3D{64, 64, 1};
    description.usage = cy::rhi::TextureUsage::ColorAttachment;
    return description;
}

}  // namespace

CY_TEST_CASE("a frame runs end to end on a machine with no GPU") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::Expected<cy::rhi::TextureHandle, cy::Error> image =
        fixture.device().create_texture(swapchain_description());
    CY_REQUIRE(image.has_value());
    CY_REQUIRE(fixture.device().begin_frame().has_value());

    Frame frame(fixture.allocator(), *image);
    CY_REQUIRE(frame.graph.status().has_value());

    GraphExecutor executor(fixture.allocator(), fixture.device());
    cy::Expected<cy::rendering::ExecutionResult, cy::Error> result = executor.execute(
        frame.graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
    CY_REQUIRE(result.has_value());

    CY_CHECK_EQ(result->submits, 1U);
    CY_CHECK_EQ(result->passes_recorded, 2U);
    CY_CHECK_EQ(result->passes_culled, 1U);
    // The culled debug pass's callback is never invoked — which is what "the renderer SHALL not
    // need to branch on the debug flag" means in practice.
    CY_CHECK_EQ(frame.compute_counter.calls, 1U);
    CY_CHECK_EQ(frame.shade_counter.calls, 1U);
    CY_CHECK_EQ(frame.debug_counter.calls, 0U);

    CY_CHECK_GT(result->barriers, 0U);
    CY_CHECK_EQ(fixture.device().statistics().draws, 1U);
    CY_CHECK_EQ(fixture.device().statistics().dispatches, 1U);
    CY_CHECK(fixture.device().end_frame().has_value());
}

CY_TEST_CASE("the recorded stream is barrier, label, breadcrumb, then the pass's own commands") {
    // The order matters: a barrier recorded after the pass's commands would synchronise the wrong
    // side of the hazard. Asserting it on the null backend's log is how that order is checked
    // without a device.
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::Expected<cy::rhi::TextureHandle, cy::Error> image =
        fixture.device().create_texture(swapchain_description());
    CY_REQUIRE(image.has_value());
    CY_REQUIRE(fixture.device().begin_frame().has_value());

    Frame frame(fixture.allocator(), *image);
    GraphExecutor executor(fixture.allocator(), fixture.device());
    cy::rhi::null::clear_command_log(fixture.device());
    CY_REQUIRE(
        executor
            .execute(frame.graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{})
            .has_value());

    const cy::Span<const cy::rhi::null::RecordedCommand> log =
        cy::rhi::null::command_log(fixture.device());
    CY_CHECK_GE(log.size(), 8U);
    CY_CHECK_EQ(log[0].kind, cy::rhi::null::CommandKind::Barriers);
    CY_CHECK_EQ(log[1].kind, cy::rhi::null::CommandKind::BeginDebugLabel);
    CY_CHECK_EQ(log[2].kind, cy::rhi::null::CommandKind::WriteBreadcrumb);
    CY_CHECK_EQ(log[3].kind, cy::rhi::null::CommandKind::Dispatch);
    CY_CHECK_EQ(log[4].kind, cy::rhi::null::CommandKind::EndDebugLabel);
    // Each pass carries its own breadcrumb value, so a device-loss report can name the pass the GPU
    // reached rather than the frame it was in.
    CY_CHECK_EQ(log[2].b, 1U);
}

CY_TEST_CASE("two runs of the same frame produce byte-identical plans and command streams") {
    // design.md §6: submission order derives from stable inputs, never from iteration order over a
    // hash map, pointer values, or the order instances happened to be published. At M3 this looks
    // like pedantry; at M9 it is the difference between a golden image that reproduces and one that
    // is flaky for reasons nobody can find.
    cy::u64 plan_hashes[2] = {0, 0};
    cy::u64 log_hashes[2] = {0, 0};

    for (cy::u32 run = 0; run < 2; ++run) {
        NullFixture fixture;
        CY_REQUIRE(fixture.ok());
        cy::Expected<cy::rhi::TextureHandle, cy::Error> image =
            fixture.device().create_texture(swapchain_description());
        CY_REQUIRE(image.has_value());
        CY_REQUIRE(fixture.device().begin_frame().has_value());

        Frame frame(fixture.allocator(), *image);
        GraphExecutor executor(fixture.allocator(), fixture.device());
        cy::rhi::null::clear_command_log(fixture.device());
        cy::Expected<cy::rendering::ExecutionResult, cy::Error> result = executor.execute(
            frame.graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
        CY_REQUIRE(result.has_value());
        plan_hashes[run] = result->plan_hash;
        log_hashes[run] = cy::rhi::null::command_log_hash(fixture.device());
    }

    CY_CHECK_NE(plan_hashes[0], 0U);
    CY_CHECK_EQ(plan_hashes[0], plan_hashes[1]);
    CY_CHECK_NE(log_hashes[0], 0U);
    CY_CHECK_EQ(log_hashes[0], log_hashes[1]);
}

CY_TEST_CASE(
    "the graph creates a view for every declared range, and none for a range nobody named") {
    // Spike gotcha 6d: a combined-image-sampler descriptor's view must match the declared range.
    // Deriving the view from the declaration rather than letting a pass author pick one is what
    // makes "a descriptor naming a subresource the graph never transitioned" unrepresentable.
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    CY_REQUIRE(fixture.device().begin_frame().has_value());

    RenderGraph graph(fixture.allocator());
    const ResourceId layered = graph.create_texture(storage_image("layers", 32, 2));
    const ResourceId out =
        graph.import_buffer(storage_buffer("result"), cy::rhi::BufferHandle::from_slot(0, 1));
    graph.add_pass("write layer 1", cy::rhi::QueueKind::Graphics)
        .write(layered, Access::ComputeStorageWrite, cy::rhi::SubresourceRange::layer(1));
    graph.add_pass("read layer 1", cy::rhi::QueueKind::Graphics)
        .read(layered, Access::ComputeStorageRead, cy::rhi::SubresourceRange::layer(1))
        .write(out, Access::TransferWrite);
    CY_REQUIRE(graph.status().has_value());

    GraphExecutor executor(fixture.allocator(), fixture.device());
    cy::Expected<CompiledGraph, cy::Error> plan =
        executor.compile_only(graph, cy::rendering::CompileOptions{});
    CY_REQUIRE(plan.has_value());

    CY_CHECK_FALSE(executor.view(layered, cy::rhi::SubresourceRange{0, 1, 1, 1}).is_null());
    // Layer 0 was never declared, so the graph never transitioned it and there is no view for it.
    // Answering with a null handle is the honest answer, not a defect.
    CY_CHECK(executor.view(layered, cy::rhi::SubresourceRange{0, 1, 0, 1}).is_null());
    executor.release();
}

CY_TEST_CASE("a transient is bound inside the pool the plan reserved") {
    NullFixture fixture;
    CY_REQUIRE(fixture.ok());
    CY_REQUIRE(fixture.device().begin_frame().has_value());

    RenderGraph graph(fixture.allocator());
    const ResourceId first = graph.create_texture(storage_image("first", 256));
    const ResourceId second = graph.create_texture(storage_image("second", 256));
    const ResourceId out =
        graph.import_buffer(storage_buffer("result"), cy::rhi::BufferHandle::from_slot(0, 1));
    graph.add_pass("write first", cy::rhi::QueueKind::Graphics)
        .write(first, Access::ComputeStorageWrite);
    graph.add_pass("first to second", cy::rhi::QueueKind::Graphics)
        .read(first, Access::ComputeStorageRead)
        .write(second, Access::ComputeStorageWrite);
    graph.add_pass("copy out", cy::rhi::QueueKind::Graphics)
        .read(second, Access::TransferRead)
        .write(out, Access::TransferWrite);

    GraphExecutor executor(fixture.allocator(), fixture.device());
    cy::Expected<CompiledGraph, cy::Error> plan =
        executor.compile_only(graph, cy::rendering::CompileOptions{});
    CY_REQUIRE(plan.has_value());
    // The device reserved exactly what the plan asked for, and every placement fits.
    CY_CHECK_EQ(fixture.device().transient_pool_bytes(), plan->memory.heap_bytes);
    CY_CHECK(cy::rendering::validate_plan(graph, *plan).has_value());
    // The transient category carries the pool, and nothing else does.
    const auto transient = static_cast<cy::u32>(cy::rhi::GpuMemoryCategory::Transient);
    CY_CHECK_EQ(fixture.device().memory_report().live_bytes[transient], plan->memory.heap_bytes);
    executor.release();
}

CY_TEST_CASE("the plan dumps as readable text and as a Graphviz digraph") {
    // `rhi-and-render-graph` requires the graph to be able to dump "passes, resources, lifetimes,
    // barriers, aliasing decisions" as text or a Graphviz diagram.
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId scratch = graph.create_texture(storage_image("scratch", 64));
    const ResourceId target = graph.import_texture(colour_target("swapchain", 64),
                                                   cy::rhi::TextureHandle::from_slot(0, 1),
                                                   cy::rhi::ImageLayout::Undefined);
    graph.add_pass("compute", cy::rhi::QueueKind::AsyncCompute)
        .write(scratch, Access::ComputeStorageWrite);
    graph.add_pass("shade", cy::rhi::QueueKind::Graphics)
        .read(scratch, Access::FragmentSampledRead)
        .write(target, Access::ColorAttachmentWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(two_queue_options());
    CY_REQUIRE(plan.has_value());

    cy::Array<char> text(cy::system_allocator(cy::MemoryDomain::Renderer));
    cy::Expected<cy::usize, cy::Error> written = cy::rendering::dump_text(graph, *plan, text);
    CY_REQUIRE(written.has_value());
    CY_CHECK_GT(*written, 0U);
    CY_CHECK(std::strstr(text.data(), "submit 0") != nullptr);
    CY_CHECK(std::strstr(text.data(), "async-compute") != nullptr);
    CY_CHECK(std::strstr(text.data(), "queue-family ownership transfer") != nullptr);
    CY_CHECK(std::strstr(text.data(), "'scratch'") != nullptr);

    cy::Array<char> dot(cy::system_allocator(cy::MemoryDomain::Renderer));
    CY_REQUIRE(cy::rendering::dump_graphviz(graph, *plan, dot).has_value());
    CY_CHECK(std::strstr(dot.data(), "digraph render_graph") != nullptr);
    CY_CHECK(std::strstr(dot.data(), "semaphore") != nullptr);
}

// --- Parallel recording (task 2.2.5) ------------------------------------------------------------

namespace {

/// A job system for the duration of a scope. `core-jobs-and-concurrency` allows exactly one running
/// system per process, and doctest runs cases one at a time, so a scope is the right lifetime.
class ScopedJobSystem {
public:
    explicit ScopedJobSystem(cy::u32 workers) noexcept {
        cy::jobs::JobSystemConfig config;
        config.worker_count = workers;
        config.task_slots_per_participant = 2048;
        config.deque_capacity = 2048;
        config.scratch_bytes_per_participant = cy::usize{256} * 1024;
        started_ = system_.start(config).has_value();
    }
    ~ScopedJobSystem() { system_.shutdown(); }

    ScopedJobSystem(const ScopedJobSystem&) = delete;
    ScopedJobSystem& operator=(const ScopedJobSystem&) = delete;
    ScopedJobSystem(ScopedJobSystem&&) = delete;
    ScopedJobSystem& operator=(ScopedJobSystem&&) = delete;

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] cy::jobs::JobSystem& get() noexcept { return system_; }

private:
    cy::jobs::JobSystem system_;
    bool started_ = false;
};

/// The recorded stream with the `execute-secondary` markers removed, so that a parallel recording
/// and a sequential one can be compared for the thing that matters: the same commands, in the same
/// order.
cy::u64 filtered_stream_hash(const cy::rhi::Device& device) noexcept {
    cy::u64 hash = 1469598103934665603ULL;
    for (const cy::rhi::null::RecordedCommand& command : cy::rhi::null::command_log(device)) {
        if (command.kind == cy::rhi::null::CommandKind::ExecuteSecondary) {
            continue;
        }
        const cy::u32 fields[] = {static_cast<cy::u32>(command.kind), command.a, command.b,
                                  command.c, command.d};
        for (const cy::u32 field : fields) {
            hash = (hash ^ field) * 1099511628211ULL;
        }
    }
    return hash;
}

/// A chain of independent passes, each writing its own transient and one of them writing the
/// imported output so the chain is a culling root.
struct WideFrame {
    static constexpr cy::u32 kPasses = 8;

    RenderGraph graph;
    RecordCounter counters[kPasses];

    explicit WideFrame(cy::Allocator& allocator, cy::rhi::TextureHandle target) noexcept
        : graph(allocator) {
        const ResourceId imported = graph.import_texture(colour_target("swapchain", 64), target,
                                                         cy::rhi::ImageLayout::Undefined);
        for (RecordCounter& counter : counters) {
            const ResourceId scratch = graph.create_texture(storage_image("scratch", 32));
            graph.add_pass("wide", cy::rhi::QueueKind::Graphics)
                .write(scratch, Access::ComputeStorageWrite)
                .write(imported, Access::ColorAttachmentWrite)
                .record(&record_draw, &counter);
        }
    }
};

}  // namespace

CY_TEST_CASE(
    "passes are recorded on job workers and executed in plan order, not completion order") {
    // `rhi-and-render-graph`: "Recording SHALL be deterministic: the same frame description SHALL
    // produce the same command stream regardless of thread scheduling." The workers decide WHEN a
    // pass is recorded; the plan decides WHERE its commands end up, because the secondaries are
    // executed in plan order in the primary. This case pins both halves.
    ScopedJobSystem jobs(4);
    CY_REQUIRE(jobs.started());

    // The sequential stream first: the reference the parallel one is compared against.
    cy::u64 sequential = 0;
    {
        NullFixture fixture;
        CY_REQUIRE(fixture.ok());
        cy::Expected<cy::rhi::TextureHandle, cy::Error> image =
            fixture.device().create_texture(swapchain_description());
        CY_REQUIRE(image.has_value());
        CY_REQUIRE(fixture.device().begin_frame().has_value());
        WideFrame frame(fixture.allocator(), *image);
        GraphExecutor executor(fixture.allocator(), fixture.device());
        cy::rhi::null::clear_command_log(fixture.device());
        CY_REQUIRE(executor
                       .execute(frame.graph, cy::rendering::CompileOptions{},
                                cy::rendering::ExecuteOptions{})
                       .has_value());
        sequential = filtered_stream_hash(fixture.device());
    }

    cy::u64 orders[2] = {0, 0};
    cy::u64 streams[2] = {0, 0};
    cy::u32 run = 0;
    for (cy::u64& recorded_order : orders) {
        NullFixture fixture;
        CY_REQUIRE(fixture.ok());
        cy::Expected<cy::rhi::TextureHandle, cy::Error> image =
            fixture.device().create_texture(swapchain_description());
        CY_REQUIRE(image.has_value());
        CY_REQUIRE(fixture.device().begin_frame().has_value());

        WideFrame frame(fixture.allocator(), *image);
        CY_REQUIRE(frame.graph.status().has_value());

        cy::rendering::ExecuteOptions options;
        options.parallel_recording = true;
        options.parallel_pass_threshold = 2;
        options.job_system = &jobs.get();

        GraphExecutor executor(fixture.allocator(), fixture.device());
        cy::rhi::null::clear_command_log(fixture.device());
        cy::Expected<cy::rendering::ExecutionResult, cy::Error> result =
            executor.execute(frame.graph, cy::rendering::CompileOptions{}, options);
        CY_REQUIRE(result.has_value());

        CY_CHECK_EQ(result->passes_recorded, WideFrame::kPasses);
        // One secondary per pass, joined before submission.
        CY_CHECK_EQ(result->secondary_command_buffers, WideFrame::kPasses);
        for (const RecordCounter& counter : frame.counters) {
            CY_CHECK_EQ(counter.calls, 1U);
        }

        // One `execute-secondary` per pass, in plan order, each naming how many commands it
        // spliced in — and no handle, because a handle is assigned in whatever order the workers
        // asked for one and would make the stream depend on thread scheduling.
        cy::u64 order = 1469598103934665603ULL;
        cy::u32 executed = 0;
        for (const cy::rhi::null::RecordedCommand& command :
             cy::rhi::null::command_log(fixture.device())) {
            if (command.kind != cy::rhi::null::CommandKind::ExecuteSecondary) {
                continue;
            }
            order = (order ^ command.a) * 1099511628211ULL;
            ++executed;
        }
        CY_CHECK_EQ(executed, WideFrame::kPasses);
        recorded_order = order;
        streams[run] = filtered_stream_hash(fixture.device());
        ++run;
    }
    CY_CHECK_EQ(orders[0], orders[1]);
    CY_CHECK_EQ(streams[0], streams[1]);

    // THE ASSERTION THE SPECIFICATION ASKS FOR: "the same frame description SHALL produce the same
    // command stream regardless of thread scheduling". Recorded on four workers, the commands are
    // the same commands in the same order as when one thread recorded them — the only difference is
    // the `execute-secondary` markers, which the filter above removes.
    CY_CHECK_EQ(streams[0], sequential);
}
