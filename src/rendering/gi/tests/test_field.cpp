// The sparse distance field: clipmaps that scroll, bricks that are not stored, sphere tracing and
// sky visibility. Task 9.1.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/distance_field.h>

#include "support.h"

#include <cmath>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::rendering::gi::ClipmapSettings;
using cy::rendering::gi::DistanceField;
using cy::rendering::gi::ScrollReport;
using cy::rendering::gi::SphereTraceHit;
using gi_support::BoxField;

/// Two levels at a quarter-metre finest voxel. Small enough for an integration budget and large
/// enough that a small object occupies a small fraction of the window — which is what makes
/// "only the newly exposed region was re-solved" a measurement rather than a rounding.
ClipmapSettings small_settings() noexcept {
    ClipmapSettings settings;
    settings.levels = 2;
    settings.resolution = 32;
    settings.base_extent_metres = 8.0F;
    return settings;
}

}  // namespace

CY_TEST_CASE("a box composites into the world field and the field agrees with the box") {
    const BoxField box(Vec3{1.0F, 1.0F, 1.0F});
    DistanceField field;
    CY_REQUIRE(field.configure(small_settings()).has_value());
    CY_REQUIRE(field.place(1, box.asset(), cy::Mat4::identity()).has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});

    // Inside is negative, on the face is about zero, and outside grows with distance.
    CY_CHECK_LT(field.distance(Vec3{0.0F, 0.0F, 0.0F}), 0.0F);
    CY_CHECK_LT(std::abs(field.distance(Vec3{1.0F, 0.0F, 0.0F})), 0.35F);
    CY_CHECK_GT(field.distance(Vec3{2.0F, 0.0F, 0.0F}), 0.5F);
    CY_CHECK_EQ(field.placement_count(), 1U);
}

CY_TEST_CASE("empty space costs no storage") {
    // "The representation SHALL be sparse: regions containing no surface SHALL not consume
    // storage." The control is the same field with a box in it, which does.
    DistanceField empty;
    CY_REQUIRE(empty.configure(small_settings()).has_value());
    (void)empty.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK_EQ(empty.diagnostics().allocated_bricks, 0U);
    CY_CHECK_EQ(empty.diagnostics().bytes, 0U);
    CY_CHECK_GT(empty.diagnostics().empty_bricks, 0U);

    const BoxField box(Vec3{1.0F, 1.0F, 1.0F});
    DistanceField occupied;
    CY_REQUIRE(occupied.configure(small_settings()).has_value());
    CY_REQUIRE(occupied.place(1, box.asset(), cy::Mat4::identity()).has_value());
    (void)occupied.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK_GT(occupied.diagnostics().allocated_bricks, 0U);
    CY_CHECK_GT(occupied.diagnostics().bytes, 0U);
    // And the surface is a small part of the world, so most bricks are still empty.
    CY_CHECK_GT(occupied.diagnostics().empty_bricks, occupied.diagnostics().allocated_bricks);
}

CY_TEST_CASE("the camera translates and only the newly exposed region is re-solved") {
    const BoxField box(Vec3{1.0F, 1.0F, 1.0F});
    DistanceField field;
    CY_REQUIRE(field.configure(small_settings()).has_value());
    CY_REQUIRE(field.place(1, box.asset(), cy::Mat4::identity()).has_value());

    const ScrollReport first = field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK_GT(first.bricks_solved, 0U);
    CY_CHECK_EQ(first.bricks_reused, 0U);

    // The same place again: everything is reused and nothing is solved.
    const ScrollReport again = field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK_EQ(again.bricks_solved, 0U);
    CY_CHECK_EQ(again.bricks_reused, first.bricks_solved);

    // One brick along: the overwhelming majority of the window is reused.
    const ScrollReport moved = field.scroll_to(Vec3{2.0F, 0.0F, 0.0F});
    CY_CHECK_GT(moved.bricks_solved, 0U);
    CY_CHECK_GT(moved.bricks_reused, moved.bricks_solved * 3U);
    CY_CHECK_GT(moved.bricks_freed, 0U);
}

