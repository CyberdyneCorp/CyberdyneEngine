// The tracing tiers, their confidence, the escalation, and the reflections that share them.
// Task 9.2.

#include <cy/test/test.h>

#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/reflections.h>
#include <cy/rendering/gi/tracing.h>

#include "support.h"

#include <cmath>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)
using gi_support::BoxField;

/// A depth buffer looking down -Z at a wall four metres away, filling the middle of the frame.
struct Screen {
    static constexpr u32 kWidth = 32;
    static constexpr u32 kHeight = 32;
    std::vector<f32> depth;
    std::vector<Vec3> normal;
    std::vector<Vec3> colour;

    Screen() {
        const auto pixels = static_cast<size_t>(kWidth) * kHeight;
        depth.assign(pixels, 0.0F);
        normal.assign(pixels, Vec3{0.0F, 0.0F, 1.0F});
        colour.assign(pixels, Vec3{0.0F, 0.0F, 0.0F});
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                const u32 pixel = (y * kWidth) + x;
                depth[pixel] = 4.0F;
                colour[pixel] = Vec3{0.8F, 0.2F, 0.2F};
            }
        }
    }

    [[nodiscard]] ScreenView view() const noexcept {
        ScreenView screen;
        screen.width = kWidth;
        screen.height = kHeight;
        screen.depth = {depth.data(), depth.size()};
        screen.normal = {normal.data(), normal.size()};
        screen.colour = {colour.data(), colour.size()};
        screen.world_to_view = cy::Mat4::identity();
        screen.world_to_clip = cy::perspective_reversed_z(1.0471975512F, 1.0F, 0.1F, 100.0F);
        screen.camera_position = Vec3{0.0F, 0.0F, 0.0F};
        screen.thickness_metres = 0.5F;
        screen.max_steps = 48;
        return screen;
    }
};

}  // namespace

CY_TEST_CASE("a screen trace that hits in the middle of the frame is trusted") {
    const Screen screen;
    const ScreenView view = screen.view();
    const RadianceSample hit =
        ScreenTracer::trace(view, Vec3{0.0F, 0.0F, -0.5F}, Vec3{0.0F, 0.0F, -1.0F}, 10.0F);
    CY_CHECK_GT(hit.confidence, 0.5F);
    CY_CHECK_EQ(hit.source, RadianceSource::ScreenTrace);
    CY_CHECK_GT(hit.radiance.x, hit.radiance.y);
    CY_CHECK_GT(hit.hit_distance, 0.0F);
    // Never fully trusted: it carries last frame's colour.
    CY_CHECK_LT(hit.confidence, 1.0F);
}

CY_TEST_CASE("a ray that leaves the viewport reports no confidence at all") {
    // "WHEN a traced ray exits the viewport THEN confidence SHALL fall toward zero near the edge
    // and a higher tier or cached source SHALL take over smoothly."
    const Screen screen;
    const ScreenView view = screen.view();
    const RadianceSample escaped =
        ScreenTracer::trace(view, Vec3{0.0F, 0.0F, -0.5F}, Vec3{1.0F, 0.0F, 0.0F}, 10.0F);
    CY_CHECK_EQ(escaped.confidence, 0.0F);

    // And a ray that stays on screen and meets nothing is a low confidence rather than a confident
    // miss: a screen tracer cannot prove a miss.
    std::vector<f32> empty(static_cast<size_t>(Screen::kWidth) * Screen::kHeight, 0.0F);
    ScreenView sky = view;
    sky.depth = {empty.data(), empty.size()};
    const RadianceSample nothing =
        ScreenTracer::trace(sky, Vec3{0.0F, 0.0F, -0.5F}, Vec3{0.0F, 0.0F, -1.0F}, 3.0F);
    CY_CHECK_GT(nothing.confidence, 0.0F);
    CY_CHECK_LT(nothing.confidence, 0.5F);
}

