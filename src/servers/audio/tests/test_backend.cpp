// The `AudioBackend` interface and the null implementation that is kept forever. Task 4.3.4.
//
// design.md §4: "The retained trivial implementation is not ceremony: it is what proves at every
// build that the interface does not leak the library." These cases are that proof — this
// translation unit includes no miniaudio header, has no device, and exercises the whole interface.

#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/audio/backend.h>
#include <cy/test/test.h>

using cy::f32;
using namespace cy::audio;

namespace {

cy::Allocator& allocator() {
    return cy::system_allocator(cy::MemoryDomain::Audio);
}

struct Counter {
    cy::u32 calls = 0;
    cy::u32 frames = 0;
    cy::u32 channels = 0;
};

void counting_render(f32* frames, cy::u32 frame_count, cy::u32 channels, void* user) noexcept {
    auto* counter = static_cast<Counter*>(user);
    if (counter != nullptr) {
        ++counter->calls;
        counter->frames += frame_count;
        counter->channels = channels;
    }
    // Write a constant so a caller can tell the callback ran from the buffer alone.
    for (cy::usize i = 0; i < static_cast<cy::usize>(frame_count) * channels; ++i) {
        frames[i] = 0.5F;
    }
}

[[nodiscard]] AudioBackendConfig config_at(cy::u32 rate, ChannelLayout layout, cy::u32 buffer) {
    AudioBackendConfig config;
    config.requested.sample_rate = rate;
    config.requested.layout = layout;
    config.requested.buffer_frames = buffer;
    return config;
}

}  // namespace

CY_TEST_CASE("the null backend grants the requested format exactly") {
    // A real device negotiates; this one does not, because a test that asked for 48 kHz stereo and
    // silently got something else would assert against the wrong numbers.
    NullAudioBackend backend;
    CY_REQUIRE(static_cast<bool>(backend.initialize(config_at(44100, ChannelLayout::Mono, 256))));
    CY_CHECK_EQ(backend.format().sample_rate, 44100U);
    CY_CHECK_EQ(backend.format().channels(), 1U);
    CY_CHECK_EQ(backend.format().buffer_frames, 256U);
    CY_CHECK(backend.is_null_backend());
    CY_CHECK_EQ(cy::Name::intern(backend.name()), cy::Name::intern("null"));
}

CY_TEST_CASE("a format with a zero in it is refused") {
    NullAudioBackend backend;
    CY_CHECK_FALSE(static_cast<bool>(backend.initialize(config_at(0, ChannelLayout::Stereo, 480))));
    CY_CHECK_FALSE(
        static_cast<bool>(backend.initialize(config_at(48000, ChannelLayout::Stereo, 0))));
    CY_CHECK_FALSE(
        static_cast<bool>(backend.initialize(config_at(48000, ChannelLayout::Count, 480))));
}

CY_TEST_CASE("the null backend advances deterministically, in whole blocks") {
    // `audio`: "**WHEN** the engine runs headless or in tests **THEN** the null backend SHALL
    // satisfy the interface, advancing playback positions deterministically without a device."
    NullAudioBackend backend;
    CY_REQUIRE(static_cast<bool>(backend.initialize(config_at(48000, ChannelLayout::Stereo, 100))));

    Counter counter;
    CY_REQUIRE(static_cast<bool>(backend.start(&counting_render, &counter)));
    CY_CHECK(backend.running());

    cy::Array<f32> output(allocator());
    CY_REQUIRE(static_cast<bool>(output.resize(static_cast<cy::usize>(1000) * 2)));

    // 450 frames at a block of 100 is four whole blocks: a partial block is not what a device would
    // ask for, and rounding up would write past what the caller sized.
    CY_CHECK_EQ(backend.advance(output.data(), 450), 400U);
    CY_CHECK_EQ(counter.calls, 4U);
    CY_CHECK_EQ(counter.frames, 400U);
    CY_CHECK_EQ(counter.channels, 2U);
    CY_CHECK_EQ(backend.frames_played(), 400U);

    // The mix is kept, which is what lets a test assert on it rather than only on the bookkeeping.
    CY_CHECK_NEAR(output[0], 0.5F, 1e-6F);
    CY_CHECK_NEAR(output[(static_cast<cy::usize>(399) * 2) + 1], 0.5F, 1e-6F);

    // Deterministic: the same call again advances by exactly the same amount.
    CY_CHECK_EQ(backend.advance(output.data(), 450), 400U);
    CY_CHECK_EQ(backend.frames_played(), 800U);
}

