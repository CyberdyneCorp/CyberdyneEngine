#pragma once
// The audio format vocabulary: rates, channel layouts, devices and the audio clock. Task 4.3.4.
//
// `audio` — "Audio driver layer": the backend "SHALL expose: mix rate, channel layout, buffer size,
// device enumeration and selection, and input capture", and "Timing and synchronisation" requires
// the engine to expose "samples played, the current output time, time to the next callback, and
// output latency — so gameplay can schedule events precisely against audio".
//
// WHY THE FORMAT IS AN ENGINE TYPE AND NOT THE BACKEND'S. `audio`: "No backend type SHALL appear in
// any engine or game-facing header outside its own backend module". A `ma_format` in this header
// would put miniaudio's vocabulary into every consumer of the audio server, and replacing miniaudio
// would then be a change to every one of them rather than to one directory.
//
// WHY THERE IS ONE SAMPLE TYPE AND IT IS `f32`. The engine mixes in 32-bit float and nothing else.
// A backend converts on the way out — that is one of the four things `audio` allows miniaudio to do
// — so the mixer, the effects and the voice gains have one representation to reason about rather
// than a conversion at every stage boundary.

#include <cy/core/base/types.h>
#include <cy/core/values/name.h>

namespace cy::audio {

/// The channel layouts the mixer knows how to pan into.
///
/// `audio` requires "panning appropriate to the output channel layout". The two the engine
/// spatialises for at Seed are mono and stereo; surround and ambisonic layouts arrive with the
/// acoustics backend at M8, and are absent here rather than declared and unhandled — a layout the
/// panner does not implement would silently pan into the first two channels and leave the rest
/// empty, which sounds like a broken speaker rather than like a missing feature.
enum class ChannelLayout : u8 {
    Mono = 0,
    Stereo,
    Count,
};

[[nodiscard]] const char* channel_layout_name(ChannelLayout layout) noexcept;
[[nodiscard]] u32 channel_count(ChannelLayout layout) noexcept;

/// What a device is running at.
struct AudioFormat {
    u32 sample_rate = 48000;
    ChannelLayout layout = ChannelLayout::Stereo;
    /// Frames per callback. The mixer's block size; see `AudioServerConfig::block_frames`.
    u32 buffer_frames = 480;

    [[nodiscard]] u32 channels() const noexcept { return channel_count(layout); }
    /// Seconds of audio one buffer holds. The floor of the latency the backend can offer.
    [[nodiscard]] f32 buffer_seconds() const noexcept {
        return (sample_rate == 0) ? 0.0F
                                  : static_cast<f32>(buffer_frames) / static_cast<f32>(sample_rate);
    }
};

/// One output or capture device, as the backend enumerated it.
struct AudioDeviceInfo {
    /// The backend's stable identifier for the device, which is what `select_output()` takes. A
    /// `Name` rather than a string: it is compared far more often than it is printed.
    Name id;
    Name display_name;
    bool is_default = false;
    bool is_capture = false;
    AudioFormat preferred;
};

/// The audio clock. `audio` — "Timing and synchronisation".
///
/// `frames_played` is the authority and everything else is derived from it, because the two numbers
/// a rhythm game needs — where the mix has reached and where the LISTENER has reached — differ by
/// exactly the output latency, and deriving one from the other is what keeps them a fixed distance
/// apart instead of two counters that drift.
struct AudioClock {
    /// Frames the mixer has produced since the device started.
    u64 frames_played = 0;
    u32 sample_rate = 48000;
    /// Frames between the mixer producing a sample and the listener hearing it.
    u32 output_latency_frames = 0;

    [[nodiscard]] f64 mixed_seconds() const noexcept {
        return (sample_rate == 0) ? 0.0
                                  : static_cast<f64>(frames_played) / static_cast<f64>(sample_rate);
    }
    /// Where the listener actually is, which is what a scheduled event must be timed against.
    [[nodiscard]] f64 heard_seconds() const noexcept {
        if (sample_rate == 0 || frames_played < output_latency_frames) {
            return 0.0;
        }
        return static_cast<f64>(frames_played - output_latency_frames) /
               static_cast<f64>(sample_rate);
    }
};

}  // namespace cy::audio
