// Two subsystems arbitrating for one budget, fed by one frame's own GPU dispatches. M7 task 4.3.
//
// ================================================================================================
// WHY THIS CASE IS NOT A UNIT TEST WITH TWO FAKE SUBSYSTEMS
// ================================================================================================
//
// `residency`'s whole subject is arbitration BETWEEN subsystems, and until M7 exactly one thing in
// this tree registered one — `samples/06-open-world`, whose own comment says "One subsystem,
// because this sample pages one kind of thing". Its unit suite has always been able to register two
// policies and drive them with synthetic requests; what it could not do is show the arbitration
// happening over a frame's real feedback, which is the difference between a policy that is
// implemented and a policy that is used.
//
// So both claimants here come from dispatches that ran on the device in this process:
//
//   `Subsystem::Texture`   the compacted page requests `VirtualTextureFrame`'s resolve pass
//                          produced. One entry per page, with how many samples asked for it.
//   `Subsystem::Geometry`  the level each instance was drawn at, which is what `GpuCullPass`'s
//                          payloads say. The cull chose the rung; the residency layer decides
//                          whether the rung's bytes fit.
//
// The registration itself lives in `cy::rendering-virtual-texturing` — shipped engine code compiled
// into every build and every profile — not in this file. What this file adds is the device.
//
// WHAT IS HONESTLY MISSING, AND IT IS NAMED HERE RATHER THAN LEFT TO BE FOUND: the binary that runs
// these two dispatches together in a shipped loop is `samples/07-fidelity`, which is section 11's
// and is not this agent's to write. `FrameResidency` is built so that adopting it there is three
// calls; until then, the frame that exercises the arbitration is this suite's frame.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gpu_culling/cull_pass.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/virtual_texturing/frame.h>
#include <cy/rendering/virtual_texturing/frame_residency.h>
#include <cy/servers/render/virtual_texturing/system.h>
#include <cy/servers/residency/server.h>
#include <cy/test/test.h>

#include <cstdio>
#include <vector>

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::render::vt::VirtualTextureDesc;
using cy::render::vt::VirtualTextureSystem;
using cy::rendering::gpu_culling::GpuCullPass;
using cy::rendering::gpu_culling::GpuCullPassDescription;
using cy::rendering::gpu_culling::GpuCullReadback;
using cy::rendering::vt::FeedbackSettings;
using cy::rendering::vt::FrameResidency;
using cy::rendering::vt::FrameResidencyReport;
using cy::rendering::vt::FrameResidencySettings;
using cy::rendering::vt::VirtualTextureFrame;
using cy::rendering::vt::VirtualTextureFrameReadback;
using cy::residency::ResidencyServer;
using cy::residency::Subsystem;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

