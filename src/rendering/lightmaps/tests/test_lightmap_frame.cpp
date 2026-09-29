// SPDX-License-Identifier: MIT
// Which ambient source a surface takes in the frame, and the view-block words.
// `unit.lightmap_frame`.

#include <cy/rendering/lightmaps/lightmap_textures.h>
#include <cy/test/test.h>

#include <cstring>
#include <vector>

namespace {

using namespace cy::rendering::lightmaps;  // NOLINT(google-build-using-namespace)
namespace gi = cy::rendering::gi;
namespace bake = cy::rendering::lightmap_bake;
using cy::u32;

[[nodiscard]] bake::BakedLightmap directional_lightmap() {
    bake::BakedLightmap lightmap;
    lightmap.mode = bake::LightmapMode::Directional;
    lightmap.page_size = 256;
    lightmap.pages = 2;
    lightmap.gutter_texels = 4;
    lightmap.texels.width = 256;
    lightmap.texels.height = 512;
    lightmap.texels.planes = 2;
    return lightmap;
}

}  // namespace

CY_TEST_CASE("the frame's ambient source is read off gi::exclusion_for") {
    // A lightmapped surface takes its lightmap in every mode that bakes one — even inside a volume,
    // which is "no double counting" in the frame's one-source form.
    for (const gi::GiMode mode : {gi::GiMode::Baked, gi::GiMode::Hybrid}) {
        CY_CHECK_EQ(frame_ambient_source(mode, true, true), AmbientSource::Lightmap);
        CY_CHECK_EQ(frame_ambient_source(mode, true, false), AmbientSource::Lightmap);
        CY_CHECK_EQ(frame_ambient_source(mode, false, true), AmbientSource::IrradianceVolume);
        CY_CHECK_EQ(frame_ambient_source(mode, false, false), AmbientSource::Sky);
    }
    // `Probe` is baked probes for dynamic objects and no lightmaps; `Dynamic` and `None` take
    // neither.
    CY_CHECK_EQ(frame_ambient_source(gi::GiMode::Probe, true, true),
                AmbientSource::IrradianceVolume);
    CY_CHECK_EQ(frame_ambient_source(gi::GiMode::Dynamic, true, true), AmbientSource::Sky);
    CY_CHECK_EQ(frame_ambient_source(gi::GiMode::None, true, true), AmbientSource::Sky);
}

CY_TEST_CASE(
    "write_lightmaps fills the words a mode admits, and leaves the frame alone otherwise") {
    const bake::BakedLightmap lightmap = directional_lightmap();
    LightmapSlots slots;
    slots.planes[0] = 40;
    slots.planes[1] = 41;

    cy::rendering::pipeline::FrameViewData view;
    CY_REQUIRE(write_lightmaps(slots, lightmap, gi::GiMode::Baked, {}, view).has_value());
    CY_CHECK_EQ(view.lightmap_control[0], 40U);
    CY_CHECK_EQ(view.lightmap_control[1], 41U);
    CY_CHECK_EQ(view.lightmap_control[2], cy::rendering::pipeline::kNoMaterialTexture);
    CY_CHECK_EQ(view.lightmap_control[3], 2U);  // directional, plus one
    CY_CHECK_EQ(view.lightmap_layout[0], 256U);
    CY_CHECK_EQ(view.lightmap_layout[1], 2U);
    CY_CHECK_EQ(view.lightmap_layout[2], 4U);

    // `Probe` excludes lightmaps: the words stay at their defaults, which is the frame as it was.
    cy::rendering::pipeline::FrameViewData untouched;
    const cy::rendering::pipeline::FrameViewData defaults;
    CY_REQUIRE(write_lightmaps(slots, lightmap, gi::GiMode::Probe, {}, untouched).has_value());
    for (u32 word = 0; word < 4U; ++word) {
        CY_CHECK_EQ(untouched.lightmap_control[word], defaults.lightmap_control[word]);
        CY_CHECK_EQ(untouched.lightmap_layout[word], defaults.lightmap_layout[word]);
        CY_CHECK_EQ(untouched.lightmap_shadow_lights[word], defaults.lightmap_shadow_lights[word]);
        CY_CHECK_EQ(untouched.lightmap_direct_lights[word], 0U);
        CY_CHECK_EQ(untouched.lightmap_debug[word], 0U);
    }
    CY_CHECK_EQ(defaults.lightmap_control[3], 0U);
    CY_CHECK_EQ(defaults.lightmap_layout[3], cy::rendering::pipeline::kNoMaterialTexture);
    CY_CHECK_EQ(defaults.lightmap_shadow_lights[0], cy::rendering::pipeline::kNoLightmapLight);

    // One slot for two planes is refused rather than read past.
    LightmapSlots one = slots;
    one.planes[1] = cy::rendering::pipeline::kNoMaterialTexture;
    CY_CHECK_FALSE(write_lightmaps(one, lightmap, gi::GiMode::Baked, {}, view).has_value());
}

