// THE TWO ACOUSTICS PATHS, AND THE CONTRACT CONTENT MAY DEPEND ON. M8.c task 4b.3.
//
// `audio` states the rule twice and in two directions:
//
//   "When absent or disabled, the engine SHALL fall back to its own panning, distance attenuation,
//    filter-based occlusion, and reverb sends."
//
//   "Content SHALL NOT depend on Steam Audio being present: it SHALL improve audio quality, never
//    enable or gate gameplay", and its scenario — "WHEN the engine is built without Steam Audio
//    THEN all audio SHALL still play, spatialised by the fallback path, with no missing sounds and
//    no gameplay difference."
//
// ================================================================================================
// WHAT "AGREE" MEANS HERE, BECAUSE IT CANNOT MEAN "THE SAME NUMBERS"
// ================================================================================================
//
// The two backends are REQUIRED to produce different numbers: one traces geometry and the other
// does not, and if they agreed on occlusion there would be no reason to integrate the first. What
// content may depend on is the CONTRACT — the shape of the answer, not its value — and that is what
// `check_contract` below asserts over whichever backends this build has:
//
//   * every query is answered, in order, and each result carries back its own source id;
//   * the arrival direction is a unit vector, so a caller may pan with it without normalising;
//   * occlusion, transmission, reflection gain and reverb send are all in [0, 1], so a caller may
//     multiply by them without clamping;
//   * reverb time is positive, so a caller may divide by it;
//   * a results span that is too small is refused rather than half-filled;
//   * and a backend never fails on a well-formed batch, so no sound is ever missing because
//     acoustics declined to answer.
//
// A caller written against that contract behaves identically in both builds. A caller that depended
// on `occlusion > 0` would not — and that is a caller `audio` forbids, which is why the contract is
// written down here rather than left to whichever backend happened to be linked.
//
// THE BUILD QUESTION IS CHECKED AGAINST THE BUILD, NOT AGAINST ITSELF. `steam_audio_compiled_in()`
// is compared with this build's own `CY_AUDIO_STEAM_AUDIO`, read from the generated header. Until
// M8.c that comparison would have FAILED with the option on: src/audio/src/acoustics.cpp did not
// include <cy_features.h>, so every `#if defined(CY_AUDIO_STEAM_AUDIO)` in it was false in a build
// that had just fetched and linked Steam Audio. This case is the regression test for that defect.

#include <cy/test/test.h>

#include <cy/audio/acoustics.h>
#include <cy/core/memory/system_allocator.h>

#include <cy_features.h>

#include <cmath>

namespace {

using namespace cy;
using namespace cy::audio;

constexpr u32 kSources = 8;

/// A batch that covers the cases a frame actually produces: a source on top of the listener, one
/// far away, and six in between at different bearings.
void fill_queries(AcousticQuery (&queries)[kSources]) noexcept {
    for (u32 index = 0; index < kSources; ++index) {
        queries[index].source = 1000 + index;
        queries[index].listener_position = Vec3{0.0F, 1.7F, 0.0F};
        const f32 distance = static_cast<f32>(index) * 7.0F;
        queries[index].source_position = Vec3{distance, 1.7F, -static_cast<f32>(index) * 3.0F};
        queries[index].importance = 1.0F / static_cast<f32>(index + 1);
    }
}

/// Everything content may depend on, asserted over one backend.
void check_contract(AcousticsBackend& backend) {
    AcousticQuery queries[kSources];
    AcousticResult results[kSources];
    fill_queries(queries);

    // A WELL-FORMED BATCH IS ALWAYS ANSWERED. "no missing sounds" is this line.
    CY_REQUIRE(backend
                   .simulate(Span<const AcousticQuery>(queries, kSources),
                             Span<AcousticResult>(results, kSources))
                   .has_value());

    for (u32 index = 0; index < kSources; ++index) {
        const AcousticResult& result = results[index];
        // In order, and each result knows which source it is about: a caller applies these to a
        // voice by id, and a backend that reordered them would move sounds between voices.
        CY_CHECK_EQ(result.source, queries[index].source);

        const f32 length = std::sqrt((result.direction.x * result.direction.x) +
                                     (result.direction.y * result.direction.y) +
                                     (result.direction.z * result.direction.z));
        CY_CHECK_NEAR(length, 1.0F, 1e-3F);

        CY_CHECK_GE(result.occlusion, 0.0F);
        CY_CHECK_LE(result.occlusion, 1.0F);
        CY_CHECK_GE(result.transmission, 0.0F);
        CY_CHECK_LE(result.transmission, 1.0F);
        CY_CHECK_GE(result.reflection_gain, 0.0F);
        CY_CHECK_LE(result.reflection_gain, 1.0F);
        CY_CHECK_GE(result.reverb_send, 0.0F);
        CY_CHECK_LE(result.reverb_send, 1.0F);
        CY_CHECK_GT(result.reverb_time, 0.0F);
    }

    // A results span that cannot hold the answers is refused, rather than half-filled: a caller
    // that got a partial batch would apply stale parameters to the sources past the end.
    AcousticResult too_few[kSources - 1];
    Status refused = backend.simulate(Span<const AcousticQuery>(queries, kSources),
                                      Span<AcousticResult>(too_few, kSources - 1));
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::BufferTooSmall);

