// Motion vectors and the classification derived from them. Task 8.3.
//
// The conventions are asserted rather than commented, because a sign error here is invisible in a
// still frame and produces a trail that looks like a tuning problem in whatever pass consumed it.

#include <cy/test/test.h>

#include <cy/core/math/projection.h>
#include <cy/rendering/temporal/motion.h>
#include <cy/rendering/temporal/reprojection.h>

#include <cmath>

namespace {

using cy::rendering::classify_history;
using cy::rendering::HistoryState;
using cy::rendering::motion_pixels;
using cy::rendering::reproject;
using cy::rendering::ReprojectionInputs;
using cy::rendering::SurfaceMotion;
using cy::rendering::SurfaceMotionInputs;

cy::Mat4 camera_at(cy::Vec3 eye) noexcept {
    const cy::Mat4 view = cy::look_at(eye, eye + cy::Vec3{0.0F, 0.0F, -1.0F});
    const cy::Mat4 projection =
        cy::perspective_reversed_z(cy::math::radians(60.0F), 16.0F / 9.0F, 0.1F, 1000.0F);
    return projection * view;
}

}  // namespace

CY_TEST_CASE("a static surface under a static camera has exactly zero motion") {
    SurfaceMotionInputs inputs;
    inputs.current_world = cy::Vec3{1.0F, 2.0F, -10.0F};
    inputs.previous_world = inputs.current_world;
    inputs.current_view_projection = camera_at(cy::Vec3{0.0F, 0.0F, 0.0F});
    inputs.previous_view_projection = inputs.current_view_projection;

    const SurfaceMotion motion = derive_surface_motion(inputs);
    CY_REQUIRE(motion.representable);
    CY_CHECK_NEAR(motion.motion.x, 0.0F, 1e-6F);
    CY_CHECK_NEAR(motion.motion.y, 0.0F, 1e-6F);
    // The convention: a motion vector takes a pixel to where its surface was.
    const cy::Vec2 history = reproject(motion.current_uv, motion.motion);
    CY_CHECK_NEAR(history.x, motion.current_uv.x, 1e-6F);
}

CY_TEST_CASE("a motion vector points at the past, and reprojection lands where the surface was") {
    // The surface moves right; the camera does not. Its motion vector must point LEFT, because
    // every consumer of a motion vector is reading history.
    SurfaceMotionInputs inputs;
    inputs.previous_world = cy::Vec3{0.0F, 0.0F, -10.0F};
    inputs.current_world = cy::Vec3{1.0F, 0.0F, -10.0F};
    inputs.current_view_projection = camera_at(cy::Vec3{0.0F, 0.0F, 0.0F});
    inputs.previous_view_projection = inputs.current_view_projection;

    const SurfaceMotion motion = derive_surface_motion(inputs);
    CY_REQUIRE(motion.representable);
    CY_CHECK_LT(motion.motion.x, 0.0F);

    // And the reprojection lands exactly on where the surface was, which is the property every
    // consumer depends on and the one a sign error breaks.
    SurfaceMotionInputs was = inputs;
    was.current_world = inputs.previous_world;
    const SurfaceMotion previous_position = derive_surface_motion(was);
    const cy::Vec2 history = reproject(motion.current_uv, motion.motion);
    CY_CHECK_NEAR(history.x, previous_position.current_uv.x, 1e-5F);
    CY_CHECK_NEAR(history.y, previous_position.current_uv.y, 1e-5F);

    // Speed in pixels, which is what a neighbourhood clamp widens on.
    CY_CHECK_GT(motion_pixels(motion.motion, 1920, 1080), 1.0F);
}

