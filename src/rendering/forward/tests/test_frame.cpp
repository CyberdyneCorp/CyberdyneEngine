// The pass order, declared into the render graph. Tasks 4.3.3 and 4.3.4.
//
// Every case builds a real `RenderGraph` and compiles a real plan, with no device: the derivation
// is device-free by construction (graph.h), and `synthetic_memory_query` answers the one question
// compilation asks. So "the pass order is correct" and "a disabled feature allocates nothing" are
// assertions rather than a diagram, and they run on a machine with no GPU.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/forward/diagnostics.h>
#include <cy/rendering/forward/frame.h>

namespace {

using cy::rendering::ForwardFrame;
using cy::rendering::FrameDescription;
using cy::rendering::FramePassKind;
using cy::rendering::kInvalidPass;
using cy::rendering::kInvalidResource;
using cy::rendering::PrepassMode;
using cy::rendering::RenderGraph;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

FrameDescription make_description() noexcept {
    FrameDescription description;
    description.width = 320;
    description.height = 180;
    description.light_count = 4;
    description.draw_instance_count = 16;
    const cy::rendering::ClusterGridConfig config{32, 8, 16};
    const cy::Expected<cy::rendering::ClusterGrid, cy::Error> grid =
        cy::rendering::make_cluster_grid(config, 320, 180, 0.1F, 100.0F);
    if (grid.has_value()) {
        description.cluster_grid = *grid;
    }
    return description;
}

cy::rendering::CompileOptions compile_options() noexcept {
    cy::rendering::CompileOptions options;
    options.query_memory = &cy::rendering::synthetic_memory_query;
    return options;
}

/// The position of a stage in the declared order, or the pass count when it is absent.
cy::usize position_of(const ForwardFrame& frame, FramePassKind kind) noexcept {
    const cy::Span<const cy::rendering::FramePass> passes = frame.passes();
    for (cy::usize index = 0; index < passes.size(); ++index) {
        if (passes[index].kind == kind) {
            return index;
        }
    }
    return passes.size();
}

bool declared(const ForwardFrame& frame, FramePassKind kind) noexcept {
    return frame.pass_of(kind) != kInvalidPass;
}

}  // namespace

CY_TEST_CASE("the prepass mode is derived from what later passes need") {
    // "WHEN SSAO is enabled and TAA is not THEN the prepass SHALL run in `DepthNormal` mode, and no
    // motion vector target SHALL be allocated."
    cy::rendering::FrameFeatures features;
    CY_CHECK_EQ(cy::rendering::select_prepass_mode(features), PrepassMode::DepthOnly);

    features.ambient_occlusion = true;
    CY_CHECK_EQ(cy::rendering::select_prepass_mode(features), PrepassMode::DepthNormal);

    features.temporal = true;
    CY_CHECK_EQ(cy::rendering::select_prepass_mode(features), PrepassMode::DepthNormalVelocity);

    // Motion blur wants velocity too, without anything temporal being on.
    cy::rendering::FrameFeatures blur;
    blur.motion_blur = true;
    CY_CHECK_EQ(cy::rendering::select_prepass_mode(blur), PrepassMode::DepthNormalVelocity);
}

CY_TEST_CASE("a plain frame declares the specification's stages, in its order") {
    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    FrameDescription description = make_description();
    CY_REQUIRE(frame.build(graph, description).has_value());
    CY_REQUIRE(graph.status().has_value());

    CY_CHECK(declared(frame, FramePassKind::Prepare));
    CY_CHECK(declared(frame, FramePassKind::DepthPrepass));
    CY_CHECK(declared(frame, FramePassKind::ClusterAssignment));
    CY_CHECK(declared(frame, FramePassKind::Opaque));
    CY_CHECK(declared(frame, FramePassKind::Sky));
    CY_CHECK(declared(frame, FramePassKind::Transparent));
    CY_CHECK(declared(frame, FramePassKind::PostProcess));
    CY_CHECK(declared(frame, FramePassKind::Present));

    // The order the specification fixes, checked pairwise rather than as one long expected list —
    // an expected list would have to be rewritten every time a feature toggles.
    CY_CHECK_LT(position_of(frame, FramePassKind::Prepare),
                position_of(frame, FramePassKind::DepthPrepass));
    CY_CHECK_LT(position_of(frame, FramePassKind::DepthPrepass),
                position_of(frame, FramePassKind::ClusterAssignment));
    CY_CHECK_LT(position_of(frame, FramePassKind::ClusterAssignment),
                position_of(frame, FramePassKind::Opaque));
    CY_CHECK_LT(position_of(frame, FramePassKind::Opaque), position_of(frame, FramePassKind::Sky));
    CY_CHECK_LT(position_of(frame, FramePassKind::Sky),
                position_of(frame, FramePassKind::Transparent));
    CY_CHECK_LT(position_of(frame, FramePassKind::Transparent),
                position_of(frame, FramePassKind::PostProcess));
    CY_CHECK_LT(position_of(frame, FramePassKind::PostProcess),
                position_of(frame, FramePassKind::UiAndDebug));
    CY_CHECK_LT(position_of(frame, FramePassKind::UiAndDebug),
                position_of(frame, FramePassKind::Present));
}

