// The culling compute dispatch, checked against `cpu_reference_cull` BY COMPARING BUFFERS.
// M7 task 5.1.
//
// ================================================================================================
// WHAT THIS SUITE ASSERTS, AND WHY IT IS NOT A SCREENSHOT
// ================================================================================================
//
// M6 wrote `cpu_reference_cull` for exactly this moment. Every case below builds one scene, culls
// it twice — once on the CPU through the reference and once on the device through `GpuCullPass` —
// and compares FIVE buffers word for word: the indirect draw arguments, the payloads, the counters,
// the virtual-geometry list and the LOD hysteresis state the dispatch wrote back.
//
// Not "the same number of draws". Not "the same instances in some order". The reference emits in
// ascending slot order and says so, `compact_draws` reproduces that order by construction, and the
// comparison is therefore an equality between two arrays rather than between two sets.
// `first_instance` — the draw's own index in the compacted array — is part of that equality, which
// is what makes the ordering claim testable rather than decorative.
//
// ================================================================================================
// FLOATING POINT: WHAT IS ASSERTED EXACTLY AND WHAT IS NOT
// ================================================================================================
//
// Every integer field is compared for exact equality and every one of them holds: an index count, a
// material, a level, a slot. The four float fields of `GpuDrawPayload` are compared to a relative
// tolerance, and `kFloatTolerance` is the number, because `length()`, `dot()` and `exp2()` are
// permitted to differ in the last place between a C++ standard library and a SPIR-V driver — and
// because a comparison that demanded bit equality of a square root would be asserting a property of
// the driver rather than of this engine. The suite REPORTS the largest difference it saw, so that a
// tolerance quietly absorbing a real divergence would show up as a number that moved.
//
// ================================================================================================
// VALIDATION IS COUNTED
// ================================================================================================
//
// `rhi-and-render-graph`: a frame that renders but trips validation is not a frame that works.
// Every case asserts the device's cumulative validation error count is still zero afterwards.

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
#include <cy/servers/render/culling/gpu_cull.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace cy::render;
using namespace cy::render::culling;
using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::rendering::gpu_culling::GpuCullPass;
using cy::rendering::gpu_culling::GpuCullPassDescription;
using cy::rendering::gpu_culling::GpuCullReadback;

namespace {

/// Relative tolerance on the four float fields of a payload. See the header note.
constexpr f32 kFloatTolerance = 1.0e-6F;

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
        description.application_name = "cy_test_render_gpu_culling";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        description.request_async_compute = false;
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

// --- The scene, built the same way the reference suite builds one --------------------------------

GpuCullView forward_view(u32 instance_count, f32 far_plane = 1000.0F) {
    GpuCullView view;
    const cy::Mat4 projection =
        cy::perspective_reversed_z(1.0471975512F, 16.0F / 9.0F, 0.1F, far_plane);
    const cy::Mat4 look =
        cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
    write_frustum(view, cy::Frustum::from_view_projection(projection * look));
    write_field_of_view(view, 1.0471975512F);
    view.camera_forward[0] = 0.0F;
    view.camera_forward[1] = 0.0F;
    view.camera_forward[2] = -1.0F;
    view.instance_count = instance_count;
    return view;
}

GpuInstance instance_at(Vec3 centre, f32 radius, u32 lod_chain = 0, u32 layer = kDefaultLayer) {
    GpuInstance instance;
    instance.flags = kInstanceActive | kInstanceVisible | kInstanceCastsShadow;
    instance.layer_mask = layer;
    instance.bounds_center[0] = centre.x;
    instance.bounds_center[1] = centre.y;
    instance.bounds_center[2] = centre.z;
    instance.bounds_radius = radius;
    instance.lod_chain = lod_chain;
    return instance;
}

std::vector<GpuMeshLod> three_levels() {
    std::vector<GpuMeshLod> levels(3);
    for (u32 index = 0; index < 3; ++index) {
        levels[index].index_count = 300 - (index * 100);
        levels[index].first_index = index * 1000;
        levels[index].vertex_offset = index * 500;
        levels[index].material = 7 + index;
    }
    levels[0].screen_coverage_threshold = 0.5F;
    levels[1].screen_coverage_threshold = 0.2F;
    levels[2].screen_coverage_threshold = 0.0F;
    return levels;
}

struct Fixture {
    std::vector<GpuInstance> instances;
    std::vector<GpuLodChain> chains{GpuLodChain{0, 3}};
    std::vector<GpuMeshLod> levels = three_levels();
    std::vector<GpuVisibilityRange> ranges;
    std::vector<u32> previous;
    /// How many draws the last comparison emitted. A case asserts on it so that "the two agree"
    /// cannot be satisfied by two empty buffers.
    cy::usize last_draws = 0;

