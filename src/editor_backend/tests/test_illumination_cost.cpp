// SPDX-License-Identifier: MIT
// What the editor's global illumination costs on the host: a rebuild, an edit applied in place and
// a debug view drawn. Measured, not bounded, so it runs in the smoke suite rather than holding an
// integration case's one-second budget; the guide and the editor backend's README quote it.

#include <cy/editor/illumination_preview.h>
#include <cy/rendering/gi/debug_views.h>
#include <cy/test/test.h>

#include <chrono>
#include <cstdio>

#include "illumination_world.h"

using namespace cy;
using cy::editor::IlluminationEmission;
using cy::editor::IlluminationPreview;
using cy::editor::IlluminationViewCamera;
using cy::illumination_test::allocator;
using cy::illumination_test::camera;
using cy::illumination_test::World;
namespace gi = cy::rendering::gi;

CY_TEST_CASE("what the editor's GI costs on the host: a rebuild, an edit in place and a view") {
    // A view is drawn at the quarter resolution the editor-window runtime asks for under a
    // 1280x720 frame.
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point since) {
        return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
    };
    World world;
    IlluminationPreview preview(allocator());
    Clock::time_point start = Clock::now();
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    const double rebuild = ms(start);

    world.settings.quality = gi::GiQuality::High;
    start = Clock::now();
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    const double settings = ms(start);

    world.emission.push_back(IlluminationEmission{2, gi::EmitterOverride::Off});
    start = Clock::now();
    CY_REQUIRE(preview.sync(world.scene()).has_value());
    const double rule = ms(start);
    CY_CHECK_EQ(preview.rebuilds(), 1U);

    // The probe view, which draws the probes it places, and indirect diffuse, which resolves the GI
    // at every pixel and is the slowest.
    const IlluminationViewCamera view = camera(320, 180);
    const auto draw = [&preview, &view, &ms](gi::GiDebugView which) {
        Array<u32> texels(allocator());
        const Clock::time_point began = Clock::now();
        CY_REQUIRE(preview.render_view(which, view, texels).has_value());
        return ms(began);
    };
    const double probes = draw(gi::GiDebugView::Probes);
    const double indirect = draw(gi::GiDebugView::IndirectDiffuse);
    std::printf(
        "MEASURED: rebuild and converge %.1f ms, quality in place %.3f ms, emitter rule in "
        "place %.3f ms; a 320x180 view: probes %.1f ms, indirect diffuse %.1f ms\n",
        rebuild, settings, rule, probes, indirect);
}
