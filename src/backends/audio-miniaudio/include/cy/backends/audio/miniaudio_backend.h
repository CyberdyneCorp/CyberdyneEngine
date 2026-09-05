#pragma once
// The miniaudio device backend. Task 4.3.4.
//
// `audio` — "miniaudio as the default low-level backend": the default `AudioBackend` "SHALL be
// implemented over **miniaudio**, used for: device enumeration and lifecycle, the realtime
// callback, sample-rate and channel conversion, decoding of the formats it supports natively, and
// streaming ring buffers", and "The engine SHALL NOT use miniaudio's node graph, its high-level
// engine API, or its built-in 3D spatialisation".
//
// ================================================================================================
// THIS HEADER NAMES NO MINIAUDIO TYPE, AND THAT IS THE REQUIREMENT
// ================================================================================================
//
// `audio`: "No backend type SHALL appear in any engine or game-facing header outside its own
// backend module", with the scenario "**WHEN** engine or game code is compiled **THEN** no backend
// library type SHALL appear in any header outside `backends/audio/`."
//
// So the device lives behind an opaque pointer allocated in `initialize()`. That costs one
// indirection on a path that is entered once per callback rather than once per sample, and it buys
// the property the specification is actually after: replacing miniaudio is a change to one
// directory rather than to every translation unit that ever included an audio header. It is the
// same shape `src/backends/rhi/vulkan/` uses for Vulkan and `platform/desktop-sdl3/` uses for SDL,
// and it is enforced the same way — by the layer checker, which fails the build on an upward
// include, and by this module being the only place `miniaudio.h` appears.
//
// ================================================================================================
// THE FOUR THINGS MINIAUDIO IS ASKED FOR, AND THE THREE IT IS NOT
// ================================================================================================
//
// USED: `ma_context` for device enumeration; `ma_device` for the output stream and its realtime
// callback; miniaudio's own sample-rate and channel conversion, by asking for `ma_format_f32` at
// the engine's rate and letting it negotiate; and the device's reported buffer size and latency.
//
// NOT USED, deliberately and by name: `ma_node_graph` (the engine has a bus graph), `ma_engine`
// (the engine has an `AudioServer`), and `ma_spatializer` / `ma_sound`'s 3D model (the engine has
// its own spatialisation policy, and the specification requires the engine to own it). Keeping the
// surface this small is what makes `audio`'s "the set of miniaudio APIs it calls SHALL be
// documented and limited to device I/O, conversion, decoding, and streaming" checkable rather than
// aspirational — `grep -o 'ma_[a-z_]*' src/backends/audio-miniaudio/src/*.cpp | sort -u` is the
// check.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/servers/audio/backend.h>

namespace cy::audio {

/// Opaque. Defined in the one translation unit that includes miniaudio.h.
struct MiniaudioState;

/// The default `AudioBackend`.
class MiniaudioBackend final : public AudioBackend {
public:
    explicit MiniaudioBackend(Allocator& allocator) noexcept;
    ~MiniaudioBackend() override;

    [[nodiscard]] const char* name() const noexcept override { return "miniaudio"; }

    [[nodiscard]] Status initialize(const AudioBackendConfig& config) noexcept override;
    void shutdown() noexcept override;

    /// What the device actually negotiated, which is rarely exactly what was asked for: a device
    /// that runs at 44.1 kHz reports 44.1 kHz here and the mixer resamples its clips against it.
    [[nodiscard]] AudioFormat format() const noexcept override { return format_; }

    [[nodiscard]] Status enumerate_outputs(Array<AudioDeviceInfo>& out) noexcept override;
    [[nodiscard]] Status select_output(Name device_id) noexcept override;

    [[nodiscard]] Status start(AudioRenderFn render, void* user) noexcept override;
    void stop() noexcept override;
    [[nodiscard]] bool running() const noexcept override;

    [[nodiscard]] u64 frames_played() const noexcept override;
    [[nodiscard]] u32 output_latency_frames() const noexcept override;

    [[nodiscard]] Status start_capture(AudioCaptureFn capture, void* user) noexcept override;
    void stop_capture() noexcept override;

private:
    Allocator* allocator_;
    /// Every miniaudio object this backend owns. Opaque here — see the header comment.
    MiniaudioState* state_ = nullptr;
    AudioFormat format_;
};

}  // namespace cy::audio
