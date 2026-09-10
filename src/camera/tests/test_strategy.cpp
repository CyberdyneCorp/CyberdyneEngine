// The strategy camera, aim assistance and the director. M8.b task 7.3.

#include <cy/camera/assist.h>
#include <cy/camera/strategy.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>

using namespace cy;
using namespace cy::camera;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A terrain that is a ramp, and counts the samples it was asked for.
class RampTerrain final : public TerrainHeightService {
public:
    void sample_heights(Span<const Vec2> points, Span<f32> heights) noexcept override {
        ++calls;
        for (usize index = 0; index < points.size() && index < heights.size(); ++index) {
            heights[index] = points[index].x * 0.1F;
        }
        samples += static_cast<u32>(points.size());
    }

    u32 calls = 0;
    u32 samples = 0;
};

}  // namespace

CY_TEST_CASE("camera_zoom: one normalised parameter moves height, distance, tilt and lens") {
    // "WHEN the player zooms out THEN height, tilt, and distance SHALL follow the zoom curve
    // together." There is no other function in this module that returns a height.
    const ZoomCurve curve;
    const ZoomState in = evaluate_zoom(curve, 0.0F);
    const ZoomState out = evaluate_zoom(curve, 1.0F);
    CY_CHECK_LT(in.height, out.height);
    CY_CHECK_LT(in.distance, out.distance);
    CY_CHECK_LT(in.tilt_radians, out.tilt_radians);
    CY_CHECK_GT(in.fov_radians, out.fov_radians);

    const ZoomState middle = evaluate_zoom(curve, 0.5F);
    CY_CHECK_NEAR(middle.height, 0.5F * (curve.near_height + curve.far_height), 1e-4F);
    // Out of range clamps rather than extrapolating a camera into orbit.
    CY_CHECK_EQ(evaluate_zoom(curve, 5.0F).height, out.height);
    CY_CHECK_EQ(evaluate_zoom(curve, -5.0F).height, in.height);
}

CY_TEST_CASE("camera_strategy: the anchor follows terrain height, from a terrain query") {
    // "WHEN the camera pans across terrain THEN its anchor height SHALL come from terrain height
    // queries, not from per-frame physics casts." The service is a separate interface from the
    // collision batch precisely so a host cannot answer this with a cast by accident.
    StrategyState state;
    state.anchor = Vec3{0.0F, 0.0F, 0.0F};
    RampTerrain terrain;
    const ZoomCurve curve;
    const MapBounds bounds;
    Transform pose;
    Lens lens;

    StrategyInput input;
    input.pan = Vec2{50.0F, 0.0F};
    input.dt = 0.2F;
    CY_REQUIRE(advance_strategy(state, input, curve, bounds, &terrain, pose, lens).has_value());
    CY_CHECK_NEAR(state.anchor.x, 10.0F, 1e-4F);
    CY_CHECK_NEAR(state.anchor.y, 1.0F, 1e-4F);
    CY_CHECK_EQ(terrain.calls, 1U);
    CY_CHECK_EQ(terrain.samples, 1U);

    // With no terrain service the anchor keeps its own height — an editor preview, not a special
    // path through the same function.
    const f32 held = state.anchor.y;
    CY_REQUIRE(advance_strategy(state, input, curve, bounds, nullptr, pose, lens).has_value());
    CY_CHECK_EQ(state.anchor.y, held);
}

CY_TEST_CASE("camera_strategy: map bounds confine the anchor, as a rectangle and as a polygon") {
    // "Map bounds SHALL constrain the camera anchor to a rectangle, polygon, or world region."
    StrategyState state;
    RampTerrain terrain;
    const ZoomCurve curve;
    Transform pose;
    Lens lens;

    MapBounds rectangle;
    rectangle.region = Aabb::from_min_max(Vec3{-5.0F, -100.0F, -5.0F}, Vec3{5.0F, 100.0F, 5.0F});
    StrategyInput input;
    input.pan = Vec2{100.0F, 0.0F};
    input.dt = 1.0F;
    CY_REQUIRE(advance_strategy(state, input, curve, rectangle, &terrain, pose, lens).has_value());
    CY_CHECK_NEAR(state.anchor.x, 5.0F, 1e-4F);
    CY_CHECK(state.at_bounds);

    // A triangle, counter-clockwise. A point pushed outside comes back to its nearest edge.
    const Vec2 triangle[3] = {Vec2{0.0F, 0.0F}, Vec2{10.0F, 0.0F}, Vec2{0.0F, 10.0F}};
    MapBounds polygon;
    polygon.polygon = Span<const Vec2>(triangle, 3);
    StrategyState inside;
    inside.anchor = Vec3{1.0F, 0.0F, 1.0F};
    StrategyInput push;
    push.pan = Vec2{20.0F, 0.0F};
    push.dt = 1.0F;
    CY_REQUIRE(advance_strategy(inside, push, curve, polygon, nullptr, pose, lens).has_value());
    CY_CHECK(inside.at_bounds);
    CY_CHECK_LE(inside.anchor.x, 10.001F);
}

