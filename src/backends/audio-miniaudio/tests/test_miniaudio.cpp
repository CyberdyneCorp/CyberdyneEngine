// The miniaudio backend, against whatever audio system this host actually has. Task 4.3.4.
//
// THIS SUITE IS NOT DEVICE-GATED, AND THAT IS DELIBERATE. `audio` requires the backend to work on
// every platform's native audio API, and a suite that skipped when it found no device would be a
// suite whose green means nothing on exactly the machines where the backend is least tested —
// continuous integration runners. So every case runs, and the two outcomes are BOTH asserted:
//
//   * a host with an audio system opens a device, reports a real format, starts, stops and mixes;
//   * a host without one fails `initialize()` with `Unavailable` — and nothing else — and shuts
//     down cleanly afterwards.
//
// The second is the one that would otherwise rot. A backend that aborted, leaked, or reported
// `Internal` on a headless runner would pass a skipping suite and fail a user's machine.
//
// WHAT IS NOT ASSERTED, because it cannot be on an arbitrary host: that a particular device exists,
// that a particular sample rate was granted, or that anything was audible. The format is checked
// for internal consistency instead, which is a property of the backend rather than of the machine.

#include <cy/backends/audio/miniaudio_backend.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/audio/server.h>
#include <cy/test/test.h>

#include <chrono>
#include <cmath>
#include <thread>

using cy::f32;
using namespace cy::audio;

namespace {

cy::Allocator& allocator() {
    return cy::system_allocator(cy::MemoryDomain::Audio);
}

[[nodiscard]] AudioBackendConfig requested() {
    AudioBackendConfig config;
    config.requested.sample_rate = 48000;
    config.requested.layout = ChannelLayout::Stereo;
    config.requested.buffer_frames = 480;
    return config;
}

/// Open a device, or report why not. Returns whether this host has an audio system at all.
[[nodiscard]] bool open(MiniaudioBackend& backend) {
    const cy::Status opened = backend.initialize(requested());
    if (opened) {
        return true;
    }
    // THE ONLY ACCEPTABLE FAILURE. Any other code means the backend misdiagnosed something.
    CY_CHECK_EQ(opened.error().code, cy::ErrorCode::Unavailable);
    CY_TEST_MESSAGE(
        "no audio device on this host: the miniaudio backend reported Unavailable, "
        "which is the documented outcome. The device cases below are vacuous here; the "
        "null backend's suite covers the mixer.");
    return false;
}

}  // namespace

CY_TEST_CASE("the backend names itself and is not the null one") {
    // True with or without a device: the name is what the diagnostic that says "asked for
    // miniaudio, ran null" prints, and it must not depend on whether the machine has a sound card.
    const MiniaudioBackend backend(allocator());
    CY_CHECK_EQ(cy::Name::intern(backend.name()), cy::Name::intern("miniaudio"));
    CY_CHECK_FALSE(backend.is_null_backend());
}

CY_TEST_CASE("initialize either opens a device or reports Unavailable, and shuts down cleanly") {
    MiniaudioBackend backend(allocator());
    const bool has_device = open(backend);
    if (has_device) {
        const AudioFormat format = backend.format();
        // Internal consistency, which is a property of the backend rather than of the host: a real
        // rate, a layout the mixer can produce, and a buffer size that is not zero.
        CY_CHECK_GT(format.sample_rate, 0U);
        CY_CHECK_GE(format.channels(), 1U);
        CY_CHECK_LE(format.channels(), 2U);
        CY_CHECK_GT(format.buffer_frames, 0U);
        CY_CHECK_GT(format.buffer_seconds(), 0.0F);
    }
    // Shutting down is safe in both states, and twice is safe as well — a destructor runs it after
    // whatever the caller did.
    backend.shutdown();
    backend.shutdown();
    CY_CHECK_FALSE(backend.running());
}

CY_TEST_CASE("initializing twice is refused rather than leaking the first device") {
    MiniaudioBackend backend(allocator());
    if (!open(backend)) {
        return;
    }
    const cy::Status again = backend.initialize(requested());
    CY_REQUIRE_FALSE(static_cast<bool>(again));
    CY_CHECK_EQ(again.error().code, cy::ErrorCode::AlreadyExists);
}

