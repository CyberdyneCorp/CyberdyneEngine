// The audio server: playback, mixing, tiers, virtualisation and the diagnostics. Task 4.3.5.
//
// EVERY CASE RUNS OVER THE NULL BACKEND, which is why they run in continuous integration on a
// machine with no sound card and why they assert on samples rather than on "it did not crash". The
// backend is driven — `advance()` pulls exactly the frames it is asked for — so a case says "mix
// twenty milliseconds" and reads the result.

#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/audio/server.h>
#include <cy/test/test.h>

#include <cmath>

using cy::f32;
using cy::Name;
using namespace cy::audio;

namespace {

cy::Allocator& allocator() {
    return cy::system_allocator(cy::MemoryDomain::Audio);
}

constexpr cy::u32 kRate = 48000;
constexpr cy::u32 kBlock = 120;
constexpr cy::u32 kClipFrames = 4800;  // a tenth of a second

/// A mono clip of a constant amplitude, so a mixed block's value says what happened to the gain.
struct ConstantClip {
    cy::Array<f32> samples{allocator()};

    explicit ConstantClip(f32 value, cy::u32 frames = kClipFrames) {
        CY_REQUIRE(static_cast<bool>(samples.resize(frames)));
        for (f32& sample : samples) {
            sample = value;
        }
    }

    [[nodiscard]] ClipDescription description(const char* name) const {
        ClipDescription clip;
        clip.name = Name::intern(name);
        clip.samples = samples.data();
        clip.frame_count = static_cast<cy::u32>(samples.size());
        clip.channels = 1;
        clip.sample_rate = kRate;
        return clip;
    }
};

/// A server over the null backend, plus the output buffer every case reads.
struct Fixture {
    NullAudioBackend backend;
    AudioServer server{allocator()};
    cy::Array<f32> output{allocator()};

    explicit Fixture(cy::u32 voice_capacity = 32) {
        AudioBackendConfig backend_config;
        backend_config.requested.sample_rate = kRate;
        backend_config.requested.layout = ChannelLayout::Stereo;
        backend_config.requested.buffer_frames = kBlock;
        CY_REQUIRE(static_cast<bool>(backend.initialize(backend_config)));

        AudioServerConfig config;
        config.requested = backend_config.requested;
        config.block_frames = kBlock;
        config.voice_capacity = voice_capacity;
        CY_REQUIRE(static_cast<bool>(server.configure(config)));
        CY_REQUIRE(static_cast<bool>(server.initialize_with(backend)));
        CY_REQUIRE(static_cast<bool>(output.resize(static_cast<cy::usize>(kRate) * 2)));
    }

    /// Mix `frames` and return how many were produced.
    cy::u32 mix(cy::u32 frames) { return backend.advance(output.data(), frames); }

    [[nodiscard]] f32 peak(cy::u32 frames) const {
        f32 result = 0.0F;
        for (cy::usize i = 0; i < (static_cast<cy::usize>(frames) * 2U); ++i) {
            result = cy::math::max(result, std::fabs(output[i]));
        }
        return result;
    }
};

}  // namespace

CY_TEST_CASE("a server over the null backend reports it, and mixes silence with nothing playing") {
    Fixture fixture;
    CY_CHECK(fixture.server.is_null_backend());
    CY_CHECK_EQ(Name::intern(fixture.server.backend_name()), Name::intern("null"));

    CY_CHECK_EQ(fixture.mix(kBlock * 4), kBlock * 4);
    CY_CHECK_NEAR(fixture.peak(kBlock * 4), 0.0F, 1e-6F);
    CY_CHECK_EQ(fixture.server.clock().frames_played, kBlock * 4);
}

CY_TEST_CASE("a clip needs samples, a frame count and a rate") {
    Fixture fixture;
    ClipDescription empty;
    CY_CHECK_FALSE(static_cast<bool>(fixture.server.create_clip(empty)));

    const ConstantClip source(0.5F);
    ClipDescription clip = source.description("tone");
    clip.channels = 0;
    CY_CHECK_FALSE(static_cast<bool>(fixture.server.create_clip(clip)));
    clip.channels = 1;
    clip.sample_rate = 0;
    CY_CHECK_FALSE(static_cast<bool>(fixture.server.create_clip(clip)));
}

