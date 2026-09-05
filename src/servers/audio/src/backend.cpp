// The default implementations of the optional backend methods, and the null backend.
// See cy/servers/audio/backend.h.

#include <cy/servers/audio/backend.h>

#include <cy/core/base/assert.h>

#include <cstring>

namespace cy::audio {

Status AudioBackend::start_capture(AudioCaptureFn capture, void* user) noexcept {
    (void)capture;
    (void)user;
    // NOT a silent no-op. A backend that cannot capture says so, so a caller that needs input gets
    // an error at start-up rather than a stream of silence it has to diagnose.
    return fail(ErrorCode::Unsupported, "this audio backend does not capture input");
}

void AudioBackend::set_device_changed_callback(AudioDeviceChangedFn callback, void* user) noexcept {
    (void)callback;
    (void)user;
    // A backend that cannot detect a device change does nothing and the server keeps running on the
    // device it has, which is the correct degraded behaviour rather than a failure.
}

// --- The null backend
// -----------------------------------------------------------------------------

Status NullAudioBackend::initialize(const AudioBackendConfig& config) noexcept {
    if (config.requested.sample_rate == 0 || config.requested.buffer_frames == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "an audio format needs a non-zero sample rate and buffer size");
    }
    if (channel_count(config.requested.layout) == 0) {
        return fail(ErrorCode::InvalidArgument, "an audio format needs a known channel layout");
    }
    // THE REQUESTED FORMAT IS GRANTED EXACTLY. A real device negotiates; this one does not, because
    // a test that asked for 48 kHz stereo and silently got something else would assert against the
    // wrong numbers. It is the one backend whose format is a promise.
    format_ = config.requested;
    frames_played_ = 0;
    initialized_ = true;
    return ok();
}

void NullAudioBackend::shutdown() noexcept {
    stop();
    initialized_ = false;
    frames_played_ = 0;
}

Status NullAudioBackend::enumerate_outputs(Array<AudioDeviceInfo>& out) noexcept {
    out.clear();
    AudioDeviceInfo info;
    info.id = Name::intern("null");
    info.display_name = Name::intern("Null output");
    info.is_default = true;
    info.preferred = format_;
    return out.push_back(info);
}

Status NullAudioBackend::select_output(Name device_id) noexcept {
    if (!device_id.is_empty() && device_id != Name::intern("null")) {
        return fail(ErrorCode::NotFound, "the null audio backend has one device, named \"null\"");
    }
    return ok();
}

Status NullAudioBackend::start(AudioRenderFn render, void* user) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::Unavailable, "the null audio backend is not initialized");
    }
    if (render == nullptr) {
        return fail(ErrorCode::InvalidArgument, "an audio backend needs a render callback");
    }
    render_ = render;
    user_ = user;
    running_ = true;
    return ok();
}

void NullAudioBackend::stop() noexcept {
    running_ = false;
    render_ = nullptr;
    user_ = nullptr;
}

u32 NullAudioBackend::advance(f32* output, u32 frames) noexcept {
    if (!running_ || render_ == nullptr || output == nullptr) {
        return 0;
    }
    const u32 block = format_.buffer_frames;
    const u32 channels = format_.channels();
    if (block == 0 || channels == 0) {
        return 0;
    }

    u32 produced = 0;
    while (produced + block <= frames) {
        f32* destination = output + (static_cast<usize>(produced) * channels);
        // Cleared before each block: a callback that writes nothing must produce silence, not
        // whatever the caller's buffer happened to hold. A device driver hands the mixer a buffer
        // it has already zeroed, so a mixer that relied on the difference would work here and click
        // on a device.
        std::memset(static_cast<void*>(destination), 0,
                    static_cast<usize>(block) * channels * sizeof(f32));
        render_(destination, block, channels, user_);
        produced += block;
        frames_played_ += block;
    }
    return produced;
}

}  // namespace cy::audio
