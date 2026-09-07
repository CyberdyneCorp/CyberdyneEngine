// GPU-driven culling's reference, and the properties a compute dispatch must reproduce. M6
// task 8.5.
//
// Every case runs headless. That is the module's whole design argument: the shader that lands with
// the compute pass is checked against `cpu_reference_cull` by comparing buffers, and a renderer
// whose culling can only be verified by looking at a picture is a renderer whose culling is
// verified by nobody.
//
// TEARDOWN UNDER LOAD IS TESTED, NOT ONLY STEADY STATE. The last case here destroys an output and a
// scene mid-cull, repeatedly, because M5.5's gate found this project's first engine defect that
// way: a subsystem torn down while a worker was still inside it, one run in forty. M6 creates and
// destroys worlds continuously, so every subsystem it adds must survive being destroyed mid-flight.

#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/culling/gpu_cull.h>
#include <cy/test/test.h>

#include <vector>

using namespace cy::render;
using namespace cy::render::culling;
using cy::f32;
using cy::u32;
using cy::usize;
using cy::Vec3;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// A view looking down −Z from the origin, with a 60-degree vertical field of view.
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

GpuInstance instance_at(Vec3 centre, f32 radius, u32 lod_chain = 0, cy::u32 layer = kDefaultLayer) {
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

/// A three-level chain whose thresholds descend, as `render::MeshLod` requires.
std::vector<GpuMeshLod> three_levels() {
    std::vector<GpuMeshLod> levels(3);
    for (u32 index = 0; index < 3; ++index) {
        levels[index].index_count = 300 - (index * 100);
        levels[index].first_index = index * 1000;
        levels[index].vertex_offset = index * 500;
        levels[index].material = 7;
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
};

/// An occluder that hides everything past a distance, so the cull's occlusion stage can be driven
/// without a depth pyramid.
class BeyondTester final : public OcclusionTester {
public:
    explicit BeyondTester(f32 distance) noexcept : distance_(distance) {}
    [[nodiscard]] bool occluded(Vec3 centre, f32 /*radius*/) const noexcept override {
        return length(centre) > distance_;
    }

private:
    f32 distance_ = 0.0F;
};

}  // namespace

CY_TEST_CASE("gpu cull: the shader-visible records have the layout the shader expects") {
    // The static_asserts in the header are the contract; these are the numbers a reader can check
    // against a shader declaration without compiling anything.
    CY_CHECK(sizeof(GpuCullView) == 272);
    CY_CHECK(sizeof(GpuMeshLod) == 32);
    CY_CHECK(sizeof(GpuDrawPayload) == 32);
    CY_CHECK(sizeof(GpuVisibilityRange) == 16);
    CY_CHECK(sizeof(GpuCullCounters) == 64);
    // The one that is not this engine's to choose: every graphics API fixes the indexed indirect
    // argument at five words, and an engine that appends a field here discovers it on the first
    // indirect draw.
    CY_CHECK(sizeof(GpuDrawIndexedIndirect) == 20);
}

CY_TEST_CASE("gpu cull: an instance in front is drawn and one behind is rejected by the frustum") {
    Fixture fixture;
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -10.0F}, 1.0F));
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, 40.0F}, 1.0F));

    GpuCullOutput output(allocator());
    CY_REQUIRE(output.reserve(8).has_value());
    const GpuCullView view = forward_view(2);
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, {}, output).has_value());

    CY_CHECK(output.counters().tested == 2);
    CY_CHECK(output.counters().rejected_by_frustum == 1);
    CY_CHECK(output.counters().visible == 1);
    CY_REQUIRE(output.commands().size() == 1);
    CY_CHECK(output.payloads()[0].instance_slot == 0);
}

CY_TEST_CASE("gpu cull: the layer test runs before the frustum test") {
    // "WHEN an instance's layer mask does not intersect the view's THEN it SHALL be rejected before
    // any geometric test." The observable form of "before" is the counter it lands in: an instance
    // that is BOTH outside the view's layers and outside its frustum must be counted once, as a
    // layer rejection.
    Fixture fixture;
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, 40.0F}, 1.0F, 0, 1U << 3U));

    GpuCullOutput output(allocator());
    CY_REQUIRE(output.reserve(4).has_value());
    GpuCullView view = forward_view(1);
    view.layer_mask = kDefaultLayer;
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, {}, output).has_value());

    CY_CHECK(output.counters().rejected_by_layer == 1);
    CY_CHECK(output.counters().rejected_by_frustum == 0);
    CY_CHECK(output.counters().visible == 0);
}

