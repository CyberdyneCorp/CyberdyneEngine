// SPDX-License-Identifier: MIT
// Virtual geometry in the forward frame's pass order, rasterised in hardware. M11.c task 4.3.
//
// Before this suite the only thing that ever drew a cluster was a harness — `VisbufferPass::record`
// in a graph of its own — and the rasteriser that ran was the compute one. The claim here is that
// the ENGINE'S frame now draws them: `ForwardFrame` declares a `virtual geometry` stage, a vertex
// and a fragment shader draw the visible records in it, and the visibility buffer that comes out
// is the one the compute rasteriser writes. Three things are asserted, none of them a picture:
//
//   1. THE SAME VISIBILITY BUFFER. One traversal, both rasterisers, compared pixel for pixel. The
//      identities agree, the bins agree with `bin_by_material` over the hardware buffer, and the
//      resolved normals agree with `reconstruct_surface` — which is what catches a buffer that is
//      the right shape and upside down.
//   2. THE STAGE IS IN THE FRAME, NOT BESIDE IT. It tests against the frame's OWN depth: a depth
//      prepass that writes a plane through the sphere hides exactly the part of the sphere behind
//      it. A stage that cleared its own depth, or drew into a target of its own, passes case 1 and
//      fails this one.
//   3. THE REFUSALS. A traversal recorded into a different graph, and a viewport the visibility
//   pass
//      was not sized for, are refused by name.

#include <cy/test/test.h>

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/virtual_geometry/forward_visibility.h>
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

/// The device `render.virtual_geometry_gpu` uses: validation AND synchronisation validation on,
/// because the whole of case 1 is a claim that the graph put the right barrier between a compute
/// dispatch and a vertex shader, and only synchronisation validation can see that barrier missing.
class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_virtual_geometry_forward";
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
constexpr f32 kFovY = 1.0471975512F;
const Vec3 kCamera{0.0F, 0.0F, 4.0F};

// The centre of a row-major pixel of the kSide x kSide target.
Vec2 pixel_centre(u32 pixel) noexcept {
    const u32 column = pixel % kSide;
    const u32 row = pixel / kSide;
    return Vec2{static_cast<f32>(column) + 0.5F, static_cast<f32>(row) + 0.5F};
}

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

