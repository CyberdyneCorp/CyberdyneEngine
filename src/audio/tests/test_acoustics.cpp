// The geometry cache, the fallback backend, and the double-buffered simulation.
// M8.b tasks 10.1 and 10.2.

#include <cy/audio/acoustics.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::audio;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Audio);
}

/// A world that answers with a fixed triangle count and records what it was asked.
class TestExtractor final : public GeometryExtractor {
public:
    Status extract_static(Vec3 centre, f32 radius, Array<AcousticTriangle>& out) noexcept override {
        ++static_calls;
        last_radius = radius;
        for (u32 index = 0; index < static_count; ++index) {
            AcousticTriangle triangle;
            triangle.a = centre;
            triangle.b = centre + Vec3{1.0F, 0.0F, 0.0F};
            triangle.c = centre + Vec3{0.0F, 1.0F, 0.0F};
            if (Status pushed = out.push_back(triangle); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    Status extract_dynamic(Vec3 centre, f32 radius, u64 since_tick,
                           Array<AcousticTriangle>& out) noexcept override {
        ++dynamic_calls;
        last_since = since_tick;
        (void)radius;
        for (u32 index = 0; index < dynamic_count; ++index) {
            AcousticTriangle triangle;
            triangle.a = centre;
            if (Status pushed = out.push_back(triangle); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    u32 static_count = 10;
    u32 dynamic_count = 2;
    u32 static_calls = 0;
    u32 dynamic_calls = 0;
    f32 last_radius = 0.0F;
    u64 last_since = 0;
};

}  // namespace

CY_TEST_CASE("acoustics_geometry: the static cache answers until the listener moves far enough") {
    // "Extraction SHALL be incremental and bounded: only geometry within a configurable radius of
    // active listeners SHALL be extracted, static geometry SHALL be cached, and dynamic geometry
    // SHALL be updated only when it moves."
    GeometryCache cache(allocator());
    cache.radius = 40.0F;
    cache.refill_distance = 10.0F;
    TestExtractor extractor;
    ExtractionReport report;

    CY_REQUIRE(cache.update(extractor, Vec3{0.0F, 0.0F, 0.0F}, 1, report).has_value());
    CY_CHECK_EQ(extractor.static_calls, 1U);
    CY_CHECK_EQ(report.static_triangles, 10U);
    CY_CHECK_EQ(extractor.last_radius, 40.0F);
    CY_CHECK_FALSE(report.cache_hit);

    // A step: the cache answers, and the host is not asked for static geometry again.
    CY_REQUIRE(cache.update(extractor, Vec3{2.0F, 0.0F, 0.0F}, 2, report).has_value());
    CY_CHECK_EQ(extractor.static_calls, 1U);
    CY_CHECK(report.cache_hit);
    // Dynamic geometry is asked for every update, with the tick it was last asked at.
    CY_CHECK_EQ(extractor.dynamic_calls, 2U);
    CY_CHECK_EQ(extractor.last_since, 1U);

    // Far enough: it refills.
    CY_REQUIRE(cache.update(extractor, Vec3{50.0F, 0.0F, 0.0F}, 3, report).has_value());
    CY_CHECK_EQ(extractor.static_calls, 2U);
    CY_CHECK_FALSE(report.cache_hit);

    // And the combined view is both sets.
    CY_CHECK_EQ(cache.triangles().size(), 12U);
    cache.invalidate();
    CY_CHECK_EQ(cache.triangles().size(), 0U);
}

CY_TEST_CASE("acoustics_fallback: every source is answered, and no capability is claimed") {
    // "WHEN the engine is built without Steam Audio THEN all audio SHALL still play, spatialised by
    // the fallback path, with no missing sounds and no gameplay difference."
    FallbackAcoustics fallback;
    CY_CHECK_EQ(std::string_view(fallback.backend_name()), "fallback");
    const AcousticsCapabilities capabilities = fallback.capabilities();
    CY_CHECK_FALSE(capabilities.hrtf);
    CY_CHECK_FALSE(capabilities.geometry_occlusion);
    CY_CHECK_FALSE(capabilities.transmission);
    CY_CHECK_FALSE(capabilities.reflections);
    CY_CHECK_FALSE(capabilities.propagation);
    CY_CHECK_FALSE(capabilities.hardware_accelerated);

    AcousticQuery queries[2];
    queries[0].source = 1;
    queries[0].source_position = Vec3{5.0F, 0.0F, 0.0F};
    queries[1].source = 2;
    queries[1].source_position = Vec3{100.0F, 0.0F, 0.0F};
    AcousticResult results[2];
    CY_REQUIRE(
        fallback.simulate(Span<const AcousticQuery>(queries, 2), Span<AcousticResult>(results, 2))
            .has_value());

    // EVERY SOURCE ANSWERED. Not "the ones it could do": all of them.
    CY_CHECK_EQ(results[0].source, 1U);
    CY_CHECK_EQ(results[1].source, 2U);
    CY_CHECK_NEAR(results[0].direction.x, 1.0F, 1e-5F);
    // The far source is wetter than the near one, which is what a distance-based send does.
    CY_CHECK_GT(results[1].reverb_send, results[0].reverb_send);
    // And it claims no occlusion rather than inventing one from nothing.
    CY_CHECK_EQ(results[0].occlusion, 0.0F);

    // A results buffer too small is refused rather than writing past it.
    AcousticResult single[1];
    CY_CHECK_FALSE(
        fallback.simulate(Span<const AcousticQuery>(queries, 2), Span<AcousticResult>(single, 1))
            .has_value());
}

CY_TEST_CASE(
    "acoustics_steam: the build answers whether it is there, and refuses cleanly when not") {
    // "Steam Audio SHALL be optional and capability-gated behind the `CY_AUDIO_STEAM_AUDIO` build
    // option and a runtime capability query."
    const bool compiled = steam_audio_compiled_in();
    auto backend = create_steam_audio(allocator());
#if defined(CY_AUDIO_STEAM_AUDIO)
    CY_CHECK(compiled);
    CY_REQUIRE(backend.has_value());
    CY_CHECK(backend.value()->capabilities().geometry_occlusion);
    destroy_steam_audio(backend.value(), allocator());
#else
    CY_CHECK_FALSE(compiled);
    CY_REQUIRE_FALSE(backend.has_value());
    // AND THE REFUSAL IS `Unavailable`, not an error a caller has to handle specially: it uses the
    // fallback, and the game sounds slightly worse and behaves identically.
    CY_CHECK_EQ(backend.error().code, ErrorCode::Unavailable);
#endif
}

CY_TEST_CASE("acoustics_store: the read side never waits, and sees the last published result") {
    ResultStore store(allocator());
    CY_CHECK_EQ(store.read().size(), 0U);

    AcousticResult first;
    first.source = 7;
    first.occlusion = 0.5F;
    CY_REQUIRE(store.back().push_back(first).has_value());
    store.publish();
    CY_REQUIRE_EQ(store.read().size(), 1U);
    CY_CHECK_EQ(store.read()[0].source, 7U);
    const u64 generation = store.generation();

    // A write to the back buffer does NOT change what the reader sees until it is published — which
    // is the whole of "the realtime callback SHALL read the most recently completed result".
    store.back().clear();
    AcousticResult second;
    second.source = 9;
    CY_REQUIRE(store.back().push_back(second).has_value());
    CY_CHECK_EQ(store.read()[0].source, 7U);
    store.publish();
    CY_CHECK_EQ(store.read()[0].source, 9U);
    CY_CHECK_GT(store.generation(), generation);
}

CY_TEST_CASE(
    "acoustics_budget: the most important sources are simulated and the rest are deferred") {
    // "WHEN more sources request simulation than the budget allows THEN the highest-importance
    // sources SHALL be simulated and the remainder deferred, with the deferral reported."
    FallbackAcoustics backend;
    ResultStore store(allocator());
    SimulationBudget budget;
    budget.sources_per_update = 2;

    AcousticQuery queries[4];
    for (u32 index = 0; index < 4U; ++index) {
        queries[index].source = index + 1U;
        queries[index].importance = static_cast<f32>(index) * 0.25F;
        queries[index].source_position = Vec3{static_cast<f32>(index), 0.0F, 0.0F};
    }

    SimulationReport report;
    CY_REQUIRE(simulate_update(backend, Span<AcousticQuery>(queries, 4), budget, store, report)
                   .has_value());
    CY_CHECK_EQ(report.requested, 4U);
    CY_CHECK_EQ(report.simulated, 2U);
    CY_CHECK_EQ(report.deferred, 2U);
    CY_REQUIRE_EQ(store.read().size(), 2U);
    // The two that were simulated are the two most important — sources 4 and 3.
    CY_CHECK_EQ(store.read()[0].source, 4U);
    CY_CHECK_EQ(store.read()[1].source, 3U);

    // A budget of zero means "no limit", which is what a project with headroom configures.
    budget.sources_per_update = 0;
    CY_REQUIRE(simulate_update(backend, Span<AcousticQuery>(queries, 4), budget, store, report)
                   .has_value());
    CY_CHECK_EQ(report.simulated, 4U);
    CY_CHECK_EQ(report.deferred, 0U);
}

CY_TEST_CASE("acoustics_interpolation: a sharp change in occlusion is applied smoothly") {
    // "WHEN a listener moves and occlusion changes sharply between simulation updates THEN the
    // applied filtering SHALL interpolate rather than switching abruptly."
    AppliedAcoustics applied;
    AcousticResult clear;
    clear.occlusion = 0.0F;
    applied.advance(clear, 1.0F / 60.0F, 0.15F);
    // The first advance primes it: there is nothing to interpolate from.
    CY_CHECK_EQ(applied.applied.occlusion, 0.0F);

    AcousticResult blocked;
    blocked.occlusion = 1.0F;
    applied.advance(blocked, 1.0F / 60.0F, 0.15F);
    CY_CHECK_GT(applied.applied.occlusion, 0.0F);
    CY_CHECK_LT(applied.applied.occlusion, 0.5F);

    // Half a second later it has arrived.
    for (u32 step = 0; step < 30U; ++step) {
        applied.advance(blocked, 1.0F / 60.0F, 0.15F);
    }
    CY_CHECK_GT(applied.applied.occlusion, 0.9F);

    // A zero interpolation time is a snap, which is what a cut wants.
    AppliedAcoustics instant;
    instant.advance(clear, 1.0F / 60.0F, 0.0F);
    instant.advance(blocked, 1.0F / 60.0F, 0.0F);
    CY_CHECK_EQ(instant.applied.occlusion, 1.0F);
}
