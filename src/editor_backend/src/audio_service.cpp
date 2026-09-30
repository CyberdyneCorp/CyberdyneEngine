// SPDX-License-Identifier: MIT
// The `audio.*` operations. See cy/editor/audio_service.h.

#include <cy/editor/audio_service.h>

#include <cmath>
#include <cstring>

namespace cy::editor {
namespace {

constexpr f32 kMaxAdvanceSeconds = 0.25F;

/// A bounds-checked cursor over a request payload.
class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    bool u8v(u8& out) noexcept {
        if (!has(1)) {
            return false;
        }
        out = bytes_[at_++];
        return true;
    }

    bool u32v(u32& out) noexcept {
        if (!has(4)) {
            return false;
        }
        out = 0;
        for (usize byte = 0; byte < 4; ++byte) {
            out |= static_cast<u32>(bytes_[at_ + byte]) << (byte * 8);
        }
        at_ += 4;
        return true;
    }

    bool f32v(f32& out) noexcept {
        u32 bits = 0;
        if (!u32v(bits)) {
            return false;
        }
        std::memcpy(&out, &bits, sizeof(out));
        return std::isfinite(out);
    }

    bool vec3(Vec3& out) noexcept { return f32v(out.x) && f32v(out.y) && f32v(out.z); }

    bool text(std::string_view& out) noexcept {
        u32 size = 0;
        if (!u32v(size) || !has(size)) {
            return false;
        }
        out = {reinterpret_cast<const char*>(bytes_.data() + at_), size};
        at_ += size;
        return true;
    }

    [[nodiscard]] bool finished() const noexcept { return at_ == bytes_.size(); }

private:
    [[nodiscard]] bool has(usize count) const noexcept { return bytes_.size() - at_ >= count; }

    Span<const u8> bytes_;
    usize at_ = 0;
};

AudioRefusal refused(const char* code, const char* detail) noexcept {
    return AudioRefusal{code, detail};
}

AudioRefusal state(const AudioAuthoring& audio, Array<u8>& reply) noexcept {
    reply.clear();
    if (!audio.encode_state(reply)) {
        return refused("audio.state", "the audio state could not be encoded");
    }
    return {};
}

AudioRefusal apply_mixer(AudioAuthoring& audio, Span<const u8> payload, Array<u8>& reply) noexcept {
    const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
    if (Status applied = audio.apply_mixer(text); !applied) {
        return refused("audio.mixer", applied.error().message);
    }
    return state(audio, reply);
}

bool read_placement(Reader& reader, PreviewPlacement& placement) noexcept {
    u8 spatial = 0;
    std::string_view model;
    if (!reader.u8v(spatial) || !reader.vec3(placement.position) ||
        !reader.vec3(placement.listener) || !reader.vec3(placement.listener_forward) ||
        !reader.f32v(placement.min_distance) || !reader.f32v(placement.max_distance) ||
        !reader.text(model)) {
        return false;
    }
    placement.spatial = spatial != 0;
    placement.model = attenuation_model_named(model);
    return placement.model != audio::AttenuationModel::Count &&
           placement.model != audio::AttenuationModel::Custom && placement.min_distance > 0.0F &&
           placement.max_distance > placement.min_distance;
}

AudioRefusal preview_cue(AudioAuthoring& audio, Span<const u8> payload, Array<u8>& reply) noexcept {
    Reader reader(payload);
    std::string_view name;
    std::string_view source;
    PreviewPlacement placement;
    if (!reader.text(name) || !reader.text(source) || !read_placement(reader, placement) ||
        !reader.finished()) {
        return refused("audio.payload",
                       "a cue preview is a name, cycue text and a well-formed placement");
    }
    if (Status loaded = audio.load_cue(name, source); !loaded) {
        return refused("audio.cue", loaded.error().message);
    }
    if (Status started = audio.preview(name, placement); !started) {
        return refused("audio.cue", started.error().message);
    }
    return state(audio, reply);
}

AudioRefusal read_state(AudioAuthoring& audio, Span<const u8> payload, Array<u8>& reply) noexcept {
    if (payload.empty()) {
        return state(audio, reply);
    }
    Reader reader(payload);
    f32 seconds = 0.0F;
    if (!reader.f32v(seconds) || !reader.finished() || seconds < 0.0F ||
        seconds > kMaxAdvanceSeconds) {
        return refused("audio.payload", "an advance is one f32 from 0 to 0.25 seconds");
    }
    if (!audio.server().is_null_backend() && seconds > 0.0F) {
        return refused("audio.state", "a device is mixing; only the null backend is advanced");
    }
    audio.pump(seconds);
    return state(audio, reply);
}

}  // namespace

AudioRefusal answer_audio(AudioAuthoring* audio, std::string_view operation, Span<const u8> payload,
                          Array<u8>& reply) noexcept {
    if (audio == nullptr || !audio->initialized()) {
        return refused("audio.unavailable", "this host has no editor audio server");
    }
    if (operation == "audio.capabilities.get") {
        reply.clear();
        return audio->encode_capabilities(reply)
                   ? AudioRefusal{}
                   : refused("audio.state", "the audio vocabulary could not be encoded");
    }
    if (operation == "audio.mixer.apply") {
        return apply_mixer(*audio, payload, reply);
    }
    if (operation == "audio.cue.preview") {
        return preview_cue(*audio, payload, reply);
    }
    if (operation == "audio.preview.stop") {
        audio->stop_preview();
        return state(*audio, reply);
    }
    if (operation == "audio.state.get") {
        return read_state(*audio, payload, reply);
    }
    return refused("operation-unsupported", "this backend does not support the audio operation");
}

}  // namespace cy::editor
