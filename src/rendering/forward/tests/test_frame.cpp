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

#include <cstring>

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

CY_TEST_CASE("an authored directional shadow pass precedes its opaque sample") {
    RenderGraph graph(allocator());
    cy::rendering::TextureRequest shadow;
    shadow.name = "shadow color";
    shadow.format = cy::rhi::Format::R32Sfloat;
    shadow.width = 256;
    shadow.height = 256;
    const auto color = graph.create_texture(shadow);
    shadow.name = "shadow depth";
    shadow.format = cy::rhi::Format::D32Sfloat;
    const auto depth = graph.create_texture(shadow);

    FrameDescription description = make_description();
    description.shadow_color = color;
    description.shadow_depth = depth;
    const cy::rendering::FrameResourceRead sample{color, cy::rhi::Access::FragmentSampledRead};
    description.callbacks[static_cast<cy::usize>(FramePassKind::Opaque)].reads =
        cy::Span<const cy::rendering::FrameResourceRead>(&sample, 1);
    ForwardFrame frame(allocator());
    CY_REQUIRE(frame.build(graph, description).has_value());
    CY_CHECK_LT(position_of(frame, FramePassKind::Prepare),
                position_of(frame, FramePassKind::Shadow));
    CY_CHECK_LT(position_of(frame, FramePassKind::Shadow),
                position_of(frame, FramePassKind::Opaque));
    bool writes_color = false;
    bool writes_depth = false;
    for (const cy::rendering::Use& use : graph.pass_uses(frame.pass_of(FramePassKind::Shadow))) {
        writes_color |=
            use.resource == color && use.access == cy::rhi::Access::ColorAttachmentWrite;
        writes_depth |=
            use.resource == depth && use.access == cy::rhi::Access::DepthStencilAttachmentWrite;
    }
    CY_CHECK(writes_color);
    CY_CHECK(writes_depth);
    CY_CHECK(graph.compile(compile_options()).has_value());
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

CY_TEST_CASE("an opaque callback declares compute-produced vertex inputs") {
    RenderGraph graph(allocator());
    cy::rendering::BufferRequest request;
    request.name = "device-produced colours";
    request.size = 4096;
    request.extra_usage = cy::rhi::BufferUsage::Storage | cy::rhi::BufferUsage::Vertex;
    const cy::rendering::ResourceId colours =
        graph.import_buffer(request, cy::rhi::BufferHandle::from_slot(0, 1));
    graph.add_pass("shade vertices", cy::rhi::QueueKind::Graphics)
        .write(colours, cy::rhi::Access::ComputeStorageWrite);

    ForwardFrame frame(allocator());
    FrameDescription description = make_description();
    description.callbacks[static_cast<cy::usize>(FramePassKind::Opaque)].vertex_reads =
        cy::Span<const cy::rendering::ResourceId>(&colours, 1);
    CY_REQUIRE(frame.build(graph, description).has_value());
    bool opaque_reads_colours = false;
    for (const cy::rendering::Use& use : graph.pass_uses(frame.pass_of(FramePassKind::Opaque))) {
        opaque_reads_colours |= use.resource == colours;
    }
    CY_REQUIRE(opaque_reads_colours);

    cy::Expected<cy::rendering::CompiledGraph, cy::Error> plan = graph.compile(compile_options());
    CY_REQUIRE(plan.has_value());

    bool compute_to_vertex_dependency = false;
    bool producer_scheduled = false;
    for (const cy::rendering::Submit& submit : plan->submits) {
        for (const cy::rendering::ScheduledPass& scheduled : submit.passes) {
            producer_scheduled |=
                std::strcmp(graph.pass_name(scheduled.pass), "shade vertices") == 0;
            if (std::strcmp(graph.pass_name(scheduled.pass), "opaque") != 0) {
                continue;
            }
            for (const cy::rhi::MemoryBarrier& barrier : scheduled.pre.memory) {
                if (barrier.src_stage == cy::rhi::Stage::ComputeShader &&
                    barrier.src_access == cy::rhi::AccessFlags::ShaderStorageWrite &&
                    barrier.dst_stage == cy::rhi::Stage::VertexInput &&
                    barrier.dst_access == cy::rhi::AccessFlags::VertexAttributeRead) {
                    compute_to_vertex_dependency = true;
                }
            }
        }
    }
    CY_REQUIRE(producer_scheduled);
    CY_REQUIRE(compute_to_vertex_dependency);
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
    options.queue_ownership_domain[static_cast<cy::u32>(cy::rhi::QueueKind::AsyncCompute)] = 2;
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

// `rendering-architecture`, "Render targets and formats": "The renderer SHALL render HDR scene
// colour in a floating-point format (`RGBA16F` by default, `R11G11B10F` where alpha is unneeded and
// precision permits), with tonemapping to the output format at the end of the chain."
//
// WHY THE ASSERTION IS OVER THE GRAPH AND NOT OVER THE DEFAULT. `FrameDescription::color_format`
// being `Rgba16Sfloat` is one line of a header; what the requirement asks is that the frame's
// colour CHAIN is that format, and the way that claim breaks is not by somebody editing the default
// — it is by one intermediate target in the middle of the chain being declared in something
// cheaper, which is invisible until a bloom or a reflection clips. So every colour-carrying target
// the frame creates is read back out of the graph and required to be the scene colour's format, and
// the two targets that are deliberately NOT colour — depth and the single-channel ambient-occlusion
// buffer — are required to differ, so the case cannot be satisfied by a frame that declares one
// format for everything.
CY_TEST_CASE("the scene colour chain is floating point, and the two targets that are not say so") {
    // The specification names the default by name, so the default is checked by name.
    CY_CHECK_EQ(FrameDescription{}.color_format, cy::rhi::Format::Rgba16Sfloat);

    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    FrameDescription description = make_description();
    // Every optional colour target at once: the ones a frame only has when a feature asks for them
    // are exactly the ones a later change is most likely to declare in a narrower format.
    description.features.ambient_occlusion = true;
    description.features.screen_space_gi = true;
    description.features.screen_space_reflections = true;
    description.features.transparent_refraction = true;
    description.features.temporal = true;
    CY_REQUIRE(frame.build(graph, description).has_value());

    const cy::rendering::FrameResources& resources = frame.resources();
    const cy::rhi::Format colour = description.color_format;
    const cy::rendering::ResourceId colour_targets[] = {
        resources.color,
        resources.screen_space_gi,
        resources.reflections,
        resources.opaque_color_copy,
        resources.temporal_history,
        resources.output,
    };
    for (const cy::rendering::ResourceId id : colour_targets) {
        CY_REQUIRE_NE(id, kInvalidResource);
        const cy::rendering::ResourceInfo& info = graph.resource(id);
        CY_REQUIRE(info.is_texture);
        CY_CHECK_EQ(info.texture.format, colour);
    }

    // The control. Depth is a depth format and ambient occlusion is one 8-bit channel, so "every
    // target is the colour format" is a claim this frame could have failed.
    CY_CHECK_EQ(graph.resource(resources.depth).texture.format, cy::rhi::Format::D32Sfloat);
    CY_CHECK_EQ(graph.resource(resources.ambient_occlusion).texture.format,
                cy::rhi::Format::R8Unorm);
    CY_CHECK_NE(graph.resource(resources.ambient_occlusion).texture.format, colour);
}

// --- Virtual geometry's stage. M11.c task 4.3 --------------------------------------------------

CY_TEST_CASE(
    "virtual geometry is a stage after the prepass and before everything that reads depth") {
    // Off, it is a stage that does not exist: no pass, no visibility target. That is the frame
    // every caller before task 4.3 built, and the one a caller that never asks still builds.
    RenderGraph plain(allocator());
    ForwardFrame plain_frame(allocator());
    FrameDescription description = make_description();
    description.features.ambient_occlusion = true;
    CY_REQUIRE(plain_frame.build(plain, description).has_value());
    CY_CHECK_FALSE(declared(plain_frame, FramePassKind::VirtualGeometry));
    CY_CHECK_EQ(plain_frame.resources().visibility, kInvalidResource);

    // On, it sits between the prepass that wrote the depth it tests against and the first stage
    // that reads that depth — the screen-space passes, then the opaque pass's `Equal` test. Any
    // other place in the order draws clusters into a depth buffer something already consumed.
    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    description.features.virtual_geometry = true;
    CY_REQUIRE(frame.build(graph, description).has_value());
    CY_REQUIRE(declared(frame, FramePassKind::VirtualGeometry));
    CY_CHECK_LT(position_of(frame, FramePassKind::DepthPrepass),
                position_of(frame, FramePassKind::VirtualGeometry));
    CY_CHECK_LT(position_of(frame, FramePassKind::VirtualGeometry),
                position_of(frame, FramePassKind::AmbientOcclusion));
    CY_CHECK_LT(position_of(frame, FramePassKind::VirtualGeometry),
                position_of(frame, FramePassKind::Opaque));

    // One word a pixel, which is the visibility payload exactly, and it WRITES the frame's own
    // depth: the same resource the prepass writes and the opaque pass reads.
    const cy::rendering::ResourceId visibility = frame.resources().visibility;
    CY_REQUIRE_NE(visibility, kInvalidResource);
    CY_CHECK_EQ(graph.resource(visibility).texture.format, cy::rhi::Format::R32Uint);
    bool writes_visibility = false;
    bool writes_depth = false;
    for (const cy::rendering::Use& use :
         graph.pass_uses(frame.pass_of(FramePassKind::VirtualGeometry))) {
        writes_visibility |=
            use.resource == visibility && use.access == cy::rhi::Access::ColorAttachmentWrite;
        writes_depth |= use.resource == frame.resources().depth &&
                        use.access == cy::rhi::Access::DepthStencilAttachmentWrite;
    }
    CY_CHECK(writes_visibility);
    CY_CHECK(writes_depth);
}

CY_TEST_CASE(
    "virtual geometry's stage reads what its callback declares, and is refused with MSAA") {
    // The stage's callback draws INDIRECTLY and pulls its vertices out of storage buffers, so
    // `vertex_reads` — which declares vertex attributes — is the wrong intent for either. The
    // general `reads` list is what makes the graph order the draw after the dispatch that wrote
    // its arguments.
    RenderGraph graph(allocator());
    cy::rendering::BufferRequest request;
    request.name = "draw arguments";
    request.size = 64;
    request.extra_usage = cy::rhi::BufferUsage::Storage | cy::rhi::BufferUsage::Indirect;
    const cy::rendering::ResourceId arguments =
        graph.import_buffer(request, cy::rhi::BufferHandle::from_slot(0, 1));
    graph.add_pass("write arguments", cy::rhi::QueueKind::Graphics)
        .write(arguments, cy::rhi::Access::ComputeStorageWrite);

    ForwardFrame frame(allocator());
    FrameDescription description = make_description();
    description.features.virtual_geometry = true;
    const cy::rendering::FrameResourceRead read{arguments, cy::rhi::Access::IndirectCommandRead};
    description.callbacks[static_cast<cy::usize>(FramePassKind::VirtualGeometry)].reads =
        cy::Span<const cy::rendering::FrameResourceRead>(&read, 1);
    CY_REQUIRE(frame.build(graph, description).has_value());
    bool reads_arguments = false;
    for (const cy::rendering::Use& use :
         graph.pass_uses(frame.pass_of(FramePassKind::VirtualGeometry))) {
        reads_arguments |=
            use.resource == arguments && use.access == cy::rhi::Access::IndirectCommandRead;
    }
    CY_CHECK(reads_arguments);
    cy::Expected<cy::rendering::CompiledGraph, cy::Error> plan = graph.compile(compile_options());
    CY_REQUIRE(plan.has_value());

    // A visibility payload names ONE triangle per pixel, so a multisampled frame is refused by
    // name rather than drawn into a single-sample target beside a multisampled depth.
    RenderGraph msaa_graph(allocator());
    ForwardFrame msaa_frame(allocator());
    description.features.msaa_samples = 4;
    const cy::Status refused = msaa_frame.build(msaa_graph, description);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, cy::ErrorCode::Unsupported);
    CY_CHECK(std::strstr(refused.error().message, "virtual geometry") != nullptr);

    // THE SAME REASON REACHES THE GRAPH. The stage declares itself single-sample, so a pass author
    // who asks the declared stage for a sample count is refused by the graph with the frame's own
    // words rather than handed a twin nothing could resolve.
    const cy::rendering::PassId stage = frame.pass_of(FramePassKind::VirtualGeometry);
    CY_REQUIRE_NE(stage, cy::rendering::kInvalidPass);
    cy::rendering::PassBuilder(&graph, stage).multisample(4);
    CY_REQUIRE_FALSE(graph.status().has_value());
    CY_CHECK(std::strstr(graph.status().error().message, "virtual geometry") != nullptr);
}

// --- Selection outlines' stage ---------------------------------------------------------------

namespace {

/// A stand-in for `selection::OutlinePass`: it records what the frame handed it and declares the
/// two passes the real producer does — a mask drawn from the frame's own draw records, and a
/// composite that reads the prepass depth and reads and writes the stage's target.
struct OutlineProducer {
    cy::rendering::ScreenSpaceStageInputs seen{};
    cy::rendering::PassId composite = kInvalidPass;
    bool refuse = false;
};

cy::rendering::PassId declare_outlines(RenderGraph& graph,
                                       const cy::rendering::ScreenSpaceStageInputs& inputs,
                                       void* user) noexcept {
    auto* producer = static_cast<OutlineProducer*>(user);
    producer->seen = inputs;
    if (producer->refuse) {
        return kInvalidPass;
    }
    cy::rendering::TextureRequest request;
    request.name = "selection mask";
    request.format = cy::rhi::Format::R32Uint;
    request.width = inputs.width;
    request.height = inputs.height;
    const cy::rendering::ResourceId mask = graph.create_texture(request);
    cy::rendering::PassBuilder draw =
        graph.add_pass("selection mask", cy::rhi::QueueKind::Graphics);
    draw.write(mask, cy::rhi::Access::ColorAttachmentWrite);
    if (inputs.draw_instances != kInvalidResource) {
        draw.read(inputs.draw_instances, cy::rhi::Access::VertexStorageRead);
    }
    producer->composite = graph.add_pass("selection composite", cy::rhi::QueueKind::Graphics)
                              .read(mask, cy::rhi::Access::FragmentSampledRead)
                              .read(inputs.depth, cy::rhi::Access::FragmentSampledRead)
                              .use(inputs.target, cy::rhi::Access::ColorAttachmentReadWrite)
                              .id();
    return draw.id();
}

}  // namespace

CY_TEST_CASE("selection outlines sit over the tonemapped colour and under the interface") {
    // Off, the stage does not exist — the frame every caller before it built.
    RenderGraph plain(allocator());
    ForwardFrame plain_frame(allocator());
    FrameDescription description = make_description();
    CY_REQUIRE(plain_frame.build(plain, description).has_value());
    CY_CHECK_FALSE(declared(plain_frame, FramePassKind::SelectionOutlines));

    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    OutlineProducer producer;
    description.features.selection_outlines = true;
    description.selection_outlines_stage =
        cy::rendering::FrameStageDeclaration{&declare_outlines, &producer};
    CY_REQUIRE(frame.build(graph, description).has_value());
    CY_REQUIRE(declared(frame, FramePassKind::SelectionOutlines));
    // After the tone curve, so the colour a game asks for is the colour on screen, and before the
    // interface, which draws over the world and its outlines alike.
    CY_CHECK_LT(position_of(frame, FramePassKind::PostProcess),
                position_of(frame, FramePassKind::SelectionOutlines));
    CY_CHECK_LT(position_of(frame, FramePassKind::SelectionOutlines),
                position_of(frame, FramePassKind::UiAndDebug));

    // Handed the colour the chain ended in — the output the post-process tonemapped into — the
    // single-sample prepass depth, and the draw records its mask draws index.
    CY_CHECK_EQ(producer.seen.target, frame.resources().output);
    CY_CHECK_EQ(producer.seen.depth, frame.resources().depth);
    CY_CHECK_EQ(producer.seen.draw_instances, frame.resources().draw_instances);
    CY_CHECK_EQ(producer.seen.width, description.width);
    CY_CHECK_EQ(producer.seen.height, description.height);
    // The stage is recorded by its first pass, and its last pass — the composite — is declared
    // before the interface's, so the interface loads the outlined colour.
    CY_REQUIRE_NE(producer.composite, kInvalidPass);
    CY_CHECK_LT(frame.pass_of(FramePassKind::SelectionOutlines), producer.composite);
    CY_CHECK_LT(producer.composite, frame.pass_of(FramePassKind::UiAndDebug));
    CY_CHECK(graph.compile(compile_options()).has_value());
}

CY_TEST_CASE("selection outlines without a producer or a prepass are refused") {
    FrameDescription description = make_description();
    description.features.selection_outlines = true;
    RenderGraph unproduced(allocator());
    ForwardFrame unproduced_frame(allocator());
    CY_CHECK_FALSE(unproduced_frame.build(unproduced, description).has_value());

    OutlineProducer producer;
    description.selection_outlines_stage =
        cy::rendering::FrameStageDeclaration{&declare_outlines, &producer};
    description.features.depth_prepass = false;
    RenderGraph no_prepass(allocator());
    ForwardFrame no_prepass_frame(allocator());
    const cy::Status refused = no_prepass_frame.build(no_prepass, description);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(std::strstr(refused.error().message, "prepass") != nullptr);

    // A producer that refuses fails the frame rather than leaving the outlines out.
    description.features.depth_prepass = true;
    producer.refuse = true;
    RenderGraph refusing(allocator());
    ForwardFrame refusing_frame(allocator());
    CY_CHECK_FALSE(refusing_frame.build(refusing, description).has_value());
}

namespace {

/// A stand-in for `depth_of_field::DepthOfFieldPass`: it records what the frame handed it and
/// declares the shape the real producer does — a half-resolution pass over the source colour and
/// the depth, and a composite that writes the stage's target.
struct FocusProducer {
    cy::rendering::ScreenSpaceStageInputs seen{};
    cy::rendering::PassId composite = kInvalidPass;
    bool refuse = false;
};

cy::rendering::PassId declare_focus(RenderGraph& graph,
                                    const cy::rendering::ScreenSpaceStageInputs& inputs,
                                    void* user) noexcept {
    auto* producer = static_cast<FocusProducer*>(user);
    producer->seen = inputs;
    if (producer->refuse) {
        return kInvalidPass;
    }
    cy::rendering::TextureRequest request;
    request.name = "depth of field gather";
    request.format = cy::rhi::Format::Rgba16Sfloat;
    request.width = inputs.width / 2U;
    request.height = inputs.height / 2U;
    const cy::rendering::ResourceId gathered = graph.create_texture(request);
    const cy::rendering::PassId gather =
        graph.add_pass("depth of field gather", cy::rhi::QueueKind::Graphics)
            .read(inputs.source, cy::rhi::Access::ComputeSampledRead)
            .read(inputs.depth, cy::rhi::Access::ComputeSampledRead)
            .write(gathered, cy::rhi::Access::ComputeStorageWrite)
            .id();
    producer->composite = graph.add_pass("depth of field composite", cy::rhi::QueueKind::Graphics)
                              .read(gathered, cy::rhi::Access::ComputeSampledRead)
                              .read(inputs.source, cy::rhi::Access::ComputeSampledRead)
                              .write(inputs.target, cy::rhi::Access::ComputeStorageWrite)
                              .id();
    return gather;
}

}  // namespace

CY_TEST_CASE("depth of field sits after the temporal resolve and before bloom and exposure") {
    // Off, neither the stage nor its target exists.
    RenderGraph plain(allocator());
    ForwardFrame plain_frame(allocator());
    FrameDescription description = make_description();
    description.features.temporal = true;
    description.features.bloom = true;
    CY_REQUIRE(plain_frame.build(plain, description).has_value());
    CY_CHECK_FALSE(declared(plain_frame, FramePassKind::DepthOfField));
    CY_CHECK_EQ(plain_frame.resources().depth_of_field, kInvalidResource);

    RenderGraph graph(allocator());
    ForwardFrame frame(allocator());
    FocusProducer producer;
    description.features.depth_of_field = true;
    description.depth_of_field_stage =
        cy::rendering::FrameStageDeclaration{&declare_focus, &producer};
    CY_REQUIRE(frame.build(graph, description).has_value());
    CY_REQUIRE(declared(frame, FramePassKind::DepthOfField));
    // Step 7: after the temporal resolve (step 5) and before bloom (step 9), which is before the
    // exposure the post-process applies.
    CY_CHECK_LT(position_of(frame, FramePassKind::Temporal),
                position_of(frame, FramePassKind::DepthOfField));
    CY_CHECK_LT(position_of(frame, FramePassKind::DepthOfField),
                position_of(frame, FramePassKind::Bloom));
    CY_CHECK_LT(position_of(frame, FramePassKind::DepthOfField),
                position_of(frame, FramePassKind::PostProcess));

    // Handed the temporal history as its source, the single-sample depth, and a target of its own.
    const cy::rendering::FrameResources& resources = frame.resources();
    CY_CHECK_EQ(producer.seen.source, resources.temporal_history);
    CY_CHECK_EQ(producer.seen.depth, resources.depth);
    CY_REQUIRE_NE(resources.depth_of_field, kInvalidResource);
    CY_CHECK_EQ(producer.seen.target, resources.depth_of_field);
    CY_CHECK_EQ(producer.seen.width, description.width);
    CY_CHECK_EQ(producer.seen.height, description.height);
    // The stage's last pass is declared before bloom's first, which reads what it wrote.
    CY_REQUIRE_NE(producer.composite, kInvalidPass);
    CY_CHECK_LT(producer.composite, frame.pass_of(FramePassKind::Bloom));
    CY_CHECK(graph.compile(compile_options()).has_value());

    // Without bloom, the post-process reads what depth of field wrote.
    description.features.bloom = false;
    RenderGraph unbloomed(allocator());
    ForwardFrame unbloomed_frame(allocator());
    CY_REQUIRE(unbloomed_frame.build(unbloomed, description).has_value());
    CY_CHECK_EQ(unbloomed_frame.resources().post_source,
                unbloomed_frame.resources().depth_of_field);
}

CY_TEST_CASE("depth of field without a producer, or multisampled without a prepass, is refused") {
    FrameDescription description = make_description();
    description.features.depth_of_field = true;
    RenderGraph unproduced(allocator());
    ForwardFrame unproduced_frame(allocator());
    const cy::Status unproduced_status = unproduced_frame.build(unproduced, description);
    CY_REQUIRE_FALSE(unproduced_status.has_value());
    CY_CHECK(std::strstr(unproduced_status.error().message, "producer") != nullptr);

    FocusProducer producer;
    description.depth_of_field_stage =
        cy::rendering::FrameStageDeclaration{&declare_focus, &producer};
    description.features.depth_prepass = false;
    description.features.msaa_samples = 4;
    RenderGraph multisampled(allocator());
    ForwardFrame multisampled_frame(allocator());
    const cy::Status refused = multisampled_frame.build(multisampled, description);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(std::strstr(refused.error().message, "single-sample depth") != nullptr);

    // Without MSAA the opaque pass writes the single-sample depth itself, so no prepass is needed.
    description.features.msaa_samples = 1;
    RenderGraph single(allocator());
    ForwardFrame single_frame(allocator());
    CY_CHECK(single_frame.build(single, description).has_value());

    // A producer that refuses fails the frame rather than leaving the blur out.
    producer.refuse = true;
    RenderGraph refusing(allocator());
    ForwardFrame refusing_frame(allocator());
    CY_CHECK_FALSE(refusing_frame.build(refusing, description).has_value());
}
