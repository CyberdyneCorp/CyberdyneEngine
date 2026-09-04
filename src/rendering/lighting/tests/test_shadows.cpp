// The shadow atlas, the directional cascades, and the bias model. Task 4.4.2.
//
// The cascade cases are the ones worth reading: "shadows do not swim as the camera rotates" is a
// property of two matrices, and a matrix is testable without a GPU. A golden image would show the
// same thing much later and much less precisely.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/lighting/cascades.h>
#include <cy/rendering/lighting/filtering.h>
#include <cy/rendering/lighting/shadow_atlas.h>

#include <cmath>

namespace {

using cy::rendering::CascadeCamera;
using cy::rendering::CascadeSettings;
using cy::rendering::ShadowAtlas;
using cy::rendering::ShadowAtlasConfig;
using cy::rendering::ShadowCascade;
using cy::rendering::ShadowRequest;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

ShadowAtlasConfig small_atlas() noexcept {
    ShadowAtlasConfig config;
    config.size = 1024;
    config.min_tile = 256;  // a 4x4 grid: sixteen cells
    config.max_tile = 512;
    config.retention_frames = 4;
    return config;
}

CascadeCamera make_camera() noexcept {
    CascadeCamera camera;
    camera.position = cy::Vec3{0.0F, 2.0F, 0.0F};
    camera.forward = cy::Vec3{0.0F, 0.0F, -1.0F};
    camera.up = cy::Vec3{0.0F, 1.0F, 0.0F};
    camera.right = cy::Vec3{1.0F, 0.0F, 0.0F};
    camera.near_plane = 0.1F;
    return camera;
}

}  // namespace

CY_TEST_CASE("a bigger light gets a bigger tile, and a hysteresis band keeps it") {
    ShadowAtlas atlas(allocator());
    CY_REQUIRE(atlas.initialize(small_atlas()).has_value());
    atlas.begin_frame(1);

    ShadowRequest request;
    request.light_id = 7;
    request.screen_coverage = 0.4F;  // above the reference: the largest tile
    cy::Expected<cy::rendering::ShadowAssignment, cy::Error> assignment = atlas.request(request);
    CY_REQUIRE(assignment.has_value());
    CY_CHECK_EQ(assignment->tile.size, 512U);
    CY_CHECK(assignment->needs_render);

    // A small drop in coverage does NOT resize, because a resize is a re-render and a light sitting
    // on a size boundary would pay one every frame.
    atlas.begin_frame(2);
    request.screen_coverage = 0.2F;
    assignment = atlas.request(request);
    CY_REQUIRE(assignment.has_value());
    CY_CHECK_EQ(assignment->tile.size, 512U);
    CY_CHECK_EQ(atlas.statistics().resizes, 0U);

    // A large one does.
    atlas.begin_frame(3);
    request.screen_coverage = 0.01F;
    assignment = atlas.request(request);
    CY_REQUIRE(assignment.has_value());
    CY_CHECK_EQ(assignment->tile.size, 256U);
    CY_CHECK_EQ(atlas.statistics().resizes, 1U);
}

CY_TEST_CASE("an unchanged light and caster set reuses its shadow without re-rendering") {
    // "WHEN a static light with only static casters has already been rendered THEN its shadow map
    // SHALL be reused without re-rendering."
    ShadowAtlas atlas(allocator());
    CY_REQUIRE(atlas.initialize(small_atlas()).has_value());

    ShadowRequest request;
    request.light_id = 1;
    request.screen_coverage = 0.4F;
    request.light_version = 5;
    request.caster_version = 9;

    atlas.begin_frame(1);
    cy::Expected<cy::rendering::ShadowAssignment, cy::Error> first = atlas.request(request);
    CY_REQUIRE(first.has_value());
    CY_CHECK(first->needs_render);

    atlas.begin_frame(2);
    cy::Expected<cy::rendering::ShadowAssignment, cy::Error> second = atlas.request(request);
    CY_REQUIRE(second.has_value());
    CY_CHECK_FALSE(second->needs_render);
    CY_CHECK_EQ(second->tile.x, first->tile.x);
    CY_CHECK_EQ(second->tile.y, first->tile.y);
    CY_CHECK_EQ(atlas.statistics().cache_hits, 1U);

    // A caster moved: the shadow is stale and must be re-rendered, in the same tile.
    atlas.begin_frame(3);
    request.caster_version = 10;
    cy::Expected<cy::rendering::ShadowAssignment, cy::Error> third = atlas.request(request);
    CY_REQUIRE(third.has_value());
    CY_CHECK(third->needs_render);
}

