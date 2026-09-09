// The assembled frame ON A DEVICE, with validation on. M8.b task 11.2.
//
// ================================================================================================
// WHAT THIS ADDS TO THE NULL-BACKEND SUITE
// ================================================================================================
//
// Two things the null backend cannot say anything about:
//
//   * the two of the eight modules that only exist on a device run — `cy::rendering-gpu-culling`'s
//     dispatch and `cy::rendering-virtual-texturing`'s feedback resolve — inside the SAME graph as
//     the frame's own thirteen stages, which is the thing that was never true before;
//   * VALIDATION IS COUNTED. `rhi-and-render-graph`: a frame that renders but trips validation is
//     not a frame that works. Every case asserts the device's cumulative error count is still zero,
//     and synchronisation validation is on — which is what makes "the graph derived every barrier"
//     a measurement of this frame rather than of the graph's own suite.
//
// It SKIPS LOUDLY on a machine with no Vulkan device, naming the backend that was selected instead.

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/servers/render/virtual_texturing/system.h>
#include <cy/test/test.h>

#include <cstdio>

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::rendering;
using namespace cy::rendering::assembly;

namespace {

constexpr u32 kWidth = 320;
constexpr u32 kHeight = 180;

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

/// A device of its own per case, for the reason tests/render/device.h gives: synchronisation
/// validation keeps per-queue state for the process's lifetime, and recycled handles across two
/// devices in one process produce phantom cross-test hazards that look damning and are not.
class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
        (void)cy::rhi::vulkan::register_vulkan_backend();
        (void)cy::rhi::null::register_null_backend();
        cy::rhi::DeviceDescription description;
        description.application_name = "cy_test_render_assembly";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        device_ = cy::rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &validation_errors_);
        }
    }

    ~DeviceFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            cy::rhi::destroy_device(allocator_, device_.value());
        }
    }

    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;

    [[nodiscard]] bool has_gpu() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == cy::rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] cy::rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] u32 validation_errors() const noexcept { return validation_errors_; }

    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no Vulkan device on this machine; the backend selected was '%s' because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    cy::Allocator& allocator_;
    cy::rhi::BackendSelection selection_{};
    u32 validation_errors_ = 0;
    cy::Expected<cy::rhi::Device*, cy::Error> device_ =
        cy::fail(cy::ErrorCode::Unavailable, "not created");
};

[[nodiscard]] AssemblyDescription make_description(bool gpu_culling) noexcept {
    AssemblyDescription description;
    description.width = kWidth;
    description.height = kHeight;
    description.near_plane = 0.1F;
    description.far_plane = 200.0F;
    description.clusters = ClusterGridConfig{32, 16, 32};
    description.post.ambient_occlusion = true;
    description.post.temporal_antialiasing = true;
    description.post.bloom = true;
    description.material_capacity = 32;
    description.max_instances = 64;
    description.max_draws = 64;
    description.gpu_culling = gpu_culling;
    return description;
}

[[nodiscard]] AssemblyView make_view(cy::Span<const cy::render::LightDescription> lights) noexcept {
    AssemblyView view;
    view.fov_y_radians = 1.0471975512F;
    view.projection = cy::perspective_reversed_z(
        view.fov_y_radians, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1F, 200.0F);
    view.view =
        cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
    view.cull.frustum = cy::Frustum::from_view_projection(view.projection * view.view);
    view.cull.camera_position = Vec3{0.0F, 0.0F, 0.0F};
    view.cull.camera_forward = Vec3{0.0F, 0.0F, -1.0F};
    view.cull.fov_y_radians = view.fov_y_radians;
    view.lights = lights;
    view.sun_direction = Vec3{0.0F, 1.0F, 0.0F};
    return view;
}

[[nodiscard]] cy::render::LightDescription sun(u64 id) noexcept {
    cy::render::LightDescription light;
    light.kind = cy::render::LightKind::Directional;
    light.intensity = 100000.0F;
    light.stable_id = id;
    return light;
}

/// One LOD chain of one level, shared by every instance. The mesh table's, in the form the
/// dispatch reads — without it the compaction has no level to emit and the frame draws nothing,
/// which is a real state and is what `AssemblyView::lod_chains` documents.
struct LodTable {
    cy::render::culling::GpuLodChain chains[1] = {{0, 1}};
    cy::render::culling::GpuMeshLod levels[1] = {};

