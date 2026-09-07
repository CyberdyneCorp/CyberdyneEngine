// The visibility buffer and its material resolve, on a real device. M7 task 7.4.
//
// Three claims, each asserted against something other than a picture:
//
//   1. THE BUFFER IDENTIFIES A SURFACE. Every covered pixel names a visible-cluster record and a
//      triangle within it, and every one of those is in range — so the two words are enough to find
//      the geometry, which is what "instance identifier, primitive identifier" has to mean.
//
//   2. THE BINS ARE THE BINS. The device's classification, prefix sum and scatter are compared
//      against `bin_by_material()` over the same visibility buffer: the same counts, the same
//      offsets, and the same SET of pixels per bin. The order within a bin is the order the atomics
//      ran and the comparison does not pretend otherwise.
//
//   3. THE ATTRIBUTES ARE RECONSTRUCTED, NOT INTERPOLATED. The resolve's normal is compared against
//      `reconstruct_surface()`, which projects the identified triangle and solves for the pixel
//      centre from the cooked asset — a completely separate path through the same geometry.

#include <cy/test/test.h>

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/virtual_geometry/visbuffer.h>

#include "meshes.h"

#include <cstdio>

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

class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_virtual_geometry_visbuffer";
        description.enable_validation = true;
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

constexpr u32 kSide = 128;

vg::BuildOptions test_options() noexcept {
    vg::BuildOptions options;
    options.policy.min_triangles = 8;
    options.policy.target_triangles = 32;
    options.policy.max_triangles = 32;
    options.policy.max_vertices = 64;
    options.policy.group_size = 4;
    options.page_bytes = 4096;
    options.resident_budget_bytes = 4096;
    return options;
}

}  // namespace

