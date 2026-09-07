// The decisions that are arithmetic: detail selection, exclusion, the far field, the reflection
// table, the probe encodings and the GI budget's control law. Tasks 9.1 and 9.2.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/budget.h>
#include <cy/rendering/gi/radiance_cache.h>
#include <cy/rendering/gi/reflections.h>
#include <cy/rendering/gi/resolve.h>
#include <cy/rendering/gi/scene.h>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <vector>

namespace {

using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace) — one module's own
                                    // vocabulary, in its own test file.
using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;

}  // namespace

CY_TEST_CASE("the GI scene asks one hierarchy for a coarser level, never a second mesh") {
    // `rendering-global-illumination` — "One hierarchy, two error targets".
    const std::vector<DetailLevel> levels = {
        {0, 0.002F, 40000},
        {1, 0.010F, 9000},
        {2, 0.050F, 2000},
        {3, 0.400F, 400},
    };
    const cy::Span<const DetailLevel> chain{levels.data(), levels.size()};

    const ErrorTargets targets;
    // Near field wants centimetres: level 1, at 1 cm of error, is the coarsest inside the 2 cm
    // target. Level 2's 5 cm is outside it, and taking it would be spending the error budget the
    // near field exists to hold.
    CY_CHECK_EQ(select_detail_level(chain, targets.near_field_metres), 1U);
    // A slacker target takes a coarser level, which is the whole mechanism.
    CY_CHECK_EQ(select_detail_level(chain, 0.06F), 2U);
    // Far field wants metres: the coarsest level there is.
    CY_CHECK_EQ(select_detail_level(chain, targets.far_field_metres), 3U);
    // A target finer than anything the hierarchy has answers with the finest rather than failing:
    // an illumination query answered too coarsely is better than one not answered at all.
    CY_CHECK_EQ(select_detail_level(chain, 0.0001F), 0U);
    CY_CHECK_EQ(select_detail_level({}, 1.0F), 0U);
}

CY_TEST_CASE("sources are combined by confidence, not selected between") {
    RadianceSample bright;
    bright.source = RadianceSource::RadianceCache;
    bright.radiance = Vec3{1.0F, 1.0F, 1.0F};
    bright.confidence = 0.25F;

    RadianceSample dark;
    dark.source = RadianceSource::ScreenTrace;
    dark.radiance = Vec3{0.0F, 0.0F, 0.0F};
    dark.confidence = 0.75F;

    const RadianceSample samples[2] = {bright, dark};
    const ResolveResult result = combine({samples, 2}, 0);
    // A weighted mean, not a maximum: 0.25 of the bright sample.
    CY_CHECK_NEAR(result.radiance.x, 0.25F, 1e-5F);
    CY_CHECK_NEAR(result.confidence, 0.75F, 1e-5F);
    CY_CHECK_EQ(result.dominant, RadianceSource::ScreenTrace);
    CY_CHECK_EQ(result.sources_used, source_bit(RadianceSource::RadianceCache) |
                                         source_bit(RadianceSource::ScreenTrace));

    // A zero-confidence sample is an answer the resolve weights to nothing, not an error.
    RadianceSample useless;
    useless.source = RadianceSource::Sky;
    useless.radiance = Vec3{9.0F, 9.0F, 9.0F};
    useless.confidence = 0.0F;
    const RadianceSample with_useless[3] = {bright, dark, useless};
    const ResolveResult unchanged = combine({with_useless, 3}, 0);
    CY_CHECK_NEAR(unchanged.radiance.x, 0.25F, 1e-5F);
    CY_CHECK_EQ(unchanged.sources_used & source_bit(RadianceSource::Sky), 0U);
}

