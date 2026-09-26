// SPDX-License-Identifier: MIT
// Screen-space contact shadows on the host. `integration.rendering_contact_shadows`.
//
// `contact_shadow_reference` — contact_shadows.slang, expression for expression — over the analytic
// scene in contact_scene.h: a floor, and a box resting on it. The geometry, not a rasteriser, says
// which surface points really have the box between them and the light within the trace's length,
// so each case compares the trace against the truth rather than against a picture.

#include "contact_scene.h"

#include <cy/rendering/contact_shadows/contact.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace cy;
using namespace cy::contact_test;
using rendering::contact_shadows::ContactShadowConstants;
using rendering::contact_shadows::ContactShadowInputs;
using rendering::contact_shadows::ContactShadowSettings;

namespace {

std::vector<f32> trace(const ContactScene& scene, const ContactShadowSettings& settings,
                       ContactShadowConstants* constants_out = nullptr) {
    const auto constants = rendering::contact_shadows::make_contact_constants(settings, scene.view);
    CY_REQUIRE(constants.has_value());
    if (constants_out != nullptr) {
        *constants_out = *constants;
    }
    ContactShadowInputs inputs;
    inputs.width = scene.view.width;
    inputs.height = scene.view.height;
    inputs.depth = Span<const f32>(scene.depth.data(), scene.depth.size());
    inputs.normals = Span<const Vec2>(scene.normals.data(), scene.normals.size());
    std::vector<f32> out(scene.depth.size(), -1.0F);
    CY_REQUIRE(rendering::contact_shadows::contact_shadow_reference(
                   inputs, *constants, Span<f32>(out.data(), out.size()))
                   .has_value());
    return out;
}

/// Where the box really is along the trace from a pixel's lifted start toward the light, in metres,
/// or infinity: the geometry's answer to "is this point in contact shadow".
f32 truth(const ContactScene& scene, const ContactShadowConstants& constants,
          const ContactShadowSettings& settings, usize pixel) {
    const Vec3 point = scene.points[pixel];
    const f32 distance = -point.z;
    const f32 footprint = 2.0F * distance / (constants.projection[1] * constants.extent[1]);
    const Vec3 start =
        point + (scene.surface_normals[pixel] * (settings.normal_offset_pixels * footprint));
    Vec3 face{0.0F, 0.0F, 0.0F};
    return ray_box(start, scene.view.to_light, kBox, face);
}

/// Whether the screen can see the box where the trace enters it: the entry point lies within
/// `thickness` behind the first surface the camera's ray toward it meets. Where it does not — the
/// trace enters through a face turned away from the camera, deeper than the thickness behind the
/// face the camera sees — no screen-space trace can find it, and the map is what answers.
bool entry_visible(const ContactScene& scene, const ContactShadowConstants& constants,
                   const ContactShadowSettings& settings, usize pixel, f32 along) {
    const Vec3 point = scene.points[pixel];
    const f32 distance = -point.z;
    const f32 footprint = 2.0F * distance / (constants.projection[1] * constants.extent[1]);
    const Vec3 start =
        point + (scene.surface_normals[pixel] * (settings.normal_offset_pixels * footprint));
    const Vec3 entry = start + (scene.view.to_light * along);
    // The camera's ray through the entry point, at unit view depth.
    const Vec3 ray = entry * (1.0F / -entry.z);
    f32 nearest = ray.y < 0.0F ? kFloorHeight / ray.y : INFINITY;
    Vec3 face{0.0F, 0.0F, 0.0F};
    nearest = std::min(nearest, ray_box(Vec3{0.0F, 0.0F, 0.0F}, ray, kBox, face));
    return -entry.z - nearest < settings.thickness;
}

}  // namespace

CY_TEST_CASE("an open plane traces to exactly one, at every light elevation") {
    ContactScene scene = make_contact_scene(false);
    const ContactShadowSettings settings;
    // Grazing, middling and steep: a trace that rounded its taps onto the plane's own depth would
    // darken the grazing case first.
    const Vec3 lights[3] = {Vec3{-0.7F, 0.12F, -0.7F}, Vec3{-0.5F, 0.7F, -0.5F},
                            Vec3{0.1F, 0.98F, 0.1F}};
    for (const Vec3 light : lights) {
        scene.view.to_light = light * (1.0F / std::sqrt(dot(light, light)));
        const std::vector<f32> visibility = trace(scene, settings);
        u32 dark = 0;
        for (const f32 value : visibility) {
            dark += value != 1.0F ? 1U : 0U;
        }
        CY_CHECK_EQ(dark, 0U);
    }
}