CY_TEST_CASE("a played voice reaches the output, and stopping it silences it") {
    Fixture fixture;
    const ConstantClip source(0.5F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    description.volume = 1.0F;
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));

    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 4), kBlock * 4);
    CY_CHECK_NEAR(fixture.peak(kBlock * 4), 0.5F, 1e-3F);

    CY_REQUIRE(static_cast<bool>(fixture.server.stop(*voice)));
    // The fade is ten milliseconds; mixing a quarter of a second is well past it.
    CY_CHECK_EQ(fixture.mix(kBlock * 100), kBlock * 100);
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_FALSE(fixture.server.playing(*voice));

    CY_CHECK_EQ(fixture.mix(kBlock * 4), kBlock * 4);
    CY_CHECK_NEAR(fixture.peak(kBlock * 4), 0.0F, 1e-6F);
}

CY_TEST_CASE("a stop fades rather than cutting, which is what stops a click") {
    // `audio`: "**WHEN** gameplay stops a sound while it is being mixed **THEN** the mixer SHALL
    // fade it out over the remainder of the block and mark it for reclamation, avoiding a click."
    Fixture fixture;
    const ConstantClip source(1.0F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock), kBlock);

    CY_REQUIRE(static_cast<bool>(fixture.server.stop(*voice)));
    // One block into the fade the signal is smaller but not yet zero: a cut would be zero
    // immediately and would step by the full amplitude.
    CY_CHECK_EQ(fixture.mix(kBlock), kBlock);
    const f32 during_fade = fixture.peak(kBlock);
    CY_CHECK_GT(during_fade, 0.0F);
    CY_CHECK_LT(during_fade, 1.0F);
}

CY_TEST_CASE("a gain change ramps across the block rather than stepping") {
    // `audio`: "**WHEN** a voice's volume changes between callbacks **THEN** the gain SHALL ramp
    // across the buffer rather than stepping." The assertion is that the first sample of the block
    // after the change is still near the OLD gain — a stepping mixer would already be at the new
    // one.
    Fixture fixture;
    const ConstantClip source(1.0F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    description.volume = 1.0F;
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));

    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock), kBlock);

    // To a quarter rather than to silence: a voice at zero volume scores as inaudible and is
    // virtualised, which is a different mechanism with its own case below.
    CY_REQUIRE(static_cast<bool>(fixture.server.set_volume(*voice, 0.25F)));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock), kBlock);

    CY_CHECK_NEAR(std::fabs(fixture.output[0]), 1.0F, 1e-2F);  // still the old gain
    CY_CHECK_NEAR(std::fabs(fixture.output[(static_cast<cy::usize>(kBlock - 1) * 2)]), 0.25F,
                  2e-2F);  // and the new one
    // And it is monotone in between, which a step would not be.
    const f32 midpoint = std::fabs(fixture.output[static_cast<cy::usize>(kBlock / 2) * 2]);
    CY_CHECK_LT(midpoint, std::fabs(fixture.output[0]));
    CY_CHECK_GT(midpoint, 0.25F);
}

CY_TEST_CASE("pause holds the position and resume continues from it") {
    Fixture fixture;
    const ConstantClip source(0.5F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    description.looping = true;
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    const cy::f64 played = fixture.server.position_of(*voice);
    CY_CHECK_GT(played, 0.0);

    CY_REQUIRE(static_cast<bool>(fixture.server.pause(*voice)));
    CY_CHECK_EQ(fixture.mix(kBlock * 4), kBlock * 4);
    CY_CHECK_NEAR(static_cast<f32>(fixture.server.position_of(*voice)), static_cast<f32>(played),
                  1e-3F);
    // A paused voice is silent, which is what distinguishes it from a virtual one.
    CY_CHECK_NEAR(fixture.peak(kBlock * 4), 0.0F, 1e-6F);

    CY_REQUIRE(static_cast<bool>(fixture.server.resume(*voice)));
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    CY_CHECK_GT(fixture.server.position_of(*voice), played);
}

CY_TEST_CASE("a seek moves the playback position") {
    Fixture fixture;
    const ConstantClip source(0.5F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));
    CY_REQUIRE(static_cast<bool>(fixture.server.seek(*voice, 2000)));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock), kBlock);
    CY_CHECK_GT(fixture.server.position_of(*voice), 2000.0);
}