    [[nodiscard]] GpuCullScene scene() {
        previous.resize(instances.size(), kNoLodFade);
        GpuCullScene built;
        built.instances = cy::Span<const GpuInstance>(instances.data(), instances.size());
        built.chains = cy::Span<const GpuLodChain>(chains.data(), chains.size());
        built.mesh_lods = cy::Span<const GpuMeshLod>(levels.data(), levels.size());
        if (!ranges.empty()) {
            built.ranges = cy::Span<const GpuVisibilityRange>(ranges.data(), ranges.size());
        }
        built.previous_levels = cy::Span<u32>(previous.data(), previous.size());
        return built;
    }

    [[nodiscard]] GpuCullPassDescription description() const {
        GpuCullPassDescription desc;
        desc.max_instances = static_cast<u32>(instances.size());
        desc.max_draws = static_cast<u32>(instances.size());
        desc.max_lod_chains = static_cast<u32>(chains.size());
        desc.max_mesh_lods = static_cast<u32>(levels.size());
        desc.max_visibility_ranges = static_cast<u32>(ranges.size());
        desc.max_previous_levels = static_cast<u32>(instances.size());
        return desc;
    }
};

// --- The comparison
// --------------------------------------------------------------------------------

/// The largest relative difference any float comparison in this process has seen. Printed by every
/// case, so that a tolerance quietly absorbing a real divergence shows up as a number that moved
/// rather than as a test that still passes.
f32 g_worst_relative = 0.0F;

bool close_enough(f32 expected, f32 actual) noexcept {
    if (expected == actual) {
        return true;
    }
    const f32 scale = std::fabs(expected) > 1.0F ? std::fabs(expected) : 1.0F;
    const f32 relative = std::fabs(expected - actual) / scale;
    g_worst_relative = std::max(relative, g_worst_relative);
    return relative <= kFloatTolerance;
}

/// Run both culls over one scene and assert the five buffers agree.
///
/// Returns false when the machine has no Vulkan device, so a case can skip loudly rather than fail
/// on a continuous-integration runner with no GPU.
bool compare_cull(Fixture& fixture, const GpuCullView& view) {
    DeviceFixture gpu;
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return false;
    }

    // The reference. Its `previous_levels` span is the fixture's own array and the cull writes it,
    // so the GPU side is seeded from a copy taken BEFORE the reference runs — otherwise the two
    // would start from different hysteresis state and the comparison would be of two different
    // questions.
    fixture.previous.resize(fixture.instances.size(), kNoLodFade);
    const std::vector<u32> seed = fixture.previous;