CY_TEST_CASE("a disabled feature has no pass and no target") {
    // "WHEN ambient occlusion, SSR, and TAA are all disabled THEN their passes SHALL be absent from
    // the graph and their targets unallocated." The absence is not a branch in a renderer — the
    // pass was never declared, so the graph never allocated its target.
    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    FrameDescription description = make_description();
    CY_REQUIRE(frame.build(graph, description).has_value());

    CY_CHECK_FALSE(declared(frame, FramePassKind::AmbientOcclusion));
    CY_CHECK_FALSE(declared(frame, FramePassKind::ScreenSpaceGi));
    CY_CHECK_FALSE(declared(frame, FramePassKind::ScreenSpaceReflections));
    CY_CHECK_FALSE(declared(frame, FramePassKind::Temporal));
    CY_CHECK_EQ(frame.resources().ambient_occlusion, kInvalidResource);
    CY_CHECK_EQ(frame.resources().reflections, kInvalidResource);
    CY_CHECK_EQ(frame.resources().temporal_history, kInvalidResource);
    // And no velocity target, which is the second half of the SSAO scenario.
    CY_CHECK_EQ(frame.resources().velocity, kInvalidResource);
    CY_CHECK_EQ(frame.prepass_mode(), PrepassMode::DepthOnly);

    // Turning ambient occlusion on adds one pass, one target and the normal buffer it reads.
    RenderGraph with_ao(allocator());
    ForwardFrame ao_frame(allocator());
    description.features.ambient_occlusion = true;
    CY_REQUIRE(ao_frame.build(with_ao, description).has_value());
    CY_CHECK(declared(ao_frame, FramePassKind::AmbientOcclusion));
    CY_CHECK_NE(ao_frame.resources().ambient_occlusion, kInvalidResource);
    CY_CHECK_NE(ao_frame.resources().normal_roughness, kInvalidResource);
    CY_CHECK_EQ(ao_frame.resources().velocity, kInvalidResource);
    CY_CHECK_EQ(ao_frame.prepass_mode(), PrepassMode::DepthNormal);
    CY_CHECK_LT(position_of(ao_frame, FramePassKind::AmbientOcclusion),
                position_of(ao_frame, FramePassKind::Opaque));
}

CY_TEST_CASE("refraction inserts the opaque copy before the transparent pass") {
    // "WHEN a transparent material samples scene colour THEN a copy of the opaque result SHALL be
    // made before the transparent pass, and the graph SHALL synchronise it." The synchronisation is
    // the graph's, derived from the declared read — no pass writes a barrier.
    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    FrameDescription description = make_description();
    description.features.transparent_refraction = true;
    CY_REQUIRE(frame.build(graph, description).has_value());

    CY_CHECK(declared(frame, FramePassKind::OpaqueColorCopy));
    CY_CHECK_NE(frame.resources().opaque_color_copy, kInvalidResource);
    CY_CHECK_LT(position_of(frame, FramePassKind::OpaqueColorCopy),
                position_of(frame, FramePassKind::Transparent));
    CY_CHECK_LT(position_of(frame, FramePassKind::Sky),
                position_of(frame, FramePassKind::OpaqueColorCopy));
}

CY_TEST_CASE("MSAA adds the multisampled targets and their resolves") {
    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    FrameDescription description = make_description();
    description.features.msaa_samples = 4;
    CY_REQUIRE(frame.build(graph, description).has_value());

    CY_CHECK_NE(frame.resources().color_multisampled, kInvalidResource);
    CY_CHECK_NE(frame.resources().depth_multisampled, kInvalidResource);
    CY_CHECK(declared(frame, FramePassKind::DepthResolve));
    CY_CHECK(declared(frame, FramePassKind::Resolve));
    // "Depth and normals SHALL be resolved before the screen-space passes, which operate at
    // single-sample resolution."
    CY_CHECK_LT(position_of(frame, FramePassKind::DepthResolve),
                position_of(frame, FramePassKind::Opaque));

    // A sample count the backends do not offer is refused rather than rounded.
    RenderGraph bad_graph(allocator());
    ForwardFrame bad_frame(allocator());
    description.features.msaa_samples = 3;
    CY_CHECK_FALSE(bad_frame.build(bad_graph, description).has_value());
}

CY_TEST_CASE("a screen-space feature without a prepass is refused, not silently wrong") {
    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    FrameDescription description = make_description();
    description.features.depth_prepass = false;
    description.features.ambient_occlusion = true;
    CY_CHECK_FALSE(frame.build(graph, description).has_value());

    // Without the screen-space feature, a prepass-less frame is a perfectly good frame: the opaque
    // pass writes depth itself.
    RenderGraph plain(allocator());
    ForwardFrame plain_frame(allocator());
    description.features.ambient_occlusion = false;
    CY_REQUIRE(plain_frame.build(plain, description).has_value());
    CY_CHECK_FALSE(declared(plain_frame, FramePassKind::DepthPrepass));
    CY_CHECK(declared(plain_frame, FramePassKind::Opaque));
}