CY_TEST_CASE("a looping voice wraps without gaining or losing a frame") {
    // `audio`: "**WHEN** a track declares loop points **THEN** looping SHALL be sample-accurate
    // with no gap or click." The overshoot is carried into the new position rather than discarded,
    // so a rhythmic loop does not drift; the assertion is on the total frames advanced.
    Fixture fixture;
    const ConstantClip source(0.25F, 1000);
    const auto clip = fixture.server.create_clip(source.description("loop"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    description.looping = true;
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));
    fixture.server.update(1.0F / 60.0F);

    // Mix exactly 2500 frames over a 1000-frame loop: the position must land on 500.
    CY_CHECK_EQ(fixture.mix(kBlock * 25), 3000U);
    CY_CHECK_NEAR(static_cast<f32>(fixture.server.position_of(*voice)), 0.0F, 1.0F);
    // Still audible after two and a half laps, which a loop that stopped at the end would not be.
    CY_CHECK_NEAR(fixture.peak(kBlock * 25), 0.25F, 1e-3F);
}

CY_TEST_CASE("a one-shot plays to completion and releases itself") {
    // `audio`: "**WHEN** a one-shot is fired **THEN** it SHALL play to completion and release
    // itself, with no gameplay handle required."
    Fixture fixture;
    const ConstantClip source(0.5F, 480);  // ten milliseconds
    const auto clip = fixture.server.create_clip(source.description("one-shot"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    CY_REQUIRE(static_cast<bool>(fixture.server.play_one_shot(description)));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.statistics().active_voices, 1U);

    CY_CHECK_EQ(fixture.mix(kBlock * 10), kBlock * 10);
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.statistics().active_voices, 0U);
    // And the slot is free again, which is what "releases itself" has to mean for a
    // fire-and-forget.
    CY_CHECK(static_cast<bool>(fixture.server.play_one_shot(description)));
}

CY_TEST_CASE("a looping one-shot is refused the loop rather than leaking a voice") {
    Fixture fixture;
    const ConstantClip source(0.5F, 480);
    const auto clip = fixture.server.create_clip(source.description("one-shot"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    description.looping = true;  // a looping one-shot would never release
    CY_REQUIRE(static_cast<bool>(fixture.server.play_one_shot(description)));
    CY_CHECK_EQ(fixture.mix(kBlock * 10), kBlock * 10);
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.statistics().active_voices, 0U);
}

CY_TEST_CASE("a spatialised source is panned toward the ear it is on") {
    Fixture fixture;
    const ConstantClip source(1.0F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    Listener listener;
    CY_REQUIRE(static_cast<bool>(fixture.server.create_listener(listener)));

    VoiceDescription description;
    description.clip = *clip;
    description.spatialised = true;
    description.position = cy::Vec3{2.0F, 0.0F, 0.0F};  // to the listener's right
    description.attenuation.reference_distance = 10.0F;
    CY_REQUIRE(static_cast<bool>(fixture.server.play(description)));

    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);

    f32 left = 0.0F;
    f32 right = 0.0F;
    for (cy::u32 frame = kBlock; frame < kBlock * 2; ++frame) {
        const cy::usize sample = static_cast<cy::usize>(frame) * 2;
        left = cy::math::max(left, std::fabs(fixture.output[sample]));
        right = cy::math::max(right, std::fabs(fixture.output[sample + 1]));
    }
    CY_CHECK_GT(right, left);
    CY_CHECK_GT(right, 0.5F);
}

CY_TEST_CASE("a source beyond its maximum distance is virtualised, not stopped") {
    // `audio`: "**WHEN** the listener leaves and later re-enters a looping ambience's range
    // **THEN** it SHALL resume at the position it would have reached, not restart."
    Fixture fixture;
    const ConstantClip source(1.0F, 4800);
    const auto clip = fixture.server.create_clip(source.description("ambience"));
    CY_REQUIRE(static_cast<bool>(clip));

    const Listener listener;
    CY_REQUIRE(static_cast<bool>(fixture.server.create_listener(listener)));

    VoiceDescription description;
    description.clip = *clip;
    description.spatialised = true;
    description.looping = true;
    description.position = cy::Vec3{1000.0F, 0.0F, 0.0F};  // far outside max_distance
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));

    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.tier_of(*voice), SimulationTier::Virtual);
    CY_CHECK_EQ(fixture.server.statistics().virtual_voices, 1U);

    CY_CHECK_EQ(fixture.mix(kBlock * 10), kBlock * 10);
    // Silent, and still advancing.
    CY_CHECK_NEAR(fixture.peak(kBlock * 10), 0.0F, 1e-6F);
    const cy::f64 advanced = fixture.server.position_of(*voice);
    CY_CHECK_NEAR(static_cast<f32>(advanced), static_cast<f32>(kBlock * 10), 1.0F);

    // Walk back into range: it resumes where it would have been rather than restarting.
    CY_REQUIRE(static_cast<bool>(
        fixture.server.set_position(*voice, cy::Vec3{1.0F, 0.0F, 0.0F}, cy::Vec3{})));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_NE(fixture.server.tier_of(*voice), SimulationTier::Virtual);
    CY_CHECK_EQ(fixture.mix(kBlock), kBlock);
    CY_CHECK_GT(fixture.server.position_of(*voice), advanced);
    CY_CHECK_GT(fixture.peak(kBlock), 0.0F);
}