CY_TEST_CASE("a lightmapped surface takes one bounce and not two") {
    // "WHEN a lightmapped surface is inside a dynamic GI region THEN it SHALL take indirect diffuse
    // from the lightmap only, and the dynamic contribution SHALL be excluded for it."
    RadianceSample lightmap;
    lightmap.source = RadianceSource::Lightmap;
    lightmap.radiance = Vec3{0.5F, 0.5F, 0.5F};
    lightmap.confidence = 1.0F;
    RadianceSample dynamic;
    dynamic.source = RadianceSource::RadianceCache;
    dynamic.radiance = Vec3{0.5F, 0.5F, 0.5F};
    dynamic.confidence = 1.0F;
    const RadianceSample samples[2] = {lightmap, dynamic};

    const u32 lightmapped = exclusion_for(GiMode::Hybrid, true, false);
    const ResolveResult once = combine({samples, 2}, lightmapped);
    CY_CHECK_NEAR(once.radiance.x, 0.5F, 1e-5F);
    CY_CHECK_EQ(once.sources_used, source_bit(RadianceSource::Lightmap));

    // The same surface without a lightmap, in the same region, takes the dynamic answer.
    const u32 unlit = exclusion_for(GiMode::Hybrid, false, false);
    const ResolveResult dynamic_only = combine({samples, 2}, unlit);
    CY_CHECK_EQ(dynamic_only.sources_used, source_bit(RadianceSource::RadianceCache));

    // And the modes: `None` excludes every bounce, `Dynamic` excludes the baked ones.
    CY_CHECK_NE(exclusion_for(GiMode::None, false, false) & source_bit(RadianceSource::Lightmap),
                0U);
    CY_CHECK_NE(
        exclusion_for(GiMode::None, false, false) & source_bit(RadianceSource::RadianceCache), 0U);
    CY_CHECK_NE(exclusion_for(GiMode::Dynamic, true, true) & source_bit(RadianceSource::Lightmap),
                0U);
    CY_CHECK_EQ(
        exclusion_for(GiMode::Dynamic, true, true) & source_bit(RadianceSource::RadianceCache), 0U);
}

CY_TEST_CASE("the far field transitions over a ramp rather than at a boundary") {
    const ErrorTargets targets;
    CY_CHECK_EQ(far_field_weight(0.0F, targets), 0.0F);
    CY_CHECK_EQ(far_field_weight(targets.far_field_begins_metres, targets), 0.0F);
    CY_CHECK_EQ(far_field_weight(1.0e4F, targets), 1.0F);

    // Monotone, and with no step anywhere: the largest jump between two metres along the ramp is
    // small, which is the "no visible boundary" scenario as a number.
    f32 previous = 0.0F;
    f32 largest_step = 0.0F;
    for (u32 metre = 0; metre < 200; ++metre) {
        const f32 weight = far_field_weight(static_cast<f32>(metre), targets);
        CY_CHECK_GE(weight, previous);
        largest_step = std::max(largest_step, weight - previous);
        previous = weight;
    }
    CY_CHECK_LT(largest_step, 0.2F);
}

CY_TEST_CASE("reflection strategy is a roughness table and rough surfaces cost no rays") {
    const ReflectionThresholds thresholds;
    CY_CHECK_EQ(reflection_strategy(0.0F, thresholds), ReflectionStrategy::DedicatedRays);
    CY_CHECK_EQ(reflection_strategy(0.3F, thresholds), ReflectionStrategy::SparseRaysAndCache);
    CY_CHECK_EQ(reflection_strategy(0.9F, thresholds), ReflectionStrategy::CacheOnly);

    // "WHEN a surface is very rough THEN its reflection SHALL be taken from the radiance cache
    // without dedicated rays." Zero, not one.
    CY_CHECK_EQ(reflection_ray_count(0.9F, thresholds), 0U);
    CY_CHECK_EQ(reflection_ray_count(0.0F, thresholds), thresholds.max_rays);
    // And the count falls monotonically between the two.
    u32 previous = reflection_ray_count(0.0F, thresholds);
    for (u32 step = 0; step <= 20; ++step) {
        const u32 rays = reflection_ray_count(static_cast<f32>(step) * 0.05F, thresholds);
        CY_CHECK_LE(rays, previous);
        previous = rays;
    }
}