CY_TEST_CASE("a hidden surface is not invented") {
    // "WHEN a ray would hit a surface not present in the depth buffer THEN the screen tier SHALL
    // report low confidence rather than returning a plausible-looking wrong answer."
    const Screen screen;
    ScreenView view = screen.view();
    view.thickness_metres = 0.1F;
    // A ray that dives well behind the visible wall. The tier cannot see behind it.
    const RadianceSample behind =
        ScreenTracer::trace(view, Vec3{0.0F, 0.0F, -0.5F}, Vec3{0.0F, 0.0F, -1.0F}, 40.0F);
    CY_CHECK_EQ(behind.confidence, 0.0F);
}

CY_TEST_CASE("the cheapest tier that can answer is the one that runs") {
    const BoxField box(Vec3{1.0F, 1.0F, 1.0F});
    DistanceField field;
    ClipmapSettings clipmaps;
    clipmaps.levels = 2;
    clipmaps.resolution = 16;
    clipmaps.base_extent_metres = 16.0F;
    CY_REQUIRE(field.configure(clipmaps).has_value());
    CY_REQUIRE(field.place(1, box.asset(), cy::Mat4::from_translation(Vec3{0.0F, 0.0F, -4.0F}))
                   .has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});

    const SoftwareTracer software(field, nullptr);
    const Screen screen;
    const ScreenView view = screen.view();

    TieredTracer tracer;
    tracer.set_software(&software);
    tracer.set_screen(&view);

    // A high-confidence screen hit: no world ray is issued.
    TraceBudget budget;
    const RadianceSample cheap =
        tracer.trace(Vec3{0.0F, 0.0F, -0.5F}, Vec3{0.0F, 0.0F, -1.0F}, 10.0F, budget);
    CY_CHECK_EQ(cheap.source, RadianceSource::ScreenTrace);
    CY_CHECK_EQ(tracer.diagnostics().software_rays, 0U);
    CY_CHECK_EQ(budget.world_rays, 64U);

    // A ray that leaves the screen escalates, and the world tier answers.
    const RadianceSample escalated =
        tracer.trace(Vec3{0.0F, 0.0F, -0.5F}, Vec3{1.0F, 0.0F, 0.0F}, 10.0F, budget);
    CY_CHECK_EQ(tracer.diagnostics().software_rays, 1U);
    CY_CHECK_EQ(tracer.diagnostics().escalations, 1U);
    CY_CHECK_LT(budget.world_rays, 64U);
    CY_CHECK_GT(escalated.confidence, 0.0F);

    // And a spent budget refuses the escalation rather than exceeding it.
    budget.world_rays = 0;
    const u32 before = tracer.diagnostics().software_rays;
    (void)tracer.trace(Vec3{0.0F, 0.0F, -0.5F}, Vec3{1.0F, 0.0F, 0.0F}, 10.0F, budget);
    CY_CHECK_EQ(tracer.diagnostics().software_rays, before);
    CY_CHECK_EQ(tracer.diagnostics().budget_refusals, 1U);
}

CY_TEST_CASE("with no hardware tier the software tier is selected, and nothing above knows") {
    const BoxField box(Vec3{1.0F, 1.0F, 1.0F});
    DistanceField field;
    ClipmapSettings clipmaps;
    clipmaps.levels = 2;
    clipmaps.resolution = 16;
    clipmaps.base_extent_metres = 16.0F;
    CY_REQUIRE(field.configure(clipmaps).has_value());
    CY_REQUIRE(field.place(1, box.asset(), cy::Mat4::from_translation(Vec3{0.0F, 0.0F, -4.0F}))
                   .has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});

    const SoftwareTracer software(field, nullptr);
    // The default acceleration service: no device support, exactly what this tree reports.
    cy::rendering::rt::AccelerationService service;
    const HardwareTracer hardware(service, nullptr);
    CY_CHECK_FALSE(hardware.available());

    TieredTracer tracer;
    tracer.set_software(&software);
    tracer.set_hardware(&hardware);
    CY_CHECK_EQ(tracer.selected_world_tier(), RadianceSource::SoftwareTrace);

    TraceBudget budget;
    const RadianceSample answer =
        tracer.trace(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, 20.0F, budget);
    CY_CHECK_EQ(tracer.diagnostics().hardware_rays, 0U);
    CY_CHECK_EQ(tracer.diagnostics().software_rays, 1U);
    CY_CHECK_GT(answer.confidence, 0.0F);
}