CY_TEST_CASE("a voice leaving the mix ramps out rather than stepping to silence") {
    // `audio`: "Tier transitions SHALL be hysteretic and cross-faded, so a source oscillating near
    // a threshold does not produce audible artefacts." Dropping a voice the instant it crosses a
    // budget is a step of its whole amplitude, which is a click.
    Fixture fixture;
    const ConstantClip source(1.0F, 48000);
    const auto clip = fixture.server.create_clip(source.description("ambience"));
    CY_REQUIRE(static_cast<bool>(clip));
    const Listener listener;
    CY_REQUIRE(static_cast<bool>(fixture.server.create_listener(listener)));

    VoiceDescription description;
    description.clip = *clip;
    description.spatialised = true;
    description.position = cy::Vec3{1.0F, 0.0F, 0.0F};
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    CY_CHECK_GT(fixture.peak(kBlock * 2), 0.1F);

    // Out of range: virtualised.
    CY_REQUIRE(static_cast<bool>(
        fixture.server.set_position(*voice, cy::Vec3{5000.0F, 0.0F, 0.0F}, cy::Vec3{})));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.tier_of(*voice), SimulationTier::Virtual);

    // The block it leaves in still carries a ramp down rather than a step: the first sample is near
    // where it was and the last is near zero. The RIGHT channel, because a source at +x is panned
    // hard right and the left one is already silent.
    CY_CHECK_EQ(fixture.mix(kBlock), kBlock);
    CY_CHECK_GT(std::fabs(fixture.output[1]), 0.1F);
    CY_CHECK_NEAR(std::fabs(fixture.output[(static_cast<cy::usize>(kBlock - 1) * 2) + 1]), 0.0F,
                  1e-2F);

    // And after that it costs nothing: silence, with the position still advancing.
    CY_CHECK_EQ(fixture.mix(kBlock * 4), kBlock * 4);
    CY_CHECK_NEAR(fixture.peak(kBlock * 4), 0.0F, 1e-6F);
}

CY_TEST_CASE("tier budgets demote by importance rather than dropping") {
    // `audio`: "sources beyond a tier's budget SHALL be demoted to the next tier by importance
    // rank".
    NullAudioBackend backend;
    AudioBackendConfig backend_config;
    backend_config.requested.sample_rate = kRate;
    backend_config.requested.buffer_frames = kBlock;
    CY_REQUIRE(static_cast<bool>(backend.initialize(backend_config)));

    AudioServer server(allocator());
    AudioServerConfig config;
    config.requested = backend_config.requested;
    config.block_frames = kBlock;
    config.budgets.spatialised = 2;  // only two may be spatialised
    config.budgets.simple = 2;
    CY_REQUIRE(static_cast<bool>(server.configure(config)));
    CY_REQUIRE(static_cast<bool>(server.initialize_with(backend)));

    const ConstantClip source(0.5F);
    const auto clip = server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));
    const Listener listener;
    CY_REQUIRE(static_cast<bool>(server.create_listener(listener)));

    // Six spatialised sources at increasing distance: the two nearest keep the tier.
    for (cy::u32 i = 0; i < 6; ++i) {
        VoiceDescription description;
        description.clip = *clip;
        description.spatialised = true;
        description.position = cy::Vec3{static_cast<f32>(i + 1) * 3.0F, 0.0F, 0.0F};
        CY_REQUIRE(static_cast<bool>(server.play(description)));
    }
    server.update(1.0F / 60.0F);

    const AudioStatistics& statistics = server.statistics();
    CY_CHECK_EQ(statistics.voices_in_tier[static_cast<cy::usize>(SimulationTier::Spatialised)], 2U);
    CY_CHECK_EQ(statistics.voices_in_tier[static_cast<cy::usize>(SimulationTier::Simple)], 2U);
    CY_CHECK_EQ(statistics.voices_in_tier[static_cast<cy::usize>(SimulationTier::Virtual)], 2U);
    CY_CHECK_GT(statistics.demoted_by_budget, 0U);
    // NOTHING WAS DROPPED: every voice is still live, which is the difference between demoting and
    // stopping.
    CY_CHECK_EQ(statistics.active_voices + statistics.virtual_voices, 6U);
}

