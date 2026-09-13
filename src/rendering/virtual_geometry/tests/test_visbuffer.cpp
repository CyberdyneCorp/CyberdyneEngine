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
        // THE IDENTITY IS DECODED, not looked up: a pixel carries `instance * stride + cluster`,
        // and this scene has one instance, so the identity IS the cluster index.
        const vg::SurfaceIdentity identity =
            vg::split_surface_identity(sample.surface, scene.cluster_stride());
        CY_CHECK_EQ(identity.instance, 0U);
        CY_REQUIRE(identity.cluster < asset.clusters.size());
        CY_CHECK_LT(sample.triangle, asset.clusters[identity.cluster].index_count / 3U);
    }

    // --- 2. The bins are the bins ----------------------------------------------------------------
    vg::MaterialBins reference(allocator);
    CY_REQUIRE(vg::bin_by_material(readback.samples.span(), traversal_readback.visible.span(),
                                   scene.cluster_stride(), visbuffer_options.material_count,
                                   reference)
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
            const vg::SurfaceIdentity identity =
                vg::split_surface_identity(sample.surface, scene.cluster_stride());
            if (asset.clusters[identity.cluster].material + instance.material_offset == material) {
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
        const vg::SurfaceIdentity identity =
            vg::split_surface_identity(sample.surface, scene.cluster_stride());
        vg::VisibleCluster record;
        record.instance = identity.instance;
        record.cluster = identity.cluster;
        record.material = asset.clusters[identity.cluster].material + instance.material_offset;
        Expected<vg::SurfaceAttributes, Error> surface =
            vg::reconstruct_surface(asset, instance, record, sample.triangle, world_to_clip, centre,
                                    kSide, kSide, allocator);
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

// ================================================================================================
// REGRESSION — the depth test and the payload write are one atomic
// ================================================================================================
//
// `vgVisRaster` used to settle depth with an `InterlockedMin` and then store `visbuffer[pixel]` as
// a SEPARATE, unordered write. The pair was not atomic, so two fragments at DIFFERENT depths both
// took the write branch whenever the farther one ran its atomic first, and whichever store retired
// last owned the pixel — sometimes the farther surface, leaving `depth` and `visbuffer`
// disagreeing.
//
// It was found by rendering samples/07-fidelity for documentation: 110-150 of 921,593 covered
// pixels changed between identical runs and `materials_seen` flipped between 4 and 5, while
// coverage and the visible-cluster count stayed bit-stable — which is what localised it to the
// pairing rather than to the traversal or the raster bounds.
//
// WHAT THIS ASSERTS, and what it deliberately does not. Depth and payload are one 64-bit atomic
// now, so a fragment at a GREATER depth can no longer take a pixel from a nearer one — that is the
// defect, and in this scene it makes the resolved buffer and the bin counts identical every run.
//
// It is not, on its own, a claim of total determinism: this scene contains no EXACT depth ties, so
// it says nothing about how one is broken. That residue — one to three pixels in 921,593 on the
// artefact's set — is the next test's subject, and closing it is what turned the payload's first
// word from a visible-list index into a stable surface identity.
CY_TEST_CASE("the visibility buffer resolves identically across runs") {
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

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
    // DEPTH CONTENTION IS THE WHOLE POINT. A single convex sphere never puts two fragments at
    // different depths on one pixel, so the unpaired store had nothing to lose and this test passed
    // against the defect it was written for. These instances are stacked along the view axis and
    // overlap in screen space, so most covered pixels are contested several times over — which is
    // the condition the race needs and the artefact's fluted, self-occluding set produced by
    // accident.
    constexpr u32 kInstances = 8;
    Array<vg::GeometryInstance> stack(allocator);
    for (u32 i = 0; i < kInstances; ++i) {
        vg::GeometryInstance placed;
        placed.translation = Vec3{static_cast<f32>(i) * 0.05F, static_cast<f32>(i) * 0.03F,
                                  static_cast<f32>(i) * -0.25F};
        placed.scale = 1.0F - (static_cast<f32>(i) * 0.02F);
        placed.material_offset = i % 3U;
        CY_REQUIRE(stack.push_back(placed).has_value());
    }
    CY_REQUIRE(scene.set_instances(stack.span()).has_value());

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

    const Vec3 camera{0.0F, 0.0F, 4.0F};
    const Mat4 view = look_at(camera, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const Mat4 world_to_clip = perspective_reversed_z_infinite(1.0471975512F, 1.0F, 0.1F) * view;

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

    // Six runs, because the defect was a race: one comparison can agree by luck. When this test was
    // written against the unpaired store it failed on the first or second comparison every time.
    constexpr u32 kRuns = 6;
    Array<Vec4> first_resolved(allocator);
    Array<u32> first_bins(allocator);
    u32 first_covered = 0;
    for (u32 run = 0; run < kRuns; ++run) {
        GraphExecutor executor(allocator, fixture.device());
        RenderGraph graph(allocator);
        CY_REQUIRE(traversal.record(graph, traversal_view, kInstances).has_value());
        CY_REQUIRE(visbuffer.record(graph, traversal, world_to_clip).has_value());
        CY_REQUIRE(graph.status().has_value());
        CY_REQUIRE(executor.execute(graph, CompileOptions{}, ExecuteOptions{}).has_value());
        CY_REQUIRE(fixture.device().wait_idle().has_value());

        vg::VisbufferReadback readback(allocator);
        CY_REQUIRE(visbuffer.read_back(readback).has_value());

        if (run == 0) {
            for (const Vec4& value : readback.resolved) {
                CY_REQUIRE(first_resolved.push_back(value).has_value());
            }
            for (const u32 count : readback.bin_counts) {
                CY_REQUIRE(first_bins.push_back(count).has_value());
            }
            first_covered = readback.covered_pixels();
            CY_CHECK_GT(first_covered, 400U);
            continue;
        }

        // THE COVERAGE AND THE BINS. `materials_seen` flipping 4 to 5 in the artefact was a bin
        // whose last pixels were the contested ones, so the bin counts are where that shows up.
        CY_CHECK_EQ(readback.covered_pixels(), first_covered);
        CY_REQUIRE_EQ(readback.bin_counts.size(), first_bins.size());
        u32 bins_differing = 0;
        for (usize i = 0; i < first_bins.size(); ++i) {
            if (readback.bin_counts[i] != first_bins[i]) {
                ++bins_differing;
            }
        }
        CY_CHECK_EQ(bins_differing, 0U);

        // THE RESOLVE. A farther fragment winning a pixel changed the attribute resolved there, so
        // an exact comparison of the resolved buffer is the sharpest statement of the fix.
        CY_REQUIRE_EQ(readback.resolved.size(), first_resolved.size());
        u32 differing = 0;
        for (usize i = 0; i < first_resolved.size(); ++i) {
            const Vec4& a = first_resolved[i];
            const Vec4& b = readback.resolved[i];
            if (a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) {
                ++differing;
            }
        }
        CY_TEST_MESSAGE("run " << run << ": " << differing
                               << " resolved pixel(s) differ from run 0");
        CY_CHECK_EQ(differing, 0U);
    }

    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// ================================================================================================
// REGRESSION — an EXACT depth tie is broken by a stable identity, not by append order
// ================================================================================================
//
// M10 task 7b.3. The test above closed the case where a fragment at a GREATER depth could take a
// pixel from a nearer one. It deliberately left the residue: where two fragments tie on the depth
// key EXACTLY, the 64-bit minimum is decided by the payload alone, and the payload used to carry
// the traversal's visible-list index — the slot an `InterlockedAdd` handed out, which permutes
// between runs of the same frame. One to three pixels of 921,593 moved between identical runs of
// samples/07-fidelity because of it, and colouring by that index repainted the whole image.
//
// THE SCENE IS BUILT TO TIE. Four pairs of instances, each pair at an IDENTICAL transform, so every
// fragment of a pair's second member lands on the same pixel at the same depth key as its twin's —
// bit-exact, because it is the same arithmetic over the same numbers. The pairs are stacked along
// the view axis so they also contend with each other, which is what the test above needs.
//
// THREE ASSERTIONS, and the first is the one that could not be made before:
//
//   1. `samples` — the visibility buffer itself — is BIT-IDENTICAL across runs. It could not have
//      been while the first word was a visible-list index: that number names the same surface
//      differently on every run.
//   2. The twin with the LOWER identity wins every contested pixel, so the bins belonging to the
//      higher twin are empty. Which one wins is arbitrary; that the same one always wins is not.
//   3. The resolve and the bin counts are identical run to run, as above.
CY_TEST_CASE("an exact depth tie is broken the same way on every run") {
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

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

    // Pair `2k` and `2k + 1` share a transform exactly. The odd member carries a material offset of
    // two, so the asset's materials 0 and 1 become bins 2 and 3 for it — which makes "the odd twin
    // never wins a pixel" a statement about two bin counts rather than about a pixel's provenance.
    constexpr u32 kPairs = 4;
    constexpr u32 kInstances = kPairs * 2U;
    Array<vg::GeometryInstance> twins(allocator);
    for (u32 pair = 0; pair < kPairs; ++pair) {
        for (u32 member = 0; member < 2U; ++member) {
            vg::GeometryInstance placed;
            placed.translation = Vec3{0.0F, 0.0F, static_cast<f32>(pair) * -0.30F};
            placed.scale = 1.0F - (static_cast<f32>(pair) * 0.04F);
            placed.material_offset = member * 2U;
            CY_REQUIRE(twins.push_back(placed).has_value());
        }
    }
    CY_REQUIRE(scene.set_instances(twins.span()).has_value());

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

    const Vec3 camera{0.0F, 0.0F, 4.0F};
    const Mat4 view = look_at(camera, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const Mat4 world_to_clip = perspective_reversed_z_infinite(1.0471975512F, 1.0F, 0.1F) * view;

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

    constexpr u32 kRuns = 6;
    Array<vg::VisibilitySample> first_samples(allocator);
    Array<Vec4> first_resolved(allocator);
    Array<u32> first_bins(allocator);
    const u32 stride = scene.cluster_stride();
    for (u32 run = 0; run < kRuns; ++run) {
        GraphExecutor executor(allocator, fixture.device());
        RenderGraph graph(allocator);
        CY_REQUIRE(traversal.record(graph, traversal_view, kInstances).has_value());
        CY_REQUIRE(visbuffer.record(graph, traversal, world_to_clip).has_value());
        CY_REQUIRE(graph.status().has_value());
        CY_REQUIRE(executor.execute(graph, CompileOptions{}, ExecuteOptions{}).has_value());
        CY_REQUIRE(fixture.device().wait_idle().has_value());

        vg::VisbufferReadback readback(allocator);
        CY_REQUIRE(visbuffer.read_back(readback).has_value());

        // 2. THE TIE GOES TO THE LOWER IDENTITY, which is the even member of each pair. The odd
        //    twin's materials are bins 2 and 3, so it winning anywhere is two non-zero counts.
        u32 odd_twin_pixels = 0;
        for (const vg::VisibilitySample& sample : readback.samples) {
            if (!sample.covered()) {
                continue;
            }
            if ((vg::split_surface_identity(sample.surface, stride).instance % 2U) != 0U) {
                ++odd_twin_pixels;
            }
        }
        CY_TEST_MESSAGE("run " << run << ": " << odd_twin_pixels
                               << " pixel(s) won by the higher-identity twin");
        CY_CHECK_EQ(odd_twin_pixels, 0U);
        CY_CHECK_EQ(readback.bin_counts[2], 0U);
        CY_CHECK_EQ(readback.bin_counts[3], 0U);
        CY_CHECK_GT(readback.bin_counts[0], 0U);
        CY_CHECK_GT(readback.bin_counts[1], 0U);

        if (run == 0) {
            for (const vg::VisibilitySample& sample : readback.samples) {
                CY_REQUIRE(first_samples.push_back(sample).has_value());
            }
            for (const Vec4& value : readback.resolved) {
                CY_REQUIRE(first_resolved.push_back(value).has_value());
            }
            for (const u32 count : readback.bin_counts) {
                CY_REQUIRE(first_bins.push_back(count).has_value());
            }
            CY_CHECK_GT(readback.covered_pixels(), 400U);
            continue;
        }

        // 1. THE VISIBILITY BUFFER ITSELF. Both words of every pixel, exactly.
        CY_REQUIRE_EQ(readback.samples.size(), first_samples.size());
        u32 samples_differing = 0;
        for (usize i = 0; i < first_samples.size(); ++i) {
            if (readback.samples[i].surface != first_samples[i].surface ||
                readback.samples[i].triangle != first_samples[i].triangle) {
                ++samples_differing;
            }
        }
        CY_TEST_MESSAGE("run " << run << ": " << samples_differing
                               << " visibility sample(s) differ from run 0");
        CY_CHECK_EQ(samples_differing, 0U);

        // 3. And what the resolve and the classification made of it.
        CY_REQUIRE_EQ(readback.bin_counts.size(), first_bins.size());
        for (usize i = 0; i < first_bins.size(); ++i) {
            CY_CHECK_EQ(readback.bin_counts[i], first_bins[i]);
        }
        CY_REQUIRE_EQ(readback.resolved.size(), first_resolved.size());
        u32 differing = 0;
        for (usize i = 0; i < first_resolved.size(); ++i) {
            const Vec4& a = first_resolved[i];
            const Vec4& b = readback.resolved[i];
            if (a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) {
                ++differing;
            }
        }
        CY_CHECK_EQ(differing, 0U);
    }

    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// ================================================================================================
// THE IDENTITY'S BOUND IS REFUSED, NOT WRAPPED
// ================================================================================================
//
// The surface identity has the 24 bits the triangle leaves of the 32-bit visibility payload, so a
// scene with more than `kMaxSurfaceIdentity` (instance, cluster) pairs cannot be named by it. The
// traversal's own DAG marks are one word per pair and it refuses only above 2^26, so this bound is
// the tighter of the two and nothing else in the tree would catch a scene between them.
//
// A wrapped identity is a pixel silently resolved against a NEIGHBOURING cluster — the class of
// defect the identity was introduced to remove — so it is a refusal with a message rather than a
// number that happens to be wrong.
CY_TEST_CASE("a scene too large for the surface identity is refused at initialise") {
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();

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
    const u32 stride = scene.cluster_stride();
    CY_REQUIRE(stride > 0U);

    // One instance past what the packing holds. The instances are never drawn — `initialise`
    // refuses before it allocates anything — so this costs one array and no device memory.
    const u32 instances = (vg::kMaxSurfaceIdentity / stride) + 1U;
    Array<vg::GeometryInstance> many(allocator);
    CY_REQUIRE(many.resize(instances).has_value());

    CY_REQUIRE(scene.set_instances(many.span()).has_value());
    CY_TEST_MESSAGE("cluster stride " << stride << ", " << instances << " instances, "
                                      << (static_cast<u64>(instances) * stride)
                                      << " identities against a limit of "
                                      << vg::kMaxSurfaceIdentity);

    vg::VisbufferOptions visbuffer_options;
    visbuffer_options.width = kSide;
    visbuffer_options.height = kSide;
    vg::VisbufferPass visbuffer(allocator, fixture.device());
    const vg::DecodedAsset* assets[1] = {&asset};
    const u32 payload_offsets[1] = {0};
    const Status refused =
        visbuffer.initialise(scene, Span<const vg::DecodedAsset* const>(assets, 1), asset.payload,
                             Span<const u32>(payload_offsets, 1), visbuffer_options);
    CY_REQUIRE(!refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);

    // One fewer instance is inside the bound and is accepted, so the refusal is a boundary rather
    // than a blanket.
    vg::GpuScene fits(allocator);
    CY_REQUIRE(fits.add_asset(asset).has_value());
    CY_REQUIRE(many.resize(instances - 1U).has_value());
    CY_REQUIRE(fits.set_instances(many.span()).has_value());
    vg::VisbufferPass accepted(allocator, fixture.device());
    CY_REQUIRE(accepted
                   .initialise(fits, Span<const vg::DecodedAsset* const>(assets, 1), asset.payload,
                               Span<const u32>(payload_offsets, 1), visbuffer_options)
                   .has_value());

    // AND THE REFUSED PASS IS DESTROYED CLEANLY. This is the first test in the tree that destroys a
    // `VisbufferPass` whose `initialise` refused, and it reported twelve
    // "destroy_buffer() on a stale or never-issued handle" the first time it ran — the destructor
    // handed the device twelve null handles because nothing had created any. The destructors of
    // both `VisbufferPass` and `GpuTraversal` now skip a null handle, and this count is the check.
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