Mat4 world_to_clip() noexcept {
    const Mat4 view = look_at(kCamera, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
    return perspective_reversed_z_infinite(kFovY, 1.0F, 0.1F) * view;
}

vg::TraversalView traversal_view() noexcept {
    vg::TraversalView view;
    view.projection.camera_position = kCamera;
    view.projection.viewport_height = static_cast<f32>(kSide);
    view.projection.fov_y_radians = kFovY;
    view.threshold_pixels = 1.0F;
    view.minimum_instance_pixels = 0.0F;
    view.cone_culling = true;
    for (Plane& plane : view.frustum.planes) {
        plane = Plane{Vec3{0.0F, 0.0F, 1.0F}, 1.0e9F};
    }
    view.frustum.refresh_corner_masks();
    return view;
}

/// The frame the stage is put in: nothing switched on but what a depth test needs. Every other
/// stage is declared with no callback, which `ForwardFrame` calls a legitimate frame.
FrameDescription frame_description(u32 width, u32 height) noexcept {
    FrameDescription description;
    description.width = width;
    description.height = height;
    description.features.sky = false;
    description.features.transparency = false;
    description.features.ui = false;
    return description;
}

/// A two-material sphere, cooked, decoded, on the device, with the traversal and the visibility
/// pass initialised over it — the scene `render.virtual_geometry_gpu` draws.
struct Scene {
    explicit Scene(DeviceFixture& fixture) noexcept
        : allocator(fixture.allocator()),
          mesh(vg::test::two_material_sphere(allocator, 3)),
          bytes(allocator),
          gpu(allocator),
          table(allocator),
          traversal(allocator, fixture.device()),
          visbuffer(allocator, fixture.device()) {}

    [[nodiscard]] bool initialise() noexcept {
        Expected<vg::GeometryBuild, Error> build =
            vg::build_geometry(mesh.source(), test_options(), allocator);
        if (!build || !vg::encode_asset(*build, vg::VertexEncoding{}, bytes)) {
            return false;
        }
        asset = vg::decode_asset(bytes.span(), allocator);
        if (!asset) {
            return false;
        }
        const vg::GeometryInstance one[1] = {instance};
        if (!gpu.add_asset(*asset) ||
            !gpu.set_instances(Span<const vg::GeometryInstance>(one, 1)) ||
            !traversal.initialise(gpu, vg::GpuTraversalOptions{}) ||
            !table.resize(asset->pages.size())) {
            return false;
        }
        for (vg::PageTableEntry& entry : table) {
            entry.generation = 1;
            entry.flags = vg::PageFlags::kResident;
        }
        if (!traversal.upload_page_table(table.span())) {
            return false;
        }
        vg::VisbufferOptions options;
        options.width = kSide;
        options.height = kSide;
        options.material_count = 4;
        const vg::DecodedAsset* assets[1] = {&*asset};
        const u32 payload_offsets[1] = {0};
        return visbuffer
            .initialise(gpu, Span<const vg::DecodedAsset* const>(assets, 1), asset->payload,
                        Span<const u32>(payload_offsets, 1), options)
            .has_value();
    }

    Allocator& allocator;
    vg::test::MeshData mesh;
    Array<u8> bytes;
    Expected<vg::DecodedAsset, Error> asset = fail(ErrorCode::Unavailable, "not decoded");
    vg::GpuScene gpu;
    Array<vg::PageTableEntry> table;
    vg::GeometryInstance instance;
    vg::GpuTraversal traversal;
    vg::VisbufferPass visbuffer;
};

/// Every covered pixel's resolved normal against `reconstruct_surface` at that pixel's centre.
/// Returns the number of disagreements; `compared` receives how many pixels were checked.
u32 disagreements_with_reference(const Scene& scene, const vg::VisbufferReadback& readback,
                                 u32& compared) noexcept {
    const Mat4 clip = world_to_clip();
    u32 disagreed = 0;
    compared = 0;
    for (u32 pixel = 0; pixel < readback.samples.size(); ++pixel) {
        const vg::VisibilitySample& sample = readback.samples[pixel];
        if (!sample.covered()) {
            continue;
        }
        const Vec2 centre = pixel_centre(pixel);
        const vg::SurfaceIdentity identity =
            vg::split_surface_identity(sample.surface, scene.gpu.cluster_stride());
        vg::VisibleCluster record;
        record.instance = identity.instance;
        record.cluster = identity.cluster;
        record.material = scene.asset->clusters[identity.cluster].material;
        Expected<vg::SurfaceAttributes, Error> surface =
            vg::reconstruct_surface(*scene.asset, scene.instance, record, sample.triangle, clip,
                                    centre, kSide, kSide, scene.allocator);
        const Vec4 got = readback.resolved[pixel];
        const Vec3 normal{(got.x * 2.0F) - 1.0F, (got.y * 2.0F) - 1.0F, (got.z * 2.0F) - 1.0F};
        // The negation, so a NaN counts as a disagreement — see `render.virtual_geometry_gpu`.
        if (!surface || !(dot(normal, surface->normal) >= 0.99F)) {
            ++disagreed;
        }
        ++compared;
    }
    return disagreed;
}

/// A depth prepass that writes a constant depth — a plane facing the camera — and nothing else.
struct PlanePrepass {
    const ForwardFrame* frame = nullptr;
    f32 depth = 0.0F;
};

void record_plane_prepass(const PassContext& context, void* user) noexcept {
    const auto& prepass = *static_cast<const PlanePrepass*>(user);
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, kSide, kSide};
    info.depth_attachment.view = context.executor->view(prepass.frame->resources().depth);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    info.depth_attachment.clear.depth_stencil.depth = prepass.depth;
    context.commands->begin_rendering(info);
    context.commands->end_rendering();
}

}  // namespace