CY_TEST_CASE("camera motion alone produces motion vectors, with no per-system work anywhere") {
    // The surface is static and the camera moves. Nothing about the surface was consulted: this is
    // "motion vectors are derived, not authored" in one case.
    SurfaceMotionInputs inputs;
    inputs.current_world = cy::Vec3{0.0F, 0.0F, -10.0F};
    inputs.previous_world = inputs.current_world;
    inputs.previous_view_projection = camera_at(cy::Vec3{-1.0F, 0.0F, 0.0F});
    inputs.current_view_projection = camera_at(cy::Vec3{0.0F, 0.0F, 0.0F});

    const SurfaceMotion motion = derive_surface_motion(inputs);
    CY_REQUIRE(motion.representable);
    CY_CHECK(std::fabs(motion.motion.x) > 1e-4F);
}

CY_TEST_CASE("a surface behind the camera is marked unrepresentable rather than smeared") {
    SurfaceMotionInputs inputs;
    inputs.current_view_projection = camera_at(cy::Vec3{0.0F, 0.0F, 0.0F});
    inputs.previous_view_projection = inputs.current_view_projection;

    // Behind now.
    inputs.current_world = cy::Vec3{0.0F, 0.0F, 10.0F};
    inputs.previous_world = cy::Vec3{0.0F, 0.0F, -10.0F};
    CY_CHECK_FALSE(derive_surface_motion(inputs).representable);

    // In front now, behind last frame — which is a disocclusion the projection cannot express.
    inputs.current_world = cy::Vec3{0.0F, 0.0F, -10.0F};
    inputs.previous_world = cy::Vec3{0.0F, 0.0F, 10.0F};
    const SurfaceMotion emerging = derive_surface_motion(inputs);
    CY_CHECK_FALSE(emerging.representable);
    // The current position is still handed back, because the caller wants it either way.
    CY_CHECK_GT(emerging.current_uv.x, 0.0F);
}

CY_TEST_CASE("the four history states, and the one that means a consumer may accumulate") {
    ReprojectionInputs inputs;
    inputs.current_uv = cy::Vec2{0.5F, 0.5F};
    inputs.motion = cy::Vec2{0.0F, 0.0F};
    inputs.current_depth = 10.0F;
    inputs.history_depth = 10.0F;

    CY_CHECK_EQ(classify_history(inputs).state, HistoryState::Valid);
    CY_CHECK(cy::rendering::history_usable(HistoryState::Valid));
    CY_CHECK_FALSE(cy::rendering::history_usable(HistoryState::Disoccluded));

    // Geometry revealed from behind an occluder: the history sample is a different surface.
    inputs.history_depth = 40.0F;
    CY_CHECK_EQ(classify_history(inputs).state, HistoryState::Disoccluded);
    inputs.history_depth = 10.2F;  // within tolerance: the same surface, slightly closer
    CY_CHECK_EQ(classify_history(inputs).state, HistoryState::Valid);

    // Off the screen.
    inputs.history_depth = 10.0F;
    inputs.motion = cy::Vec2{-0.8F, 0.0F};
    const cy::rendering::ReprojectionResult out = classify_history(inputs);
    CY_CHECK_EQ(out.state, HistoryState::OutOfFrame);
    CY_CHECK_NEAR(out.history_uv.x, -0.3F, 1e-6F);

    // Unrepresentable motion, and an invalidated history, are the SAME state to a consumer: both
    // mean reconstruct spatially, and a consumer that had to combine a state with a separate flag
    // is a consumer that can forget the flag.
    inputs.motion = cy::Vec2{0.0F, 0.0F};
    inputs.representable = false;
    CY_CHECK_EQ(classify_history(inputs).state, HistoryState::Unrepresentable);
    inputs.representable = true;
    inputs.history_valid = false;
    CY_CHECK_EQ(classify_history(inputs).state, HistoryState::Unrepresentable);

    cy::rendering::ClassificationCounts counts;
    counts.record(HistoryState::Valid);
    counts.record(HistoryState::Valid);
    counts.record(HistoryState::Disoccluded);
    CY_CHECK_EQ(counts.total(), 3U);
    CY_CHECK_NEAR(counts.fraction(HistoryState::Valid), 2.0F / 3.0F, 1e-6F);
    counts.reset();
    CY_CHECK_EQ(counts.fraction(HistoryState::Valid), 0.0F);
}