    LodTable() noexcept {
        levels[0].index_count = 36;
        levels[0].first_index = 0;
        levels[0].vertex_offset = 0;
        levels[0].material = 3;
        levels[0].screen_coverage_threshold = 0.0F;
    }
};

[[nodiscard]] cy::render::LightDescription point_light(Vec3 at, u64 id) noexcept {
    cy::render::LightDescription light;
    light.kind = cy::render::LightKind::Point;
    light.transform = cy::Transform::from_translation(at);
    light.intensity = 1000.0F;
    light.range = 20.0F;
    light.stable_id = id;
    return light;
}

/// Eight instances in front of the camera and two behind it, so a cull that did nothing and a cull
/// that worked report different numbers.
[[nodiscard]] cy::Status fill(SpatialIndex& index) noexcept {
    for (u32 which = 0; which < 10; ++which) {
        SpatialEntry entry;
        const bool visible = which < 8;
        const f32 z = visible ? -5.0F - static_cast<f32>(which) : 40.0F;
        entry.bounds = cy::Aabb::from_center_extents(Vec3{static_cast<f32>(which % 4U), 0.0F, z},
                                                     Vec3{0.5F, 0.5F, 0.5F});
        entry.stable_id = 300U + which;
        entry.gpu_slot = which;
        entry.radius = 0.9F;
        if (auto slot = index.insert(entry); !slot) {
            return cy::make_unexpected(slot.error());
        }
    }
    return cy::ok();
}