CY_TEST_CASE("an oversubscribed atlas reports the shortfall instead of corrupting a tile") {
    ShadowAtlas atlas(allocator());
    CY_REQUIRE(atlas.initialize(small_atlas()).has_value());
    atlas.begin_frame(1);

    // Four 512² tiles fill a 1024² atlas exactly. Requests arrive in descending importance, which
    // is the caller's contract, so the fifth is the least important light in the frame.
    for (cy::u64 light = 0; light < 4; ++light) {
        ShadowRequest request;
        request.light_id = light;
        request.screen_coverage = 0.4F;
        CY_REQUIRE(atlas.request(request).has_value());
    }
    ShadowRequest overflow;
    overflow.light_id = 99;
    overflow.screen_coverage = 0.4F;
    CY_CHECK_FALSE(atlas.request(overflow).has_value());
    CY_CHECK_EQ(atlas.statistics().shortfall, 1U);
    // The four that fit are untouched: a light that misses out renders unshadowed, and nothing else
    // changes.
    CY_CHECK_EQ(atlas.statistics().tiles_live, 4U);
}

CY_TEST_CASE("the retention period stops two lights thrashing over one tile") {
    ShadowAtlasConfig config = small_atlas();
    config.max_tile = 1024;  // one tile fills the whole atlas
    ShadowAtlas atlas(allocator());
    CY_REQUIRE(atlas.initialize(config).has_value());

    ShadowRequest first;
    first.light_id = 1;
    first.screen_coverage = 1.0F;
    atlas.begin_frame(1);
    CY_REQUIRE(atlas.request(first).has_value());

    // A second light wants the same tile one frame later. Inside the retention period it does not
    // get it — which is what stops both re-rendering every frame forever.
    ShadowRequest second;
    second.light_id = 2;
    second.screen_coverage = 1.0F;
    atlas.begin_frame(2);
    CY_CHECK_FALSE(atlas.request(second).has_value());

    // Past the retention period the tile is fair game.
    atlas.begin_frame(1 + config.retention_frames + 1);
    CY_REQUIRE(atlas.request(second).has_value());
    CY_CHECK_EQ(atlas.statistics().evictions, 1U);
}

CY_TEST_CASE("releasing a light's tile frees its cells") {
    ShadowAtlas atlas(allocator());
    CY_REQUIRE(atlas.initialize(small_atlas()).has_value());
    atlas.begin_frame(1);
    ShadowRequest request;
    request.light_id = 3;
    request.screen_coverage = 0.4F;
    CY_REQUIRE(atlas.request(request).has_value());
    CY_CHECK_EQ(atlas.statistics().cells_used, 4U);  // a 512 tile is 2x2 cells of 256

    // A light switched to the virtual path "SHALL consume no atlas tile".
    atlas.release(3);
    CY_CHECK_EQ(atlas.statistics().cells_used, 0U);
    CY_CHECK_FALSE(atlas.tile_of(3).valid());
}

CY_TEST_CASE("cascade splits blend the logarithmic and uniform distributions") {
    CascadeSettings settings;
    settings.count = 4;
    settings.shadow_distance = 100.0F;

    cy::f32 uniform[4] = {};
    settings.split_blend = 0.0F;
    CY_REQUIRE(cy::rendering::compute_cascade_splits(settings, 1.0F, cy::Span<cy::f32>(uniform, 4))
                   .has_value());
    CY_CHECK_NEAR(uniform[0], 1.0F + (99.0F * 0.25F), 1e-3F);
    CY_CHECK_NEAR(uniform[3], 100.0F, 1e-3F);

    cy::f32 logarithmic[4] = {};
    settings.split_blend = 1.0F;
    CY_REQUIRE(
        cy::rendering::compute_cascade_splits(settings, 1.0F, cy::Span<cy::f32>(logarithmic, 4))
            .has_value());
    CY_CHECK_NEAR(logarithmic[0], std::pow(100.0F, 0.25F), 1e-3F);
    CY_CHECK_NEAR(logarithmic[3], 100.0F, 1e-3F);

    // The logarithmic distribution puts far more resolution near the camera, which is the whole
    // reason the blend defaults towards it.
    CY_CHECK_LT(logarithmic[0], uniform[0]);
}