CY_TEST_CASE("the declared frame compiles into a plan with no device at all") {
    // The whole point of the derivation being device-free: a frame's barriers, its transient
    // placement and its submit boundaries are all derivable in continuous integration.
    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    FrameDescription description = make_description();
    description.features.ambient_occlusion = true;
    description.features.transparent_refraction = true;
    CY_REQUIRE(frame.build(graph, description).has_value());
    CY_REQUIRE(graph.status().has_value());

    cy::Expected<cy::rendering::CompiledGraph, cy::Error> plan = graph.compile(compile_options());
    CY_REQUIRE(plan.has_value());
    CY_CHECK_GT(plan->stats.passes_declared, 0U);
    // Barriers were derived, and no pass could have written one: `PassBuilder` has no such method.
    CY_CHECK_GT(plan->stats.image_barriers + plan->stats.buffer_barriers, 0U);
    CY_CHECK_EQ(plan->stats.passes_culled, 0U);

    // Aliasing has something to work with, and reports both numbers so the saving is a measurement.
    CY_CHECK_GT(plan->memory.naive_bytes, 0U);
    CY_CHECK_LE(plan->memory.heap_bytes, plan->memory.naive_bytes);
}

CY_TEST_CASE("the same frame compiles to the same plan twice") {
    // design.md §6, one level below the sort key: the plan itself is deterministic, and `plan_hash`
    // is how that is asserted with a number rather than a dump.
    FrameDescription description = make_description();
    description.features.ambient_occlusion = true;

    cy::u64 hashes[2] = {};
    for (cy::u64& hash : hashes) {
        RenderGraph graph(allocator());
        ForwardFrame frame(allocator());
        CY_REQUIRE(frame.build(graph, description).has_value());
        cy::Expected<cy::rendering::CompiledGraph, cy::Error> plan =
            graph.compile(compile_options());
        CY_REQUIRE(plan.has_value());
        hash = plan->plan_hash;
    }
    CY_CHECK_EQ(hashes[0], hashes[1]);
    CY_CHECK_NE(hashes[0], 0U);
}

CY_TEST_CASE("cluster assignment lands on the async queue when the device has one") {
    // The pass declares its queue; the graph decides what that costs. With async compute off the
    // identical declarations fold onto one submit, which is the null backend's path and is not a
    // special case anywhere.
    FrameDescription description = make_description();
    description.cluster_queue = cy::rhi::QueueKind::AsyncCompute;

    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    CY_REQUIRE(frame.build(graph, description).has_value());

    cy::rendering::CompileOptions options = compile_options();
    options.queue_available[static_cast<cy::u32>(cy::rhi::QueueKind::AsyncCompute)] = true;
    options.queue_family[static_cast<cy::u32>(cy::rhi::QueueKind::AsyncCompute)] = 2;
    cy::Expected<cy::rendering::CompiledGraph, cy::Error> split = graph.compile(options);
    CY_REQUIRE(split.has_value());
    CY_CHECK_GT(split->stats.submits, 1U);

    RenderGraph single_queue(allocator());
    ForwardFrame folded(allocator());
    CY_REQUIRE(folded.build(single_queue, description).has_value());
    cy::rendering::CompileOptions no_async = compile_options();
    no_async.enable_async_compute = false;
    cy::Expected<cy::rendering::CompiledGraph, cy::Error> one = single_queue.compile(no_async);
    CY_REQUIRE(one.has_value());
    CY_CHECK_EQ(one->stats.submits, 1U);
    CY_CHECK_EQ(one->stats.queue_ownership_transfers, 0U);
}

CY_TEST_CASE("the diagnostics report refuses to record one pass twice") {
    cy::rendering::FrameDiagnostics diagnostics(allocator());
    cy::rendering::PassDiagnostics pass;
    pass.kind = FramePassKind::Opaque;
    pass.name = "opaque";
    pass.draw_calls = 12;
    pass.triangles = 1000;
    CY_REQUIRE(cy::rendering::record_pass(diagnostics, pass).has_value());
    CY_CHECK_FALSE(cy::rendering::record_pass(diagnostics, pass).has_value());

    pass.kind = FramePassKind::Transparent;
    pass.name = "transparent";
    pass.draw_calls = 3;
    pass.triangles = 90;
    pass.gpu_nanoseconds = 500000;
    pass.gpu_measured = true;
    CY_REQUIRE(cy::rendering::record_pass(diagnostics, pass).has_value());

    CY_CHECK_EQ(diagnostics.total_draw_calls(), 15U);
    CY_CHECK_EQ(diagnostics.total_triangles(), 1090U);
    // A device with no timestamp queries reports no GPU time, and zero is not the same answer as
    // "not measured" — which is why the flag is on every timing.
    CY_CHECK(diagnostics.any_gpu_measured());
    CY_CHECK_EQ(diagnostics.total_gpu_nanoseconds(), 500000U);

    cy::render::FrameStatistics statistics;
    cy::rendering::accumulate_into(statistics, diagnostics);
    CY_CHECK_EQ(statistics.draw_calls, 15U);
    CY_CHECK_EQ(statistics.passes, 2U);
}
