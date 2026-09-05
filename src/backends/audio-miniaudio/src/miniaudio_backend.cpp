// The miniaudio device backend. See cy/backends/audio/miniaudio_backend.h.
//
// THE ONLY TRANSLATION UNIT IN THE ENGINE THAT INCLUDES miniaudio.h. `audio`: "No backend type
// SHALL appear in any engine or game-facing header outside its own backend module." The
// implementation bodies are compiled into the `miniaudio` dependency target from its own
// `miniaudio.c`, so this file takes the declarations only — including it without
// `MINIAUDIO_IMPLEMENTATION` is the intended usage and is what keeps one copy of the library in the
// link.

#include <cy/backends/audio/miniaudio_backend.h>

#include <cy/core/base/assert.h>
#include <cy/core/diagnostics/log.h>

#include <miniaudio.h>

#include <cstring>
#include <new>

namespace cy::audio {
namespace {

/// The largest device list this backend reports. A machine with more output devices than this has
/// virtual devices; the list is for a settings menu, and a menu with sixty-four entries is already
/// unusable.
constexpr u32 kMaxDevices = 64;

[[nodiscard]] ChannelLayout layout_for(u32 channels) noexcept {
    return (channels <= 1) ? ChannelLayout::Mono : ChannelLayout::Stereo;
}

}  // namespace

/// Everything miniaudio owns, in one allocation. Opaque to every other translation unit.
///
/// `context` and `device` ARE DELIBERATELY NOT INITIALISED. `ma_device` contains an enum with no
/// zero enumerator, so `{}` would give that member a value the enum does not have — which
/// clang-tidy reports as `bugprone-invalid-enum-default-initialization` and which would be a real
/// defect if anything read it. Nothing does: `ma_context_init` and `ma_device_init` fill them
/// completely, and the two `_ready` flags are what guards every use. The suppression is on the
/// struct because the diagnostic is about its implicit constructor.
// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
struct MiniaudioState {
    ma_context context;
    ma_device device;
    bool context_ready = false;
    bool device_ready = false;
    bool started = false;

    AudioRenderFn render = nullptr;
    void* render_user = nullptr;
    AudioCaptureFn capture = nullptr;
    void* capture_user = nullptr;

    /// The device the next `initialize()` should open, copied out of an enumeration. Held as a
    /// miniaudio id rather than as a name because that is what `ma_device_init` takes, and
    /// resolving a name to an id needs the enumeration that produced it.
    ma_device_id selected_id{};
    bool has_selected = false;