void count_validation(cy::rhi::ValidationSeverity severity, const char* message,
                      void* user) noexcept {
    if (severity == cy::rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n",
                 severity == cy::rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

VirtualTextureDesc terrain_desc() noexcept {
    VirtualTextureDesc desc;
    desc.id = 11;
    desc.width = 2048;
    desc.height = 2048;
    desc.tile_size = 128;
    desc.border = 4;
    desc.mip_count = 5;
    desc.layers = 1;
    desc.mip_tail_levels = 2;
    desc.bytes_per_tile = 16 * 1024;
    return desc;
}

}  // namespace

CY_TEST_CASE("residency: texture and geometry arbitrate for one budget over one frame's feedback") {
    cy::Allocator& gpu_allocator = cy::system_allocator(cy::MemoryDomain::Gpu);
    (void)cy::rhi::vulkan::register_vulkan_backend();
    (void)cy::rhi::null::register_null_backend();
    cy::rhi::DeviceDescription description;
    description.application_name = "cy_test_render_virtual_texturing";
    description.enable_validation = true;
    description.enable_synchronisation_validation = true;
    description.request_async_compute = false;
    cy::rhi::BackendSelection selection{};
    cy::Expected<cy::rhi::Device*, cy::Error> created =
        cy::rhi::create_device(gpu_allocator, "vulkan", description, selection);
    if (!created.has_value() ||
        created.value()->capabilities().backend() != cy::rhi::BackendKind::Vulkan) {
        std::fprintf(stderr,
                     "no Vulkan device on this machine; the backend selected was '%s' because %s\n",
                     selection.selected != nullptr ? selection.selected : "(none)",
                     selection.reason != nullptr ? selection.reason : "(no reason given)");
        if (created.has_value()) {
            cy::rhi::destroy_device(gpu_allocator, created.value());
        }
        return;
    }
    cy::rhi::Device& device = *created.value();
    u32 validation_errors = 0;
    device.set_validation_callback(&count_validation, &validation_errors);

    const VirtualTextureDesc desc = terrain_desc();

    // --- The frame's two dispatches --------------------------------------------------------------
    VirtualTextureSystem system(allocator());
    CY_REQUIRE(system.register_texture(desc).has_value());
    VirtualTextureFrame texture_frame;
    FeedbackSettings feedback;
    feedback.grid_width = 256;
    feedback.grid_height = 256;
    CY_REQUIRE(texture_frame.create(allocator(), device, desc, feedback).has_value());
    CY_REQUIRE(texture_frame.upload_page_table(*system.page_table(desc.id)).has_value());

    constexpr u32 kInstances = 32;
    std::vector<cy::render::GpuInstance> instances(kInstances);
    for (u32 slot = 0; slot < kInstances; ++slot) {
        cy::render::GpuInstance& instance = instances[slot];
        instance.flags = cy::render::kInstanceActive | cy::render::kInstanceVisible;
        instance.layer_mask = cy::render::kDefaultLayer;
        instance.bounds_center[2] = -5.0F - (static_cast<f32>(slot) * 7.0F);
        instance.bounds_radius = 2.0F;
        instance.lod_chain = 0;
    }
    std::vector<cy::render::culling::GpuMeshLod> levels(3);
    for (u32 level = 0; level < 3; ++level) {
        levels[level].index_count = 300 - (level * 100);
        levels[level].first_index = level * 1000;
        levels[level].material = level;
    }
    levels[0].screen_coverage_threshold = 0.5F;
    levels[1].screen_coverage_threshold = 0.2F;
    levels[2].screen_coverage_threshold = 0.0F;
    const cy::render::culling::GpuLodChain chains[1] = {{0, 3}};
    std::vector<u32> previous(kInstances, cy::render::culling::kNoLodFade);

    cy::render::culling::GpuCullScene scene;
    scene.instances = cy::Span<const cy::render::GpuInstance>(instances.data(), instances.size());
    scene.chains = cy::Span<const cy::render::culling::GpuLodChain>(chains, 1);
    scene.mesh_lods = cy::Span<const cy::render::culling::GpuMeshLod>(levels.data(), levels.size());
    scene.previous_levels = cy::Span<u32>(previous.data(), previous.size());

    cy::render::culling::GpuCullView view;
    const cy::Mat4 projection =
        cy::perspective_reversed_z(1.0471975512F, 16.0F / 9.0F, 0.1F, 1000.0F);
    const cy::Mat4 look =
        cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
    cy::render::culling::write_frustum(view, cy::Frustum::from_view_projection(projection * look));
    cy::render::culling::write_field_of_view(view, 1.0471975512F);
    view.camera_forward[2] = -1.0F;
    view.instance_count = kInstances;

    GpuCullPassDescription cull_desc;
    cull_desc.max_instances = kInstances;
    cull_desc.max_draws = kInstances;
    cull_desc.max_lod_chains = 1;
    cull_desc.max_mesh_lods = static_cast<u32>(levels.size());
    cull_desc.max_previous_levels = kInstances;
    GpuCullPass cull;
    CY_REQUIRE(cull.create(allocator(), device, cull_desc).has_value());
    CY_REQUIRE(cull.upload(scene, view).has_value());

    // ONE FRAME, ONE GRAPH, BOTH DISPATCHES. The two feedback streams are two passes of the same
    // frame rather than two runs, which is what "exercised by a frame" has to mean.
    CY_REQUIRE(device.begin_frame().has_value());
    {
        cy::rendering::RenderGraph graph(allocator());
        CY_REQUIRE(texture_frame.declare(graph).has_value());
        CY_REQUIRE(cull.declare(graph).has_value());
        cy::rendering::GraphExecutor executor(allocator(), device);
        CY_REQUIRE(
            executor
                .execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{})
                .has_value());
        CY_REQUIRE(device.wait_idle().has_value());
    }
    CY_REQUIRE(device.end_frame().has_value());
    cy::Expected<VirtualTextureFrameReadback, cy::Error> texture_feedback =
        texture_frame.read_back();
    CY_REQUIRE(texture_feedback.has_value());
    cy::Expected<GpuCullReadback, cy::Error> geometry_feedback = cull.read_back();
    CY_REQUIRE(geometry_feedback.has_value());

    // NOT VACUOUS: two empty streams would arbitrate perfectly and prove nothing.
    CY_REQUIRE(texture_feedback->requests.size() > 0);
    CY_REQUIRE(geometry_feedback->payloads.size() > 0);

    // --- The arbitration ----------------------------------------------------------------------
    //
    // Both budgets are deliberately SMALLER than what the frame asked for, because a budget that
    // fits everything is not a budget and the two claimants would never meet.
    ResidencyServer server(allocator());
    FrameResidencySettings settings;
    settings.bytes_per_tile = desc.bytes_per_tile;
    settings.bytes_per_geometry_level = 64 * 1024;
    settings.texture_budget_bytes =
        (texture_feedback->requests.size() / 2) * settings.bytes_per_tile;
    settings.geometry_budget_bytes =
        (geometry_feedback->payloads.size() / 2) * settings.bytes_per_geometry_level;

    FrameResidency residency;
    CY_REQUIRE(residency.register_subsystems(server, settings).has_value());
    CY_CHECK(server.registered(Subsystem::Texture));
    CY_CHECK(server.registered(Subsystem::Geometry));

    CY_REQUIRE(residency.submit_texture_feedback(texture_feedback->requests, 0.0).has_value());
    CY_REQUIRE(residency.submit_geometry_feedback(geometry_feedback->payloads, 2, 0.0).has_value());

    FrameResidencyReport report;
    CY_REQUIRE(residency.arbitrate(0.0, 0, report).has_value());

    CY_CHECK(report.texture_requests == texture_feedback->requests.size());
    CY_CHECK(report.geometry_requests == geometry_feedback->payloads.size());
    // BOTH WON SOMETHING. One claimant taking the whole budget while the other starves is exactly
    // what a coordinated policy exists to prevent, and it is what a single-subsystem test cannot
    // see.
    CY_CHECK(report.texture_resident_bytes > 0);
    CY_CHECK(report.geometry_resident_bytes > 0);
    // AND NEITHER EXCEEDED ITS OWN. Two subsystems drawing from one memory domain must each stay
    // inside the budget the policy gave them, or "budget" is a label.
    CY_CHECK(report.texture_resident_bytes <= settings.texture_budget_bytes);
    CY_CHECK(report.geometry_resident_bytes <= settings.geometry_budget_bytes);
    std::fprintf(stderr,
                 "residency: texture %llu/%llu bytes over %u requests, geometry %llu/%llu bytes "
                 "over %u requests, %u admissions\n",
                 static_cast<unsigned long long>(report.texture_resident_bytes),
                 static_cast<unsigned long long>(settings.texture_budget_bytes),
                 report.texture_requests,
                 static_cast<unsigned long long>(report.geometry_resident_bytes),
                 static_cast<unsigned long long>(settings.geometry_budget_bytes),
                 report.geometry_requests, report.admissions);

    // --- The coordinated reduction ---------------------------------------------------------------
    //
    // `residency`: "On rising memory pressure the layer SHALL apply a coordinated reduction across
    // subsystems ... rather than each subsystem independently evicting." The plan is one list over
    // both subsystems, walked in DECLARED order, and with two claimants registered that order is
    // finally observable.
    server.on_pressure(cy::PressureLevel::Critical, cy::PressureLevel::Normal);
    cy::Array<cy::residency::ReductionStep> plan(allocator());
    CY_REQUIRE(residency.reduction_plan(plan).has_value());
    CY_REQUIRE(plan.size() > 0);

    bool saw_texture = false;
    bool saw_geometry = false;
    u32 previous_order = 0;
    for (const cy::residency::ReductionStep& step : plan) {
        saw_texture = saw_texture || step.subsystem == Subsystem::Texture;
        saw_geometry = saw_geometry || step.subsystem == Subsystem::Geometry;
        // Non-decreasing: the plan is a walk over `reduction_order`, and a plan that jumped back
        // would be six independent evictions with extra steps.
        CY_CHECK(step.order >= previous_order);
        previous_order = step.order;
        std::fprintf(stderr, "residency: reduce %s lever %s %.2f -> %.2f (order %u)\n",
                     cy::residency::subsystem_name(step.subsystem),
                     cy::residency::lever_name(step.lever), static_cast<double>(step.from),
                     static_cast<double>(step.to), step.order);
    }
    // THE CLAIM TASK 4.3 MAKES: more than one subsystem, arbitrated together.
    CY_CHECK(saw_texture);
    CY_CHECK(saw_geometry);
    // Texture declares reduction_order 0 and geometry 1, so texture gives way first. That is a
    // decision the policy DECLARED, not one that emerged from the order the two happened to
    // register in.
    CY_CHECK(plan[0].subsystem == Subsystem::Texture);
    CY_CHECK(report.first_reduced == Subsystem::Count);

    CY_CHECK(validation_errors == 0);
    cull.destroy();
    texture_frame.destroy();
    (void)device.wait_idle();
    cy::rhi::destroy_device(gpu_allocator, created.value());
}