CY_TEST_CASE("the three probe encodings are real alternatives with a stated trade-off") {
    const EncodingTraits sh = encoding_traits(ProbeEncoding::SphericalHarmonicsL1);
    const EncodingTraits octahedral = encoding_traits(ProbeEncoding::Octahedral);
    const EncodingTraits gaussian = encoding_traits(ProbeEncoding::SphericalGaussian);

    CY_CHECK_EQ(sh.floats_per_probe, 12U);
    CY_CHECK_GT(octahedral.floats_per_probe, gaussian.floats_per_probe);
    CY_CHECK_GT(gaussian.floats_per_probe, sh.floats_per_probe);
    // More memory buys more directionality and costs more to evaluate. That is the trade-off, and
    // an encoding that was better in every column would mean the other two should not exist.
    CY_CHECK_GT(octahedral.directionality, gaussian.directionality);
    CY_CHECK_GT(gaussian.directionality, sh.directionality);
    CY_CHECK_GT(octahedral.evaluation_cost, sh.evaluation_cost);
    CY_CHECK_LE(sh.floats_per_probe, kMaxProbeFloats);
    CY_CHECK_LE(octahedral.floats_per_probe, kMaxProbeFloats);
}

CY_TEST_CASE("every encoding round-trips a directional signal and keeps its direction") {
    // A bright light from +X and nothing from -X. Every encoding must come back brighter looking
    // toward the light than away from it, or it is not directional at all.
    for (u32 index = 0; index < static_cast<u32>(ProbeEncoding::Count); ++index) {
        const auto encoding = static_cast<ProbeEncoding>(index);
        std::vector<f32> payload(kMaxProbeFloats, 0.0F);
        constexpr u32 kSamples = 64;
        f32 weight = 0.0F;
        for (u32 sample = 0; sample < kSamples; ++sample) {
            const f32 angle = 6.2831853F * static_cast<f32>(sample) / static_cast<f32>(kSamples);
            const Vec3 direction{std::cos(angle), 0.0F, std::sin(angle)};
            const Vec3 radiance =
                direction.x > 0.5F ? Vec3{4.0F, 4.0F, 4.0F} : Vec3{0.0F, 0.0F, 0.0F};
            encode_sample(encoding, payload.data(), direction, radiance, 1.0F);
            weight += 1.0F;
        }
        normalise_payload(encoding, payload.data(), weight, kSamples);

        const Vec3 toward = decode_payload(encoding, payload.data(), Vec3{1.0F, 0.0F, 0.0F});
        const Vec3 away = decode_payload(encoding, payload.data(), Vec3{-1.0F, 0.0F, 0.0F});
        CY_CHECK_GT(toward.x, away.x);
        CY_CHECK_GE(toward.x, 0.0F);
        CY_CHECK_GE(away.x, 0.0F);
    }
}

CY_TEST_CASE("the GI budget cannot be handed a frame time") {
    // The structural half of "It SHALL measure and report its own cost, and SHALL NOT measure total
    // frame time": `update` takes one number and it is this system's own. There is no parameter a
    // frame time could arrive through, which is why this case is a compile-time property asserted
    // by the signature and a runtime one asserted here — the report echoes what it was given.
    GiBudget budget;
    GiBudgetSettings settings;
    settings.allocation_ms = 3.0F;
    budget.configure(settings);
    const GiBudgetReport report = budget.update(2.0F, 1);
    CY_CHECK_EQ(report.measured_ms, 2.0F);
    CY_CHECK_EQ(report.allocation_ms, 3.0F);
}

