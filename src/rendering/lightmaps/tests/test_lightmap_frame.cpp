// SPDX-License-Identifier: MIT
// Which ambient source a surface takes in the frame, and the view-block words.
// `unit.lightmap_frame`.

#include <cy/rendering/lightmaps/lightmap_textures.h>
#include <cy/test/test.h>

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
    const cy::rhi::BindlessIndex slots[2] = {40, 41};

    cy::rendering::pipeline::FrameViewData view;
    CY_REQUIRE(write_lightmaps({slots, 2}, lightmap, gi::GiMode::Baked, view).has_value());
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
    CY_REQUIRE(write_lightmaps({slots, 2}, lightmap, gi::GiMode::Probe, untouched).has_value());
    for (u32 word = 0; word < 4U; ++word) {
        CY_CHECK_EQ(untouched.lightmap_control[word], defaults.lightmap_control[word]);
        CY_CHECK_EQ(untouched.lightmap_layout[word], defaults.lightmap_layout[word]);
    }
    CY_CHECK_EQ(defaults.lightmap_control[3], 0U);

    // One slot for two planes is refused rather than read past.
    CY_CHECK_FALSE(write_lightmaps({slots, 1}, lightmap, gi::GiMode::Baked, view).has_value());
}