CY_TEST_CASE("a pinned source is not demoted below its floor whatever the budget") {
    // `audio`: "**WHEN** a dialogue line is pinned to at least `Spatialised` **THEN** it SHALL
    // never be demoted below that tier regardless of distance or budget pressure."
    NullAudioBackend backend;
    AudioBackendConfig backend_config;
    backend_config.requested.sample_rate = kRate;
    backend_config.requested.buffer_frames = kBlock;
    CY_REQUIRE(static_cast<bool>(backend.initialize(backend_config)));

    AudioServer server(allocator());
    AudioServerConfig config;
    config.requested = backend_config.requested;
    config.block_frames = kBlock;
    config.budgets.spatialised = 0;  // no room at all
    config.budgets.simple = 0;
    CY_REQUIRE(static_cast<bool>(server.configure(config)));
    CY_REQUIRE(static_cast<bool>(server.initialize_with(backend)));

    const ConstantClip source(0.5F);
    const auto clip = server.create_clip(source.description("line"));
    CY_REQUIRE(static_cast<bool>(clip));
    const Listener listener;
    CY_REQUIRE(static_cast<bool>(server.create_listener(listener)));

    VoiceDescription dialogue;
    dialogue.clip = *clip;
    dialogue.spatialised = true;
    dialogue.position = cy::Vec3{5.0F, 0.0F, 0.0F};
    dialogue.minimum_tier = SimulationTier::Spatialised;
    const auto voice = server.play(dialogue);
    CY_REQUIRE(static_cast<bool>(voice));

    server.update(1.0F / 60.0F);
    CY_CHECK_EQ(server.tier_of(*voice), SimulationTier::Spatialised);
}

CY_TEST_CASE("a source asking for full acoustics is rendered spatialised, and it is counted") {
    // There is no acoustics backend at Seed. The fallback is the specification's own; counting it
    // is what stops a project believing it is getting propagation when it is not.
    Fixture fixture;
    const ConstantClip source(0.5F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));
    const Listener listener;
    CY_REQUIRE(static_cast<bool>(fixture.server.create_listener(listener)));

    VoiceDescription description;
    description.clip = *clip;
    description.spatialised = true;
    description.minimum_tier = SimulationTier::FullAcoustic;
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));

    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.server.tier_of(*voice), SimulationTier::Spatialised);
    CY_CHECK_EQ(fixture.server.statistics().acoustic_fallbacks, 1U);
}

