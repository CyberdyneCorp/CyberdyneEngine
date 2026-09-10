// Framing, composition constraints and camera volumes. M8.b task 7.3.
//
// Each case is a scenario `camera-system` states, quoted where it is asserted.

#include <cy/camera/framing.h>
#include <cy/camera/volume.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>

using namespace cy;
using namespace cy::camera;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] TargetBinding binding_of(u64 id, f32 weight, f32 minimum_screen) noexcept {
    TargetBinding binding;
    binding.kind = TargetKind::Entity;
    binding.stable_id = id;
    binding.weight = weight;
    binding.min_screen_size = minimum_screen;
    return binding;
}

[[nodiscard]] TargetSample sample_at(Vec3 position, f32 half_height) noexcept {
    TargetSample sample;
    sample.transform = Transform::from_translation(position);
    sample.bounds = Aabb::from_center_extents(Vec3{}, Vec3{half_height, half_height, half_height});
    sample.valid = true;
    return sample;
}

[[nodiscard]] FramingRequest request_for(Span<const TargetBinding> bindings,
                                         Span<const TargetSample> samples) noexcept {
    FramingRequest request;
    request.bindings = bindings;
    request.samples = samples;
    request.lens.gameplay.vertical_fov_radians = 1.0F;
    // Looking down −Z from ten metres out, which is where the samples are.
    request.current = Transform::from_translation(Vec3{0.0F, 0.0F, 10.0F});
    return request;
}

/// A host that answers bindings, and refuses one of them the way a streamed-out entity does.
class PartialResolver final : public TargetResolver {
public:
    void resolve(const TargetBinding& binding, TargetSample& out) noexcept override {
        ++calls;
        if (binding.stable_id == missing) {
            return;  // left invalid on purpose
        }
        out.transform =
            Transform::from_translation(Vec3{static_cast<f32>(binding.stable_id), 0.0F, 0.0F});
        out.valid = true;
    }

    u64 missing = 0;
    u32 calls = 0;
};

}  // namespace

CY_TEST_CASE("camera_framing: a fixed target is answered from the binding, not from the host") {
    // A callback that can only repeat what its argument already says is a callback whose absence is
    // simpler than its presence — and a `Position` binding is exactly that case.
    TargetBinding fixed;
    fixed.kind = TargetKind::Position;
    fixed.position = Vec3{3.0F, 1.0F, 0.0F};
    TargetBinding entity = binding_of(7, 1.0F, 0.0F);
    const TargetBinding bindings[2] = {fixed, entity};

    PartialResolver resolver;
    Array<TargetSample> samples(allocator());
    CY_REQUIRE(
        resolve_targets(Span<const TargetBinding>(bindings, 2), &resolver, samples).has_value());
    CY_REQUIRE_EQ(samples.size(), 2U);
    CY_CHECK(samples[0].valid);
    CY_CHECK_EQ(samples[0].transform.translation.x, 3.0F);
    CY_CHECK_EQ(resolver.calls, 1U);  // only the entity reached the host
    CY_CHECK(samples[1].valid);
}

CY_TEST_CASE("camera_framing: a target the world could not resolve is dropped, not framed") {
    const TargetBinding bindings[2] = {binding_of(4, 1.0F, 0.0F), binding_of(9, 1.0F, 0.0F)};
    PartialResolver resolver;
    resolver.missing = 9;
    Array<TargetSample> samples(allocator());
    CY_REQUIRE(
        resolve_targets(Span<const TargetBinding>(bindings, 2), &resolver, samples).has_value());

    FramingSolution solution;
    const FramingRequest request =
        request_for(Span<const TargetBinding>(bindings, 2), samples.span());
    CY_REQUIRE(solve_framing(request, solution).has_value());
    CY_CHECK_EQ(solution.contributors, 1U);
    // The unresolved target would have dragged the anchor toward the origin had it counted.
    CY_CHECK_NEAR(solution.anchor.x, 4.0F, 1e-4F);
}