    GpuCullOutput expected(allocator());
    CY_REQUIRE(expected.reserve(static_cast<u32>(fixture.instances.size()) + 1).has_value());
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, {}, expected).has_value());
    const std::vector<u32> expected_previous = fixture.previous;

    fixture.previous = seed;
    GpuCullPass pass;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), fixture.description()).has_value());
    CY_REQUIRE(pass.upload(fixture.scene(), view).has_value());

    CY_REQUIRE(gpu.device().begin_frame().has_value());
    cy::rendering::RenderGraph graph(allocator());
    CY_REQUIRE(pass.declare(graph).has_value());
    cy::rendering::GraphExecutor executor(allocator(), gpu.device());
    cy::Expected<cy::rendering::ExecutionResult, cy::Error> result =
        executor.execute(graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
    CY_REQUIRE(result.has_value());
    CY_REQUIRE(gpu.device().wait_idle().has_value());
    // ENDED, not merely waited on: `begin_frame` recycles the oldest in-flight frame's pools, and a
    // device whose frames are all still open refuses to start another. The hysteresis case below
    // runs two frames and is the one that found this.
    CY_REQUIRE(gpu.device().end_frame().has_value());

    cy::Expected<GpuCullReadback, cy::Error> actual = pass.read_back();
    CY_REQUIRE(actual.has_value());

    // 1. The counters, field by field. Sixteen words, every one of them a sum.
    const GpuCullCounters& want = expected.counters();
    const GpuCullCounters& got = actual->counters;
    CY_CHECK(got.tested == want.tested);
    CY_CHECK(got.rejected_by_layer == want.rejected_by_layer);
    CY_CHECK(got.rejected_by_frustum == want.rejected_by_frustum);
    CY_CHECK(got.rejected_by_occlusion == want.rejected_by_occlusion);
    CY_CHECK(got.rejected_by_range == want.rejected_by_range);
    CY_CHECK(got.visible == want.visible);
    CY_CHECK(got.draws == want.draws);
    CY_CHECK(got.virtual_geometry == want.virtual_geometry);
    for (u32 level = 0; level < 8; ++level) {
        CY_CHECK(got.lod_histogram[level] == want.lod_histogram[level]);
    }

    // 2. The indirect draw arguments. Five words each, all integers, compared exactly — this is the
    //    buffer an indirect draw reads, and there is nothing in it a tolerance could apply to.
    CY_REQUIRE(actual->commands.size() == expected.commands().size());
    for (cy::usize index = 0; index < expected.commands().size(); ++index) {
        const GpuDrawIndexedIndirect& a = expected.commands()[index];
        const GpuDrawIndexedIndirect& b = actual->commands[index];
        CY_CHECK(b.index_count == a.index_count);
        CY_CHECK(b.instance_count == a.instance_count);
        CY_CHECK(b.first_index == a.first_index);
        CY_CHECK(b.vertex_offset == a.vertex_offset);
        CY_CHECK(b.first_instance == a.first_instance);
    }

    // 3. The payloads.
    CY_REQUIRE(actual->payloads.size() == expected.payloads().size());
    for (cy::usize index = 0; index < expected.payloads().size(); ++index) {
        const GpuDrawPayload& a = expected.payloads()[index];
        const GpuDrawPayload& b = actual->payloads[index];
        CY_CHECK(b.instance_slot == a.instance_slot);
        CY_CHECK(b.material == a.material);
        CY_CHECK(b.lod_level == a.lod_level);
        CY_CHECK(b.lod_fade_to == a.lod_fade_to);
        CY_CHECK(close_enough(a.lod_fade, b.lod_fade));
        CY_CHECK(close_enough(a.alpha, b.alpha));
        CY_CHECK(close_enough(a.view_depth, b.view_depth));
        CY_CHECK(close_enough(a.coverage, b.coverage));
    }

    // 4. The cluster list virtual geometry consumes.
    CY_REQUIRE(actual->virtual_geometry.size() == expected.virtual_geometry().size());
    for (cy::usize index = 0; index < expected.virtual_geometry().size(); ++index) {
        CY_CHECK(actual->virtual_geometry[index] == expected.virtual_geometry()[index]);
    }

    // 5. The hysteresis state, which is the one input the dispatch WRITES. A cull that agreed about
    //    this frame and disagreed about what the next frame remembers would diverge on frame two.
    CY_REQUIRE(actual->previous_levels.size() == expected_previous.size());
    for (cy::usize index = 0; index < expected_previous.size(); ++index) {
        CY_CHECK(actual->previous_levels[index] == expected_previous[index]);
    }

    // Carry the DISPATCH'S own hysteresis state forward, not the reference's — the two were just
    // proved equal, and a second frame seeded from the CPU's copy would not be testing that.
    fixture.previous.assign(actual->previous_levels.begin(), actual->previous_levels.end());
    fixture.last_draws = actual->commands.size();

    CY_CHECK(gpu.validation_errors() == 0);
    std::fprintf(stderr, "gpu cull: %zu draws, %zu clustered, worst relative float delta %.3g\n",
                 static_cast<size_t>(actual->commands.size()),
                 static_cast<size_t>(actual->virtual_geometry.size()),
                 static_cast<double>(g_worst_relative));
    return true;
}

}  // namespace