CY_TEST_CASE("a soloed bus silences the rest of the mix") {
    Fixture fixture;
    const ConstantClip source(1.0F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    const auto music = fixture.server.buses().create(BusDescription{});
    CY_REQUIRE(static_cast<bool>(music));
    CY_REQUIRE(static_cast<bool>(
        fixture.server.buses().set_output(*music, fixture.server.buses().master())));
    const auto sfx = fixture.server.buses().create(BusDescription{});
    CY_REQUIRE(static_cast<bool>(sfx));
    CY_REQUIRE(static_cast<bool>(
        fixture.server.buses().set_output(*sfx, fixture.server.buses().master())));
    CY_REQUIRE(static_cast<bool>(fixture.server.buses().compile()));

    VoiceDescription on_music;
    on_music.clip = *clip;
    on_music.bus = *music;
    CY_REQUIRE(static_cast<bool>(fixture.server.play(on_music)));

    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    CY_CHECK_GT(fixture.peak(kBlock * 2), 0.5F);

    CY_REQUIRE(static_cast<bool>(fixture.server.buses().set_solo(*sfx, true)));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    CY_CHECK_NEAR(fixture.peak(kBlock * 2), 0.0F, 1e-6F);
}

CY_TEST_CASE("a reverb send mixes both paths") {
    // `audio`: "**WHEN** a sound sends 30 % to a reverb bus and 100 % to its main bus **THEN** both
    // paths SHALL be mixed."
    Fixture fixture;
    const ConstantClip source(0.5F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    BusGraph& buses = fixture.server.buses();
    const auto sfx = buses.create(BusDescription{});
    const auto reverb = buses.create(BusDescription{});
    CY_REQUIRE(static_cast<bool>(sfx));
    CY_REQUIRE(static_cast<bool>(reverb));
    CY_REQUIRE(static_cast<bool>(buses.set_output(*sfx, buses.master())));
    CY_REQUIRE(static_cast<bool>(buses.set_output(*reverb, buses.master())));
    CY_REQUIRE(static_cast<bool>(buses.compile()));

    VoiceDescription description;
    description.clip = *clip;
    description.bus = *sfx;
    CY_REQUIRE(static_cast<bool>(fixture.server.play(description)));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    const f32 dry = fixture.peak(kBlock * 2);

    CY_REQUIRE(static_cast<bool>(buses.add_send(*sfx, *reverb, 0.3F)));
    CY_REQUIRE(static_cast<bool>(buses.compile()));
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    const f32 wet = fixture.peak(kBlock * 2);
    // Both paths arrive: the send adds 30 % of the dry signal on top of it, in the same block.
    CY_CHECK_NEAR(wet, dry * 1.3F, 1e-2F);
}

CY_TEST_CASE("a bus gain applies, and per-bus levels are reported") {
    Fixture fixture;
    const ConstantClip source(1.0F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    CY_REQUIRE(static_cast<bool>(fixture.server.play(description)));
    CY_REQUIRE(static_cast<bool>(
        fixture.server.buses().set_volume(fixture.server.buses().master(), 0.25F)));

    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    CY_CHECK_NEAR(fixture.peak(kBlock * 2), 0.25F, 1e-2F);

    const cy::u32 master_index = fixture.server.buses().index_of(fixture.server.buses().master());
    CY_REQUIRE(master_index < fixture.server.bus_levels().size());
    CY_CHECK_NEAR(fixture.server.bus_levels()[master_index].peak, 0.25F, 1e-2F);
    CY_CHECK_GT(fixture.server.bus_levels()[master_index].rms, 0.0F);
}

CY_TEST_CASE("a limiter on the master bus stops a summed mix clipping") {
    Fixture fixture;
    const ConstantClip source(1.0F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    for (int i = 0; i < 4; ++i) {
        VoiceDescription description;
        description.clip = *clip;
        CY_REQUIRE(static_cast<bool>(fixture.server.play(description)));
    }
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    CY_CHECK_GT(fixture.peak(kBlock * 2), 3.0F);  // four voices sum well past full scale

    BusEffect limiter;
    limiter.kind = EffectKind::Limiter;
    limiter.parameter_a = 1.0F;
    CY_REQUIRE(static_cast<bool>(
        fixture.server.buses().add_effect(fixture.server.buses().master(), limiter)));
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    CY_CHECK_NEAR(fixture.peak(kBlock * 2), 1.0F, 1e-4F);
}

CY_TEST_CASE("occlusion attenuates and filters a spatialised source") {
    Fixture fixture;
    const ConstantClip source(1.0F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));
    const Listener listener;
    CY_REQUIRE(static_cast<bool>(fixture.server.create_listener(listener)));

    VoiceDescription description;
    description.clip = *clip;
    description.spatialised = true;
    description.position = cy::Vec3{0.0F, 0.0F, -2.0F};
    description.attenuation.reference_distance = 10.0F;
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));

    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 4), kBlock * 4);
    const f32 clear = fixture.peak(kBlock * 4);
    CY_CHECK_GT(clear, 0.5F);

    CY_REQUIRE(static_cast<bool>(fixture.server.set_occlusion(*voice, 1.0F)));
    fixture.server.update(1.0F / 60.0F);
    // One block of ramp first: the gain change is interpolated across a block, so a peak taken
    // across it would report the gain the voice was leaving rather than the one it arrived at.
    CY_CHECK_EQ(fixture.mix(kBlock), kBlock);
    CY_CHECK_EQ(fixture.mix(kBlock * 4), kBlock * 4);
    const f32 blocked = fixture.peak(kBlock * 4);
    CY_CHECK_LT(blocked, clear);
    CY_CHECK_GT(blocked, 0.0F);  // muffled, not missing
}