CY_TEST_CASE("write_lightmaps names the frame's baked lights by their place in the frame") {
    bake::BakedLightmap lightmap = directional_lightmap();
    // Two stationary lights with mask channels, and two static ones whose direct term is baked.
    CY_REQUIRE(lightmap.shadow_lights.push_back(11).has_value());
    CY_REQUIRE(lightmap.shadow_lights.push_back(12).has_value());
    CY_REQUIRE(lightmap.direct_lights.push_back(21).has_value());
    CY_REQUIRE(lightmap.direct_lights.push_back(22).has_value());
    LightmapSlots slots;
    slots.planes[0] = 40;
    slots.planes[1] = 41;

    // A mask with no slot would leave both stationary lights unshadowed: refused.
    cy::rendering::pipeline::FrameViewData view;
    CY_CHECK_FALSE(write_lightmaps(slots, lightmap, gi::GiMode::Baked, {}, view).has_value());
    slots.shadow_mask = 45;

    // The frame shades 12 first, then a movable 30, then 21 and 11; 22 is not in the frame.
    const cy::u64 frame[4] = {12, 30, 21, 11};
    CY_REQUIRE(write_lightmaps(slots, lightmap, gi::GiMode::Baked, {frame, 4}, view).has_value());
    CY_CHECK_EQ(view.lightmap_layout[3], 45U);
    CY_CHECK_EQ(view.lightmap_shadow_lights[0], 3U);  // channel 0 is light 11, frame light 3
    CY_CHECK_EQ(view.lightmap_shadow_lights[1], 0U);  // channel 1 is light 12, frame light 0
    CY_CHECK_EQ(view.lightmap_shadow_lights[2], cy::rendering::pipeline::kNoLightmapLight);
    CY_CHECK_EQ(view.lightmap_direct_lights[0], 1U << 2U);  // light 21 is frame light 2
    CY_CHECK_EQ(view.lightmap_direct_lights[1], 0U);

    // A second write into the same block replaces the lights rather than adding to them.
    const cy::u64 reordered[3] = {21, 11, 12};
    CY_REQUIRE(
        write_lightmaps(slots, lightmap, gi::GiMode::Baked, {reordered, 3}, view).has_value());
    CY_CHECK_EQ(view.lightmap_shadow_lights[0], 1U);
    CY_CHECK_EQ(view.lightmap_shadow_lights[1], 2U);
    CY_CHECK_EQ(view.lightmap_direct_lights[0], 1U);

    // A baked light past the set's 128 bits is refused rather than shaded twice.
    std::vector<cy::u64> many(130, 99);
    many[129] = 21;
    CY_CHECK_FALSE(write_lightmaps(slots, lightmap, gi::GiMode::Baked, {many.data(), many.size()},
                                   view)
                       .has_value());
}

CY_TEST_CASE("the density view is one word and a target, and off by default") {
    cy::rendering::pipeline::FrameViewData view;
    CY_CHECK_EQ(view.lightmap_debug[0], 0U);
    write_lightmap_density_view(2.5F, view);
    CY_CHECK_EQ(view.lightmap_debug[0], cy::rendering::pipeline::kLightmapDensityView);
    cy::f32 target = 0.0F;
    std::memcpy(&target, &view.lightmap_debug[1], sizeof(target));
    CY_CHECK_EQ(target, 2.5F);

    // The viewport's debug view reaches the same words, and no other mode touches them.
    cy::rendering::pipeline::FrameViewData other;
    CY_CHECK_FALSE(
        write_lightmap_debug_view(cy::render::DebugViewMode::Overdraw, 2.5F, other));
    CY_CHECK_EQ(other.lightmap_debug[0], 0U);
    CY_CHECK(
        write_lightmap_debug_view(cy::render::DebugViewMode::LightmapDensity, 2.5F, other));
    CY_CHECK_EQ(other.lightmap_debug[0], cy::rendering::pipeline::kLightmapDensityView);
}