    // An empty batch is not an error. A frame with no audible source is an ordinary frame.
    CY_CHECK(backend.simulate(Span<const AcousticQuery>(), Span<AcousticResult>()).has_value());
}

CY_TEST_CASE("audio.parity: the fallback answers every query in every build") {
    FallbackAcoustics fallback;
    CY_CHECK_EQ(std::string_view(fallback.backend_name()), std::string_view("fallback"));
    check_contract(fallback);
}

CY_TEST_CASE("audio.parity: the build question is answered by the build, not by a guess") {
#if defined(CY_AUDIO_STEAM_AUDIO)
    // THE REGRESSION TEST. Before M8.c this assertion failed with the option ON, because the
    // translation unit that answers it could not see the option. See src/audio/src/acoustics.cpp.
    CY_CHECK(steam_audio_compiled_in());
#else
    CY_CHECK_FALSE(steam_audio_compiled_in());
#endif
}

CY_TEST_CASE("audio.parity: with the option off the refusal is nameable and the fallback stands") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    Expected<AcousticsBackend*, Error> steam = create_steam_audio(allocator);

    if (!steam_audio_compiled_in()) {
        CY_REQUIRE_FALSE(steam.has_value());
        // Unavailable, not Internal: a caller reads this as "use the fallback", which is what
        // "content SHALL NOT depend on Steam Audio being present" requires of it.
        CY_CHECK_EQ(steam.error().code, ErrorCode::Unavailable);

        // And the game still has acoustics.
        FallbackAcoustics fallback;
        check_contract(fallback);
        return;
    }

    // With the option on the backend must be constructible, and it must satisfy the same contract
    // the fallback does. `audio` permits it to answer differently; it does not permit it to answer
    // in a shape a caller cannot use.
    CY_REQUIRE(steam.has_value());
    check_contract(*steam.value());
    destroy_steam_audio(steam.value(), allocator);
}

CY_TEST_CASE("audio.parity: the capability query, not the linked backend, is what a caller asks") {
    FallbackAcoustics fallback;
    const AcousticsCapabilities capabilities = fallback.capabilities();
    // The fallback claims nothing it does not do. Its reverb is a send the mix performs, and
    // saying it traced geometry would defeat the capability query with its own implementation.
    CY_CHECK_FALSE(capabilities.geometry_occlusion);
    CY_CHECK_FALSE(capabilities.hrtf);
    CY_CHECK_FALSE(capabilities.propagation);
    CY_CHECK(capabilities.reverb);

    // Geometry handed to a backend that has none is ignored rather than refused: a host that
    // extracts geometry every frame must not have to ask which backend it has.
    const AcousticTriangle triangle{Vec3{0.0F, 0.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F},
                                    Vec3{0.0F, 1.0F, 0.0F}, 0};
    const AcousticMaterial material;
    fallback.set_geometry(Span<const AcousticTriangle>(&triangle, 1),
                          Span<const AcousticMaterial>(&material, 1));
    check_contract(fallback);
}

}  // namespace
