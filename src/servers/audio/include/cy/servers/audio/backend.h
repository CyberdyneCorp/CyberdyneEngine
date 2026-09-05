#pragma once
// `AudioBackend`: the driver layer, and the null implementation that is kept forever. Task 4.3.4.
//
// `audio` — "Engine-owned audio architecture": `AudioServer`, the bus graph, voice management,
// streaming policy, spatialisation policy and the importance system "SHALL be engine code", and
// third-party libraries sit behind two engine-owned interfaces, of which this is the first:
//
//   **`AudioBackend`** — device lifecycle and the realtime callback, sample-rate and channel
//   conversion, decoding, and streaming primitives.
//
// `AcousticsBackend` — the second — is M8's, and is absent here rather than declared empty.
//
// ================================================================================================
// INTERFACE FIRST, AND THE NULL IMPLEMENTATION IS NOT SCAFFOLDING
// ================================================================================================
//
// design.md §4: "`PhysicsServer` and the audio driver layer are defined and exercised by a trivial
// implementation **before** Jolt and miniaudio are linked… The retained trivial implementation is
// not ceremony: it is what proves at every build that the interface does not leak the library."
//
// `NullAudioBackend` is therefore permanent, not temporary. Three things depend on it:
//
//   * every audio test in continuous integration, on machines with no sound card;
//   * `audio`'s own requirement — "**WHEN** the engine runs headless or in tests **THEN** the null
//     backend SHALL satisfy the interface, advancing playback positions deterministically without a
//     device";
//   * the proof that the interface is an interface. A method miniaudio needed that the null backend
//     could not implement would be a method that had leaked the library.
//
// It is DRIVEN, not free-running: `advance()` pulls exactly the frames it is asked for, so a test
// mixes a known number of frames and asserts on them. A null backend with a thread would make every
// audio test a race.
//
// ================================================================================================
// THE REALTIME CONTRACT, WHICH IS A PROPERTY OF THE CALLBACK AND NOT OF THE BACKEND
// ================================================================================================
//
// `audio`: "**WHEN** the driver callback runs **THEN** the mixer SHALL NOT allocate, take a
// blocking lock, perform file I/O, or call into script." The callback below is a plain function
// pointer taking a POD buffer for exactly that reason — there is nothing in its signature that
// could allocate, and `AudioServer::render()` (the only implementation of it in the engine) takes
// its input through a lock-free queue and pre-allocated buffers.
//
// A BACKEND MAY CALL IT FROM ITS OWN THREAD. Everything reachable from it must be safe to touch
// there; that is `commands.h`'s whole subject.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/servers/audio/format.h>

namespace cy::audio {

/// The realtime render callback. Writes `frame_count * channels` interleaved floats.
///
/// `noexcept` and a raw pointer, deliberately: this is called from a realtime thread and there is
/// nothing in the signature that can allocate, throw or block.
using AudioRenderFn = void (*)(f32* frames, u32 frame_count, u32 channels, void* user) noexcept;

/// The capture callback, for input. Same constraints.
using AudioCaptureFn = void (*)(const f32* frames, u32 frame_count, u32 channels,
                                void* user) noexcept;

/// Called when the default device changed underneath a running stream. Called from the backend's
/// own thread, so it may only mark: the reinitialisation is the server's, on the game thread.
using AudioDeviceChangedFn = void (*)(void* user) noexcept;

/// What a backend is asked for when it is initialised. A REQUEST, not a promise: a device runs at
/// its own rate and the backend reports what it got through `format()`.
struct AudioBackendConfig {
    AudioFormat requested;
    /// Empty means "the default output device".
    Name device_id;
    /// Ask the backend to open a capture stream as well.
    bool capture = false;
};

/// The driver layer.
///
/// The engine owns everything above this line — the graph, the voices, the policy — and this
/// interface is deliberately the smallest surface that lets somebody else own the device. `audio`
/// requires the depended-upon surface to be "documented and limited to device I/O, conversion,
/// decoding, and streaming", and a short interface is how that is kept true rather than intended.
class AudioBackend {
public:
    AudioBackend() = default;
    virtual ~AudioBackend() = default;

    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;
    AudioBackend(AudioBackend&&) = delete;
    AudioBackend& operator=(AudioBackend&&) = delete;