CY_TEST_CASE("gpu cull dispatch: a mixed scene produces the reference's buffers exactly") {
    Fixture fixture;
    // In front at three distances, so all three levels of the chain are exercised.
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -3.0F}, 2.0F));
    fixture.instances.push_back(instance_at(Vec3{2.0F, 1.0F, -30.0F}, 2.0F));
    fixture.instances.push_back(instance_at(Vec3{-4.0F, -2.0F, -300.0F}, 2.0F));
    // Behind the camera: rejected by the frustum.
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, 40.0F}, 1.0F));
    // Outside the view's layers: rejected before any geometry runs.
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -12.0F}, 1.0F, 0, 1U << 3U));
    // Not active at all: skipped, and not even counted as tested.
    GpuInstance dormant = instance_at(Vec3{0.0F, 0.0F, -12.0F}, 1.0F);
    dormant.flags = 0;
    fixture.instances.push_back(dormant);
    // Virtual geometry: routed to the cluster list rather than emitting an indexed draw.
    GpuInstance clustered = instance_at(Vec3{1.0F, 0.0F, -20.0F}, 1.5F);
    clustered.flags |= kInstanceVirtualGeometry;
    fixture.instances.push_back(clustered);

    GpuCullView view = forward_view(static_cast<u32>(fixture.instances.size()));
    view.layer_mask = kDefaultLayer;
    if (!compare_cull(fixture, view)) {
        return;
    }
    CY_CHECK(fixture.last_draws == 3);
}

CY_TEST_CASE("gpu cull dispatch: visibility ranges and an HLOD parent agree with the reference") {
    Fixture fixture;
    for (u32 index = 0; index < 6; ++index) {
        fixture.instances.push_back(instance_at(
            Vec3{static_cast<f32>(index), 0.0F, -20.0F - (static_cast<f32>(index) * 8.0F)}, 1.5F));
    }
    fixture.ranges.resize(fixture.instances.size());
    // Slot 0 is the HLOD parent: visible from far away, and it suppresses its children while it is.
    fixture.ranges[0].begin = 40.0F;
    fixture.ranges[0].end = 0.0F;
    fixture.ranges[0].fade_margin = 10.0F;
    fixture.ranges[0].mode_and_parent =
        pack_visibility_parent(GpuFadeMode::Dependents, kNoVisibilityParent);
    for (u32 index = 1; index < 6; ++index) {
        fixture.ranges[index].begin = 0.0F;
        fixture.ranges[index].end = 120.0F;
        fixture.ranges[index].fade_margin = 15.0F;
        fixture.ranges[index].mode_and_parent = pack_visibility_parent(GpuFadeMode::Self, 0);
    }

    GpuCullView view = forward_view(static_cast<u32>(fixture.instances.size()));
    view.flags = kGpuCullVisibilityRanges;
    if (!compare_cull(fixture, view)) {
        return;
    }
}