    /// The last enumeration, so `select_output()` can resolve a name to an id without
    /// re-enumerating — and so the ids it hands out cannot come from a list the caller never saw.
    ma_device_info devices[kMaxDevices]{};
    u32 device_count = 0;
};

namespace {

/// miniaudio's realtime callback. Forwards to the engine's render function and nothing else.
///
/// EVERYTHING THE REALTIME CONTRACT FORBIDS IS ABSENT FROM THIS FUNCTION: no allocation, no lock,
/// no file, no script. It is four lines for exactly that reason — the moment a backend callback
/// grows logic, the logic is running on a thread with a hard deadline.
void device_data_callback(ma_device* device, void* output, const void* input,
                          ma_uint32 frame_count) noexcept {
    (void)input;
    auto* state = static_cast<MiniaudioState*>(device->pUserData);
    if (state == nullptr || state->render == nullptr || output == nullptr) {
        return;
    }
    state->render(static_cast<f32*>(output), frame_count, device->playback.channels,
                  state->render_user);
}

}  // namespace

MiniaudioBackend::MiniaudioBackend(Allocator& allocator) noexcept : allocator_(&allocator) {}

MiniaudioBackend::~MiniaudioBackend() {
    shutdown();
}

Status MiniaudioBackend::initialize(const AudioBackendConfig& config) noexcept {
    if (state_ != nullptr) {
        return fail(ErrorCode::AlreadyExists, "the miniaudio backend is already initialized");
    }
    void* memory = allocator_->allocate(sizeof(MiniaudioState), alignof(MiniaudioState));
    if (memory == nullptr) {
        return fail(ErrorCode::OutOfMemory, "could not allocate the miniaudio backend's state");
    }
    state_ = ::new (memory) MiniaudioState();

    if (ma_context_init(nullptr, 0, nullptr, &state_->context) != MA_SUCCESS) {
        shutdown();
        return fail(ErrorCode::Unavailable,
                    "miniaudio could not open an audio context: there is no usable audio system on "
                    "this host");
    }
    state_->context_ready = true;

    // Resolve the requested device NAME to an id, if one was asked for. See `select_output()` for
    // why the identifier a caller stores is the device's reported name.
    if (!config.device_id.is_empty()) {
        if (Status selected = select_output(config.device_id); !selected) {
            shutdown();
            return selected;
        }
    }

    ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
    // f32 IS THE ENGINE'S ONE SAMPLE TYPE. Asking for it here is what makes miniaudio do the format
    // conversion the specification allows it to do, rather than the mixer doing it.
    device_config.playback.format = ma_format_f32;
    device_config.playback.channels = channel_count(config.requested.layout);
    device_config.playback.pDeviceID = state_->has_selected ? &state_->selected_id : nullptr;
    device_config.sampleRate = config.requested.sample_rate;
    device_config.periodSizeInFrames = config.requested.buffer_frames;
    device_config.dataCallback = &device_data_callback;
    device_config.pUserData = state_;
    // The output buffer arrives silenced, which is what the mixer's "write into a cleared buffer"
    // contract expects; leaving it undefined would make a voice that mixes nothing play back the
    // previous block.
    device_config.noPreSilencedOutputBuffer = MA_FALSE;

    if (ma_device_init(&state_->context, &device_config, &state_->device) != MA_SUCCESS) {
        shutdown();
        return fail(ErrorCode::Unavailable, "miniaudio could not open the output device");
    }
    state_->device_ready = true;

    // WHAT THE DEVICE NEGOTIATED, NOT WHAT WAS ASKED FOR. A device that runs at 44.1 kHz reports it
    // here and the mixer resamples its clips against it; reporting the request would make every
    // clip play at the wrong speed and nothing would say why.
    format_.sample_rate = state_->device.sampleRate;
    format_.layout = layout_for(state_->device.playback.channels);
    format_.buffer_frames = (state_->device.playback.internalPeriodSizeInFrames != 0)
                                ? state_->device.playback.internalPeriodSizeInFrames
                                : config.requested.buffer_frames;
    return ok();
}

void MiniaudioBackend::shutdown() noexcept {
    if (state_ == nullptr) {
        return;
    }
    if (state_->device_ready) {
        ma_device_uninit(&state_->device);
        state_->device_ready = false;
    }
    if (state_->context_ready) {
        (void)ma_context_uninit(&state_->context);
        state_->context_ready = false;
    }
    state_->~MiniaudioState();
    allocator_->deallocate(state_, sizeof(MiniaudioState), alignof(MiniaudioState));
    state_ = nullptr;
}

Status MiniaudioBackend::enumerate_outputs(Array<AudioDeviceInfo>& out) noexcept {
    out.clear();
    if (state_ == nullptr || !state_->context_ready) {
        return fail(ErrorCode::Unavailable, "the miniaudio backend is not initialized");
    }

    ma_device_info* playback = nullptr;
    ma_uint32 playback_count = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 capture_count = 0;
    if (ma_context_get_devices(&state_->context, &playback, &playback_count, &capture,
                               &capture_count) != MA_SUCCESS) {
        return fail(ErrorCode::Unavailable, "miniaudio could not enumerate the output devices");
    }

    state_->device_count = (playback_count < kMaxDevices) ? playback_count : kMaxDevices;
    for (u32 i = 0; i < state_->device_count; ++i) {
        // The id is COPIED into this backend's own table. miniaudio's returned array belongs to the
        // context and is invalidated by the next enumeration, so a caller holding one across a
        // device change would pass a dangling id to `ma_device_init`.
        state_->devices[i] = playback[i];

        AudioDeviceInfo info;
        info.id = Name::intern(playback[i].name);
        info.display_name = info.id;
        info.is_default = playback[i].isDefault != 0;
        info.preferred = format_;
        if (Status pushed = out.push_back(info); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status MiniaudioBackend::select_output(Name device_id) noexcept {
    if (state_ == nullptr || !state_->context_ready) {
        return fail(ErrorCode::Unavailable, "the miniaudio backend is not initialized");
    }
    if (device_id.is_empty()) {
        state_->has_selected = false;
        return ok();
    }
    if (state_->device_count == 0) {
        // Enumerate on demand: a caller restoring a device from a settings file has a name and no
        // reason to have listed the devices first.
        Array<AudioDeviceInfo> scratch(*allocator_);
        if (Status enumerated = enumerate_outputs(scratch); !enumerated) {
            return enumerated;
        }
    }
    for (u32 i = 0; i < state_->device_count; ++i) {
        // THE IDENTIFIER A CALLER STORES IS THE DEVICE'S REPORTED NAME. `ma_device_id` is a union
        // of platform-specific handles — a GUID on Windows, a string on ALSA, an integer on
        // CoreAudio — so it is neither printable nor portable, and a settings file that held one
        // would not survive a move between machines. The name is what a settings menu shows anyway.
        if (Name::intern(state_->devices[i].name) == device_id) {
            state_->selected_id = state_->devices[i].id;
            state_->has_selected = true;
            return ok();
        }
    }
    return fail(ErrorCode::NotFound, "no output device by that name");
}

Status MiniaudioBackend::start(AudioRenderFn render, void* user) noexcept {
    if (state_ == nullptr || !state_->device_ready) {
        return fail(ErrorCode::Unavailable, "the miniaudio backend has no device");
    }
    if (render == nullptr) {
        return fail(ErrorCode::InvalidArgument, "an audio backend needs a render callback");
    }
    // Published BEFORE the device is started, so the callback cannot run against a null pointer.
    state_->render = render;
    state_->render_user = user;
    if (ma_device_start(&state_->device) != MA_SUCCESS) {
        state_->render = nullptr;
        return fail(ErrorCode::Unavailable, "miniaudio could not start the output device");
    }
    state_->started = true;
    return ok();
}

void MiniaudioBackend::stop() noexcept {
    if (state_ == nullptr || !state_->device_ready) {
        return;
    }
    if (state_->started) {
        (void)ma_device_stop(&state_->device);
        state_->started = false;
    }
    // Cleared AFTER the device has stopped: `ma_device_stop` waits for the callback thread, so
    // clearing first would leave a live callback reading a null render function.
    state_->render = nullptr;
    state_->render_user = nullptr;
}

bool MiniaudioBackend::running() const noexcept {
    return state_ != nullptr && state_->device_ready &&
           ma_device_is_started(&state_->device) != MA_FALSE;
}

u64 MiniaudioBackend::frames_played() const noexcept {
    // miniaudio does not expose a played-frame counter for a playback device, so the engine's own
    // count — maintained by `AudioServer::render()`, which is called once per callback — is the
    // authority. Reporting zero here rather than a plausible number is deliberate: a clock that
    // looked right and was not would put a rhythm game's notes in the wrong place with nothing to
    // point at.
    return 0;
}

u32 MiniaudioBackend::output_latency_frames() const noexcept {
    if (state_ == nullptr || !state_->device_ready) {
        return 0;
    }
    // The device buffers `internalPeriods` periods, so a sample the mixer produces is heard after
    // that much audio has been consumed. This is what `AudioClock::heard_seconds()` subtracts.
    return state_->device.playback.internalPeriodSizeInFrames *
           state_->device.playback.internalPeriods;
}

Status MiniaudioBackend::start_capture(AudioCaptureFn capture, void* user) noexcept {
    (void)capture;
    (void)user;
    // Not implemented rather than silently absent. Capture needs a second device (or a duplex one)
    // and a second ring buffer, and nothing at M4 consumes input — the first consumer is voice chat
    // at M9. A caller gets an error naming the milestone rather than a stream of silence.
    return fail(ErrorCode::NotImplemented,
                "the miniaudio backend does not capture input yet; the first consumer is M9");
}

void MiniaudioBackend::stop_capture() noexcept {}

}  // namespace cy::audio
