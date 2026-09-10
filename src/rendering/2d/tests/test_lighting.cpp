// 2D lights, one-sided occluders, the screen-space distance field and Camera2D. M8.b task 9.5.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/2d/lighting.h>
#include <cy/test/test.h>

#include <cmath>

using namespace cy;
using namespace cy::rendering2d;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

[[nodiscard]] Light2D point_light(Vec2 position, u32 mask = 0xFFFFFFFFU) noexcept {
    Light2D light;
    light.kind = Light2DKind::Point;
    light.position = position;
    light.layer_mask = mask;
    return light;
}

}  // namespace

CY_TEST_CASE("light2d: an unlit layer costs nothing — no light is even examined") {
    // "WHEN a layer is marked unlit THEN 2D lights SHALL not affect it and no lighting cost SHALL
    // be incurred for it."
    const Light2D lights[3] = {point_light(Vec2{0.0F, 0.0F}), point_light(Vec2{10.0F, 0.0F}),
                               point_light(Vec2{20.0F, 0.0F})};
    Array<u32> gathered(allocator());
    LightingReport report;

    Layer2D lit;
    lit.index = 0;
    lit.lit = true;
    CY_REQUIRE(gather_lights(Span<const Light2D>(lights, 3), lit, gathered, report).has_value());
    CY_CHECK_EQ(gathered.size(), 3U);
    CY_CHECK_EQ(report.lights_considered, 3U);

    Layer2D unlit;
    unlit.index = 1;
    unlit.lit = false;
    LightingReport skipped;
    CY_REQUIRE(gather_lights(Span<const Light2D>(lights, 3), unlit, gathered, skipped).has_value());
    CY_CHECK_EQ(gathered.size(), 0U);
    // NOT ONE LIGHT EXAMINED. The cost is the measurement.
    CY_CHECK_EQ(skipped.lights_considered, 0U);
    CY_CHECK_EQ(skipped.unlit_layers, 1U);
}

CY_TEST_CASE("light2d: a layer mask and a dead light are both skipped") {
    const Light2D lights[3] = {point_light(Vec2{}, 0b0001U), point_light(Vec2{}, 0b0010U),
                               point_light(Vec2{}, 0xFFFFFFFFU)};
    Light2D dark = lights[2];
    dark.energy = 0.0F;
    const Light2D set[4] = {lights[0], lights[1], lights[2], dark};

    Layer2D layer;
    layer.index = 1;  // bit 1
    Array<u32> gathered(allocator());
    LightingReport report;
    CY_REQUIRE(gather_lights(Span<const Light2D>(set, 4), layer, gathered, report).has_value());
    CY_REQUIRE_EQ(gathered.size(), 2U);
    CY_CHECK_EQ(gathered[0], 1U);  // the light whose mask names layer 1
    CY_CHECK_EQ(gathered[1], 2U);  // and the one that names everything
    CY_CHECK_EQ(report.lights_considered, 4U);
    CY_CHECK_EQ(report.lights_active, 2U);
}

CY_TEST_CASE("light2d: a one-sided occluder blocks from one side and passes from the other") {
    // "WHEN an occluder uses one-sided culling THEN light SHALL pass from one side and be blocked
    // from the other, letting a light inside a room escape through the wall's inner face."
    const Vec2 wall[2] = {Vec2{0.0F, 0.0F}, Vec2{10.0F, 0.0F}};

    const Light2D above = point_light(Vec2{5.0F, 5.0F});
    const Light2D below = point_light(Vec2{5.0F, -5.0F});

    // Counter-clockwise culling: the front face is the side the light is to the LEFT of a→b.
    CY_CHECK(edge_casts(above, wall[0], wall[1], OccluderCulling::CounterClockwise));
    CY_CHECK_FALSE(edge_casts(below, wall[0], wall[1], OccluderCulling::CounterClockwise));
    // Clockwise culling is the mirror image.
    CY_CHECK_FALSE(edge_casts(above, wall[0], wall[1], OccluderCulling::Clockwise));
    CY_CHECK(edge_casts(below, wall[0], wall[1], OccluderCulling::Clockwise));
    // And a two-sided occluder blocks from both.
    CY_CHECK(edge_casts(above, wall[0], wall[1], OccluderCulling::Both));
    CY_CHECK(edge_casts(below, wall[0], wall[1], OccluderCulling::Both));
}