CY_TEST_CASE("camera_framing: two combatants separating are both kept in frame") {
    // "WHEN two weighted targets separate THEN framing SHALL adjust distance and position to keep
    // both visible at their minimum screen sizes."
    // A fifth of the view each: close together both fit at that size, and eight metres apart they
    // cannot — which is the conflict the solver has to resolve and report.
    const TargetBinding bindings[2] = {binding_of(1, 1.0F, 0.2F), binding_of(2, 1.0F, 0.2F)};
    const TargetSample close[2] = {sample_at(Vec3{-1.0F, 0.0F, 0.0F}, 0.9F),
                                   sample_at(Vec3{1.0F, 0.0F, 0.0F}, 0.9F)};
    const TargetSample apart[2] = {sample_at(Vec3{-8.0F, 0.0F, 0.0F}, 0.9F),
                                   sample_at(Vec3{8.0F, 0.0F, 0.0F}, 0.9F)};

    FramingRequest request =
        request_for(Span<const TargetBinding>(bindings, 2), Span<const TargetSample>(close, 2));
    FramingSolution near_solution;
    CY_REQUIRE(solve_framing(request, near_solution).has_value());

    request.samples = Span<const TargetSample>(apart, 2);
    FramingSolution far_solution;
    CY_REQUIRE(solve_framing(request, far_solution).has_value());

    CY_CHECK_EQ(near_solution.contributors, 2U);
    CY_CHECK_GT(far_solution.distance, near_solution.distance);
    CY_CHECK_NEAR(far_solution.anchor.x, 0.0F, 1e-4F);
    // The minimum screen size could not be honoured for both once they separated, and the solver
    // SAYS SO rather than silently dropping one of the two obligations.
    CY_CHECK(far_solution.screen_size_yielded);
    CY_CHECK_FALSE(near_solution.screen_size_yielded);
}

CY_TEST_CASE("camera_framing: a target inside the dead zone moves nothing") {
    // "WHEN a target moves slightly within the dead zone THEN the camera SHALL not move."
    const TargetBinding bindings[1] = {binding_of(1, 1.0F, 0.0F)};
    const TargetSample samples[1] = {sample_at(Vec3{0.02F, 0.0F, 0.0F}, 0.5F)};
    FramingRequest request =
        request_for(Span<const TargetBinding>(bindings, 1), Span<const TargetSample>(samples, 1));
    request.composition.dead_zone = Vec2{0.2F, 0.2F};

    FramingSolution solution;
    CY_REQUIRE(solve_framing(request, solution).has_value());
    CY_CHECK(solution.inside_dead_zone);
    // NOT damped movement — none. The pose that came in is the pose that goes out.
    CY_CHECK_EQ(solution.pose.translation.z, 10.0F);
    CY_CHECK_EQ(solution.pose.translation.x, 0.0F);
}

CY_TEST_CASE("camera_framing: headroom places the subject off centre") {
    const TargetBinding bindings[1] = {binding_of(1, 1.0F, 0.0F)};
    const TargetSample samples[1] = {sample_at(Vec3{0.0F, 0.0F, 0.0F}, 0.5F)};
    FramingRequest request =
        request_for(Span<const TargetBinding>(bindings, 1), Span<const TargetSample>(samples, 1));
    request.composition.dead_zone = Vec2{0.0F, 0.0F};
    request.composition.screen_offset = Vec2{0.0F, -0.25F};

    FramingSolution solution;
    CY_REQUIRE(solve_framing(request, solution).has_value());
    CY_CHECK_FALSE(solution.inside_dead_zone);
    // The camera moved UP so the subject sits low in frame, which is what headroom is.
    CY_CHECK_GT(solution.pose.translation.y, 0.0F);
}

