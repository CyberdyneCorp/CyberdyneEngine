// Voice vocabulary and the importance score. See cy/servers/audio/voice.h.

#include <cy/servers/audio/voice.h>

#include <cy/core/math/scalar.h>

namespace cy::audio {

const char* simulation_tier_name(SimulationTier tier) noexcept {
    switch (tier) {
        case SimulationTier::FullAcoustic:
            return "full-acoustic";
        case SimulationTier::Spatialised:
            return "spatialised";
        case SimulationTier::Simple:
            return "simple";
        case SimulationTier::Virtual:
            return "virtual";
        case SimulationTier::Count:
            break;
    }
    return "unknown";
}

const char* voice_state_name(VoiceState state) noexcept {
    switch (state) {
        case VoiceState::Stopped:
            return "stopped";
        case VoiceState::Playing:
            return "playing";
        case VoiceState::Paused:
            return "paused";
        case VoiceState::Stopping:
            return "stopping";
        case VoiceState::Count:
            break;
    }
    return "unknown";
}

f32 voice_importance(const VoiceControl& voice, const Listener& listener) noexcept {
    if (!voice.live) {
        return 0.0F;
    }

    f32 score = voice.desc.volume * voice.volume_scale;
    score *= static_cast<f32>(voice.desc.priority) / 255.0F;

    // A NON-SPATIALISED VOICE TAKES THE EMITTER'S TERMS ONLY. Music and interface sounds have no
    // position, so no distance term applies to them; scoring them at a distance of zero would put
    // them above every diegetic sound rather than beside them.
    if (voice.desc.spatialised) {
        const f32 distance = length(voice.desc.position - listener.transform.translation);
        score *= attenuation_gain(voice.desc.attenuation, distance);
        score *= cone_gain(voice.desc.cone, voice.desc.forward,
                           listener.transform.translation - voice.desc.position);
        score *= math::clamp(1.0F - (voice.desc.occlusion * 0.5F), 0.0F, 1.0F);

        // LISTENER ORIENTATION, which `audio` lists among the score's inputs: a source in front
        // scores above one behind, gently — 1.0 in front, 0.8 behind. Enough to break a tie between
        // two equally distant sources, not enough to make turning round demote a sound that
        // matters.
        const Vec3 to_source = voice.desc.position - listener.transform.translation;
        if (length_squared(to_source) > 1e-8F) {
            const f32 facing = dot(listener.transform.forward(), normalize(to_source));
            score *= math::lerp(0.8F, 1.0F, math::clamp((facing + 1.0F) * 0.5F, 0.0F, 1.0F));
        }
    }

    // THE PREVIOUS TIER IS AN INPUT, and that is the hysteresis: a voice already above `Virtual`
    // scores a little higher, so a source hovering at a budget boundary keeps the tier it has
    // instead of flapping between two every update. `audio` requires exactly that.
    if (voice.tier != SimulationTier::Virtual) {
        score *= 1.05F;
    }
    return math::max(score, 0.0F);
}

}  // namespace cy::audio