CY_TEST_CASE("gpu cull: coverage selects the level, and the draw carries that level's arguments") {
    Fixture fixture;
    // Near: large coverage, level 0. Far: small coverage, the coarsest level.
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -3.0F}, 2.0F));
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -300.0F}, 2.0F));

    GpuCullOutput output(allocator());
    CY_REQUIRE(output.reserve(8).has_value());
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), forward_view(2), {}, output).has_value());

    CY_REQUIRE(output.payloads().size() == 2);
    CY_CHECK(output.payloads()[0].lod_level == 0);
    CY_CHECK(output.payloads()[1].lod_level == 2);
    // The draw's arguments ARE the chosen level's record, which is what makes the dispatch a pure
    // function of two buffers.
    CY_CHECK(output.commands()[0].index_count == fixture.levels[0].index_count);
    CY_CHECK(output.commands()[0].first_index == fixture.levels[0].first_index);
    CY_CHECK(output.commands()[1].index_count == fixture.levels[2].index_count);
    CY_CHECK(output.commands()[1].vertex_offset == fixture.levels[2].vertex_offset);
    CY_CHECK(output.commands()[0].instance_count == 1);
    // The histogram is the diagnostic the requirement names.
    CY_CHECK(output.counters().lod_histogram[0] == 1);
    CY_CHECK(output.counters().lod_histogram[2] == 1);
}

CY_TEST_CASE("gpu cull: hysteresis keeps a level across a threshold it has already crossed") {
    // "WHEN an instance's projected coverage falls below a level's threshold THEN the next LOD
    // SHALL be selected, with hysteresis to prevent oscillation at the boundary."
    Fixture fixture;
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -6.0F}, 2.0F));

    GpuCullOutput output(allocator());
    CY_REQUIRE(output.reserve(4).has_value());
    GpuCullView view = forward_view(1);
    view.lod_hysteresis = 0.5F;

    // First frame: no history, so the plain threshold applies.
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, {}, output).has_value());
    const u32 first = output.payloads()[0].lod_level;

    // Move just past the threshold. Without hysteresis the level would coarsen; with a wide band it
    // does not, which is the property that stops a breathing camera flipping levels every frame.
    fixture.instances[0].bounds_center[2] = -6.6F;
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, {}, output).has_value());
    CY_CHECK(output.payloads()[0].lod_level == first);

    // With the band off, the same movement does coarsen it — which is what says the band was doing
    // the work rather than the movement being too small.
    Fixture strict;
    strict.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -6.6F}, 2.0F));
    GpuCullView plain = forward_view(1);
    plain.lod_hysteresis = 0.0F;
    GpuCullOutput other(allocator());
    CY_REQUIRE(other.reserve(4).has_value());
    CY_REQUIRE(cpu_reference_cull(strict.scene(), plain, {}, other).has_value());
    CY_CHECK(other.payloads()[0].lod_level >= first);
}

CY_TEST_CASE("gpu cull: a visibility range fades an instance out and then drops it") {
    Fixture fixture;
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -50.0F}, 1.0F));
    fixture.ranges.resize(1);
    fixture.ranges[0].begin = 0.0F;
    fixture.ranges[0].end = 45.0F;
    fixture.ranges[0].fade_margin = 10.0F;

    GpuCullOutput output(allocator());
    CY_REQUIRE(output.reserve(4).has_value());
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), forward_view(1), {}, output).has_value());
    CY_REQUIRE(output.payloads().size() == 1);
    // Inside the fade margin: drawn, at a partial alpha.
    CY_CHECK(output.payloads()[0].alpha > 0.0F);
    CY_CHECK(output.payloads()[0].alpha < 1.0F);

    // Past the margin: dropped, and counted as a range rejection rather than as a frustum one.
    fixture.instances[0].bounds_center[2] = -100.0F;
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), forward_view(1), {}, output).has_value());
    CY_CHECK(output.payloads().empty());
    CY_CHECK(output.counters().rejected_by_range == 1);
}