CY_TEST_CASE("camera_strategy: edge scrolling comes from a pointer in a viewport") {
    // "Edge scrolling SHALL be derived from a pointer position exposed as an action, and SHALL NOT
    // be implemented in platform mouse handling." Nothing in this call knows what a mouse is.
    const render::ViewportRect viewport{1000, 0, 1000, 1000};

    CY_CHECK_EQ(edge_scroll(viewport, Vec2{1500.0F, 500.0F}).x, 0.0F);
    CY_CHECK_GT(edge_scroll(viewport, Vec2{1995.0F, 500.0F}).x, 0.5F);
    CY_CHECK_LT(edge_scroll(viewport, Vec2{1005.0F, 500.0F}).x, -0.5F);
    CY_CHECK_GT(edge_scroll(viewport, Vec2{1500.0F, 5.0F}).y, 0.5F);

    // A pointer in the OTHER player's half of a split screen scrolls nothing here, which is the
    // whole reason the viewport is a parameter.
    const Vec2 elsewhere = edge_scroll(viewport, Vec2{10.0F, 500.0F});
    CY_CHECK_EQ(elsewhere.x, 0.0F);
    CY_CHECK_EQ(elsewhere.y, 0.0F);
}

CY_TEST_CASE("camera_strategy: advancing backwards is refused rather than integrated") {
    StrategyState state;
    const ZoomCurve curve;
    const MapBounds bounds;
    Transform pose;
    Lens lens;
    StrategyInput input;
    input.dt = -0.1F;
    const Status advanced = advance_strategy(state, input, curve, bounds, nullptr, pose, lens);
    CY_REQUIRE_FALSE(advanced.has_value());
    CY_CHECK_EQ(advanced.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("camera_assist: assistance is a modifier on the look, and it is observable") {
    // "WHEN aim assistance is active THEN diagnostics SHALL show the raw look, the assisted look,
    // and the candidate that influenced it."
    AimAssistSettings settings;
    settings.magnetism = 2.0F;
    settings.slowdown = 0.5F;
    settings.slowdown_radians = 0.2F;
    settings.capture_radians = 0.3F;

    AimCandidate candidates[2];
    candidates[0].direction = Vec3{0.05F, 0.0F, -1.0F};
    candidates[0].weight = 1.0F;
    candidates[1].direction = Vec3{1.0F, 0.0F, 0.0F};  // outside the capture window
    candidates[1].weight = 5.0F;

    AimAssistReport report;
    const Vec2 assisted =
        apply_aim_assist(settings, Vec2{0.4F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F},
                         Span<const AimCandidate>(candidates, 2), 1.0F / 60.0F, report);
    CY_CHECK(report.active);
    CY_CHECK_EQ(report.candidate, 0U);
    CY_CHECK_EQ(report.raw_look.x, 0.4F);
    CY_CHECK_NE(report.assisted_look.x, report.raw_look.x);
    CY_CHECK_EQ(assisted.x, report.assisted_look.x);

    // NOTHING IN RANGE: the report says inactive rather than the caller having to infer it, and the
    // look comes back untouched — a heavier candidate outside the window does not steal assistance.
    AimAssistReport quiet;
    const Vec2 untouched =
        apply_aim_assist(settings, Vec2{0.4F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F},
                         Span<const AimCandidate>(candidates + 1, 1), 1.0F / 60.0F, quiet);
    CY_CHECK_FALSE(quiet.active);
    CY_CHECK_EQ(untouched.x, 0.4F);
    CY_CHECK_EQ(quiet.candidate, AimAssistReport::kNoCandidate);

    // ZERO SETTINGS ARE THE IDENTITY: assistance off is assistance that does nothing, not a hidden
    // constant somewhere else.
    AimAssistReport off_report;
    const AimAssistSettings off;
    const Vec2 raw =
        apply_aim_assist(off, Vec2{0.4F, 0.1F}, Vec3{0.0F, 0.0F, -1.0F},
                         Span<const AimCandidate>(candidates, 1), 1.0F / 60.0F, off_report);
    CY_CHECK_EQ(raw.x, 0.4F);
    CY_CHECK_EQ(raw.y, 0.1F);
}

CY_TEST_CASE("camera_director: a better shot is taken, and repetition stops it oscillating") {
    // "Director scoring SHALL consider visibility of the action, target importance, activity, and
    // shot repetition", and "the selection SHALL blend rather than cut unnecessarily".
    DirectorState director(allocator());
    DirectorWeights weights;
    weights.minimum_shot_seconds = 1.0F;
    Array<ShotScore> scores(allocator());

    ShotCandidate candidates[2];
    candidates[0].name = Name::intern("wide");
    candidates[0].visibility = 0.9F;
    candidates[0].importance = 0.5F;
    candidates[0].activity = 0.5F;
    candidates[1].name = Name::intern("close");
    candidates[1].visibility = 0.4F;
    candidates[1].importance = 0.5F;
    candidates[1].activity = 0.5F;

    CY_REQUIRE(director.score_shots(Span<const ShotCandidate>(candidates, 2), weights, scores)
                   .has_value());
    CY_REQUIRE_EQ(scores.size(), 2U);
    CY_CHECK_EQ(scores[0].name, Name::intern("wide"));
    CY_CHECK_EQ(director.select(scores.span(), weights), Name::intern("wide"));
    CY_CHECK(director.changed());

    // The close shot becomes the better one — but the minimum shot length holds the wide one.
    candidates[1].visibility = 1.0F;
    candidates[0].visibility = 0.1F;
    CY_REQUIRE(director.score_shots(Span<const ShotCandidate>(candidates, 2), weights, scores)
                   .has_value());
    director.advance(0.2F);
    CY_CHECK_EQ(director.select(scores.span(), weights), Name::intern("wide"));
    CY_CHECK_FALSE(director.changed());

    // Past the minimum, it changes — and reports that it changed, which is the caller's cue to
    // blend rather than cut.
    director.advance(1.0F);
    CY_CHECK_EQ(director.select(scores.span(), weights), Name::intern("close"));
    CY_CHECK(director.changed());

    // And the shot it just left carries a repetition penalty, so a momentary swing does not send it
    // straight back.
    candidates[0].visibility = 1.0F;
    candidates[1].visibility = 0.95F;
    director.advance(1.5F);
    CY_REQUIRE(director.score_shots(Span<const ShotCandidate>(candidates, 2), weights, scores)
                   .has_value());
    f32 wide_penalty = 0.0F;
    for (const ShotScore& score : scores.span()) {
        if (score.name == Name::intern("wide")) {
            wide_penalty = score.repetition_penalty;
        }
    }
    CY_CHECK_GT(wide_penalty, 0.0F);
    CY_CHECK_EQ(director.select(scores.span(), weights), Name::intern("close"));
}

CY_TEST_CASE("camera_director: the repetition memory decays, so a shot becomes available again") {
    DirectorState director(allocator());
    DirectorWeights weights;
    weights.minimum_shot_seconds = 0.0F;
    weights.repetition_half_life = 1.0F;
    Array<ShotScore> scores(allocator());

    ShotCandidate candidates[2];
    candidates[0].name = Name::intern("a");
    candidates[0].visibility = 1.0F;
    candidates[1].name = Name::intern("b");
    candidates[1].visibility = 0.9F;

    CY_REQUIRE(director.score_shots(Span<const ShotCandidate>(candidates, 2), weights, scores)
                   .has_value());
    CY_CHECK_EQ(director.select(scores.span(), weights), Name::intern("a"));
    candidates[1].visibility = 1.2F;
    CY_REQUIRE(director.score_shots(Span<const ShotCandidate>(candidates, 2), weights, scores)
                   .has_value());
    CY_CHECK_EQ(director.select(scores.span(), weights), Name::intern("b"));

    // "a" is penalised now, and ten half-lives later it is not — a shot used a minute ago should be
    // available again rather than blacklisted.
    CY_REQUIRE(director.score_shots(Span<const ShotCandidate>(candidates, 2), weights, scores)
                   .has_value());
    f32 penalty_now = 0.0F;
    for (const ShotScore& score : scores.span()) {
        if (score.name == Name::intern("a")) {
            penalty_now = score.repetition_penalty;
        }
    }
    CY_CHECK_GT(penalty_now, 0.0F);

    director.advance(10.0F);
    CY_REQUIRE(director.score_shots(Span<const ShotCandidate>(candidates, 2), weights, scores)
                   .has_value());
    for (const ShotScore& score : scores.span()) {
        if (score.name == Name::intern("a")) {
            // Ten half-lives later the penalty is a thousandth of what it was — small enough that a
            // shot's own score decides again, which is what "available again rather than
            // blacklisted" means. It is not asserted to be exactly zero: the memory decays
            // geometrically and a floor at exactly zero would be a second rule nobody asked for.
            CY_CHECK_LT(score.repetition_penalty, penalty_now * 0.01F);
        }
    }
}