CY_TEST_CASE("every declared lever prices its ladder and reduces in a declared order") {
    // design.md §2.10: "a subsystem must declare what each ladder position COSTS relative to
    // position 0". Without it an arbiter allocating milliseconds over the ladder is choosing blind.
    GiBudget budget;
    std::vector<u32> orders;
    for (u32 index = 0; index < kGiLeverCount; ++index) {
        const auto lever = static_cast<GiLever>(index);
        const GiLeverSchedule& schedule = budget.schedule(lever);
        CY_CHECK(schedule.declared);
        CY_CHECK_GT(schedule.position_count, 0U);
        CY_CHECK_EQ(schedule.relative_cost[0], 1.0F);
        for (u32 position = 1; position < schedule.position_count; ++position) {
            CY_CHECK_LE(schedule.relative_cost[position], schedule.relative_cost[position - 1]);
        }
        CY_CHECK_NE(gi_lever_name(lever), nullptr);
        orders.push_back(schedule.reduction_order);
    }
    // The reduction order is a total order: two levers sharing one would make "which reduces first"
    // depend on the enumeration, which is construction order wearing a disguise.
    std::ranges::sort(orders);
    CY_CHECK(std::ranges::adjacent_find(orders) == orders.end());

    // A ladder whose cost rises along it is refused rather than accepted and mis-priced.
    GiLeverSchedule bad;
    bad.declared = true;
    bad.position_count = 2;
    bad.relative_cost[0] = 1.0F;
    bad.relative_cost[1] = 2.0F;
    CY_CHECK_FALSE(budget.declare(GiLever::TracedRays, bad).has_value());
    bad.relative_cost[0] = 0.5F;
    bad.relative_cost[1] = 0.25F;
    CY_CHECK_FALSE(budget.declare(GiLever::TracedRays, bad).has_value());
}

CY_TEST_CASE("the budget tightens on its own and relaxes only when the arbiter permits") {
    // design.md §2.6, the half nobody writes down: a controller may tighten on its own authority
    // and must never relax on it, because the time a step back up costs comes out of a frame it is
    // forbidden to measure.
    GiBudget budget;
    GiBudgetSettings settings;
    settings.allocation_ms = 3.0F;
    settings.relax_dwell_frames = 2;
    budget.configure(settings);

    // Over budget: it reduces, walking the declared order, and the first lever to move is the one
    // that declared reduction order zero.
    u64 frame = 0;
    u32 tightened = 0;
    for (; frame < 20 && !budget.at_reserved_minimum(); ++frame) {
        const GiBudgetReport report = budget.update(9.0F, frame);
        tightened += report.levers_tightened;
    }
    CY_CHECK_GT(tightened, 0U);
    CY_CHECK_GT(budget.position(GiLever::DenoiserQuality), 0U);

    // Under budget with no permission: it reports that it would have relaxed and does not.
    const u32 before = budget.position(GiLever::DenoiserQuality);
    bool withheld = false;
    for (u32 index = 0; index < 20; ++index, ++frame) {
        const GiBudgetReport report = budget.update(0.4F, frame);
        withheld = withheld || report.relaxation_withheld;
        CY_CHECK_EQ(report.levers_relaxed, 0U);
    }
    CY_CHECK(withheld);
    CY_CHECK_EQ(budget.position(GiLever::DenoiserQuality), before);

    // With permission, one step at a time and never more.
    u32 relaxed = 0;
    for (u32 index = 0; index < 40; ++index, ++frame) {
        budget.permit_relaxation();
        const GiBudgetReport report = budget.update(0.4F, frame);
        CY_CHECK_LE(report.levers_relaxed, 1U);
        relaxed += report.levers_relaxed;
    }
    CY_CHECK_GT(relaxed, 0U);
}