    /// "miniaudio", "null". Reported by the server and named in the diagnostic that says which
    /// backend actually ran.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    [[nodiscard]] virtual Status initialize(const AudioBackendConfig& config) noexcept = 0;
    virtual void shutdown() noexcept = 0;

    /// The format the device is actually running at, which is what the mixer must produce.
    [[nodiscard]] virtual AudioFormat format() const noexcept = 0;

    /// Every output device, appended to `out`. `out` is cleared first.
    [[nodiscard]] virtual Status enumerate_outputs(Array<AudioDeviceInfo>& out) noexcept = 0;
    /// Switch device. `audio` requires playback to continue across it: the backend reinitialises at
    /// the new device's rate and layout, and the server re-derives its buffers from `format()`.
    [[nodiscard]] virtual Status select_output(Name device_id) noexcept = 0;

    [[nodiscard]] virtual Status start(AudioRenderFn render, void* user) noexcept = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;

    /// Frames the device has consumed. The audio clock's authority.
    [[nodiscard]] virtual u64 frames_played() const noexcept = 0;
    /// Frames between the mixer producing a sample and it being heard.
    [[nodiscard]] virtual u32 output_latency_frames() const noexcept = 0;

    /// Input capture. Optional: a backend that cannot capture says so rather than pretending.
    [[nodiscard]] virtual Status start_capture(AudioCaptureFn capture, void* user) noexcept;
    virtual void stop_capture() noexcept {}

    /// Ask to be told when the default device changes. A backend that cannot detect it does
    /// nothing, and the server keeps running on the device it has.
    virtual void set_device_changed_callback(AudioDeviceChangedFn callback, void* user) noexcept;

    /// True for the implementation that keeps bookkeeping valid and produces no sound.
    [[nodiscard]] virtual bool is_null_backend() const noexcept { return false; }
};

/// The retained trivial implementation. See the header comment for why it is permanent.
///
/// Deterministic and driven: `advance(frames)` calls the render callback in whole blocks and
/// advances the clock by exactly what it asked for, so a test can say "mix one second" and assert
/// on the result. No thread, no device, no timer.
class NullAudioBackend final : public AudioBackend {
public:
    [[nodiscard]] const char* name() const noexcept override { return "null"; }

    [[nodiscard]] Status initialize(const AudioBackendConfig& config) noexcept override;
    void shutdown() noexcept override;

    [[nodiscard]] AudioFormat format() const noexcept override { return format_; }
    [[nodiscard]] Status enumerate_outputs(Array<AudioDeviceInfo>& out) noexcept override;
    [[nodiscard]] Status select_output(Name device_id) noexcept override;

    [[nodiscard]] Status start(AudioRenderFn render, void* user) noexcept override;
    void stop() noexcept override;
    [[nodiscard]] bool running() const noexcept override { return running_; }

    [[nodiscard]] u64 frames_played() const noexcept override { return frames_played_; }
    /// Zero. There is no device, so there is nothing between the mix and the "listener", and
    /// reporting a plausible latency would make a scheduling test pass against a number nobody
    /// measured.
    [[nodiscard]] u32 output_latency_frames() const noexcept override { return 0; }

    [[nodiscard]] bool is_null_backend() const noexcept override { return true; }

    /// Pull `frames` through the render callback, in blocks of `format().buffer_frames`, writing
    /// the mixed result into `output` — which must hold `frames * format().channels()` floats.
    ///
    /// Returns the frames actually produced, which is `frames` rounded DOWN to a whole number of
    /// blocks: a partial block is not what a device would ask for, and rounding up would write past
    /// what the caller sized. The output is kept rather than discarded because that is what makes a
    /// test able to assert on the mix rather than only on the bookkeeping.
    [[nodiscard]] u32 advance(f32* output, u32 frames) noexcept;

private:
    AudioFormat format_;
    AudioRenderFn render_ = nullptr;
    void* user_ = nullptr;
    bool running_ = false;
    bool initialized_ = false;
    u64 frames_played_ = 0;
};

}  // namespace cy::audio