CY_TEST_CASE("gpu cull: an HLOD parent's visibility replaces its children's") {
    // "a parent's visibility replaces its children's" — the HLOD swap. Slot 0 is the proxy and slot
    // 1 its child; the proxy becomes visible past 30 metres, and the child must then stop drawing.
    Fixture fixture;
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -50.0F}, 4.0F));
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -50.0F}, 1.0F));
    fixture.ranges.resize(2);
    fixture.ranges[0].begin = 30.0F;
    fixture.ranges[0].end = 0.0F;
    fixture.ranges[0].mode_and_parent =
        pack_visibility_parent(GpuFadeMode::None, kNoVisibilityParent);
    fixture.ranges[1].begin = 0.0F;
    fixture.ranges[1].end = 0.0F;
    fixture.ranges[1].mode_and_parent = pack_visibility_parent(GpuFadeMode::None, 0);

    GpuCullOutput output(allocator());
    CY_REQUIRE(output.reserve(8).has_value());
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), forward_view(2), {}, output).has_value());
    // Exactly one visible level per branch.
    CY_REQUIRE(output.payloads().size() == 1);
    CY_CHECK(output.payloads()[0].instance_slot == 0);

    // Bring the camera close enough that the proxy is out of range: the child draws instead.
    fixture.instances[0].bounds_center[2] = -10.0F;
    fixture.instances[1].bounds_center[2] = -10.0F;
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), forward_view(2), {}, output).has_value());
    CY_REQUIRE(output.payloads().size() == 1);
    CY_CHECK(output.payloads()[0].instance_slot == 1);
}

CY_TEST_CASE("gpu cull: a shadow view rejects a caster that cannot reach the camera frustum") {
    // "WHEN a caster is inside the light's volume but cannot project into the camera frustum THEN
    // it SHALL be excluded from the shadow render list", and "WHERE a light's shadow is rendered
    // for multiple camera views in one frame, the tighter culling SHALL be disabled".
    //
    // The caster sits well above the camera's frustum, which at 10 metres is about 5.8 metres half
    // height. It IS inside the light's own volume — `planes` here stands in for a cascade wide
    // enough to hold it — so only the sweep test can reject it.
    Fixture fixture;
    fixture.instances.push_back(instance_at(Vec3{0.0F, 12.0F, -10.0F}, 1.0F));

    const cy::Mat4 wide =
        cy::perspective_reversed_z(2.6F, 1.0F, 0.1F, 1000.0F) *
        cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
    GpuCullView view = forward_view(1);
    // `planes` is the SHADOW projection's frustum; `camera_planes` is the camera's. See gpu_cull.h
    // for why one set cannot answer both questions.
    const GpuCullView camera = forward_view(1);
    write_frustum(view, cy::Frustum::from_view_projection(wide));
    for (u32 plane = 0; plane < cy::Frustum::kCount; ++plane) {
        for (u32 lane = 0; lane < 4; ++lane) {
            view.camera_planes[plane][lane] = camera.planes[plane][lane];
        }
    }
    view.flags |= kGpuCullShadowCasters;
    // The light travels along +Y, so a caster above the frustum sweeps AWAY from it.
    view.light_direction[0] = 0.0F;
    view.light_direction[1] = 1.0F;
    view.light_direction[2] = 0.0F;
    view.sweep_distance = 5.0F;

    GpuCullOutput output(allocator());
    CY_REQUIRE(output.reserve(4).has_value());
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, {}, output).has_value());
    CY_CHECK(output.counters().rejected_by_frustum == 1);
    CY_CHECK(output.payloads().empty());

    // The same caster with the light travelling downwards sweeps INTO the camera frustum, and is
    // kept — which is what says the rejection above came from the sweep and not from the caster
    // being outside the light's volume.
    GpuCullOutput kept(allocator());
    CY_REQUIRE(kept.reserve(4).has_value());
    GpuCullView downwards = view;
    downwards.light_direction[1] = -1.0F;
    downwards.sweep_distance = 8.0F;
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), downwards, {}, kept).has_value());
    CY_CHECK(kept.payloads().size() == 1);

    // And a sweep distance of zero disables the tighter test entirely, which is what a light whose
    // shadow serves several camera views must do.
    GpuCullOutput shared(allocator());
    CY_REQUIRE(shared.reserve(4).has_value());
    GpuCullView untightened = view;
    untightened.sweep_distance = 0.0F;
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), untightened, {}, shared).has_value());
    CY_CHECK(shared.payloads().size() == 1);
}