CY_TEST_CASE("residency: every declared ladder position carries a price") {
    // M7 design.md §2.10: the ONE thing the budget arbiter's spike found `LeverSchedule` was
    // missing. Every mechanism the arbiter uses is expressed in terms of it — the deadband is half
    // the coarsest reachable lever quantum, the actuator forces one step per subsystem until
    // `gain * error` of the deficit is covered, and a restore is granted all-or-nothing only when
    // the measured headroom covers the step's predicted increase.
    FrameResidencySettings settings;
    settings.texture_budget_bytes = 1024;
    settings.geometry_budget_bytes = 1024;
    settings.bytes_per_tile = 64;
    settings.bytes_per_geometry_level = 64;

    const cy::residency::SubsystemPolicy texture = cy::rendering::vt::texture_policy(settings);
    const cy::residency::SubsystemPolicy geometry = cy::rendering::vt::geometry_policy(settings);
    CY_CHECK(texture.reduction_order < geometry.reduction_order);

    u32 declared = 0;
    for (const cy::residency::SubsystemPolicy* policy : {&texture, &geometry}) {
        for (const cy::residency::LeverSchedule& schedule : policy->levers) {
            if (!schedule.declared) {
                continue;
            }
            ++declared;
            // Position 0 is the reference and costs 1 by definition; every coarser position costs
            // strictly less, or it is not a reduction lever.
            CY_CHECK(schedule.cost_at(cy::PressureLevel::Normal) == 1.0F);
            CY_CHECK(schedule.cost_at(cy::PressureLevel::Elevated) <
                     schedule.cost_at(cy::PressureLevel::Normal));
            CY_CHECK(schedule.cost_at(cy::PressureLevel::Critical) <
                     schedule.cost_at(cy::PressureLevel::Elevated));
            CY_CHECK(schedule.cost_at(cy::PressureLevel::Critical) > 0.0F);
        }
    }
    CY_CHECK(declared == 3);

    // A schedule nobody filled in declares a FLAT ladder — every position costs what position 0
    // costs — which is the honest reading of "this lever changes quality and not cost", and is what
    // a zero-initialised structure would NOT have given.
    const cy::residency::LeverSchedule untouched;
    CY_CHECK(untouched.cost_at(cy::PressureLevel::Critical) == 1.0F);
}