/// A 2048-texel terrain page set, the same one src/rendering/virtual_texturing/tests uses:
/// 128-texel tiles, five mips, two of them the tail.
[[nodiscard]] cy::render::vt::VirtualTextureDesc terrain_desc() noexcept {
    cy::render::vt::VirtualTextureDesc desc;
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

CY_TEST_CASE("the assembled frame executes on a real device without tripping validation") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }

    FrameAssembly assembly(allocator());
    CY_REQUIRE(assembly.initialize(make_description(false)).has_value());
    CY_REQUIRE(assembly.attach_device(fixture.device()).has_value());

    SpatialIndex index(allocator());
    CY_REQUIRE(fill(index).has_value());
    const cy::render::LightDescription lights[] = {sun(1), point_light(Vec3{0, 0, -6}, 2)};

    RenderGraph graph(allocator());
    AssemblyReport report;
    CY_REQUIRE(
        assembly.assemble(index, make_view({lights, 2}), FrameSinks{}, graph, report).has_value());
    CY_CHECK_EQ(report.cull.tested, 10U);
    CY_CHECK_EQ(report.cull.visible, 8U);
    CY_CHECK_EQ(report.draws, 8U);

    CY_REQUIRE(fixture.device().begin_frame().has_value());
    GraphExecutor executor(allocator(), fixture.device());
    CY_REQUIRE(assembly.execute(executor, graph, report).has_value());
    CY_CHECK(fixture.device().end_frame().has_value());
    CY_CHECK(report.executed);
    CY_CHECK_GT(report.execution.passes_recorded, 0U);
    // The graph derived barriers for a frame nobody hand-synchronised, and synchronisation
    // validation agreed.
    CY_CHECK_GT(report.execution.barriers, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the device cull dispatch runs inside the assembled frame's own graph") {
    // `cy::rendering-gpu-culling` was linked by its own two test binaries and by nothing else. This
    // is the first frame it has ever been part of: the dispatch, the compaction and the read-back
    // are declared into the SAME graph as the thirteen forward stages, and the graph orders them.
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }

    FrameAssembly assembly(allocator());
    CY_REQUIRE(assembly.initialize(make_description(true)).has_value());
    CY_REQUIRE(assembly.attach_device(fixture.device()).has_value());
    if (!assembly.device_culling()) {
        std::fprintf(stderr,
                     "this device cannot run the cull dispatch; the CPU path is the same "
                     "answer and the case above covers it\n");
        return;
    }

    SpatialIndex index(allocator());
    CY_REQUIRE(fill(index).has_value());
    const cy::render::LightDescription lights[] = {sun(1)};
    const LodTable lods;
    AssemblyView view = make_view({lights, 1});
    view.lod_chains = {lods.chains, 1};
    view.mesh_lods = {lods.levels, 1};

    RenderGraph graph(allocator());
    AssemblyReport report;
    CY_REQUIRE(assembly.assemble(index, view, FrameSinks{}, graph, report).has_value());
    CY_CHECK(report.culled_on_device);

    CY_REQUIRE(fixture.device().begin_frame().has_value());
    GraphExecutor executor(allocator(), fixture.device());
    CY_REQUIRE(assembly.execute(executor, graph, report).has_value());
    CY_CHECK(fixture.device().end_frame().has_value());
    CY_CHECK(report.executed);

    // The counters come back out of the dispatch, not out of a CPU loop: ten instances tested and
    // the eight in front of the camera surviving — the same answer `cull_view` gives for the same
    // index, which is what makes the two paths interchangeable in `assemble`.
    CY_CHECK_EQ(report.cull.tested, 10U);
    CY_CHECK_EQ(report.cull.visible, 8U);
    CY_CHECK_EQ(report.draws, 8U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the virtual texture's feedback resolve runs in the assembled frame's own graph") {
    // `cy::rendering-virtual-texturing` was the eighth of the eight modules M7's gate named, and it
    // had the same problem as the seventh: its dispatches ran in its own test's graph and in no
    // frame. Here they run in the frame's — declared beside the thirteen forward stages, ordered by
    // the same compilation, submitted in the same submit set.
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }

    const cy::render::vt::VirtualTextureDesc desc = terrain_desc();
    cy::render::vt::VirtualTextureSystem system(allocator());
    cy::render::vt::TileCacheDesc cache;
    cache.format = desc.format_class();
    cache.tile_size = desc.tile_size;
    cache.border = desc.border;
    cache.bytes_per_tile = desc.bytes_per_tile;
    cache.tile_capacity = 64;
    CY_REQUIRE(system.configure_cache(cache).has_value());
    CY_REQUIRE(system.register_texture(desc).has_value());
    // The mip tail resident, so the feedback the resolve compacts is about pages that exist —
    // "a surface is never missing, only blurry".
    CY_REQUIRE(system.make_mip_tail_resident(desc.id).has_value());

    FrameAssembly assembly(allocator());
    CY_REQUIRE(assembly.initialize(make_description(false)).has_value());
    CY_REQUIRE(assembly.attach_device(fixture.device()).has_value());
    vt::FeedbackSettings settings;
    settings.grid_width = 128;
    settings.grid_height = 128;
    settings.density = 1;
    settings.request_capacity = 512;
    CY_REQUIRE(assembly.attach_virtual_texture(desc, settings).has_value());
    CY_CHECK(assembly.virtual_texturing());

    SpatialIndex index(allocator());
    CY_REQUIRE(fill(index).has_value());
    const cy::render::LightDescription lights[] = {sun(1)};
    AssemblyView view = make_view({lights, 1});
    view.page_table = system.page_table(desc.id);

    RenderGraph graph(allocator());
    AssemblyReport report;
    CY_REQUIRE(assembly.assemble(index, view, FrameSinks{}, graph, report).has_value());
    CY_CHECK(report.virtual_texture_declared);

    CY_REQUIRE(fixture.device().begin_frame().has_value());
    GraphExecutor executor(allocator(), fixture.device());
    CY_REQUIRE(assembly.execute(executor, graph, report).has_value());
    CY_CHECK(fixture.device().end_frame().has_value());

    // The recording grid is 128 by 128 at density 1, so every one of its threads asked for a page
    // and the resolve compacted them into far fewer requests than samples. Both halves matter: a
    // zero here is a dispatch that did not run, and a request count equal to the sample count is a
    // resolve that compacted nothing.
    CY_CHECK_GT(report.virtual_texture_requests, 0U);
    CY_CHECK_LT(report.virtual_texture_requests, 128U * 128U);
    CY_CHECK_EQ(report.virtual_texture_dropped, 0U);
    // "A per-pixel request stream SHALL NOT reach the CPU": what the CPU mapped is the compacted
    // list, not the grid.
    CY_CHECK_LT(report.virtual_texture_bytes_read, 128ULL * 128ULL * 4ULL);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