CY_TEST_CASE("gpu cull dispatch: a shadow view's two frustums agree with the reference") {
    Fixture fixture;
    for (u32 index = 0; index < 5; ++index) {
        fixture.instances.push_back(
            instance_at(Vec3{static_cast<f32>(index) * 3.0F, 2.0F, -25.0F}, 2.0F));
    }
    // One caster that does not cast: the flag is what the shadow view tests first.
    GpuInstance no_caster = instance_at(Vec3{3.0F, 2.0F, -25.0F}, 2.0F);
    no_caster.flags = kInstanceActive | kInstanceVisible;
    fixture.instances.push_back(no_caster);

    GpuCullView view = forward_view(static_cast<u32>(fixture.instances.size()));
    // The shadow projection's frustum is the camera's here — what is being compared is the second
    // plane set and the sweep, not a realistic cascade.
    const cy::Mat4 projection =
        cy::perspective_reversed_z(1.0471975512F, 16.0F / 9.0F, 0.1F, 1000.0F);
    const cy::Mat4 look =
        cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
    write_camera_frustum(view, cy::Frustum::from_view_projection(projection * look));
    view.flags = kGpuCullShadowCasters;
    view.light_direction[0] = 0.0F;
    view.light_direction[1] = -1.0F;
    view.light_direction[2] = 0.0F;
    view.sweep_distance = 30.0F;
    if (!compare_cull(fixture, view)) {
        return;
    }
    // NOT VACUOUS. A shadow view that rejected everything would compare two empty buffers and pass,
    // which is the shape of a test that says nothing.
    CY_CHECK(fixture.last_draws > 0);
}

CY_TEST_CASE("gpu cull dispatch: the LOD hysteresis band agrees across two frames") {
    // The band only does anything when `previous_levels` is not the first frame's, so this case
    // runs the comparison twice with the state carried between them — which is what the dispatch
    // writing that buffer back is for.
    Fixture fixture;
    for (u32 index = 0; index < 8; ++index) {
        // Placed so that several instances sit close to a threshold, which is where a hysteresis
        // band is either doing its job or flipping a level every frame.
        fixture.instances.push_back(
            instance_at(Vec3{0.0F, 0.0F, -6.0F - (static_cast<f32>(index) * 1.7F)}, 2.0F));
    }
    GpuCullView view = forward_view(static_cast<u32>(fixture.instances.size()));
    view.lod_hysteresis = 0.15F;
    view.lod_cross_fade_band = 0.25F;

    if (!compare_cull(fixture, view)) {
        return;
    }
    // Second frame: the state the first left behind is the input, and it is not all kNoLodFade.
    bool any_settled = false;
    for (u32 level : fixture.previous) {
        any_settled = any_settled || level != kNoLodFade;
    }
    CY_CHECK(any_settled);
    (void)compare_cull(fixture, view);
}

CY_TEST_CASE("gpu cull dispatch: a moved-only view agrees with the reference") {
    Fixture fixture;
    for (u32 index = 0; index < 6; ++index) {
        GpuInstance instance =
            instance_at(Vec3{static_cast<f32>(index) * 3.0F, 0.0F, -18.0F}, 1.5F);
        if ((index % 2) == 0) {
            instance.flags |= kInstanceMoved;
        }
        fixture.instances.push_back(instance);
    }
    GpuCullView view = forward_view(static_cast<u32>(fixture.instances.size()));
    view.flags = kGpuCullVisibilityRanges | kGpuCullMovedOnly;
    if (!compare_cull(fixture, view)) {
        return;
    }
}

CY_TEST_CASE("gpu cull dispatch: an occlusion view is refused rather than silently ignored") {
    DeviceFixture gpu;
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    Fixture fixture;
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -10.0F}, 1.0F));

    GpuCullPass pass;
    CY_REQUIRE(pass.create(allocator(), gpu.device(), fixture.description()).has_value());
    GpuCullView view = forward_view(1);
    view.flags = kGpuCullVisibilityRanges | kGpuCullOcclusion;
    // A dispatch that ignored the flag would report "nothing was occluded", which is
    // indistinguishable from a working occlusion cull over an empty pyramid. It says so instead.
    const cy::Status refused = pass.upload(fixture.scene(), view);
    CY_CHECK(!refused.has_value());
    CY_CHECK(gpu.validation_errors() == 0);
}