CY_TEST_CASE("camera_framing: a sample count that disagrees with the bindings is refused") {
    const TargetBinding bindings[2] = {binding_of(1, 1.0F, 0.0F), binding_of(2, 1.0F, 0.0F)};
    const TargetSample samples[1] = {sample_at(Vec3{}, 0.5F)};
    const FramingRequest request =
        request_for(Span<const TargetBinding>(bindings, 2), Span<const TargetSample>(samples, 1));
    FramingSolution solution;
    const Status solved = solve_framing(request, solution);
    CY_REQUIRE_FALSE(solved.has_value());
    CY_CHECK_EQ(solved.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("camera_framing: a camera a thousand kilometres out rebases before it frames") {
    // "Camera position SHALL use the world's cell-relative representation ... so cameras remain
    // exact at planetary distances." The subtraction happens in f64, above the camera server, which
    // is what lets everything below it work in one frame's floats.
    world::PartitionConfig config;
    config.base_cell_size = 128.0F;
    config.levels = 3;
    config.level_ratio = 4;

    const world::CellCoord frame{8000, 0, 0, 0};  // 8000 × 128 m ≈ 1,024 km
    const world::WorldPosition target{frame, Vec3{7.5F, 1.0F, 2.0F}};
    const Vec3 local = rebase(config, target, frame);
    CY_CHECK_NEAR(local.x, 7.5F, 1e-5F);

    // A target one cell over comes back as a small number too, rather than as a million.
    const world::WorldPosition neighbour{world::CellCoord{8001, 0, 0, 0}, Vec3{1.0F, 0.0F, 0.0F}};
    CY_CHECK_NEAR(rebase(config, neighbour, frame).x, 129.0F, 1e-3F);
}

CY_TEST_CASE("camera_constraints: a rail camera is a spline constraint, not a camera type") {
    // "WHEN a camera follows a spline through a level THEN it SHALL be a follow node constrained to
    // a spline, not a separate camera type."
    const Vec3 rail[3] = {Vec3{0.0F, 2.0F, 0.0F}, Vec3{10.0F, 2.0F, 0.0F}, Vec3{20.0F, 2.0F, 0.0F}};
    Constraint constraint;
    constraint.kind = ConstraintKind::Spline;
    constraint.spline = Span<const Vec3>(rail, 3);

    const Vec3 result =
        apply_constraints(Span<const Constraint>(&constraint, 1), Vec3{5.0F, 9.0F, 7.0F}, Vec3{});
    CY_CHECK_NEAR(result.x, 5.0F, 1e-4F);
    CY_CHECK_NEAR(result.y, 2.0F, 1e-4F);
    CY_CHECK_NEAR(result.z, 0.0F, 1e-4F);
}

CY_TEST_CASE("camera_constraints: a side-scroller holds one axis and an orbit clamps two angles") {
    Constraint plane;
    plane.kind = ConstraintKind::Plane;
    plane.plane_normal = Vec3{0.0F, 0.0F, 1.0F};
    plane.plane_offset = 12.0F;
    const Vec3 flattened =
        apply_constraints(Span<const Constraint>(&plane, 1), Vec3{4.0F, 3.0F, 40.0F}, Vec3{});
    CY_CHECK_NEAR(flattened.z, 12.0F, 1e-4F);
    CY_CHECK_NEAR(flattened.x, 4.0F, 1e-4F);

    Constraint orbit;
    orbit.kind = ConstraintKind::Orbit;
    orbit.pitch_range = Vec2{0.0F, 0.2F};
    const Vec3 anchor{0.0F, 0.0F, 0.0F};
    const Vec3 clamped =
        apply_constraints(Span<const Constraint>(&orbit, 1), Vec3{0.0F, 10.0F, 0.1F}, anchor);
    // The pitch was almost straight up; the limit brings it down to 0.2 radians without changing
    // the distance, which is what an orbit limit means.
    CY_CHECK_NEAR(length(clamped - anchor), 10.0F, 1e-3F);
    CY_CHECK_LT(clamped.y, 2.1F);
}

CY_TEST_CASE("camera_volumes: an interior blends in over the blend distance") {
    // "WHEN the camera enters a tunnel volume THEN its distance and collision policy SHALL blend to
    // the volume's settings over the blend distance."
    Array<VolumeInfluence> influences(allocator());
    CameraVolume tunnel;
    tunnel.name = Name::intern("tunnel");
    tunnel.bounds = Aabb::from_min_max(Vec3{0.0F, 0.0F, 0.0F}, Vec3{10.0F, 4.0F, 4.0F});
    tunnel.blend_distance = 4.0F;
    tunnel.settings.overrides_distance = true;
    tunnel.settings.distance = 2.0F;
    tunnel.settings.overrides_collision = true;
    tunnel.settings.collision.collision_response = CollisionResponse::Slide;

    const VolumeBlend outside =
        blend_volumes(Span<const CameraVolume>(&tunnel, 1), Vec3{-6.0F, 1.0F, 1.0F}, influences);
    CY_CHECK_EQ(outside.contributors, 0U);
    CY_CHECK_EQ(outside.total_weight, 0.0F);
    CY_CHECK_FALSE(outside.settings.overrides_distance);

    const VolumeBlend edge =
        blend_volumes(Span<const CameraVolume>(&tunnel, 1), Vec3{-2.0F, 1.0F, 1.0F}, influences);
    CY_CHECK_NEAR(edge.total_weight, 0.5F, 1e-4F);

    const VolumeBlend inside =
        blend_volumes(Span<const CameraVolume>(&tunnel, 1), Vec3{5.0F, 1.0F, 1.0F}, influences);
    CY_CHECK_EQ(inside.total_weight, 1.0F);
    CY_CHECK(inside.settings.overrides_distance);
    CY_CHECK_EQ(inside.settings.distance, 2.0F);
    CY_CHECK_EQ(inside.settings.collision.collision_response, CollisionResponse::Slide);
}

CY_TEST_CASE(
    "camera_volumes: overlapping volumes are resolved by priority, and both are reported") {
    Array<VolumeInfluence> influences(allocator());
    CameraVolume wide;
    wide.name = Name::intern("wide");
    wide.bounds = Aabb::from_min_max(Vec3{-20.0F, -5.0F, -20.0F}, Vec3{20.0F, 5.0F, 20.0F});
    wide.priority = 0;
    wide.settings.overrides_fov = true;
    wide.settings.fov_y_radians = 1.2F;

    CameraVolume inner = wide;
    inner.name = Name::intern("inner");
    inner.bounds = Aabb::from_min_max(Vec3{-2.0F, -2.0F, -2.0F}, Vec3{2.0F, 2.0F, 2.0F});
    inner.priority = 5;
    inner.settings.fov_y_radians = 0.7F;

    const CameraVolume volumes[2] = {wide, inner};
    const VolumeBlend blend =
        blend_volumes(Span<const CameraVolume>(volumes, 2), Vec3{0.0F, 0.0F, 0.0F}, influences);
    CY_CHECK_EQ(blend.settings.fov_y_radians, 0.7F);
    CY_CHECK_EQ(blend.contributors, 1U);
    // BOTH are reported. "the active volumes and their weights" is a diagnostic, and a volume the
    // camera is inside that changes nothing is exactly what a developer is hunting for.
    CY_REQUIRE_EQ(influences.size(), 2U);
}

CY_TEST_CASE("camera_volumes: a thousand volumes are not tested one by one") {
    // "Volumes SHALL be found through the world's spatial index rather than by testing every volume
    // each frame." A measurement, not an assertion about the implementation.
    VolumeSet set(allocator(), 32.0F);
    // SIXTY-FOUR RATHER THAN A THOUSAND, AND THE NUMBER IS THE INSTRUMENT'S RATHER THAN THE
    // PROPERTY'S. What is being measured is that the query tests a HANDFUL whatever the set holds:
    // `tested() <= 4` against `size() == 64` is a factor of sixteen, and a linear scan cannot
    // produce it at any set size. What the set size costs is BUILDING the index — a thousand took
    // 1.5 ms and two hundred took 2.09 ms in the Debug configuration, where nothing is inlined —
    // and hard rule 7 says a case that expensive does not belong in the unit tier. M8.b's closing
    // gate found this one red in Debug and green in Development, which is the shape of a case
    // sitting on its budget rather than of a regression.
    for (u32 index = 0; index < 64U; ++index) {
        CameraVolume volume;
        volume.name = Name::intern("far");
        const f32 offset = static_cast<f32>(index) * 64.0F;
        volume.bounds =
            Aabb::from_min_max(Vec3{offset, 0.0F, 0.0F}, Vec3{offset + 8.0F, 4.0F, 4.0F});
        volume.blend_distance = 1.0F;
        CY_REQUIRE(set.add(volume).has_value());
    }
    Array<CameraVolume> found(allocator());
    CY_REQUIRE(set.query(Vec3{4.0F, 1.0F, 1.0F}, found).has_value());
    CY_CHECK_EQ(found.size(), 1U);
    CY_CHECK_EQ(set.size(), 64U);
    CY_CHECK_LE(set.tested(), 4U);

    CY_REQUIRE(set.query(Vec3{40.0F, 1.0F, 1.0F}, found).has_value());
    CY_CHECK_EQ(found.size(), 0U);
    CY_CHECK_LE(set.tested(), 4U);
}