CY_TEST_CASE("light2d: a shadow volume is extruded away from the light") {
    const Vec2 wall[2] = {Vec2{-1.0F, 0.0F}, Vec2{1.0F, 0.0F}};
    Occluder2D occluder;
    occluder.points = Span<const Vec2>(wall, 2);
    occluder.closed = false;
    occluder.culling = OccluderCulling::Both;

    const Light2D light = point_light(Vec2{0.0F, -10.0F});
    Array<ShadowQuad> quads(allocator());
    LightingReport report;
    CY_REQUIRE(build_shadow(light, occluder, 100.0F, quads, report).has_value());
    CY_REQUIRE_EQ(quads.size(), 1U);
    // The far edge is on the far side of the wall from the light.
    CY_CHECK_GT(quads[0].far_a.y, quads[0].near_a.y);
    CY_CHECK_EQ(report.shadow_quads, 1U);

    // A light that does not cast shadows produces none — the cheapest possible saving, and it is
    // per light rather than per frame.
    Light2D fill = light;
    fill.casts_shadows = false;
    quads.clear();
    LightingReport quiet;
    CY_REQUIRE(build_shadow(fill, occluder, 100.0F, quads, quiet).has_value());
    CY_CHECK_EQ(quads.size(), 0U);
}

CY_TEST_CASE("light2d: a directional light extrudes along its direction, not away from a point") {
    const Vec2 wall[2] = {Vec2{-1.0F, 0.0F}, Vec2{1.0F, 0.0F}};
    Occluder2D occluder;
    occluder.points = Span<const Vec2>(wall, 2);
    occluder.closed = false;

    Light2D sun;
    sun.kind = Light2DKind::Directional;
    sun.direction = Vec2{0.0F, 1.0F};
    Array<ShadowQuad> quads(allocator());
    LightingReport report;
    CY_REQUIRE(build_shadow(sun, occluder, 50.0F, quads, report).has_value());
    CY_REQUIRE_EQ(quads.size(), 1U);
    // Both far corners moved the SAME way: a directional shadow does not spread.
    CY_CHECK_NEAR(quads[0].far_a.y, 50.0F, 1e-3F);
    CY_CHECK_NEAR(quads[0].far_b.y, 50.0F, 1e-3F);
    CY_CHECK_NEAR(quads[0].far_b.x - quads[0].far_a.x, 2.0F, 1e-3F);
}

CY_TEST_CASE("sdf2d: the field is negative inside an occluder and grows outside it") {
    const Vec2 box[4] = {Vec2{40.0F, 40.0F}, Vec2{60.0F, 40.0F}, Vec2{60.0F, 60.0F},
                         Vec2{40.0F, 60.0F}};
    Occluder2D occluder;
    occluder.points = Span<const Vec2>(box, 4);
    occluder.closed = true;

    SdfSettings settings;
    settings.scale = 0.5F;
    settings.oversize = 0.0F;
    SignedDistanceField field(allocator());
    CY_REQUIRE(rasterise_sdf(Span<const Occluder2D>(&occluder, 1),
                             Rect2D{0.0F, 0.0F, 100.0F, 100.0F}, settings, field)
                   .has_value());
    CY_CHECK_EQ(field.width(), 50U);
    CY_CHECK_EQ(field.height(), 50U);

    // Inside the box: negative. Outside: positive and growing with distance.
    CY_CHECK_LT(field.sample(Vec2{50.0F, 50.0F}), 0.0F);
    CY_CHECK_GT(field.sample(Vec2{70.0F, 50.0F}), 0.0F);
    CY_CHECK_GT(field.sample(Vec2{90.0F, 50.0F}), field.sample(Vec2{70.0F, 50.0F}));
    // And near the edge it is near zero, which is what a collision response reads.
    CY_CHECK_LT(std::fabs(field.sample(Vec2{60.0F, 50.0F})), 3.0F);
}

CY_TEST_CASE("sdf2d: the oversize lets an occluder just off screen contribute") {
    // "WHEN the SDF is oversized beyond the viewport THEN occluders just off screen SHALL still
    // contribute" — which matters because a particle can be pushed by geometry it cannot see.
    const Vec2 box[4] = {Vec2{110.0F, 40.0F}, Vec2{130.0F, 40.0F}, Vec2{130.0F, 60.0F},
                         Vec2{110.0F, 60.0F}};
    Occluder2D occluder;
    occluder.points = Span<const Vec2>(box, 4);
    occluder.closed = true;
    const Rect2D viewport{0.0F, 0.0F, 100.0F, 100.0F};

    // THE RESOLUTION IS THE INSTRUMENT'S, NOT THE PROPERTY'S. What is asserted below is geometric —
    // an occluder outside the viewport contributes to an oversized field and not to a tight one —
    // and it is true at any sampling rate. At `scale = 0.5` the two rasterisations cost 2.681 ms in
    // the Debug configuration against the unit tier's one millisecond, which M8.b's closing gate
    // found; hard rule 7 says a case that expensive belongs in the tier above or gets cheaper, and
    // a distance field sampled a fifth as finely still knows where its own occluder is.
    SdfSettings tight;
    tight.oversize = 0.0F;
    tight.scale = 0.1F;
    SignedDistanceField small(allocator());
    CY_REQUIRE(
        rasterise_sdf(Span<const Occluder2D>(&occluder, 1), viewport, tight, small).has_value());
    CY_CHECK_LE(small.bounds().width, 100.5F);

    SdfSettings wide;
    wide.oversize = 0.6F;
    wide.scale = 0.1F;
    SignedDistanceField big(allocator());
    CY_REQUIRE(
        rasterise_sdf(Span<const Occluder2D>(&occluder, 1), viewport, wide, big).has_value());
    CY_CHECK_GT(big.bounds().width, 150.0F);
    // The off-screen box is inside the oversized field, so a sample there is negative.
    CY_CHECK_LT(big.sample(Vec2{120.0F, 50.0F}), 0.0F);
}

