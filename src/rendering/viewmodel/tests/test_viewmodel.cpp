// SPDX-License-Identifier: MIT
// The first-person weapon view, with no device: the squeezed projection and what it guarantees,
// the two views one camera produces, the lens across frames, and the view-block words.
//
// `camera-system` — "One camera, several views": "WHEN a first-person camera renders the world and
// a separate weapon view THEN both SHALL be render views derived from one evaluated camera". The
// device suite (`render.weapon_view`) holds the frame these numbers produce to the pixels.

#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/viewmodel/viewmodel.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstring>

using cy::f32;
using cy::Mat4;
using cy::u32;
using cy::Vec3;
using cy::Vec4;
using namespace cy::rendering;
using namespace cy::rendering::viewmodel;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

constexpr cy::render::LayerMask kWeaponLayer = 1U << 4U;
constexpr u32 kWidth = 320;
constexpr u32 kHeight = 180;
constexpr f32 kAspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);

/// Reversed-Z NDC depth of a view-space point through `projection`.
[[nodiscard]] f32 ndc_depth(const Mat4& projection, Vec3 view_point) {
    const Vec4 clip = projection * Vec4{view_point.x, view_point.y, view_point.z, 1.0F};
    return clip.z / clip.w;
}

[[nodiscard]] ViewmodelSettings weapon_settings() {
    ViewmodelSettings settings;
    settings.layers = kWeaponLayer;
    settings.vertical_fov_radians = 0.9F;
    settings.near_plane = 0.01F;
    settings.far_plane = 10.0F;
    return settings;
}

}  // namespace

CY_TEST_CASE("the squeezed depth puts every weapon fragment in front of every world surface") {
    const ViewmodelSettings settings = weapon_settings();
    const Mat4 weapon = viewmodel_projection(settings, kAspect);
    const Mat4 world = cy::perspective_reversed_z(1.3F, kAspect, 0.1F, 500.0F);
    const f32 clear = guaranteed_clear_distance(0.1F, settings.depth_fraction);
    CY_CHECK_NEAR(clear, 0.1F / (1.0F - settings.depth_fraction), 1e-7F);
    // The weapon's FARTHEST point against the world's NEAREST surface past the guarantee: the
    // weapon still wins. Every pair in between is a smaller margin of the same sign.
    const f32 weapon_far = ndc_depth(weapon, Vec3{0.0F, 0.0F, -settings.far_plane * 0.999F});
    const f32 world_near = ndc_depth(world, Vec3{0.0F, 0.0F, -clear * 1.0001F});
    CY_CHECK(weapon_far > world_near);
    CY_CHECK(weapon_far >= 1.0F - settings.depth_fraction - 1e-6F);
    // And it is a slice, not a constant: the weapon still orders itself.
    const f32 near_part = ndc_depth(weapon, Vec3{0.0F, 0.0F, -0.2F});
    const f32 far_part = ndc_depth(weapon, Vec3{0.0F, 0.0F, -0.21F});
    CY_CHECK(near_part > far_part);
}

CY_TEST_CASE("the squeeze moves depth alone: the weapon lands on its own projection's pixels") {
    const ViewmodelSettings settings = weapon_settings();
    const Mat4 squeezed = viewmodel_projection(settings, kAspect, true);
    const Mat4 plain = viewmodel_projection(settings, kAspect, false);
    const Vec4 point{0.07F, -0.05F, -0.4F, 1.0F};
    const Vec4 a = squeezed * point;
    const Vec4 b = plain * point;
    CY_CHECK_EQ(a.x, b.x);
    CY_CHECK_EQ(a.y, b.y);
    CY_CHECK_EQ(a.w, b.w);
    CY_CHECK_NEAR(a.z / a.w,
                  (1.0F - settings.depth_fraction) + (settings.depth_fraction * b.z / b.w), 1e-6F);
}

CY_TEST_CASE("a lens that is not one is refused") {
    ViewmodelSettings settings = weapon_settings();
    CY_CHECK(static_cast<bool>(validate(settings)));
    settings.vertical_fov_radians = 0.0F;
    CY_CHECK(!validate(settings));
    settings = weapon_settings();
    settings.near_plane = 0.0F;
    CY_CHECK(!validate(settings));
    settings = weapon_settings();
    settings.far_plane = settings.near_plane;
    CY_CHECK(!validate(settings));
    settings = weapon_settings();
    settings.depth_fraction = 0.0F;
    CY_CHECK(!validate(settings));
    settings.depth_fraction = 0.75F;
    CY_CHECK(!validate(settings));
}