CY_TEST_CASE("a settled load does not pump the levers") {
    // "WHEN load hovers around the allocation THEN hysteresis SHALL prevent visible pumping of GI
    // quality." A load sitting exactly on the allocation for four hundred frames, with the arbiter
    // permitting relaxation every frame — the worst case for a loop with no hysteresis.
    GiBudget budget;
    GiBudgetSettings settings;
    settings.allocation_ms = 3.0F;
    budget.configure(settings);

    u32 changes = 0;
    for (u64 frame = 0; frame < 400; ++frame) {
        // +/-3% measurement noise, the figure the arbiter spike modelled.
        const f32 noise =
            ((static_cast<f32>((frame * 2654435761U) % 1000U) / 1000.0F) - 0.5F) * 0.06F;
        budget.permit_relaxation();
        const GiBudgetReport report = budget.update(3.0F * (1.0F + noise), frame);
        if (frame > 100) {
            changes += report.levers_tightened + report.levers_relaxed;
        }
    }
    CY_CHECK_EQ(changes, 0U);
}

CY_TEST_CASE("pinned mode stops the loop and reports the overrun") {
    GiBudget budget;
    GiBudgetSettings settings;
    settings.allocation_ms = 3.0F;
    budget.configure(settings);
    budget.set_pinned(true);

    for (u64 frame = 0; frame < 50; ++frame) {
        const GiBudgetReport report = budget.update(11.0F, frame);
        CY_CHECK(report.pinned);
        CY_CHECK_EQ(report.levers_tightened, 0U);
        CY_CHECK_EQ(report.levers_relaxed, 0U);
        CY_CHECK_GT(report.measured_ms, report.allocation_ms);
    }
    CY_CHECK_EQ(budget.position(GiLever::DenoiserQuality), 0U);
}

CY_TEST_CASE("quality follows what matters in the game, not distance alone") {
    GiBudget budget;
    // A gameplay-critical unit and background scenery at the same size on screen.
    const f32 unit = budget.importance_weight(0.2F, 3.0F);
    const f32 scenery = budget.importance_weight(0.2F, 0.2F);
    CY_CHECK_GT(unit, scenery);
    // And the scenery is still lit: the floor is what keeps a reduction from being a switch.
    CY_CHECK_GT(scenery, 0.0F);
    CY_CHECK_LE(unit, 1.0F);

    // A foveation mask, where one is supplied.
    const std::vector<f32> mask = {1.0F, 0.25F, 0.25F, 1.0F};
    CY_CHECK(budget.set_foveation_mask({mask.data(), mask.size()}, 2, 2).has_value());
    CY_CHECK(budget.has_foveation_mask());
    CY_CHECK_NEAR(budget.foveation_at(0.1F, 0.1F), 1.0F, 1e-5F);
    CY_CHECK_NEAR(budget.foveation_at(0.9F, 0.1F), 0.25F, 1e-5F);
    CY_CHECK_FALSE(budget.set_foveation_mask({mask.data(), mask.size()}, 3, 3).has_value());
}

CY_TEST_CASE("convergence is a measurement and an unobserved region is not converged") {
    ConvergenceTracker tracker;
    tracker.configure(4.0F, 1.0F);

    // Nothing observed: not converged, because unknown is not settled.
    CY_CHECK_EQ(tracker.convergence(Vec3{0.0F, 0.0F, 0.0F}), 0.0F);
    CY_CHECK_FALSE(tracker.converged(0.9F));

    for (u32 index = 0; index < 10; ++index) {
        tracker.observe(Vec3{1.0F, 1.0F, 1.0F}, 0.0F);
    }
    CY_CHECK_GT(tracker.convergence(Vec3{1.0F, 1.0F, 1.0F}), 0.95F);
    CY_CHECK(tracker.converged(0.9F));

    // A region that is still moving drags the whole answer down, which is what a capture waits on.
    tracker.observe(Vec3{40.0F, 0.0F, 0.0F}, 0.8F);
    CY_CHECK_FALSE(tracker.converged(0.9F));
    CY_CHECK_EQ(tracker.unconverged_regions(0.9F), 1U);
    CY_CHECK_EQ(tracker.region_count(), 2U);
}