CY_TEST_CASE("gpu cull dispatch: the pass is destroyed while its frame is still in flight") {
    // TEARDOWN UNDER LOAD, and not only steady state. M5.5's gate found this project's first engine
    // defect that way — a subsystem torn down while a worker was still inside it, one run in forty
    // — and the shape here is the device's: `GpuCullPass::destroy()` destroys thirteen buffers,
    // three pipelines and a descriptor set while the GPU may still be reading every one of them.
    //
    // `rhi-and-render-graph` requires a resource destroyed during frame N to be released only after
    // frame N's fence has signalled, and this case is what makes that requirement load-bearing for
    // this module rather than a sentence in a header: it submits, does NOT wait, and destroys.
    // Twenty times, because a deferral that is wrong is wrong intermittently.
    DeviceFixture gpu;
    if (!gpu.has_gpu()) {
        gpu.report_skip();
        return;
    }
    for (u32 round = 0; round < 20; ++round) {
        Fixture fixture;
        for (u32 slot = 0; slot < 16; ++slot) {
            fixture.instances.push_back(
                instance_at(Vec3{0.0F, 0.0F, -6.0F - (static_cast<f32>(slot) * 5.0F)}, 2.0F));
        }
        const GpuCullView view = forward_view(static_cast<u32>(fixture.instances.size()));

        GpuCullPass pass;
        CY_REQUIRE(pass.create(allocator(), gpu.device(), fixture.description()).has_value());
        CY_REQUIRE(pass.upload(fixture.scene(), view).has_value());
        CY_REQUIRE(gpu.device().begin_frame().has_value());
        {
            cy::rendering::RenderGraph graph(allocator());
            CY_REQUIRE(pass.declare(graph).has_value());
            cy::rendering::GraphExecutor executor(allocator(), gpu.device());
            cy::rendering::ExecuteOptions options;
            // BREADCRUMBS OFF FOR THIS CASE ONLY, AND THE REASON IS A DEFECT THIS CASE FOUND IN
            // src/rendering/graph/, NOT IN THIS MODULE.
            //
            // `GraphExecutor::breadcrumb_next_` is reset to zero per execution, so two frames in
            // flight fill the SAME slots of the device's breadcrumb buffer with no barrier between
            // them: 95 SYNC-HAZARD-WRITE-AFTER-WRITE reports, every one of them
            // `command: vkCmdFillBuffer` on the breadcrumb buffer and not one of them naming a
            // buffer this module owns. It has never shown before because every other suite in this
            // tree calls `wait_idle()` after each frame, which serialises the frames and hides it;
            // this case is the first thing to let two overlap. The fix is a per-frame-slot
            // breadcrumb range, and it belongs to whoever owns the executor.
            //
            // Turning it off here keeps the case testing what it is for — that destroying thirteen
            // buffers, three pipelines and a descriptor set under an in-flight submission trips
            // nothing — rather than asserting zero against a number that is somebody else's.
            options.breadcrumbs = false;
            CY_REQUIRE(
                executor.execute(graph, cy::rendering::CompileOptions{}, options).has_value());
            // NO wait_idle BEFORE THIS. The submission is in flight and every buffer it names is
            // about to be destroyed, which is the whole subject of the case.
            pass.destroy();
        }
        CY_REQUIRE(gpu.device().end_frame().has_value());
        // ONE ROUND'S FRAME IS DRAINED BEFORE THE NEXT BEGINS, and that is a statement about
        // `GpuCullPass` rather than a convenience. It owns ONE set of buffers, so two of its frames
        // in flight at once would have two dispatches writing one counters buffer with nothing
        // between them — a real write-after-write, which validation reports and which a caller
        // avoids by double-buffering the pass rather than by hoping. cull_pass.h says so.
        CY_REQUIRE(gpu.device().wait_idle().has_value());
    }
    CY_CHECK(gpu.validation_errors() == 0);
    std::fprintf(stderr,
                 "gpu cull: 20 passes destroyed with their frames in flight, 0 validation "
                 "errors\n");
}