CY_TEST_CASE("a reflection probe is captured amortised and re-aimed by box projection") {
    ReflectionProbeSet probes;
    ReflectionProbe probe;
    probe.position = Vec3{0.0F, 0.0F, 0.0F};
    probe.is_box = true;
    probe.box = cy::Aabb::from_min_max(Vec3{-4.0F, -2.0F, -4.0F}, Vec3{4.0F, 2.0F, 4.0F});
    probe.blend_distance = 1.0F;
    probe.box_projection = true;
    probe.capture_mode = ProbeCaptureMode::Realtime;
    const auto handle = probes.add(probe);
    CY_REQUIRE(handle.has_value());

    // Capture is spread across calls: four texels a call, so the probe is not valid until the last
    // one lands. A probe half-captured shows its previous contents, not half a room.
    ReflectionCaptureContext context;
    context.texel_budget = 4;
    context.sky.zenith = Vec3{0.2F, 0.3F, 0.9F};
    context.sky.horizon = Vec3{0.2F, 0.3F, 0.9F};
    context.sky.ground = Vec3{0.2F, 0.3F, 0.9F};
    u32 calls = 0;
    while (!probes.probe(handle.value()).valid && calls < 20) {
        context.frame = calls;
        const ReflectionCaptureReport report = probes.capture(context);
        CY_CHECK_LE(report.texels_captured, 4U);
        calls += 1;
    }
    CY_CHECK_EQ(calls, kReflectionTexels / 4U);
    CY_CHECK(probes.probe(handle.value()).valid);

    // Assignment and the influence blend.
    u32 handles[4] = {};
    f32 weights[4] = {};
    CY_CHECK_EQ(probes.assign(Vec3{0.0F, 0.0F, 0.0F}, {handles, 4}, {weights, 4}), 1U);
    CY_CHECK_GT(weights[0], 0.0F);
    CY_CHECK_EQ(probes.assign(Vec3{100.0F, 0.0F, 0.0F}, {handles, 4}, {weights, 4}), 0U);

    const RadianceSample inside =
        probes.sample(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, 0.2F);
    CY_CHECK_GT(inside.confidence, 0.0F);
    CY_CHECK_EQ(inside.source, RadianceSource::ReflectionProbe);
    const RadianceSample outside =
        probes.sample(Vec3{100.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, 0.2F);
    CY_CHECK_EQ(outside.confidence, 0.0F);
}

CY_TEST_CASE("box projection re-aims the reflection at the wall rather than at infinity") {
    // Two probes, identical but for box projection, in a room whose walls differ by direction.
    // A surface away from the probe centre must read a different texel with projection than
    // without, or the projection is doing nothing.
    ReflectionProbeSet with_projection;
    ReflectionProbeSet without;
    ReflectionProbe probe;
    probe.position = Vec3{0.0F, 0.0F, 0.0F};
    probe.is_box = true;
    probe.box = cy::Aabb::from_min_max(Vec3{-4.0F, -2.0F, -4.0F}, Vec3{4.0F, 2.0F, 4.0F});
    probe.blend_distance = 0.5F;
    probe.capture_mode = ProbeCaptureMode::Baked;

    probe.box_projection = true;
    const auto projected = with_projection.add(probe);
    probe.box_projection = false;
    const auto flat = without.add(probe);
    CY_REQUIRE(projected.has_value());
    CY_REQUIRE(flat.has_value());

    // Seed both with a directional environment: bright toward +X, dark toward -X.
    const std::vector<Vec3> directions = {{1.0F, 0.0F, 0.0F}, {-1.0F, 0.0F, 0.0F},
                                          {0.0F, 1.0F, 0.0F}, {0.0F, -1.0F, 0.0F},
                                          {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -1.0F}};
    const std::vector<Vec3> radiance = {{4.0F, 4.0F, 4.0F}, {0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F},
                                        {1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F}};
    CY_REQUIRE(with_projection
                   .seed(projected.value(), {directions.data(), directions.size()},
                         {radiance.data(), radiance.size()})
                   .has_value());
    CY_REQUIRE(without
                   .seed(flat.value(), {directions.data(), directions.size()},
                         {radiance.data(), radiance.size()})
                   .has_value());

    // A surface near the -X wall reflecting back along +X. Without projection it reads the +X
    // direction from the probe centre; with projection the ray is intersected with the box and
    // re-aimed, which lands somewhere else on the map.
    const Vec3 position{-3.5F, 0.0F, 3.5F};
    const Vec3 reflection{1.0F, 0.0F, 0.0F};
    const RadianceSample aimed = with_projection.sample(position, reflection, 0.0F);
    const RadianceSample unaimed = without.sample(position, reflection, 0.0F);
    CY_CHECK_GT(aimed.confidence, 0.0F);
    CY_CHECK_GT(unaimed.confidence, 0.0F);
    CY_CHECK_NE(aimed.radiance.x, unaimed.radiance.x);
}

CY_TEST_CASE("an interior probe does not reflect a sky it cannot see") {
    ReflectionProbeSet probes;
    ReflectionProbe probe;
    probe.position = Vec3{0.0F, 0.0F, 0.0F};
    probe.radius = 6.0F;
    probe.blend_distance = 1.0F;
    probe.interior = true;
    probe.capture_mode = ProbeCaptureMode::OnDemand;
    const auto handle = probes.add(probe);
    CY_REQUIRE(handle.has_value());

    ReflectionCaptureContext context;
    context.texel_budget = kReflectionTexels;
    context.sky.zenith = Vec3{9.0F, 9.0F, 9.0F};
    context.sky.horizon = Vec3{9.0F, 9.0F, 9.0F};
    context.sky.ground = Vec3{9.0F, 9.0F, 9.0F};
    (void)probes.capture(context);
    CY_REQUIRE(probes.probe(handle.value()).valid);

    const RadianceSample sample =
        probes.sample(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, 0.3F);
    // No tracer and no sky: the interior probe captured black rather than the sky's nine.
    CY_CHECK_LT(sample.radiance.x, 0.01F);
}

CY_TEST_CASE("the reflection lobe spreads with roughness and stays above the surface") {
    const Vec3 normal{0.0F, 1.0F, 0.0F};
    const Vec3 reflection = cy::normalize(Vec3{0.4F, 0.6F, 0.0F});
    f32 sharp_spread = 0.0F;
    f32 rough_spread = 0.0F;
    for (u32 index = 0; index < 16; ++index) {
        const Vec3 sharp = reflection_lobe_direction(reflection, normal, 0.02F, index, 16, 0);
        const Vec3 rough = reflection_lobe_direction(reflection, normal, 0.9F, index, 16, 0);
        sharp_spread = std::max(sharp_spread, 1.0F - cy::dot(sharp, reflection));
        rough_spread = std::max(rough_spread, 1.0F - cy::dot(rough, reflection));
        // A sample below the surface is not a reflection.
        CY_CHECK_GE(cy::dot(sharp, normal), -1e-4F);
        CY_CHECK_GE(cy::dot(rough, normal), -1e-4F);
    }
    CY_CHECK_GT(rough_spread, sharp_spread * 4.0F);
    // One ray is the reflection itself, whatever the roughness.
    CY_CHECK(
        cy::nearly_equal(reflection_lobe_direction(reflection, normal, 0.9F, 0, 1, 0), reflection));
}
