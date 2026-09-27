// SPDX-License-Identifier: MIT
// The `audio` adapter: `cy::abi::game::AudioBackend` over `cy::audio::AudioServer`.
// `add-swift-game-api`.
//
// OWNER: implementer C. See openspec/changes/add-swift-game-api/design.md and the adapter's header.
//
// The thunks have already checked the phase, the pointers and that every number is finite and not
// negative, and have dropped the writes of a resimulated tick. What is left here is the domain:
// which names exist, which handles are still alive, and turning the ABI's "zero means as authored"
// into the server's own defaults.
//
// VOICE EXHAUSTION IS NOT AN ERROR (cy_abi.h, `audio_play`). When the server has no free voice slot
// the request is answered OK with a null voice, which `audio_voice_playing` reports as not playing
// — the same answer a voice that lost the mixer's priority contest gets.

#include <cy/game_backend/audio_backend.h>

#include <cy/abi/errors.h>
#include <cy/abi/game/audio.h>
#include <cy/core/math/transform.h>
#include <cy/scene/node.h>
#include <cy/scene/tree.h>

namespace cy::game_backend {
namespace {

[[nodiscard]] audio::ClipHandle clip_of(CyAudioCue cue) noexcept {
    return audio::ClipHandle::from_bits(cue);
}
[[nodiscard]] audio::BusHandle bus_of(CyAudioBus bus) noexcept {
    return audio::BusHandle::from_bits(bus);
}
[[nodiscard]] audio::VoiceHandle voice_of(CyAudioVoice voice) noexcept {
    return audio::VoiceHandle::from_bits(voice);
}

/// The ABI's "zero means as authored" for a gain or a rate.
[[nodiscard]] f32 or_one(f32 value) noexcept {
    return value == 0.0F ? 1.0F : value;
}

}  // namespace

f32 AudioAdapter::Ramp::advance(f32 delta) noexcept {
    elapsed += delta;
    const f32 t = (seconds <= 0.0F || elapsed >= seconds) ? 1.0F : elapsed / seconds;
    return from + ((to - from) * t);
}

AudioAdapter::AudioAdapter(audio::AudioServer& server, Allocator& allocator,
                           scene::SceneTree* tree) noexcept
    : server_(server), tree_(tree), cues_(allocator), voices_(allocator), bus_fades_(allocator) {}

Status AudioAdapter::add_cue(Name name, audio::ClipHandle clip) noexcept {
    if (name.is_empty() || server_.clip(clip) == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a cue needs a name and a clip the server holds");
    }
    for (Cue& cue : cues_) {
        if (cue.name == name) {
            cue.clip = clip;
            return ok();
        }
    }
    return cues_.push_back(Cue{name, clip});
}

// --- Names ---------------------------------------------------------------------------------------

CyResult AudioAdapter::find_cue(const char* name, CyAudioCue& out_cue) noexcept {
    const Name wanted = Name::find(name);
    for (const Cue& cue : cues_) {
        if (!wanted.is_empty() && cue.name == wanted && server_.clip(cue.clip) != nullptr) {
            out_cue = cue.clip.bits();
            return CY_RESULT_OK;
        }
    }
    return abi::report(CY_RESULT_NOT_FOUND, "audio_find_cue: no cue of that name");
}

CyResult AudioAdapter::find_bus(const char* name, CyAudioBus& out_bus) noexcept {
    const Name wanted = Name::find(name);
    const audio::BusGraph& buses = server_.buses();
    for (u32 index = 0; !wanted.is_empty() && index < buses.size(); ++index) {
        const audio::BusHandle handle = buses.handle_at(index);
        const audio::BusDescription* description = buses.description(handle);
        if (description != nullptr && description->name == wanted) {
            out_bus = handle.bits();
            return CY_RESULT_OK;
        }
    }
    return abi::report(CY_RESULT_NOT_FOUND, "audio_find_bus: no bus of that name");
}

// --- Playing -------------------------------------------------------------------------------------

CyResult AudioAdapter::attachment_position(ecs::Entity entity, Vec3 offset,
                                           Vec3& out_position) const noexcept {
    if (tree_ == nullptr) {
        return abi::report(CY_RESULT_UNAVAILABLE,
                           "audio_play: this audio backend has no scene to attach a voice to");
    }
    const scene::Node node = tree_->node(entity);
    if (!node.valid()) {
        return abi::report(CY_RESULT_NOT_FOUND, "audio_play: the entity to attach to is not alive");
    }
    out_position = node.world_transform().transform_point(offset);
    return CY_RESULT_OK;
}

CyResult AudioAdapter::describe(const CyAudioPlay& play,
                                audio::VoiceDescription& out) const noexcept {
    out.clip = clip_of(play.cue);
    if (server_.clip(out.clip) == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "audio_play: the cue is null or stale");
    }
    if (play.bus != 0) {
        out.bus = bus_of(play.bus);
        if (!server_.buses().alive(out.bus)) {
            return abi::report(CY_RESULT_NOT_FOUND, "audio_play: the bus is stale");
        }
    }
    out.volume = or_one(play.volume);
    out.pitch = or_one(play.pitch);
    out.looping = (play.flags & CY_AUDIO_PLAY_LOOP) != 0U;
    const Vec3 position{play.position[0], play.position[1], play.position[2]};
    out.position = position;
    out.spatialised = (play.flags & (CY_AUDIO_PLAY_SPATIAL | CY_AUDIO_PLAY_ATTACH)) != 0U;
    if ((play.flags & CY_AUDIO_PLAY_ATTACH) != 0U) {
        return attachment_position(abi::from_abi(play.attach_to), position, out.position);
    }
    return CY_RESULT_OK;
}