CY_TEST_CASE("the enumeration lists devices by a name a settings file can hold") {
    // `ma_device_id` is a union of platform-specific handles — a GUID, a string, an integer — so
    // the identifier this backend hands out is the device's reported NAME, which is what a settings
    // menu shows and what survives a move between machines.
    MiniaudioBackend backend(allocator());
    if (!open(backend)) {
        return;
    }
    cy::Array<AudioDeviceInfo> devices(allocator());
    CY_REQUIRE(static_cast<bool>(backend.enumerate_outputs(devices)));

    for (const AudioDeviceInfo& device : devices) {
        CY_CHECK_FALSE(device.id.is_empty());
        // Every enumerated device can be selected by the identifier the enumeration gave.
        CY_CHECK(static_cast<bool>(backend.select_output(device.id)));
    }
    // And one that was never enumerated cannot.
    CY_CHECK_FALSE(
        static_cast<bool>(backend.select_output(cy::Name::intern("a device that does not exist"))));
    // An empty identifier means the default, which is always selectable.
    CY_CHECK(static_cast<bool>(backend.select_output(cy::Name{})));
}

CY_TEST_CASE("a started device runs and stops, and starting needs a callback") {
    MiniaudioBackend backend(allocator());
    if (!open(backend)) {
        return;
    }
    CY_CHECK_FALSE(static_cast<bool>(backend.start(nullptr, nullptr)));

    AudioServer server(allocator());
    AudioServerConfig config;
    config.requested = backend.format();
    config.block_frames = 120;
    CY_REQUIRE(static_cast<bool>(server.configure(config)));
    // `initialize_with` starts the device, which is what puts the mixer on the realtime thread.
    CY_REQUIRE(static_cast<bool>(server.initialize_with(backend)));
    CY_CHECK(backend.running());
    CY_CHECK_EQ(cy::Name::intern(server.backend_name()), cy::Name::intern("miniaudio"));
    CY_CHECK_FALSE(server.is_null_backend());

    server.shutdown();
    CY_CHECK_FALSE(backend.running());
}

CY_TEST_CASE("the mixer runs on the device's own thread and the clock advances") {
    // The one case that exercises the realtime path for real: the callback is miniaudio's thread,
    // and `AudioServer::render()` runs on it. What is asserted is that the clock moved — that the
    // device asked for frames and the mixer produced them — not that anything was audible.
    MiniaudioBackend backend(allocator());
    if (!open(backend)) {
        return;
    }
    AudioServer server(allocator());
    AudioServerConfig config;
    config.requested = backend.format();
    config.block_frames = 120;
    CY_REQUIRE(static_cast<bool>(server.configure(config)));
    CY_REQUIRE(static_cast<bool>(server.initialize_with(backend)));

    cy::Array<f32> samples(allocator());
    CY_REQUIRE(static_cast<bool>(samples.resize(4800)));
    for (cy::usize i = 0; i < samples.size(); ++i) {
        // A quiet sine, so a developer running this on a machine with speakers hears something
        // deliberate rather than a click.
        samples[i] = 0.05F * std::sin(static_cast<f32>(i) * 0.05F);
    }
    ClipDescription clip;
    clip.name = cy::Name::intern("probe");
    clip.samples = samples.data();
    clip.frame_count = static_cast<cy::u32>(samples.size());
    clip.channels = 1;
    clip.sample_rate = backend.format().sample_rate;
    const auto handle = server.create_clip(clip);
    CY_REQUIRE(static_cast<bool>(handle));

    VoiceDescription description;
    description.clip = *handle;
    description.looping = true;
    CY_REQUIRE(static_cast<bool>(server.play(description)));
    server.update(1.0F / 60.0F);

    // Wait for the device to ask for at least one buffer. A poll rather than a fixed sleep: the
    // period is the device's and a fixed wait would be either flaky or slow.
    const cy::u32 target = backend.format().buffer_frames;
    for (int attempt = 0; attempt < 200 && server.clock().frames_played < target; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CY_CHECK_GE(server.clock().frames_played, target);
    CY_CHECK_EQ(server.statistics().underruns, 0U);
    CY_CHECK_EQ(server.statistics().dropped_commands, 0U);

    server.shutdown();
}
