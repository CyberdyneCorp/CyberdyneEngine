// The GPU traversal, on a real device, compared against the CPU reference buffer for buffer.
// M7 task 7.3.
//
// ================================================================================================
// WHY THIS IS THE CASE THAT MATTERS
// ================================================================================================
//
// A compute shader that culls is a compute shader nothing can check. A screenshot cannot: a hole
// left by a wrong cull looks like a hole left by a wrong hierarchy, and both look like a hole left
// by a wrong camera. So the claim asserted here is the strongest one available — the dispatch and
// `traverse_reference()` select the SAME SET OF CLUSTERS — and it is asserted over a SWEEP of
// distances and thresholds rather than at one, because a single view passes on a shader whose
// selection test is off by a factor.
//
// M7 task 5.1 makes the same argument one level up for `cpu_reference_cull`, and this is that
// argument applied to the hierarchy.
//
// THE COMPARISON SORTS BOTH SIDES. The GPU appends through an atomic, so its order is the order the
// workgroups happened to finish in. That is not a defect and the test must not pretend it is one:
// the claim is about the SET, and the canonical order both sides are sorted into is what makes a
// mismatch nameable rather than merely detectable.
//
// IT SKIPS RATHER THAN FAILS WITH NO DEVICE. Most continuous integration machines have no GPU, and
// a suite that failed there is a suite somebody disables. The skip is loud and names the backend
// that was selected instead.

#include <cy/test/test.h>

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/virtual_geometry/gpu.h>
#include <cy/servers/render/culling/hzb.h>

#include "meshes.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

using namespace cy;             // NOLINT(google-build-using-namespace) — the suite's own subject
using namespace cy::rendering;  // NOLINT(google-build-using-namespace)

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