CY_TEST_CASE("a door that opens invalidates what it touched and nothing else") {
    // A tight margin on the door's own field: the margin is part of what a move invalidates, and a
    // metre of it around a half-metre door would make the invalidated region mostly margin.
    const BoxField door(Vec3{0.5F, 1.0F, 0.1F}, 0.3F, 9);
    DistanceField field;
    CY_REQUIRE(field.configure(small_settings()).has_value());
    CY_REQUIRE(field.place(7, door.asset(), cy::Mat4::from_translation(Vec3{0.0F, 0.0F, 0.0F}))
                   .has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    const u32 window = field.diagnostics().last_scroll.bricks_solved +
                       field.diagnostics().last_scroll.bricks_reused;

    // The door swings to one side. A transform, never a regeneration.
    CY_REQUIRE(field.move(7, cy::Mat4::from_translation(Vec3{3.0F, 0.0F, 0.0F})).has_value());
    const ScrollReport after = field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK_GT(after.bricks_solved, 0U);
    // Only the bricks it left and the bricks it entered: a small fraction of the window, not the
    // world. A global rebuild would report `window`.
    CY_CHECK_LT(after.bricks_solved, window / 3U);
    CY_CHECK_GT(after.bricks_reused, after.bricks_solved);

    // And the field followed it: the old place is open and the new place is solid.
    CY_CHECK_GT(field.distance(Vec3{0.0F, 0.0F, 0.0F}), 0.3F);
    CY_CHECK_LT(field.distance(Vec3{3.0F, 0.0F, 0.0F}), 0.2F);
}

CY_TEST_CASE("sphere tracing hits a surface and reports where it grazed one") {
    const BoxField box(Vec3{1.0F, 1.0F, 1.0F});
    DistanceField field;
    CY_REQUIRE(field.configure(small_settings()).has_value());
    CY_REQUIRE(field.place(1, box.asset(), cy::Mat4::identity()).has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});

    cy::Ray straight;
    straight.origin = Vec3{0.0F, 0.0F, 6.0F};
    straight.direction = Vec3{0.0F, 0.0F, -1.0F};
    const SphereTraceHit hit = field.sphere_trace(straight, 12.0F);
    CY_REQUIRE(hit.hit);
    CY_CHECK_NEAR(hit.t, 5.0F, 0.4F);
    CY_CHECK_GT(hit.normal.z, 0.7F);

    // A ray that misses cleanly, well away from the box, reports a high cone ratio; one that passes
    // close reports a low one, which is what the software tier's confidence is built on.
    cy::Ray grazing;
    grazing.origin = Vec3{1.15F, 0.0F, 6.0F};
    grazing.direction = Vec3{0.0F, 0.0F, -1.0F};
    const SphereTraceHit close = field.sphere_trace(grazing, 12.0F);
    CY_CHECK_FALSE(close.hit);

    cy::Ray clear;
    clear.origin = Vec3{6.0F, 0.0F, 6.0F};
    clear.direction = Vec3{0.0F, 0.0F, -1.0F};
    const SphereTraceHit far = field.sphere_trace(clear, 12.0F);
    CY_CHECK_FALSE(far.hit);
    // The clean miss never came near anything; the grazing one passed within a voxel or two of the
    // box. That difference is what the software tier's confidence is built on.
    CY_CHECK_GT(far.closest_approach_metres, close.closest_approach_metres);
    CY_CHECK_LT(close.closest_approach_metres, 0.5F);
}

CY_TEST_CASE("tracing the field toward the sky reports occlusion indoors") {
    // "WHEN a surface is indoors THEN tracing the field toward the sky SHALL report occlusion, and
    // the surface SHALL not receive full outdoor irradiance."
    const BoxField ceiling(Vec3{3.0F, 0.2F, 3.0F});
    DistanceField field;
    CY_REQUIRE(field.configure(small_settings()).has_value());
    CY_REQUIRE(field.place(1, ceiling.asset(), cy::Mat4::from_translation(Vec3{0.0F, 2.0F, 0.0F}))
                   .has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});

    const f32 indoors = field.sky_visibility(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, 16);
    const f32 outdoors = field.sky_visibility(Vec3{7.0F, 0.0F, 7.0F}, Vec3{0.0F, 1.0F, 0.0F}, 16);
    CY_CHECK_LT(indoors, 0.4F);
    CY_CHECK_GT(outdoors, 0.9F);
    CY_CHECK_GE(indoors, 0.0F);
}

CY_TEST_CASE("a misconfigured clipmap is refused rather than silently corrected") {
    DistanceField field;
    ClipmapSettings settings = small_settings();
    settings.levels = 0;
    CY_CHECK_FALSE(field.configure(settings).has_value());
    settings = small_settings();
    settings.resolution = 6;  // not a multiple of the brick edge
    CY_CHECK_FALSE(field.configure(settings).has_value());
    settings = small_settings();
    settings.base_extent_metres = 0.0F;
    CY_CHECK_FALSE(field.configure(settings).has_value());
}