CyResult AudioAdapter::play(const CyAudioPlay& play, CyAudioVoice& out_voice) noexcept {
    audio::VoiceDescription description;
    if (const CyResult described = describe(play, description); described != CY_RESULT_OK) {
        return described;
    }
    const f32 volume = description.volume;
    if (play.fade_in_seconds > 0.0F) {
        description.volume = 0.0F;
    }
    const Expected<audio::VoiceHandle, Error> started = server_.play(description);
    if (!started) {
        if (started.error().code == ErrorCode::OutOfRange) {
            out_voice = 0;  // exhausted: not an error, see the file comment
            return CY_RESULT_OK;
        }
        return abi::report(started.error());
    }
    if (const CyResult tracked_ok = track(play, *started, volume); tracked_ok != CY_RESULT_OK) {
        (void)server_.stop(*started);
        return tracked_ok;
    }
    out_voice = started->bits();
    return CY_RESULT_OK;
}

CyResult AudioAdapter::track(const CyAudioPlay& play, audio::VoiceHandle handle,
                             f32 volume) noexcept {
    const bool attached = (play.flags & CY_AUDIO_PLAY_ATTACH) != 0U;
    if (!attached && play.fade_in_seconds <= 0.0F) {
        return CY_RESULT_OK;
    }
    Voice voice;
    voice.handle = handle;
    voice.volume = volume;
    if (attached) {
        voice.attached = abi::from_abi(play.attach_to);
        voice.offset = Vec3{play.position[0], play.position[1], play.position[2]};
    }
    if (play.fade_in_seconds > 0.0F) {
        voice.volume = 0.0F;
        voice.fade = Ramp{0.0F, volume, play.fade_in_seconds, 0.0F};
    }
    if (const Status pushed = voices_.push_back(voice); !pushed) {
        return abi::report(pushed.error());
    }
    return CY_RESULT_OK;
}

AudioAdapter::Voice* AudioAdapter::tracked(audio::VoiceHandle handle) noexcept {
    for (Voice& voice : voices_) {
        if (voice.handle == handle) {
            return &voice;
        }
    }
    return nullptr;
}