/// A Vulkan device with validation and synchronisation validation on, and a count of the errors it
/// reported. Every case builds its own: synchronisation validation keeps per-queue state for the
/// process's lifetime, and recycled handles across two devices in one process produce phantom
/// cross-test hazards. (M3's spike, gotcha 6e.)
class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_virtual_geometry_gpu";
        description.enable_validation = true;
        // Off by default even when the layers are on, and without it none of the hazard checks
        // fire at all. (M3 spike, gotcha 6h.)
        description.enable_synchronisation_validation = true;
        description.request_async_compute = false;
        device_ = rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &errors_);
        }
    }

    ~DeviceFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }

    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;

    [[nodiscard]] bool have_vulkan() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] Allocator& allocator() const noexcept { return allocator_; }
    [[nodiscard]] u32 validation_errors() const noexcept { return errors_; }
    void report_skip() const noexcept {
        std::fprintf(stderr, "no Vulkan device; the backend selected was '%s' because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

vg::BuildOptions test_options() noexcept {
    vg::BuildOptions options;
    options.policy.min_triangles = 8;
    options.policy.target_triangles = 32;
    options.policy.max_triangles = 32;
    options.policy.max_vertices = 64;
    options.policy.group_size = 4;
    options.page_bytes = 2048;
    options.resident_budget_bytes = 2048;
    return options;
}

vg::TraversalView view_at(f32 distance, f32 threshold) noexcept {
    vg::TraversalView view;
    view.projection.camera_position = Vec3{0.0F, 0.0F, distance};
    view.projection.viewport_height = 1080.0F;
    view.projection.fov_y_radians = 1.0471975512F;
    view.threshold_pixels = threshold;
    view.minimum_instance_pixels = 0.0F;
    view.cone_culling = false;
    for (Plane& plane : view.frustum.planes) {
        plane = Plane{Vec3{0.0F, 0.0F, 1.0F}, 1.0e9F};
    }
    view.frustum.refresh_corner_masks();
    return view;
}

bool visible_before(const vg::VisibleCluster& a, const vg::VisibleCluster& b) noexcept {
    if (a.instance != b.instance) {
        return a.instance < b.instance;
    }
    return a.cluster < b.cluster;
}

/// The whole GPU side of one case: a cooked asset, its scene buffers, and the traversal.
struct GpuFixture {
    GpuFixture(Allocator& allocator, rhi::Device& device) noexcept
        : bytes(allocator),
          asset(allocator),
          scene(allocator),
          traversal(allocator, device),
          executor(allocator, device) {}

    Array<u8> bytes;
    vg::DecodedAsset asset;
    vg::GpuScene scene;
    vg::GpuTraversal traversal;
    GraphExecutor executor;
};

}  // namespace

CY_TEST_CASE("the GPU traversal selects the same clusters as the reference, over a sweep") {
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 3);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), test_options(), allocator);
    CY_REQUIRE(build.has_value());

    GpuFixture gpu(allocator, fixture.device());
    CY_REQUIRE(vg::encode_asset(*build, vg::VertexEncoding{}, gpu.bytes).has_value());
    Expected<vg::DecodedAsset, Error> decoded = vg::decode_asset(gpu.bytes.span(), allocator);
    CY_REQUIRE(decoded.has_value());
    gpu.asset = std::move(*decoded);

    Expected<u32, Error> asset_index = gpu.scene.add_asset(gpu.asset);
    CY_REQUIRE(asset_index.has_value());

    // Three instances at different distances, so instance culling and the per-instance local-space
    // projection are both exercised rather than being a special case of one instance at the origin.
    Array<vg::GeometryInstance> instances(allocator);
    for (u32 index = 0; index < 3; ++index) {
        vg::GeometryInstance instance;
        instance.translation =
            Vec3{static_cast<f32>(index) * 3.0F, 0.0F, -static_cast<f32>(index) * 5.0F};
        instance.scale = 1.0F + (static_cast<f32>(index) * 0.5F);
        instance.asset = *asset_index;
        CY_REQUIRE(instances.push_back(instance).has_value());
    }
    CY_REQUIRE(gpu.scene.set_instances(instances.span()).has_value());

    vg::GpuTraversalOptions options;
    options.max_levels = 12;
    CY_REQUIRE(gpu.traversal.initialise(gpu.scene, options).has_value());

    // Every page resident: this case is about the selection test, and the streaming fallback has
    // its own. The table is uploaded the way a frame would, in one batched write.
    Array<vg::PageTableEntry> table(allocator);
    CY_REQUIRE(table.resize(gpu.asset.pages.size()).has_value());
    for (vg::PageTableEntry& entry : table) {
        entry.location = 0;
        entry.bytes = 0;
        entry.generation = 1;
        entry.flags = vg::PageFlags::kResident;
    }
    CY_REQUIRE(gpu.traversal.upload_page_table(table.span()).has_value());

    const vg::DecodedAsset* assets[1] = {&gpu.asset};
    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    inputs.instances = instances.span();

    vg::TraversalResult reference(allocator);
    vg::TraversalReadback readback(allocator);
    u32 compared = 0;
    u32 mismatches = 0;
    u32 total_clusters = 0;

    for (u32 distance_step = 0; distance_step < 6; ++distance_step) {
        for (u32 threshold_step = 0; threshold_step < 4; ++threshold_step) {
            const f32 distance = 2.0F + (static_cast<f32>(distance_step) * 6.0F);
            const f32 threshold = 0.25F * static_cast<f32>(1U << threshold_step);
            const vg::TraversalView view = view_at(distance, threshold);

            CY_REQUIRE(vg::traverse_reference(inputs, view, reference).has_value());

            RenderGraph graph(allocator);
            CY_REQUIRE(
                gpu.traversal.record(graph, view, static_cast<u32>(instances.size())).has_value());
            CY_REQUIRE(graph.status().has_value());
            Expected<ExecutionResult, Error> executed =
                gpu.executor.execute(graph, CompileOptions{}, ExecuteOptions{});
            CY_REQUIRE(executed.has_value());
            CY_REQUIRE(fixture.device().wait_idle().has_value());
            CY_REQUIRE(gpu.traversal.read_back(readback).has_value());

            CY_CHECK_FALSE(readback.overflowed);
            CY_CHECK_FALSE(readback.levels_exhausted);

            std::ranges::sort(reference.visible, visible_before);
            std::ranges::sort(readback.visible, visible_before);

            ++compared;
            total_clusters += reference.stats.visible_clusters;
            if (readback.visible.size() != reference.visible.size()) {
                ++mismatches;
                std::fprintf(stderr,
                             "distance %.2f threshold %.2f: gpu %zu clusters, reference %zu\n",
                             static_cast<double>(distance), static_cast<double>(threshold),
                             readback.visible.size(), reference.visible.size());
                continue;
            }
            bool differed = false;
            for (usize index = 0; index < reference.visible.size(); ++index) {
                const vg::VisibleCluster& want = reference.visible[index];
                const vg::VisibleCluster& got = readback.visible[index];
                if (want.instance != got.instance || want.cluster != got.cluster ||
                    want.material != got.material) {
                    differed = true;
                    std::fprintf(stderr,
                                 "distance %.2f threshold %.2f entry %zu: gpu (%u, %u, %u), "
                                 "reference (%u, %u, %u)\n",
                                 static_cast<double>(distance), static_cast<double>(threshold),
                                 index, got.instance, got.cluster, got.material, want.instance,
                                 want.cluster, want.material);
                    break;
                }
                // The screen error is computed from the same expression on both sides, so it agrees
                // to the precision of the two floating-point orders rather than exactly.
                CY_CHECK_NEAR(static_cast<f64>(got.screen_error),
                              static_cast<f64>(want.screen_error),
                              0.01 * static_cast<f64>(want.screen_error + 1.0F));
            }
            mismatches += differed ? 1U : 0U;

            // The counters the shader keeps are the same counts the reference keeps.
            CY_CHECK_EQ(readback.stats.visible_clusters, reference.stats.visible_clusters);
            CY_CHECK_EQ(readback.stats.visible_triangles, reference.stats.visible_triangles);
            CY_CHECK_EQ(readback.stats.nodes_visited, reference.stats.nodes_visited);
            CY_CHECK_EQ(readback.stats.instances_visible, reference.stats.instances_visible);
            CY_CHECK_EQ(readback.stats.missing_pages, 0U);
        }
    }
    CY_TEST_MESSAGE("compared " << compared << " views, " << total_clusters
                                << " selected clusters in total, " << mismatches << " mismatched");
    CY_CHECK_EQ(mismatches, 0U);
    CY_CHECK_GT(total_clusters, 100U);
    // A frame that renders but trips validation is not a frame that works.
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the GPU traversal falls back and asks when a page is not resident") {
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 3);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), test_options(), allocator);
    CY_REQUIRE(build.has_value());

    GpuFixture gpu(allocator, fixture.device());
    CY_REQUIRE(vg::encode_asset(*build, vg::VertexEncoding{}, gpu.bytes).has_value());
    Expected<vg::DecodedAsset, Error> decoded = vg::decode_asset(gpu.bytes.span(), allocator);
    CY_REQUIRE(decoded.has_value());
    gpu.asset = std::move(*decoded);
    CY_REQUIRE(gpu.scene.add_asset(gpu.asset).has_value());

    const vg::GeometryInstance one[1] = {vg::GeometryInstance{}};
    CY_REQUIRE(gpu.scene.set_instances(Span<const vg::GeometryInstance>(one, 1)).has_value());
    CY_REQUIRE(gpu.traversal.initialise(gpu.scene, vg::GpuTraversalOptions{}).has_value());

    // The root page only — the state a frame is in the instant an object streams in.
    Array<vg::PageTableEntry> table(allocator);
    CY_REQUIRE(table.resize(gpu.asset.pages.size()).has_value());
    for (usize index = 0; index < table.size(); ++index) {
        table[index] = vg::PageTableEntry{};
        table[index].generation = 1;
        table[index].flags =
            index == 0 ? static_cast<u8>(vg::PageFlags::kResident | vg::PageFlags::kPinned)
                       : vg::PageFlags::kInvalid;
    }
    CY_REQUIRE(gpu.traversal.upload_page_table(table.span()).has_value());

    RenderGraph graph(allocator);
    CY_REQUIRE(gpu.traversal.record(graph, view_at(1.6F, 0.25F), 1).has_value());
    Expected<ExecutionResult, Error> executed =
        gpu.executor.execute(graph, CompileOptions{}, ExecuteOptions{});
    CY_REQUIRE(executed.has_value());
    CY_REQUIRE(fixture.device().wait_idle().has_value());

    vg::TraversalReadback readback(allocator);
    CY_REQUIRE(gpu.traversal.read_back(readback).has_value());

    // THE OBJECT IS STILL THERE, drawn from pages that are, and it asked for the ones it wanted.
    CY_CHECK_GT(readback.stats.visible_clusters, 0U);
    CY_CHECK_GT(readback.stats.missing_pages, 0U);
    CY_CHECK_GT(readback.requests.size(), 0U);
    for (const vg::VisibleCluster& visible : readback.visible) {
        CY_CHECK_EQ(gpu.asset.clusters[visible.cluster].page, 0U);
    }
    for (const vg::PageRequest& request : readback.requests) {
        CY_CHECK_GT(request.priority, 0.0F);
        CY_CHECK_LT(request.page, gpu.asset.pages.size());
    }
    CY_TEST_MESSAGE("root only on the device: "
                    << readback.stats.visible_clusters << " clusters drawn, "
                    << readback.stats.missing_pages << " page misses, " << readback.requests.size()
                    << " requests appended");
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a traversal is destroyed with its frames in flight, in a loop") {
    // RULE OF THIS PROJECT SINCE M5.5's GATE, which found a Jolt job bridge destroying its free
    // list underneath a worker one run in forty: teardown is tested under load rather than at rest.
    //
    // The load here is a submitted frame. `GpuTraversal`'s destructor destroys fourteen buffers,
    // six pipelines and a descriptor set while the graph that used them is still executing, and the
    // whole of what makes that safe is `rhi-and-render-graph`'s deferred retirement — "a resource
    // destroyed during frame N is released only after frame N's fence". A destructor that waited
    // for idle would hide a defect in that mechanism; this one does not wait, and the device's
    // validation layer is what says whether the promise held.
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 2);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), test_options(), allocator);
    CY_REQUIRE(build.has_value());
    Array<u8> bytes(allocator);
    CY_REQUIRE(vg::encode_asset(*build, vg::VertexEncoding{}, bytes).has_value());
    Expected<vg::DecodedAsset, Error> asset = vg::decode_asset(bytes.span(), allocator);
    CY_REQUIRE(asset.has_value());

    Array<vg::PageTableEntry> table(allocator);
    CY_REQUIRE(table.resize(asset->pages.size()).has_value());
    for (vg::PageTableEntry& entry : table) {
        entry.generation = 1;
        entry.flags = vg::PageFlags::kResident;
    }

    GraphExecutor executor(allocator, fixture.device());
    for (u32 cycle = 0; cycle < 32; ++cycle) {
        vg::GpuScene scene(allocator);
        CY_REQUIRE(scene.add_asset(*asset).has_value());
        const vg::GeometryInstance one[1] = {vg::GeometryInstance{}};
        CY_REQUIRE(scene.set_instances(Span<const vg::GeometryInstance>(one, 1)).has_value());

        vg::GpuTraversal traversal(allocator, fixture.device());
        CY_REQUIRE(traversal.initialise(scene, vg::GpuTraversalOptions{}).has_value());
        CY_REQUIRE(traversal.upload_page_table(table.span()).has_value());

        RenderGraph graph(allocator);
        CY_REQUIRE(traversal.record(graph, view_at(4.0F, 1.0F), 1).has_value());
        ExecuteOptions options;
        // BREADCRUMBS OFF FOR THIS CASE ONLY, AND THE DEFECT IS SOMEBODY ELSE'S — the same one
        // `src/rendering/gpu_culling/tests/test_gpu_cull_pass.cpp` records at its own teardown
        // case. `GraphExecutor` resets its breadcrumb cursor per execution, so two frames in flight
        // fill the SAME slots of the device's breadcrumb buffer with no barrier between them:
        // measured here as 806 SYNC-HAZARD-WRITE-AFTER-WRITE reports, every one of them `command:
        // vkCmdFillBuffer` and not one of them naming a buffer this module owns. Every other suite
        // calls `wait_idle()` after each frame, which serialises them and hides it.
        //
        // Turning it off keeps the case testing what it is for — that destroying fourteen buffers,
        // six pipelines and a descriptor set under an in-flight submission trips nothing — rather
        // than asserting zero against a number the executor owns.
        options.breadcrumbs = false;
        Expected<ExecutionResult, Error> executed =
            executor.execute(graph, CompileOptions{}, options);
        CY_REQUIRE(executed.has_value());
        // No wait: the traversal goes out of scope here with its buffers referenced by a submitted
        // command buffer.
    }
    CY_REQUIRE(fixture.device().wait_idle().has_value());
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// ================================================================================================
// CLUSTER-GRANULAR OCCLUSION, OVER THE SAME PYRAMID THE CULLING PASS READS. M11.c task 4.1.
// ================================================================================================
//
// From M7 to M10 this module's README recorded occlusion as ABSENT and named the seam rather than
// leaving it to be read off a counter: "`TraversalStatistics::nodes_pruned_by_occlusion` and
// `rejected_by_occlusion` exist, read zero, and are the seam: the shader takes the cull tests in
// the order the requirement fixes and the HZB test slots in beside the cone test."
//
// This is that test, and what makes it evidence rather than a counter moving off zero is that both
// sides are asked the same question through the same structure: the device reads the pyramid
// `cy::rendering::hzb::HzbPass` built, and `TraversalView::occlusion` is
// `cy::render::culling::HzbOcclusionTester` over the CPU model of the same pyramid — which is the
// SAME interface `GpuCullOptions::occlusion` takes. One piece of device work, two rows.
CY_TEST_CASE(
    "cluster occlusion reads the shared hierarchical depth buffer and agrees with the "
    "reference") {
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

    constexpr u32 kWidth = 128;
    constexpr u32 kHeight = 72;
    constexpr f32 kDistance = 4.0F;

    // A FLAT OCCLUDER AT A CHOSEN DEPTH, AND THE DEPTH IS SWEPT. Under reversed Z with a near plane
    // of 0.1, an occluder at `0.1 / d` hides everything further than `d` units from the camera, so
    // sweeping `d` across the object sweeps the plane through it — and the two occlusion tests the
    // requirement names answer differently on either side of that plane.
    //
    // WHY FLAT RATHER THAN A SHAPE. The pyramid's test reads a level chosen from the rectangle's
    // own span, so a narrow occluder is diluted away by the conservative minimum before a large
    // rectangle ever reads it. That is correct and it means a SHAPED occluder makes the two tests
    // differ for a reason about the pyramid's levels rather than about what is hidden. A flat one
    // is uniform at every level, so the only thing that separates a NODE prune from a CLUSTER
    // rejection is the thing the requirement is about: a group sphere contains the cluster and
    // reaches nearer than it, so there is a band of depths where the cluster is hidden and the
    // group that contains it is not.
    Array<f32> depths(allocator);
    CY_REQUIRE(depths.resize(static_cast<usize>(kWidth) * kHeight).has_value());

    const Mat4 projection = cy::perspective_reversed_z(
        1.0471975512F, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1F, 1000.0F);
    const Mat4 look =
        cy::look_at(Vec3{0.0F, 0.0F, kDistance}, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const Mat4 view_projection = projection * look;

    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 3);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), test_options(), allocator);
    CY_REQUIRE(build.has_value());

    GpuFixture gpu(allocator, fixture.device());
    CY_REQUIRE(vg::encode_asset(*build, vg::VertexEncoding{}, gpu.bytes).has_value());
    Expected<vg::DecodedAsset, Error> decoded = vg::decode_asset(gpu.bytes.span(), allocator);
    CY_REQUIRE(decoded.has_value());
    gpu.asset = std::move(*decoded);
    Expected<u32, Error> asset_index = gpu.scene.add_asset(gpu.asset);
    CY_REQUIRE(asset_index.has_value());

    Array<vg::GeometryInstance> instances(allocator);
    vg::GeometryInstance instance;
    instance.asset = *asset_index;
    CY_REQUIRE(instances.push_back(instance).has_value());
    CY_REQUIRE(gpu.scene.set_instances(instances.span()).has_value());

    vg::GpuTraversalOptions options;
    options.max_levels = 12;
    CY_REQUIRE(gpu.traversal.initialise(gpu.scene, options).has_value());

    Array<vg::PageTableEntry> table(allocator);
    CY_REQUIRE(table.resize(gpu.asset.pages.size()).has_value());
    for (vg::PageTableEntry& entry : table) {
        entry.location = 0;
        entry.bytes = 0;
        entry.generation = 1;
        entry.flags = vg::PageFlags::kResident;
    }
    CY_REQUIRE(gpu.traversal.upload_page_table(table.span()).has_value());

    // The depth the pyramid is seeded from, as a device buffer the graph can name.
    rhi::BufferDescription depth_description;
    depth_description.name = "vg.test.depth";
    depth_description.size = depths.size() * sizeof(f32);
    depth_description.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
    depth_description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> depth_buffer =
        fixture.device().create_buffer(depth_description);
    CY_REQUIRE(depth_buffer.has_value());
    void* mapped = fixture.device().buffer_mapped_pointer(*depth_buffer);
    CY_REQUIRE(mapped != nullptr);
    std::memcpy(mapped, depths.data(), depths.size() * sizeof(f32));

    hzb::HzbPass pyramid;
    hzb::HzbPassDescription pyramid_description;
    pyramid_description.width = kWidth;
    pyramid_description.height = kHeight;
    CY_REQUIRE(pyramid.create(allocator, fixture.device(), pyramid_description).has_value());
    pyramid.mark_valid();
    CY_REQUIRE(gpu.traversal.set_occlusion(&pyramid, view_projection).has_value());

    const vg::DecodedAsset* assets[1] = {&gpu.asset};
    vg::TraversalInputs inputs;
    inputs.assets = Span<const vg::DecodedAsset* const>(assets, 1);
    inputs.instances = instances.span();

    // THE SWEEP IS TWO-DIMENSIONAL: the occluder's distance and the selection threshold.
    //
    // The threshold decides WHICH clusters are selected — a coarse one selects interior clusters,
    // whose `lod_sphere` is the group they were simplified from and is strictly larger than their
    // own bounds; a fine one selects leaves, whose `lod_sphere` IS their own sphere, so for a leaf
    // the two occlusion tests ask the same question and the node prune answers first. The occluder
    // distance decides WHAT is hidden. Both are swept because either alone leaves one of the two
    // tests unexercised, and a test that cannot detect the deletion of half its subject is exactly
    // the shape this project has found seven times.
    vg::TraversalResult reference(allocator);
    vg::TraversalReadback readback(allocator);
    u32 total_pruned = 0;
    u32 total_rejected = 0;
    u32 total_visible = 0;
    u32 mismatches = 0;
    u32 views = 0;

    const f32 occluder_distances[6] = {3.2F, 3.6F, 4.0F, 4.4F, 4.8F, 5.2F};
    for (const f32 occluder : occluder_distances) {
        const f32 depth = 0.1F / occluder;
        for (f32& texel : depths) {
            texel = depth;
        }
        std::memcpy(mapped, depths.data(), depths.size() * sizeof(f32));

        render::culling::Hzb model(allocator);
        CY_REQUIRE(model.resize(kWidth, kHeight).has_value());
        {
            const Span<f32> level0 = model.level(0);
            CY_REQUIRE(level0.size() == depths.size());
            for (usize index = 0; index < depths.size(); ++index) {
                level0[index] = depths[index];
            }
        }
        model.reduce();
        model.mark_valid();
        const render::culling::HzbOcclusionTester tester(model, view_projection);

        for (u32 step = 0; step < 4; ++step) {
            const f32 threshold = 0.25F * static_cast<f32>(1U << (step * 2U));
            vg::TraversalView view = view_at(kDistance, threshold);
            view.occlusion = &tester;

            CY_REQUIRE(vg::traverse_reference(inputs, view, reference).has_value());

            RenderGraph graph(allocator);
            BufferRequest depth_request;
            depth_request.name = "vg.test.depth";
            depth_request.size = depths.size() * sizeof(f32);
            depth_request.extra_usage = rhi::BufferUsage::TransferSource;
            const ResourceId depth_resource = graph.import_buffer(depth_request, *depth_buffer);
            Expected<ResourceId, Error> pyramid_resource = pyramid.declare(graph, depth_resource);
            CY_REQUIRE(pyramid_resource.has_value());
            CY_REQUIRE(
                gpu.traversal
                    .record(graph, view, static_cast<u32>(instances.size()), *pyramid_resource)
                    .has_value());
            CY_REQUIRE(graph.status().has_value());
            Expected<ExecutionResult, Error> executed =
                gpu.executor.execute(graph, CompileOptions{}, ExecuteOptions{});
            CY_REQUIRE(executed.has_value());
            CY_REQUIRE(fixture.device().wait_idle().has_value());
            CY_REQUIRE(gpu.traversal.read_back(readback).has_value());

            ++views;
            total_pruned += reference.stats.nodes_pruned_by_occlusion;
            total_rejected += reference.stats.rejected_by_occlusion;
            total_visible += reference.stats.visible_clusters;

            std::ranges::sort(reference.visible, visible_before);
            std::ranges::sort(readback.visible, visible_before);
            if (readback.stats.nodes_pruned_by_occlusion !=
                    reference.stats.nodes_pruned_by_occlusion ||
                readback.stats.rejected_by_occlusion != reference.stats.rejected_by_occlusion ||
                readback.stats.visible_clusters != reference.stats.visible_clusters ||
                readback.visible.size() != reference.visible.size()) {
                ++mismatches;
                std::fprintf(stderr,
                             "occluder %.2f threshold %.3f: reference pruned %u rejected %u "
                             "visible %u; device pruned %u rejected %u visible %u\n",
                             static_cast<double>(occluder), static_cast<double>(threshold),
                             reference.stats.nodes_pruned_by_occlusion,
                             reference.stats.rejected_by_occlusion,
                             reference.stats.visible_clusters,
                             readback.stats.nodes_pruned_by_occlusion,
                             readback.stats.rejected_by_occlusion, readback.stats.visible_clusters);
                continue;
            }
            for (usize index = 0; index < reference.visible.size(); ++index) {
                if (readback.visible[index].instance != reference.visible[index].instance ||
                    readback.visible[index].cluster != reference.visible[index].cluster) {
                    ++mismatches;
                    break;
                }
            }
        }
    }

    CY_TEST_MESSAGE("occlusion over " << views << " views: " << total_pruned << " node(s) pruned, "
                                      << total_rejected << " cluster(s) rejected, " << total_visible
                                      << " left visible, " << mismatches << " view(s) mismatched");

    // NOT VACUOUS, AND IN THREE DIRECTIONS: BOTH occlusion tests fired somewhere in the sweep, and
    // something survived. An agreement between two answers of zero is not evidence of anything.
    CY_CHECK_GT(total_pruned, 0U);
    CY_CHECK_GT(total_rejected, 0U);
    CY_CHECK_GT(total_visible, 0U);
    CY_CHECK_EQ(mismatches, 0U);

    // AND THE PYRAMID IS THE ONE THE CULLING PASS WOULD READ: the same object, the same buffer, the
    // same `hzb_sample.slang`. `integration.rendering_culling` compares that pyramid against the
    // CPU model texel for texel; this case compares what a second consumer does with it.
    CY_CHECK_EQ(pyramid.level_count(), render::culling::hzb_level_count(kWidth, kHeight));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);

    pyramid.destroy();
    fixture.device().destroy_buffer(*depth_buffer);
}