CY_TEST_CASE("a cascade's radius does not change when the camera rotates") {
    // THE PROPERTY THAT STOPS SHADOWS CRAWLING. A box fitted to the frustum corners changes size as
    // the camera turns; a sphere does not, and a constant size is what makes a texel a fixed world
    // size.
    CascadeSettings settings;
    settings.count = 2;
    ShadowCascade first[2];
    ShadowCascade rotated[2];

    CascadeCamera camera = make_camera();
    CY_REQUIRE(cy::rendering::compute_cascades(settings, camera, cy::Vec3{0.0F, -1.0F, 0.2F},
                                               cy::Span<ShadowCascade>(first, 2))
                   .has_value());

    // Yaw by 40 degrees: a new orthonormal basis for the same camera position.
    const cy::f32 angle = 0.7F;
    camera.forward = cy::Vec3{std::sin(angle), 0.0F, -std::cos(angle)};
    camera.right = cy::Vec3{std::cos(angle), 0.0F, std::sin(angle)};
    CY_REQUIRE(cy::rendering::compute_cascades(settings, camera, cy::Vec3{0.0F, -1.0F, 0.2F},
                                               cy::Span<ShadowCascade>(rotated, 2))
                   .has_value());

    for (cy::u32 index = 0; index < 2; ++index) {
        CY_CHECK_NEAR(rotated[index].radius, first[index].radius, first[index].radius * 1e-4F);
        CY_CHECK_NEAR(rotated[index].texel_world_size, first[index].texel_world_size,
                      first[index].texel_world_size * 1e-4F);
    }
}

CY_TEST_CASE("the cascade centre is snapped to whole texels") {
    // The other half of stabilisation: with a fixed radius the remaining motion is the centre
    // sliding, and rounding it to a lattice fixed in the LIGHT's space removes it. So a fixed world
    // point must land on the same texel of the shadow map, up to a whole number of texels, however
    // the camera moves — a fractional shift is exactly what shows as crawling.
    CascadeSettings settings;
    settings.count = 1;
    settings.resolution = 512;

    const cy::Vec3 probe{3.0F, 0.0F, -20.0F};
    const auto texel_of = [&](const ShadowCascade& cascade) {
        const cy::Vec3 clip = cy::transform_point(cascade.view_projection, probe);
        return ((clip.x * 0.5F) + 0.5F) * static_cast<cy::f32>(settings.resolution);
    };

    ShadowCascade first[1];
    ShadowCascade nudged[1];
    CascadeCamera camera = make_camera();
    CY_REQUIRE(cy::rendering::compute_cascades(settings, camera, cy::Vec3{0.0F, -1.0F, 0.0F},
                                               cy::Span<ShadowCascade>(first, 1))
                   .has_value());
    // A shift that is not a whole number of texels: the snapping has to absorb the fraction.
    camera.position.x += first[0].texel_world_size * 3.37F;
    CY_REQUIRE(cy::rendering::compute_cascades(settings, camera, cy::Vec3{0.0F, -1.0F, 0.0F},
                                               cy::Span<ShadowCascade>(nudged, 1))
                   .has_value());
    CY_CHECK_NEAR(nudged[0].radius, first[0].radius, first[0].radius * 1e-4F);

    const cy::f32 difference = texel_of(nudged[0]) - texel_of(first[0]);
    CY_CHECK_NEAR(difference - std::round(difference), 0.0F, 2e-2F);
    // And it did move — a snapping that pinned the projection would pass the test above trivially.
    CY_CHECK_GT(std::abs(difference), 0.5F);
}