CY_TEST_CASE(
    "a contact darkens where the box is between the floor and the light, and nowhere else") {
    const ContactScene scene = make_contact_scene(true);
    const ContactShadowSettings settings;
    ContactShadowConstants constants;
    const std::vector<f32> visibility = trace(scene, settings, &constants);

    u32 darkened = 0;
    u32 false_dark = 0;
    u32 beyond_reach_dark = 0;
    u32 near_truth = 0;
    u32 near_seen = 0;
    u32 near_found = 0;
    for (usize pixel = 0; pixel < visibility.size(); ++pixel) {
        if (scene.surface[pixel] == 0) {
            CY_CHECK_EQ(visibility[pixel], 1.0F);
            continue;
        }
        const f32 along = truth(scene, constants, settings, pixel);
        if (visibility[pixel] < 1.0F) {
            ++darkened;
            // The trace found an occluder the geometry does not have within its reach.
            false_dark += along > settings.length ? 1U : 0U;
            // Farther than the trace reaches, even by the lift: never.
            beyond_reach_dark += along > (settings.length * 1.5F) ? 1U : 0U;
        }
        // The first half of the trace is full shadow; the floor there is where the contact is.
        if (scene.surface[pixel] == 1 && along < settings.length * 0.45F) {
            ++near_truth;
            const bool seen = entry_visible(scene, constants, settings, pixel, along);
            near_seen += seen ? 1U : 0U;
            near_found += seen && visibility[pixel] < 0.5F ? 1U : 0U;
        }
    }
    std::fprintf(stderr,
                 "contact: %u pixel(s) darkened, %u without the box within reach (%u beyond 1.5x "
                 "reach); %u floor pixels with the box within the first half, %u of them where the "
                 "screen sees the box, %u of those found\n",
                 darkened, false_dark, beyond_reach_dark, near_truth, near_seen, near_found);
    CY_CHECK_GT(darkened, 100U);
    CY_CHECK_GT(near_truth, 100U);
    // Where the screen can see the occluder, the trace finds it. The rest — the trace entering the
    // box through a face turned away from the camera — is screen space's own blind spot, and the
    // reason the frame keeps the map's answer beside this one.
    CY_CHECK_GT(near_seen, 50U);
    CY_CHECK_GE(static_cast<f32>(near_found), 0.95F * static_cast<f32>(near_seen));
    // Silhouette taps: a tap behind a grazing face by less than the thickness is inside the box
    // only until the ray exits it. Bounded, and never far from the box.
    CY_CHECK_LE(static_cast<f32>(false_dark), 0.02F * static_cast<f32>(darkened));
    CY_CHECK_EQ(beyond_reach_dark, 0U);
}

CY_TEST_CASE(
    "refinement is the budget lever: none traces nothing, and a shorter reach drops the far "
    "receivers") {
    const ContactScene scene = make_contact_scene(true);
    const ContactShadowSettings authored;
    const std::vector<f32> full = trace(scene, authored);
    const std::vector<f32> none =
        trace(scene, rendering::contact_shadows::apply_refinement(authored, 0.0F));
    u32 dark_full = 0;
    u32 dark_none = 0;
    for (usize pixel = 0; pixel < full.size(); ++pixel) {
        dark_full += full[pixel] < 1.0F ? 1U : 0U;
        dark_none += none[pixel] < 1.0F ? 1U : 0U;
    }
    CY_CHECK_GT(dark_full, 0U);
    CY_CHECK_EQ(dark_none, 0U);

    // A shorter reach — what a lever value between one and zero does to the authored forty metres —
    // drops every receiver beyond it: the box four metres away is not traced at three.
    ContactShadowSettings near = authored;
    near.max_distance = 3.0F;
    const std::vector<f32> shortened = trace(scene, near);
    u32 dark_far = 0;
    for (usize pixel = 0; pixel < shortened.size(); ++pixel) {
        const bool far = scene.surface[pixel] != 0 && -scene.points[pixel].z > near.max_distance;
        dark_far += far && shortened[pixel] < 1.0F ? 1U : 0U;
    }
    CY_CHECK_EQ(dark_far, 0U);
    CY_CHECK_EQ(rendering::contact_shadows::apply_refinement(authored, 0.5F).max_distance,
                authored.max_distance * 0.5F);
}

CY_TEST_CASE("the constants refuse an empty view and a degenerate trace") {
    rendering::contact_shadows::ContactShadowView view;
    CY_CHECK_FALSE(rendering::contact_shadows::make_contact_constants(ContactShadowSettings{}, view)
                       .has_value());
    view.width = 4;
    view.height = 4;
    ContactShadowSettings settings;
    settings.steps = 0;
    CY_CHECK_FALSE(rendering::contact_shadows::make_contact_constants(settings, view).has_value());
    settings = ContactShadowSettings{};
    settings.length = 0.0F;
    CY_CHECK_FALSE(rendering::contact_shadows::make_contact_constants(settings, view).has_value());
    CY_CHECK(rendering::contact_shadows::make_contact_constants(ContactShadowSettings{}, view)
                 .has_value());
}