CY_TEST_CASE(
    "the visibility buffer identifies surfaces, bins them, and resolves their attributes") {
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

    // A two-material sphere, so the bins are bins rather than one bin.
    const vg::test::MeshData mesh = vg::test::two_material_sphere(allocator, 3);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), test_options(), allocator);
    CY_REQUIRE(build.has_value());

    Array<u8> bytes(allocator);
    CY_REQUIRE(vg::encode_asset(*build, vg::VertexEncoding{}, bytes).has_value());
    Expected<vg::DecodedAsset, Error> decoded = vg::decode_asset(bytes.span(), allocator);
    CY_REQUIRE(decoded.has_value());
    const vg::DecodedAsset& asset = *decoded;

    vg::GpuScene scene(allocator);
    CY_REQUIRE(scene.add_asset(asset).has_value());
    vg::GeometryInstance instance;
    const vg::GeometryInstance one[1] = {instance};
    CY_REQUIRE(scene.set_instances(Span<const vg::GeometryInstance>(one, 1)).has_value());

    vg::GpuTraversal traversal(allocator, fixture.device());
    CY_REQUIRE(traversal.initialise(scene, vg::GpuTraversalOptions{}).has_value());
    Array<vg::PageTableEntry> table(allocator);
    CY_REQUIRE(table.resize(asset.pages.size()).has_value());
    for (vg::PageTableEntry& entry : table) {
        entry.generation = 1;
        entry.flags = vg::PageFlags::kResident;
    }
    CY_REQUIRE(traversal.upload_page_table(table.span()).has_value());

    vg::VisbufferOptions visbuffer_options;
    visbuffer_options.width = kSide;
    visbuffer_options.height = kSide;
    visbuffer_options.material_count = 4;
    vg::VisbufferPass visbuffer(allocator, fixture.device());
    const vg::DecodedAsset* assets[1] = {&asset};
    const u32 payload_offsets[1] = {0};
    CY_REQUIRE(visbuffer
                   .initialise(scene, Span<const vg::DecodedAsset* const>(assets, 1), asset.payload,
                               Span<const u32>(payload_offsets, 1), visbuffer_options)
                   .has_value());

    // A camera four units back looking down −Z, with the engine's own reversed-Z projection so that
    // the compute rasteriser's depth test is the renderer's convention and not this file's.
    const Vec3 camera{0.0F, 0.0F, 4.0F};
    const Mat4 view = look_at(camera, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const Mat4 projection = perspective_reversed_z_infinite(1.0471975512F, 1.0F, 0.1F);
    const Mat4 world_to_clip = projection * view;

    vg::TraversalView traversal_view;
    traversal_view.projection.camera_position = camera;
    traversal_view.projection.viewport_height = static_cast<f32>(kSide);
    traversal_view.projection.fov_y_radians = 1.0471975512F;
    traversal_view.threshold_pixels = 1.0F;
    traversal_view.minimum_instance_pixels = 0.0F;
    traversal_view.cone_culling = true;
    for (Plane& plane : traversal_view.frustum.planes) {
        plane = Plane{Vec3{0.0F, 0.0F, 1.0F}, 1.0e9F};
    }
    traversal_view.frustum.refresh_corner_masks();

    GraphExecutor executor(allocator, fixture.device());
    RenderGraph graph(allocator);
    CY_REQUIRE(traversal.record(graph, traversal_view, 1).has_value());
    CY_REQUIRE(visbuffer.record(graph, traversal, world_to_clip).has_value());
    CY_REQUIRE(graph.status().has_value());
    Expected<ExecutionResult, Error> executed =
        executor.execute(graph, CompileOptions{}, ExecuteOptions{});
    CY_REQUIRE(executed.has_value());
    CY_REQUIRE(fixture.device().wait_idle().has_value());

    vg::TraversalReadback traversal_readback(allocator);
    CY_REQUIRE(traversal.read_back(traversal_readback).has_value());
    CY_REQUIRE(traversal_readback.visible.size() > 0U);

    vg::VisbufferReadback readback(allocator);
    CY_REQUIRE(visbuffer.read_back(readback).has_value());
    CY_REQUIRE_EQ(readback.samples.size(), static_cast<usize>(kSide) * kSide);

    // --- 1. Every covered pixel identifies a surface -------------------------------------------
    const u32 covered = readback.covered_pixels();
    CY_TEST_MESSAGE("visible clusters " << traversal_readback.visible.size() << ", covered pixels "
                                        << covered << " of " << (kSide * kSide));
    // A unit sphere at four units through a 60-degree field of view covers about a tenth of the
    // frame; anything above a few hundred pixels means the rasteriser ran and hit the object.
    CY_CHECK_GT(covered, 400U);
    CY_CHECK_LT(covered, static_cast<u32>(kSide) * kSide);
    for (const vg::VisibilitySample& sample : readback.samples) {
        if (!sample.covered()) {
            continue;
        }
        CY_REQUIRE(sample.visible < traversal_readback.visible.size());
        const vg::VisibleCluster& record = traversal_readback.visible[sample.visible];
        CY_REQUIRE(record.cluster < asset.clusters.size());
        CY_CHECK_LT(sample.triangle, asset.clusters[record.cluster].index_count / 3U);
    }

    // --- 2. The bins are the bins ----------------------------------------------------------------
    vg::MaterialBins reference(allocator);
    CY_REQUIRE(vg::bin_by_material(readback.samples.span(), traversal_readback.visible.span(),
                                   visbuffer_options.material_count, reference)
                   .has_value());
    for (u32 material = 0; material < visbuffer_options.material_count; ++material) {
        CY_CHECK_EQ(readback.bin_counts[material], reference.counts[material]);
        CY_CHECK_EQ(readback.bin_offsets[material], reference.offsets[material]);
    }
    CY_CHECK_EQ(readback.bin_offsets[visbuffer_options.material_count],
                reference.offsets[visbuffer_options.material_count]);
    CY_CHECK_EQ(readback.bin_offsets[visbuffer_options.material_count], covered);
    // Two materials, both present: a single bin would make the whole classification vacuous.
    CY_CHECK_GT(reference.counts[0], 0U);
    CY_CHECK_GT(reference.counts[1], 0U);

    // The pixels of each bin, as sets. The GPU's order within a bin is the order its atomics ran.
    for (u32 material = 0; material < visbuffer_options.material_count; ++material) {
        const u32 first = reference.offsets[material];
        const u32 last = reference.offsets[material + 1];
        u32 matched = 0;
        for (u32 slot = first; slot < last; ++slot) {
            const u32 pixel = readback.bin_pixels[slot];
            CY_REQUIRE(pixel < readback.samples.size());
            const vg::VisibilitySample& sample = readback.samples[pixel];
            CY_REQUIRE(sample.covered());
            if (traversal_readback.visible[sample.visible].material == material) {
                ++matched;
            }
        }
        CY_CHECK_EQ(matched, last - first);
    }

    // --- 3. The attributes are reconstructed -----------------------------------------------------
    u32 compared = 0;
    u32 disagreed = 0;
    f32 worst = 0.0F;
    for (u32 pixel = 0; pixel < readback.samples.size(); ++pixel) {
        const vg::VisibilitySample& sample = readback.samples[pixel];
        if (!sample.covered()) {
            // An uncovered pixel must have been left alone by the resolve.
            CY_CHECK_EQ(readback.resolved[pixel].w, 0.0F);
            continue;
        }
        // The row and the column, taken as integers before either becomes a float: writing
        // `static_cast<f32>(pixel / kSide)` would divide in floating point and shift every pixel of
        // the frame by a fraction of a row.
        const u32 column = pixel % kSide;
        const u32 row = pixel / kSide;
        const Vec2 centre{static_cast<f32>(column) + 0.5F, static_cast<f32>(row) + 0.5F};
        Expected<vg::SurfaceAttributes, Error> surface = vg::reconstruct_surface(
            asset, instance, traversal_readback.visible[sample.visible], sample.triangle,
            world_to_clip, centre, kSide, kSide, allocator);
        CY_REQUIRE(surface.has_value());

        const Vec4 got = readback.resolved[pixel];
        const Vec3 gpu_normal{(got.x * 2.0F) - 1.0F, (got.y * 2.0F) - 1.0F, (got.z * 2.0F) - 1.0F};
        const f32 agreement = dot(gpu_normal, surface->normal);
        worst = agreement < worst || compared == 0 ? agreement : worst;
        // The quantised normals go through the same octahedral decode on both sides, so the two
        // agree to the precision of two different orders of the same arithmetic.
        // Spelled as the negation, because `NaN < 0.99` is false and a NaN normal would otherwise
        // count as agreement — which is exactly the state the resolve was in while its blended
        // normal was denormal garbage.
        if (!(agreement >= 0.99F)) {
            ++disagreed;
        }
        ++compared;
    }
    CY_TEST_MESSAGE("reconstructed " << compared << " pixels against the CPU reference, "
                                     << disagreed << " disagreeing; worst agreement " << worst);
    CY_CHECK_GT(compared, 400U);
    CY_CHECK_EQ(disagreed, 0U);

    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