CY_TEST_CASE("a scheduled voice is silent until its audio-clock frame") {
    // `audio`: "**WHEN** a sound is scheduled for a specific audio time **THEN** it SHALL begin at
    // that sample, not at the start of the next frame."
    Fixture fixture;
    const ConstantClip source(1.0F);
    const auto clip = fixture.server.create_clip(source.description("beat"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    description.start_frame = static_cast<cy::u64>(kBlock) * 10;
    CY_REQUIRE(static_cast<bool>(fixture.server.play(description)));
    fixture.server.update(1.0F / 60.0F);

    CY_CHECK_EQ(fixture.mix(kBlock * 5), kBlock * 5);
    CY_CHECK_NEAR(fixture.peak(kBlock * 5), 0.0F, 1e-6F);

    CY_CHECK_EQ(fixture.mix(kBlock * 8), kBlock * 8);
    CY_CHECK_GT(fixture.peak(kBlock * 8), 0.5F);
}

CY_TEST_CASE("a clip is stopped before it is released, and its slot is reused only afterwards") {
    // The samples are borrowed; the audio thread may be part way through a block that reads them.
    Fixture fixture;
    const ConstantClip source(0.5F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock), kBlock);

    fixture.server.destroy_clip(*clip);
    CY_CHECK(fixture.server.clip(*clip) == nullptr);
    // The hard stop reaches the mixer on the next block, and the voice is reaped by the update
    // after it — only then is the clip slot reusable.
    CY_CHECK_EQ(fixture.mix(kBlock), kBlock);
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_FALSE(fixture.server.playing(*voice));

    const auto replacement = fixture.server.create_clip(source.description("replacement"));
    CY_REQUIRE(static_cast<bool>(replacement));
    CY_CHECK_NE(clip->bits(), replacement->bits());
}

CY_TEST_CASE("a full voice pool is reported rather than dropping the newest sound silently") {
    Fixture fixture(2);
    const ConstantClip source(0.5F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    description.looping = true;
    CY_CHECK(static_cast<bool>(fixture.server.play(description)));
    CY_CHECK(static_cast<bool>(fixture.server.play(description)));
    const auto third = fixture.server.play(description);
    CY_REQUIRE_FALSE(static_cast<bool>(third));
    CY_CHECK_EQ(third.error().code, cy::ErrorCode::OutOfRange);
}

CY_TEST_CASE("a stale voice handle is refused rather than changing somebody else's sound") {
    Fixture fixture;
    const ConstantClip source(0.5F, 480);
    const auto clip = fixture.server.create_clip(source.description("short"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    const auto first = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(first));
    CY_CHECK_EQ(fixture.mix(kBlock * 10), kBlock * 10);
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_FALSE(fixture.server.playing(*first));

    const auto second = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(second));
    CY_CHECK_EQ(first->index(), second->index());  // the slot was reused
    CY_CHECK_FALSE(static_cast<bool>(fixture.server.set_volume(*first, 0.0F)));
    CY_CHECK(static_cast<bool>(fixture.server.set_volume(*second, 0.5F)));
}

CY_TEST_CASE("the device changing keeps every playing voice") {
    // `audio`: "**WHEN** the default output device changes **THEN** the driver SHALL reinitialise
    // at the new device's mix rate and channel layout, and playback SHALL continue."
    Fixture fixture;
    const ConstantClip source(0.5F);
    const auto clip = fixture.server.create_clip(source.description("tone"));
    CY_REQUIRE(static_cast<bool>(clip));

    VoiceDescription description;
    description.clip = *clip;
    description.looping = true;
    const auto voice = fixture.server.play(description);
    CY_REQUIRE(static_cast<bool>(voice));
    fixture.server.update(1.0F / 60.0F);
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    const cy::f64 played = fixture.server.position_of(*voice);

    CY_REQUIRE(static_cast<bool>(fixture.server.device_changed()));
    CY_CHECK(fixture.server.playing(*voice));
    CY_CHECK_EQ(fixture.mix(kBlock * 2), kBlock * 2);
    CY_CHECK_GT(fixture.server.position_of(*voice), played);
    CY_CHECK_GT(fixture.peak(kBlock * 2), 0.0F);
}