CyResult AudioAdapter::stop(CyAudioVoice voice, f32 fade_out_seconds) noexcept {
    const audio::VoiceHandle handle = voice_of(voice);
    if (!server_.playing(handle)) {
        return CY_RESULT_OK;  // ended, stale or null: stopping is idempotent
    }
    if (fade_out_seconds <= 0.0F) {
        (void)server_.stop(handle);
        return CY_RESULT_OK;
    }
    Voice* existing = tracked(handle);
    if (existing == nullptr) {
        Voice fresh;
        fresh.handle = handle;
        if (const Status pushed = voices_.push_back(fresh); !pushed) {
            return abi::report(pushed.error());
        }
        existing = &voices_.back();
    }
    existing->stopping = true;
    existing->fade = Ramp{existing->volume, 0.0F, fade_out_seconds, 0.0F};
    return CY_RESULT_OK;
}

bool AudioAdapter::playing(CyAudioVoice voice) const noexcept {
    return server_.playing(voice_of(voice));
}

CyResult AudioAdapter::set_bus_volume(CyAudioBus bus, f32 volume, f32 fade_seconds) noexcept {
    const audio::BusHandle handle = bus_of(bus);
    const audio::BusDescription* description = server_.buses().description(handle);
    if (description == nullptr) {
        return abi::report(CY_RESULT_NOT_FOUND, "audio_set_bus_volume: the bus is null or stale");
    }
    // A new request replaces any ramp already running on the bus.
    for (usize index = 0; index < bus_fades_.size(); ++index) {
        if (bus_fades_[index].bus == handle) {
            bus_fades_.erase(index);
            break;
        }
    }
    if (fade_seconds <= 0.0F) {
        const Status set = server_.buses().set_volume(handle, volume);
        return set ? CY_RESULT_OK : abi::report(set.error());
    }
    const Status pushed = bus_fades_.push_back(
        BusFade{handle, Ramp{description->volume, volume, fade_seconds, 0.0F}});
    return pushed ? CY_RESULT_OK : abi::report(pushed.error());
}

// --- The frame -----------------------------------------------------------------------------------

bool AudioAdapter::advance_voice(Voice& voice, f32 delta_seconds) noexcept {
    if (!server_.playing(voice.handle)) {
        return false;
    }
    if (voice.attached.valid() && tree_ != nullptr) {
        const scene::Node node = tree_->node(voice.attached);
        if (node.valid()) {
            const Vec3 position = node.world_transform().transform_point(voice.offset);
            (void)server_.set_position(voice.handle, position, Vec3{0.0F, 0.0F, 0.0F});
        } else {
            voice.attached = ecs::Entity();  // the entity died: the voice stays where it was
        }
    }
    if (voice.fade.active()) {
        voice.volume = voice.fade.advance(delta_seconds);
        (void)server_.set_volume(voice.handle, voice.volume);
        if (voice.fade.done()) {
            voice.fade = Ramp{};
            if (voice.stopping) {
                (void)server_.stop(voice.handle);
                return false;
            }
        }
    }
    return voice.attached.valid() || voice.fade.active();
}

void AudioAdapter::advance_buses(f32 delta_seconds) noexcept {
    usize kept = 0;
    for (const BusFade& current : bus_fades_) {
        BusFade fade = current;
        const f32 volume = fade.ramp.advance(delta_seconds);
        const bool alive = static_cast<bool>(server_.buses().set_volume(fade.bus, volume));
        if (alive && !fade.ramp.done()) {
            bus_fades_[kept++] = fade;
        }
    }
    while (bus_fades_.size() > kept) {
        bus_fades_.pop_back();
    }
}

void AudioAdapter::update(f32 delta_seconds) noexcept {
    advance_buses(delta_seconds);
    // Compacted in place, in order, so the voices are always visited in the order they started.
    usize kept = 0;
    for (const Voice& current : voices_) {
        Voice voice = current;
        if (advance_voice(voice, delta_seconds)) {
            voices_[kept++] = voice;
        }
    }
    while (voices_.size() > kept) {
        voices_.pop_back();
    }
}

void bind(cy::abi::Host& host, AudioAdapter* adapter) noexcept {
    host.game.audio = adapter;
}

}  // namespace cy::game_backend