CY_TEST_CASE("camera2d: smoothing settles over the same time at any frame rate") {
    Camera2D fast;
    fast.smoothing_half_life = 0.25F;
    Camera2D slow = fast;

    for (u32 step = 0; step < 144U; ++step) {
        advance_camera(fast, Vec2{100.0F, 0.0F}, 1.0F / 144.0F);
    }
    for (u32 step = 0; step < 60U; ++step) {
        advance_camera(slow, Vec2{100.0F, 0.0F}, 1.0F / 60.0F);
    }
    CY_CHECK_NEAR(fast.position.x, slow.position.x, 0.5F);
    // And after a second at a quarter-second half-life it is most of the way there.
    CY_CHECK_GT(fast.position.x, 90.0F);
}

CY_TEST_CASE("camera2d: limits confine the position and pixel-perfect snaps it") {
    Camera2D camera;
    camera.limited = true;
    camera.limits = Rect2D{0.0F, 0.0F, 50.0F, 50.0F};
    advance_camera(camera, Vec2{500.0F, -20.0F}, 1.0F / 60.0F);
    CY_CHECK_EQ(camera.position.x, 50.0F);
    CY_CHECK_EQ(camera.position.y, 0.0F);

    Camera2D snapped;
    snapped.pixel_perfect = true;
    snapped.pixels_per_unit = 4.0F;  // a quarter-unit texel grid
    advance_camera(snapped, Vec2{1.1F, 2.3F}, 1.0F / 60.0F);
    CY_CHECK_NEAR(snapped.position.x, 1.0F, 1e-4F);
    CY_CHECK_NEAR(snapped.position.y, 2.25F, 1e-4F);
}

CY_TEST_CASE("camera2d: the expand strategy shows more world rather than stretching") {
    // "WHEN the window aspect changes with the 'expand' strategy THEN more of the world SHALL
    // become visible rather than the image stretching."
    Camera2D camera;
    camera.reference = Vec2{640.0F, 360.0F};
    camera.strategy = CanvasStrategy::ExpandCanvas;

    const Rect2D at_reference = visible_bounds(camera, Vec2{1280.0F, 720.0F});
    const Rect2D at_wide = visible_bounds(camera, Vec2{2560.0F, 720.0F});
    CY_CHECK_NEAR(at_reference.width, 640.0F, 0.5F);
    CY_CHECK_GT(at_wide.width, at_reference.width);
    CY_CHECK_NEAR(at_wide.height, at_reference.height, 0.5F);

    // A scaled canvas shows the SAME world however wide the window is: the image stretches, which
    // is the documented behaviour of that strategy rather than a defect.
    camera.strategy = CanvasStrategy::ScaledCanvas;
    CY_CHECK_NEAR(visible_bounds(camera, Vec2{2560.0F, 720.0F}).width, 640.0F, 0.5F);
}

CY_TEST_CASE("camera2d: a world point maps into the view, with zoom and offset") {
    Camera2D camera;
    camera.reference = Vec2{640.0F, 360.0F};
    camera.strategy = CanvasStrategy::ScaledCanvas;
    camera.position = Vec2{100.0F, 100.0F};

    const Vec2 output{640.0F, 360.0F};
    // The camera's own position is the centre of the view.
    const Vec2 centre = world_to_view(camera, output, Vec2{100.0F, 100.0F});
    CY_CHECK_NEAR(centre.x, 320.0F, 0.5F);
    CY_CHECK_NEAR(centre.y, 180.0F, 0.5F);

    // Zoomed in, the same world offset covers more pixels.
    camera.zoom = 2.0F;
    const Vec2 zoomed = world_to_view(camera, output, Vec2{110.0F, 100.0F});
    camera.zoom = 1.0F;
    const Vec2 plain = world_to_view(camera, output, Vec2{110.0F, 100.0F});
    CY_CHECK_GT(zoomed.x - 320.0F, plain.x - 320.0F);
}