CY_TEST_CASE("cascade selection blends at the boundary and fades past the last one") {
    CascadeSettings settings;
    settings.count = 3;
    settings.shadow_distance = 90.0F;
    settings.transition_fraction = 0.2F;
    settings.fade_start_fraction = 0.8F;

    ShadowCascade cascades[3];
    CY_REQUIRE(cy::rendering::compute_cascades(settings, make_camera(), cy::Vec3{0.0F, -1.0F, 0.0F},
                                               cy::Span<ShadowCascade>(cascades, 3))
                   .has_value());
    const cy::Span<const ShadowCascade> span(cascades, 3);

    // Well inside the first cascade: no blend, no fade.
    const cy::rendering::CascadeLookup inside =
        cy::rendering::select_cascade(settings, span, cascades[0].near_distance + 0.01F);
    CY_CHECK_EQ(inside.index, 0U);
    CY_CHECK_EQ(inside.blend, 0.0F);
    CY_CHECK_EQ(inside.fade, 1.0F);

    // At the very edge of the first cascade: both are sampled, which is what removes the seam.
    const cy::rendering::CascadeLookup edge =
        cy::rendering::select_cascade(settings, span, cascades[0].far_distance - 1e-4F);
    CY_CHECK_EQ(edge.index, 0U);
    CY_CHECK_GT(edge.blend, 0.9F);

    // Past the fade start: shadowing reaches zero smoothly rather than stopping.
    const cy::rendering::CascadeLookup far_away =
        cy::rendering::select_cascade(settings, span, 89.0F);
    CY_CHECK_LT(far_away.fade, 1.0F);
    CY_CHECK_EQ(cy::rendering::select_cascade(settings, span, 200.0F).fade, 0.0F);
}

CY_TEST_CASE(
    "the bias grows with the angle to the light, and the normal offset vanishes at normal "
    "incidence") {
    cy::rendering::ShadowBiasSettings settings;
    // Face-on to the light: the slope term contributes nothing and the constant is all that is
    // left.
    CY_CHECK_NEAR(cy::rendering::shadow_depth_bias(settings, 1.0F), settings.constant, 1e-6F);
    // Grazing: more bias, and bounded rather than unbounded.
    const cy::f32 grazing = cy::rendering::shadow_depth_bias(settings, 0.05F);
    CY_CHECK_GT(grazing, settings.constant);
    CY_CHECK_LE(grazing, settings.constant + (settings.slope_scaled * 4.0F) + 1e-6F);

    // THE NORMAL OFFSET IS WHAT KEEPS A CONTACT SHADOW ATTACHED. At normal incidence it is zero, so
    // a shadow at a contact point is not displaced at all — which is the peter-panning scenario.
    const cy::Vec3 normal{0.0F, 1.0F, 0.0F};
    const cy::Vec3 at_normal = cy::rendering::shadow_normal_offset(settings, normal, 1.0F, 0.05F);
    CY_CHECK_NEAR(length(at_normal), 0.0F, 1e-6F);
    const cy::Vec3 at_grazing = cy::rendering::shadow_normal_offset(settings, normal, 0.1F, 0.05F);
    CY_CHECK_GT(length(at_grazing), 0.0F);
}

CY_TEST_CASE("PCSS finds no penumbra when nothing blocks, and widens as the blocker recedes") {
    // REVERSED-Z: a blocker is nearer and therefore has the GREATER depth. A search that found
    // nothing must produce no kernel rather than a negative one.
    CY_CHECK_EQ(cy::rendering::pcss_penumbra_radius(0.5F, 0.5F, 0.01F), 0.0F);
    CY_CHECK_EQ(cy::rendering::pcss_penumbra_radius(0.5F, 0.2F, 0.01F), 0.0F);

    const cy::f32 contact = cy::rendering::pcss_penumbra_radius(0.79F, 0.8F, 0.01F);
    const cy::f32 distant = cy::rendering::pcss_penumbra_radius(0.4F, 0.8F, 0.01F);
    CY_CHECK_GT(contact, 0.0F);
    CY_CHECK_GT(distant, contact);  // sharp at contact, softening with distance
}

CY_TEST_CASE("the sample count is the specialization constant's, and the tiers differ") {
    CY_CHECK_EQ(cy::rendering::shadow_sample_count(cy::rendering::ShadowQuality::Low), 1U);
    CY_CHECK_LT(cy::rendering::shadow_sample_count(cy::rendering::ShadowQuality::Medium),
                cy::rendering::shadow_sample_count(cy::rendering::ShadowQuality::Ultra));
    CY_CHECK_GE(cy::rendering::blocker_search_sample_count(cy::rendering::ShadowQuality::Low), 4U);
}