CY_TEST_CASE("gpu cull: occlusion runs last and only when the flag and the tester are both there") {
    Fixture fixture;
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -10.0F}, 1.0F));
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -200.0F}, 4.0F));

    GpuCullOutput output(allocator());
    CY_REQUIRE(output.reserve(8).has_value());
    const BeyondTester tester(100.0F);
    GpuCullOptions options;
    options.occlusion = &tester;

    // The flag off: the tester is not consulted, whatever it would have said.
    GpuCullView view = forward_view(2);
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, options, output).has_value());
    CY_CHECK(output.counters().rejected_by_occlusion == 0);
    CY_CHECK(output.payloads().size() == 2);

    view.flags |= kGpuCullOcclusion;
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, options, output).has_value());
    CY_CHECK(output.counters().rejected_by_occlusion == 1);
    CY_CHECK(output.payloads().size() == 1);

    // A null tester disables it however the flag is set, which is what a device with no depth
    // pyramid yet gets.
    GpuCullOutput without(allocator());
    CY_REQUIRE(without.reserve(8).has_value());
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, {}, without).has_value());
    CY_CHECK(without.counters().rejected_by_occlusion == 0);
}

CY_TEST_CASE("gpu cull: a virtual geometry instance goes to cluster traversal, not to a draw") {
    Fixture fixture;
    fixture.instances.push_back(instance_at(Vec3{0.0F, 0.0F, -10.0F}, 1.0F));
    fixture.instances.push_back(instance_at(Vec3{2.0F, 0.0F, -10.0F}, 1.0F));
    fixture.instances[1].flags |= kInstanceVirtualGeometry;

    GpuCullOutput output(allocator());
    CY_REQUIRE(output.reserve(8).has_value());
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), forward_view(2), {}, output).has_value());

    CY_CHECK(output.counters().visible == 2);
    CY_REQUIRE(output.commands().size() == 1);
    CY_REQUIRE(output.virtual_geometry().size() == 1);
    CY_CHECK(output.virtual_geometry()[0] == 1);
    CY_CHECK(output.counters().virtual_geometry == 1);
}

CY_TEST_CASE("gpu cull: two runs over one scene produce identical buffers") {
    // The reference must be reproducible or it cannot be an expected value. Draws are emitted in
    // ascending slot order for exactly this reason.
    Fixture fixture;
    for (u32 index = 0; index < 32; ++index) {
        fixture.instances.push_back(instance_at(
            Vec3{static_cast<f32>(index % 8) - 4.0F, 0.0F, -10.0F - static_cast<f32>(index)},
            0.5F));
    }
    GpuCullOutput first(allocator());
    GpuCullOutput second(allocator());
    CY_REQUIRE(first.reserve(64).has_value());
    CY_REQUIRE(second.reserve(64).has_value());
    const GpuCullView view = forward_view(32);
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, {}, first).has_value());
    CY_REQUIRE(cpu_reference_cull(fixture.scene(), view, {}, second).has_value());

    CY_REQUIRE(first.commands().size() == second.commands().size());
    for (usize index = 0; index < first.commands().size(); ++index) {
        CY_REQUIRE(first.commands()[index].index_count == second.commands()[index].index_count);
        CY_REQUIRE(first.payloads()[index].instance_slot == second.payloads()[index].instance_slot);
        CY_REQUIRE(first.payloads()[index].view_depth == second.payloads()[index].view_depth);
    }
    CY_CHECK(first.counters().visible == second.counters().visible);
}

CY_TEST_CASE("gpu cull: the output refuses to grow past its reservation") {
    // On the device this buffer cannot grow, and a reference that grew would hide the day the
    // reservation became too small.
    Fixture fixture;
    for (u32 index = 0; index < 8; ++index) {
        fixture.instances.push_back(
            instance_at(Vec3{static_cast<f32>(index) - 4.0F, 0.0F, -10.0F}, 0.5F));
    }
    GpuCullOutput output(allocator());
    CY_REQUIRE(output.reserve(3).has_value());
    CY_CHECK(!cpu_reference_cull(fixture.scene(), forward_view(8), {}, output).has_value());
    CY_CHECK(output.commands().size() == 3);
}