CY_TEST_CASE("the forward frame's virtual geometry stage writes the compute rasteriser's buffer") {
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();
    Scene scene(fixture);
    CY_REQUIRE(scene.initialise());
    GraphExecutor executor(allocator, fixture.device());

    // The compute rasteriser, in a harness graph of its own — the only way a cluster was ever drawn
    // before this task.
    vg::VisbufferReadback computed(allocator);
    vg::TraversalReadback traversed(allocator);
    {
        RenderGraph graph(allocator);
        CY_REQUIRE(scene.traversal.record(graph, traversal_view(), 1).has_value());
        CY_REQUIRE(scene.visbuffer.record(graph, scene.traversal, world_to_clip()).has_value());
        CY_REQUIRE(executor.execute(graph, CompileOptions{}, ExecuteOptions{}).has_value());
        CY_REQUIRE(fixture.device().wait_idle().has_value());
        CY_REQUIRE(scene.visbuffer.read_back(computed).has_value());
        CY_REQUIRE(scene.traversal.read_back(traversed).has_value());
    }

    // The same traversal, and the hardware rasteriser as a stage of the forward frame.
    vg::ForwardVisibility forward(fixture.device());
    CY_REQUIRE(forward.initialise(scene.visbuffer, rhi::Format::D32Sfloat).has_value());
    vg::VisbufferReadback drawn(allocator);
    {
        RenderGraph graph(allocator);
        ForwardFrame frame(allocator);
        CY_REQUIRE(scene.traversal.record(graph, traversal_view(), 1).has_value());
        CY_REQUIRE(forward
                       .build(graph, scene.traversal, world_to_clip(), frame,
                              frame_description(kSide, kSide))
                       .has_value());
        // The stage is where the pass order says it is, in the frame the graph will execute.
        CY_REQUIRE_NE(frame.pass_of(FramePassKind::VirtualGeometry), kInvalidPass);
        CY_REQUIRE_NE(frame.resources().visibility, kInvalidResource);
        Expected<ExecutionResult, Error> executed =
            executor.execute(graph, CompileOptions{}, ExecuteOptions{});
        CY_REQUIRE(executed.has_value());
        CY_REQUIRE(fixture.device().wait_idle().has_value());
        CY_REQUIRE(scene.visbuffer.read_back(drawn).has_value());
    }
    // THE CALLBACK RAN. Zero here is a stage the graph culled or a frame that never declared it,
    // and the visibility buffer below would then be the clear pass's output — all empty.
    CY_CHECK_EQ(forward.report().stages_recorded, 1U);
    CY_CHECK_EQ(forward.report().draws, 1U);
    CY_CHECK_FALSE(forward.report().loaded_prepass_depth);

    // --- 1. The same visibility buffer
    // ------------------------------------------------------------
    u32 both = 0;
    u32 only_computed = 0;
    u32 only_drawn = 0;
    u32 same_surface = 0;
    u32 same_triangle = 0;
    for (usize pixel = 0; pixel < drawn.samples.size(); ++pixel) {
        const vg::VisibilitySample& a = computed.samples[pixel];
        const vg::VisibilitySample& b = drawn.samples[pixel];
        if (a.covered() && b.covered()) {
            ++both;
            same_surface += a.surface == b.surface ? 1U : 0U;
            same_triangle += (a.surface == b.surface && a.triangle == b.triangle) ? 1U : 0U;
        } else if (a.covered()) {
            ++only_computed;
        } else if (b.covered()) {
            ++only_drawn;
        }
    }
    CY_TEST_MESSAGE("visible clusters " << traversed.visible.size() << "; covered by both " << both
                                        << ", compute only " << only_computed << ", hardware only "
                                        << only_drawn << "; same cluster " << same_surface
                                        << ", same triangle " << same_triangle);
    // The sphere is there — the same order of coverage `render.virtual_geometry_gpu` asserts.
    CY_CHECK_GT(both, 400U);
    // THE TWO RASTERISERS DIFFER ONLY WHERE A FILL RULE DECIDES. The compute one takes a pixel
    // whose centre lies exactly on an edge for BOTH triangles and lets the depth atomic choose; the
    // hardware one applies the top-left rule and gives it to one. So the silhouette can move by a
    // pixel and an interior pixel on a shared edge can name the neighbouring triangle — and nothing
    // else can differ. MEASURED on an RTX 5060: 2,529 pixels covered by both, NONE covered by one
    // alone, and all 2,529 naming the same cluster AND the same triangle — the two buffers are
    // identical. The bound is one percent of the covered pixels, which is headroom for another
    // driver's handling of a centre exactly on an edge and far below what a flipped row order, a
    // wrong identity stride or a stale target produces (each of those moves most of the frame).
    const u32 tolerance = both / 100U;
    CY_CHECK_LE(only_computed + only_drawn, tolerance);
    CY_CHECK_LE(both - same_surface, tolerance);
    CY_CHECK_LE(both - same_triangle, tolerance);

    // The bins the resolve chain produced over the HARDWARE buffer are the reference's bins over
    // the same buffer: the chain ran after the gather, not over what the clear left.
    vg::MaterialBins reference(allocator);
    CY_REQUIRE(vg::bin_by_material(drawn.samples.span(), traversed.visible.span(),
                                   scene.gpu.cluster_stride(), 4U, reference)
                   .has_value());
    for (u32 material = 0; material < 4U; ++material) {
        CY_CHECK_EQ(drawn.bin_counts[material], reference.counts[material]);
    }
    CY_CHECK_EQ(drawn.bin_offsets[4], drawn.covered_pixels());
    CY_CHECK_GT(reference.counts[0], 0U);
    CY_CHECK_GT(reference.counts[1], 0U);

    // And the attributes: every covered pixel's resolved normal is the one `reconstruct_surface`
    // finds at that pixel's centre. A buffer written upside down names real triangles at the wrong
    // rows, which the counts above cannot see and this can.
    u32 compared = 0;
    const u32 disagreed = disagreements_with_reference(scene, drawn, compared);
    CY_TEST_MESSAGE("hardware path: " << compared << " pixels reconstructed, " << disagreed
                                      << " disagreeing with the CPU reference");
    CY_CHECK_GT(compared, 400U);
    CY_CHECK_EQ(disagreed, 0U);

    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the virtual geometry stage tests against the depth the frame's prepass wrote") {
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();
    Scene scene(fixture);
    CY_REQUIRE(scene.initialise());
    GraphExecutor executor(allocator, fixture.device());
    vg::ForwardVisibility forward(fixture.device());
    CY_REQUIRE(forward.initialise(scene.visbuffer, rhi::Format::D32Sfloat).has_value());

    // A plane 0.8 units in front of the sphere's centre, written by the frame's own prepass as a
    // constant depth. Its depth is the projection's own answer for that distance, so the test and
    // the frame share one convention.
    const f32 plane_z = 0.8F;
    const Vec4 on_plane = world_to_clip() * Vec4{0.0F, 0.0F, plane_z, 1.0F};
    const f32 plane_depth = on_plane.z / on_plane.w;

    auto render = [&](bool with_plane, vg::VisbufferReadback& out) noexcept {
        RenderGraph graph(allocator);
        ForwardFrame frame(allocator);
        PlanePrepass prepass{&frame, plane_depth};
        FrameDescription description = frame_description(kSide, kSide);
        if (with_plane) {
            description.callbacks[static_cast<usize>(FramePassKind::DepthPrepass)] =
                FramePassCallback(&record_plane_prepass, &prepass);
        }
        if (!scene.traversal.record(graph, traversal_view(), 1) ||
            !forward.build(graph, scene.traversal, world_to_clip(), frame, description) ||
            !executor.execute(graph, CompileOptions{}, ExecuteOptions{}) ||
            !fixture.device().wait_idle()) {
            return false;
        }
        return scene.visbuffer.read_back(out).has_value();
    };

    vg::VisbufferReadback open(allocator);
    CY_REQUIRE(render(false, open));
    CY_CHECK_FALSE(forward.report().loaded_prepass_depth);
    vg::VisbufferReadback behind(allocator);
    CY_REQUIRE(render(true, behind));
    CY_CHECK(forward.report().loaded_prepass_depth);

    // Only the cap of the sphere nearer the camera than the plane survives. THE ARITHMETIC, because
    // a first draft of this bound got it wrong: seen from four units, the silhouette of a unit
    // sphere is the circle where the sight lines are tangent — at z = 1/4, radius sqrt(15)/4, 3.75
    // units from the camera, so 0.258 in projection. The cap in front of the plane ends at z = 0.8,
    // radius 0.6, 3.2 units away: 0.1875. The surviving area is (0.1875 / 0.258)^2 = 53% of the
    // unoccluded sphere. The bounds are a quarter and three quarters: far outside the pixel
    // quantisation of a 128-pixel frame, and still unable to hold for a stage that ignored the
    // prepass (100%) or one that tested against a cleared depth of its own (100%).
    const u32 all = open.covered_pixels();
    const u32 in_front = behind.covered_pixels();
    CY_TEST_MESSAGE("covered without the prepass " << all << ", with a plane at z = " << plane_z
                                                   << ": " << in_front);
    CY_CHECK_GT(in_front, all / 4U);
    CY_CHECK_LT(in_front, (all * 3U) / 4U);

    // AND EVERY PIXEL THAT SURVIVED IS IN FRONT OF THE PLANE. Reconstructed on the CPU from the
    // payload the stage wrote, independently of any depth the device kept.
    const Mat4 clip = world_to_clip();
    u32 behind_the_plane = 0;
    for (u32 pixel = 0; pixel < behind.samples.size(); ++pixel) {
        const vg::VisibilitySample& sample = behind.samples[pixel];
        if (!sample.covered()) {
            continue;
        }
        const vg::SurfaceIdentity identity =
            vg::split_surface_identity(sample.surface, scene.gpu.cluster_stride());
        vg::VisibleCluster record;
        record.instance = identity.instance;
        record.cluster = identity.cluster;
        Expected<vg::SurfaceAttributes, Error> surface =
            vg::reconstruct_surface(*scene.asset, scene.instance, record, sample.triangle, clip,
                                    pixel_centre(pixel), kSide, kSide, allocator);
        CY_REQUIRE(surface.has_value());
        // A hundredth of a unit is the quantised position's error at this scale several times over.
        behind_the_plane += surface->position.z < plane_z - 0.01F ? 1U : 0U;
    }
    CY_CHECK_EQ(behind_the_plane, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "the forward visibility stage refuses a traversal from another graph, and a mismatch") {
    DeviceFixture fixture;
    if (!fixture.have_vulkan()) {
        fixture.report_skip();
        return;
    }
    Allocator& allocator = fixture.allocator();
    Scene scene(fixture);
    CY_REQUIRE(scene.initialise());
    vg::ForwardVisibility forward(fixture.device());

    // Not initialised yet.
    {
        RenderGraph graph(allocator);
        ForwardFrame frame(allocator);
        CY_CHECK_FALSE(forward
                           .build(graph, scene.traversal, world_to_clip(), frame,
                                  frame_description(kSide, kSide))
                           .has_value());
    }
    CY_REQUIRE(forward.initialise(scene.visbuffer, rhi::Format::D32Sfloat).has_value());

    // The traversal recorded into ANOTHER graph: the stage would read a visible list this graph has
    // no pass writing, and derive no barrier for it.
    {
        RenderGraph elsewhere(allocator);
        CY_REQUIRE(scene.traversal.record(elsewhere, traversal_view(), 1).has_value());
        RenderGraph graph(allocator);
        ForwardFrame frame(allocator);
        CY_CHECK_FALSE(forward
                           .build(graph, scene.traversal, world_to_clip(), frame,
                                  frame_description(kSide, kSide))
                           .has_value());
    }
    // A frame of a different size from the visibility pass: the gather copies texel for texel.
    {
        RenderGraph graph(allocator);
        ForwardFrame frame(allocator);
        CY_REQUIRE(scene.traversal.record(graph, traversal_view(), 1).has_value());
        CY_CHECK_FALSE(forward
                           .build(graph, scene.traversal, world_to_clip(), frame,
                                  frame_description(kSide * 2U, kSide))
                           .has_value());
    }
    CY_CHECK_EQ(forward.report().stages_recorded, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