CY_TEST_CASE("a stopped backend produces nothing") {
    NullAudioBackend backend;
    CY_REQUIRE(static_cast<bool>(backend.initialize(config_at(48000, ChannelLayout::Stereo, 100))));
    Counter counter;
    CY_REQUIRE(static_cast<bool>(backend.start(&counting_render, &counter)));
    backend.stop();
    CY_CHECK_FALSE(backend.running());

    cy::Array<f32> output(allocator());
    CY_REQUIRE(static_cast<bool>(output.resize(static_cast<cy::usize>(200) * 2)));
    CY_CHECK_EQ(backend.advance(output.data(), 200), 0U);
    CY_CHECK_EQ(counter.calls, 0U);
}

CY_TEST_CASE("a backend that is not initialized cannot be started") {
    NullAudioBackend backend;
    Counter counter;
    CY_CHECK_FALSE(static_cast<bool>(backend.start(&counting_render, &counter)));
    CY_REQUIRE(static_cast<bool>(backend.initialize(config_at(48000, ChannelLayout::Stereo, 100))));
    CY_CHECK_FALSE(static_cast<bool>(backend.start(nullptr, nullptr)));
}

CY_TEST_CASE("the null backend enumerates one device and refuses any other") {
    NullAudioBackend backend;
    CY_REQUIRE(static_cast<bool>(backend.initialize(config_at(48000, ChannelLayout::Stereo, 480))));

    cy::Array<AudioDeviceInfo> devices(allocator());
    CY_REQUIRE(static_cast<bool>(backend.enumerate_outputs(devices)));
    CY_REQUIRE_EQ(devices.size(), 1U);
    CY_CHECK(devices[0].is_default);
    CY_CHECK_EQ(devices[0].id, cy::Name::intern("null"));

    CY_CHECK(static_cast<bool>(backend.select_output(devices[0].id)));
    CY_CHECK(static_cast<bool>(backend.select_output(cy::Name{})));  // the default
    CY_CHECK_FALSE(static_cast<bool>(backend.select_output(cy::Name::intern("headphones"))));
}

CY_TEST_CASE("a backend that cannot capture says so rather than producing silence") {
    // An error at start-up beats a stream of silence a caller has to diagnose.
    NullAudioBackend backend;
    const cy::Status captured = backend.start_capture(nullptr, nullptr);
    CY_REQUIRE_FALSE(static_cast<bool>(captured));
    CY_CHECK_EQ(captured.error().code, cy::ErrorCode::Unsupported);
}

CY_TEST_CASE("the null backend reports no output latency, rather than a plausible number") {
    // There is no device, so there is nothing between the mix and the listener. A made-up latency
    // would make a scheduling test pass against a number nobody measured.
    NullAudioBackend backend;
    CY_REQUIRE(static_cast<bool>(backend.initialize(config_at(48000, ChannelLayout::Stereo, 480))));
    CY_CHECK_EQ(backend.output_latency_frames(), 0U);
}

CY_TEST_CASE("the audio clock derives what is heard from what was mixed") {
    // `audio`: a rhythm game "SHALL combine the audio clock with output latency to obtain the
    // position the listener is actually hearing".
    AudioClock clock;
    clock.sample_rate = 48000;
    clock.frames_played = 96000;         // two seconds mixed
    clock.output_latency_frames = 4800;  // a tenth of a second in the device
    CY_CHECK_NEAR(static_cast<f32>(clock.mixed_seconds()), 2.0F, 1e-5F);
    CY_CHECK_NEAR(static_cast<f32>(clock.heard_seconds()), 1.9F, 1e-5F);

    // Before the first buffer has been heard the answer is zero rather than negative.
    clock.frames_played = 1000;
    CY_CHECK_NEAR(static_cast<f32>(clock.heard_seconds()), 0.0F, 1e-6F);
}