CY_TEST_CASE("the weapon view is accepted only as a second view of the primary view's camera") {
    cy::render::ViewDescription primary;
    primary.purpose = cy::render::ViewPurpose::Primary;
    primary.camera.translation = Vec3{3.0F, 1.7F, 2.0F};
    primary.history_id = 10;
    cy::render::ViewDescription weapon = primary;
    weapon.purpose = cy::render::ViewPurpose::Viewmodel;
    weapon.history_id = 11;
    weapon.layer_mask = kWeaponLayer;
    weapon.projection.fov_y_radians = 0.8F;
    weapon.projection.near_plane = 0.01F;
    weapon.projection.far_plane = 0.0F;  // infinite, the camera's default
    const ViewmodelShadowPolicy shadows;

    const auto settings = settings_from_views(primary, weapon, shadows);
    CY_REQUIRE(settings.has_value());
    CY_CHECK_EQ(settings->layers, kWeaponLayer);
    CY_CHECK_NEAR(settings->vertical_fov_radians, 0.8F, 1e-7F);
    CY_CHECK_EQ(settings->far_plane, kDefaultViewmodelFar);

    cy::render::ViewDescription moved = weapon;
    moved.camera.translation.x += 0.01F;  // a second camera
    CY_CHECK(!settings_from_views(primary, moved, shadows));
    cy::render::ViewDescription shared = weapon;
    shared.history_id = primary.history_id;
    CY_CHECK(!settings_from_views(primary, shared, shadows));
    cy::render::ViewDescription not_weapon = weapon;
    not_weapon.purpose = cy::render::ViewPurpose::Primary;
    CY_CHECK(!settings_from_views(primary, not_weapon, shadows));
    cy::render::ViewDescription empty = weapon;
    empty.layer_mask = cy::render::kNoLayers;
    CY_CHECK(!settings_from_views(primary, empty, shadows));
}

CY_TEST_CASE("last frame's lens is the weapon's previous projection, and a reset forgets it") {
    TemporalFramework temporal(allocator());
    CY_REQUIRE(temporal.initialize(TemporalConfig{}).has_value());
    TemporalView view;
    view.width = kWidth;
    view.height = kHeight;
    view.projection = cy::perspective_reversed_z(1.2F, kAspect, 0.1F, 200.0F);
    view.view = Mat4::identity();
    temporal.begin_frame(view);

    ViewmodelSettings settings = weapon_settings();
    ViewmodelFrame frame;
    const Mat4 rotation = Mat4::identity();
    const ViewmodelMatrices first = frame.advance(settings, kAspect, rotation, temporal);
    // The first frame has no last lens: the two are one.
    CY_CHECK(std::memcmp(&first.relative_to_clip, &first.previous_relative_to_clip, sizeof(Mat4)) ==
             0);

    temporal.begin_frame(view);
    settings.vertical_fov_radians = 0.7F;  // the sights come up
    const ViewmodelMatrices second = frame.advance(settings, kAspect, rotation, temporal);
    const Mat4 old_lens = viewmodel_projection(weapon_settings(), kAspect);
    CY_CHECK(std::memcmp(&second.previous_relative_to_clip, &old_lens, sizeof(Mat4)) == 0);
    CY_CHECK(std::memcmp(&second.relative_to_clip, &second.previous_relative_to_clip,
                         sizeof(Mat4)) != 0);

    frame.reset();
    temporal.begin_frame(view);
    const ViewmodelMatrices third = frame.advance(settings, kAspect, rotation, temporal);
    CY_CHECK(std::memcmp(&third.relative_to_clip, &third.previous_relative_to_clip, sizeof(Mat4)) ==
             0);
}

CY_TEST_CASE("the view block carries the rows, the world's jitter and the contact policy") {
    ViewmodelSettings settings = weapon_settings();
    ViewmodelMatrices matrices;
    matrices.relative_to_clip = viewmodel_projection(settings, kAspect);
    matrices.previous_relative_to_clip = matrices.relative_to_clip;
    pipeline::FrameViewData data;
    write_viewmodel(matrices, settings, cy::Vec2{0.5F, -0.25F}, kWidth, kHeight, data);
    // Row 0 picked up the jitter times row 3 (`upload_for`'s rule); row 2 (depth) did not move.
    const Mat4& m = matrices.relative_to_clip;
    for (u32 column = 0; column < 4; ++column) {
        const f32 jittered =
            m.columns[column].x + ((0.5F * 2.0F / static_cast<f32>(kWidth)) * m.columns[column].w);
        CY_CHECK_EQ(data.viewmodel_to_clip[column], jittered);
        CY_CHECK_EQ(data.viewmodel_to_clip[8 + column], m.columns[column].z);
        CY_CHECK_EQ(data.previous_viewmodel_to_clip[column], m.columns[column].x);
    }
    CY_CHECK_EQ(data.viewmodel_control[0], 0U);
    settings.shadows.contact = true;
    write_viewmodel(matrices, settings, cy::Vec2{0.0F, 0.0F}, kWidth, kHeight, data);
    CY_CHECK_EQ(data.viewmodel_control[0], kViewmodelContactShadows);
    // A frame with no weapon view never writes these words: the default block reads zero.
    const pipeline::FrameViewData untouched;
    CY_CHECK_EQ(untouched.viewmodel_control[0], 0U);
}
