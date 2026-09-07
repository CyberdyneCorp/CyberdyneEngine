// The renderer consumes the GPU cull. M7 task 5.2.
//
// ================================================================================================
// WHAT "CONSUMES" HAS TO MEAN, AND WHY A LINK EDGE IS NOT ENOUGH
// ================================================================================================
//
// M6's closing gate recorded that `cy::servers-render-culling` was linked by nothing but its own
// test binaries. A milestone could satisfy the letter of that by adding a dependency line, and the
// module would still be a data model nothing consumed. So this case runs the whole path:
//
//   1. A `SpatialIndex` — the renderer's own scene, the one `cull_view()` reads.
//   2. `publish_gpu_cull_scene()` turns it into the `GpuInstance` records a dispatch reads.
//   3. `GpuCullPass` runs the dispatch on the device.
//   4. `apply_gpu_cull()` turns the compacted payloads back into `CullResults`.
//   5. `cull_view()` + `select_lods()` produce a `CullResults` for the same index and view.
//
// And it asserts that (4) and (5) name the same instances at the same levels. `CullResults` is the
// structure `cy::rendering-forward` consumes, so the claim being tested is precisely the one task
// 5.2 makes: NOTHING ABOVE THE CULL CAN TELL WHICH CULL RAN.
//
// The link edge is real too — `libcy_rendering_forward.a` now carries
// `libcy_servers_render_culling.a` in its dependency list — but the edge is a consequence of this
// path existing, not the deliverable.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/culling/cull.h>
#include <cy/rendering/culling/gpu_bridge.h>
#include <cy/rendering/culling/spatial.h>
#include <cy/rendering/gpu_culling/cull_pass.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/test/test.h>

#include <cstdio>
#include <numbers>
#include <vector>

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::rendering::CullOptions;
using cy::rendering::CullResults;
using cy::rendering::CullView;
using cy::rendering::CullWorkspace;
using cy::rendering::GpuCullPublication;
using cy::rendering::SpatialEntry;
using cy::rendering::SpatialIndex;
using cy::rendering::VisibleInstance;
using cy::rendering::gpu_culling::GpuCullPass;
using cy::rendering::gpu_culling::GpuCullPassDescription;
using cy::rendering::gpu_culling::GpuCullReadback;

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

/// The mesh LOD chain every instance in this case shares. `select_lods` reads it through a callback
/// because this module may not hold a second copy of the render server's mesh records.
const cy::render::MeshLod* chain_records() noexcept {
    static const cy::render::MeshLod levels[3] = {
        {0, 1, 0.5F, 300},
        {1, 1, 0.2F, 200},
        {2, 1, 0.0F, 100},
    };
    return levels;
}

cy::Span<const cy::render::MeshLod> chain_of(u32 /*lod_chain*/, void* /*user*/) noexcept {
    return {chain_records(), 3};
}

}  // namespace

CY_TEST_CASE("renderer: a GPU cull and a CPU cull fill the same CullResults") {
    cy::Allocator& gpu_allocator = cy::system_allocator(cy::MemoryDomain::Gpu);
    (void)cy::rhi::vulkan::register_vulkan_backend();
    (void)cy::rhi::null::register_null_backend();
    cy::rhi::DeviceDescription description;
    description.application_name = "cy_test_render_gpu_culling";
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

    // 1. The renderer's own scene. `gpu_slot` is the spatial slot here, because this case publishes
    //    every instance itself — a renderer whose GPU scene is published by `render::GpuScene`
    //    passes whatever slot that allocator gave, and the reverse map is what makes the difference
    //    invisible to everything below.
    SpatialIndex index(allocator());
    constexpr u32 kInstances = 24;
    for (u32 slot = 0; slot < kInstances; ++slot) {
        SpatialEntry entry;
        const f32 z = -4.0F - (static_cast<f32>(slot) * 9.0F);
        const f32 x = static_cast<f32>(slot % 5) * 2.0F;
        const Vec3 centre{x, 0.0F, z};
        const Vec3 half{1.5F, 1.5F, 1.5F};
        entry.bounds = cy::Aabb{centre - half, centre + half};
        entry.radius = 1.5F * std::numbers::sqrt3_v<f32>;
        entry.stable_id = 1000 + slot;
        entry.gpu_slot = slot;
        entry.lod_chain = 0;
        entry.importance = 0.5F;
        // Every fourth instance is transparent, so the routing into the two typed lists is
        // exercised rather than asserted over an empty list.
        if ((slot % 4) == 3) {
            entry.flags |= cy::rendering::kSpatialTransparent;
        }
        // Every third one moved, so `motion` is populated too.
        if ((slot % 3) == 0) {
            entry.flags |= cy::rendering::kSpatialMoved;
        }
        CY_REQUIRE(index.insert(entry).has_value());
    }
    // Four behind the camera, so the frustum test rejects something. Two culls that agree while
    // nothing was rejected have agreed about a question neither was asked.
    for (u32 extra = 0; extra < 4; ++extra) {
        SpatialEntry entry;
        const Vec3 centre{0.0F, 0.0F, 30.0F + (static_cast<f32>(extra) * 10.0F)};
        const Vec3 half{1.5F, 1.5F, 1.5F};
        entry.bounds = cy::Aabb{centre - half, centre + half};
        entry.radius = 1.5F * std::numbers::sqrt3_v<f32>;
        entry.stable_id = 2000 + extra;
        entry.gpu_slot = kInstances + extra;
        CY_REQUIRE(index.insert(entry).has_value());
    }

    CullView view;
    const cy::Mat4 projection =
        cy::perspective_reversed_z(1.0471975512F, 16.0F / 9.0F, 0.1F, 400.0F);
    const cy::Mat4 look =
        cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
    view.frustum = cy::Frustum::from_view_projection(projection * look);
    view.camera_position = Vec3{0.0F, 0.0F, 0.0F};
    view.camera_forward = Vec3{0.0F, 0.0F, -1.0F};
    view.fov_y_radians = 1.0471975512F;
    // Hysteresis off: the CPU path and the dispatch must be compared on one question at a time, and
    // the band's state is `test_gpu_cull_pass.cpp`'s subject.
    view.lod.hysteresis = 0.0F;
    view.lod.cross_fade_band = 0.0F;

    // 2. Publish.
    GpuCullPublication published(allocator());
    CY_REQUIRE(cy::rendering::publish_gpu_cull_scene(index, published).has_value());
    CY_CHECK(published.high_water == kInstances + 4);

    cy::render::culling::GpuCullView gpu_view;
    cy::rendering::write_gpu_cull_view(view, published.high_water, gpu_view);

    // The chain table the dispatch reads. `GpuMeshLod` is the shader-visible form of the same
    // records `chain_of` hands the CPU path, and the thresholds are what both select on.
    std::vector<cy::render::culling::GpuMeshLod> levels(3);
    for (u32 level = 0; level < 3; ++level) {
        levels[level].index_count = 300 - (level * 100);
        levels[level].first_index = level * 1000;
        levels[level].vertex_offset = level * 500;
        levels[level].material = level;
        levels[level].screen_coverage_threshold = chain_records()[level].screen_coverage_threshold;
    }
    const cy::render::culling::GpuLodChain chains[1] = {{0, 3}};
    std::vector<u32> previous(kInstances + 4, cy::render::culling::kNoLodFade);

    cy::render::culling::GpuCullScene scene;
    scene.instances =
        cy::Span<const cy::render::GpuInstance>(published.instances.data(), published.high_water);
    scene.chains = cy::Span<const cy::render::culling::GpuLodChain>(chains, 1);
    scene.mesh_lods = cy::Span<const cy::render::culling::GpuMeshLod>(levels.data(), levels.size());
    scene.previous_levels = cy::Span<u32>(previous.data(), previous.size());

    // 3. Dispatch.
    GpuCullPassDescription desc;
    desc.max_instances = kInstances + 4;
    desc.max_draws = kInstances + 4;
    desc.max_lod_chains = 1;
    desc.max_mesh_lods = static_cast<u32>(levels.size());
    desc.max_previous_levels = kInstances + 4;
    GpuCullPass pass;
    CY_REQUIRE(pass.create(allocator(), device, desc).has_value());
    CY_REQUIRE(pass.upload(scene, gpu_view).has_value());

    // The graph and its executor live in a scope of their own: both hold the device, and this case
    // destroys the device by hand at the end rather than through a fixture. An executor outliving
    // the device it was built against is a use-after-free that reproduces as a segmentation fault
    // in the last three lines of a passing test, which is exactly how it was found.
    {
        CY_REQUIRE(device.begin_frame().has_value());
        cy::rendering::RenderGraph graph(allocator());
        CY_REQUIRE(pass.declare(graph).has_value());
        cy::rendering::GraphExecutor executor(allocator(), device);
        CY_REQUIRE(
            executor
                .execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{})
                .has_value());
        CY_REQUIRE(device.wait_idle().has_value());
    }
    CY_REQUIRE(device.end_frame().has_value());
    cy::Expected<GpuCullReadback, cy::Error> readback = pass.read_back();
    CY_REQUIRE(readback.has_value());

    // 4. Back into the renderer's own structure.
    CullResults from_gpu(allocator());
    CY_REQUIRE(cy::rendering::apply_gpu_cull(index, published, readback->payloads,
                                             readback->counters, from_gpu)
                   .has_value());

    // 5. The CPU path over the same index and the same view.
    CullResults from_cpu(allocator());
    CullWorkspace workspace(allocator());
    CY_REQUIRE(
        cy::rendering::cull_view(index, view, CullOptions{}, workspace, from_cpu).has_value());
    std::vector<u32> cpu_previous(index.slot_count(), cy::rendering::kInvalidLod);
    CY_REQUIRE(cy::rendering::select_lods(index, view, &chain_of, nullptr,
                                          cy::Span<u32>(cpu_previous.data(), cpu_previous.size()),
                                          from_cpu)
                   .has_value());

    // NOT VACUOUS: something has to have survived, and something has to have been rejected, or the
    // two culls agreeing says nothing at all.
    CY_CHECK(from_cpu.opaque.size() > 0);
    CY_CHECK(from_cpu.transparent.size() > 0);
    CY_CHECK(from_cpu.motion.size() > 0);
    CY_CHECK(from_cpu.stats.rejected_by_frustum > 0);

    const auto same_list = [](const cy::Array<VisibleInstance>& want,
                              const cy::Array<VisibleInstance>& got, const char* which) {
        CY_REQUIRE(got.size() == want.size());
        for (cy::usize index_of = 0; index_of < want.size(); ++index_of) {
            CY_CHECK(got[index_of].slot == want[index_of].slot);
            CY_CHECK(got[index_of].gpu_slot == want[index_of].gpu_slot);
            CY_CHECK(got[index_of].stable_id == want[index_of].stable_id);
            CY_CHECK(got[index_of].lod_level == want[index_of].lod_level);
        }
        std::fprintf(stderr, "renderer: %s agrees over %zu instances\n", which,
                     static_cast<size_t>(want.size()));
    };
    same_list(from_cpu.opaque, from_gpu.opaque, "opaque");
    same_list(from_cpu.transparent, from_gpu.transparent, "transparent");
    same_list(from_cpu.motion, from_gpu.motion, "motion");

    // The diagnostics too. `apply_gpu_cull` copies the dispatch's counters verbatim rather than
    // recomputing them, so this is a comparison of two independent counts of the same frame.
    CY_CHECK(from_gpu.stats.tested == from_cpu.stats.tested);
    CY_CHECK(from_gpu.stats.visible == from_cpu.stats.visible);
    CY_CHECK(from_gpu.stats.rejected_by_frustum == from_cpu.stats.rejected_by_frustum);
    for (u32 level = 0; level < 8; ++level) {
        CY_CHECK(from_gpu.stats.lod_histogram[level] == from_cpu.stats.lod_histogram[level]);
    }

    CY_CHECK(validation_errors == 0);
    pass.destroy();
    (void)device.wait_idle();
    cy::rhi::destroy_device(gpu_allocator, created.value());
}
