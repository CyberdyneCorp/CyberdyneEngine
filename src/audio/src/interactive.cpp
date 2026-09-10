// Playlists, layered stems, and transitions on the audio clock. M8.b task 10.2.

#include <cy/audio/interactive.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cy::audio {
namespace {

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

/// A small deterministic stream. Fixed constants, because a shuffle that differs between a session
/// and its replay is a shuffle `simulation-and-determinism` forbids.
[[nodiscard]] u64 next_random(u64& state) noexcept {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

}  // namespace

const char* transition_point_name(TransitionPoint point) noexcept {
    switch (point) {
        case TransitionPoint::Immediate:
            return "immediate";
        case TransitionPoint::NextBeat:
            return "next-beat";
        case TransitionPoint::NextBar:
            return "next-bar";
        case TransitionPoint::NextMarker:
            return "next-marker";
        case TransitionPoint::EndOfClip:
            return "end-of-clip";
        case TransitionPoint::Count:
            break;
    }
    return "unknown";
}

f64 samples_per_beat(const MusicalClip& clip) noexcept {
    const f64 tempo = (clip.tempo_bpm > 0.0F) ? static_cast<f64>(clip.tempo_bpm) : 120.0;
    return (static_cast<f64>(clip.sample_rate) * 60.0) / tempo;
}

u64 next_transition_point(const MusicalClip& clip, u64 playhead, TransitionPoint point) noexcept {
    switch (point) {
        case TransitionPoint::Immediate:
            return playhead;
        case TransitionPoint::EndOfClip:
            return clip.length_samples;
        case TransitionPoint::NextMarker: {
            for (const u64 marker : clip.markers) {
                if (marker > playhead) {
                    return marker;
                }
            }
            // No marker ahead: the end of the clip is the next musically meaningful point there is.
            return clip.length_samples;
        }
        case TransitionPoint::NextBeat:
        case TransitionPoint::NextBar: {
            const f64 beat = samples_per_beat(clip);
            const f64 unit =
                (point == TransitionPoint::NextBar)
                    ? (beat * static_cast<f64>((clip.beats_per_bar == 0) ? 1U : clip.beats_per_bar))
                    : beat;
            if (unit <= 0.0) {
                return playhead;
            }
            // THE NEXT BOUNDARY STRICTLY AFTER THE PLAYHEAD. A transition asked for on a bar line
            // means the NEXT bar, not this instant — otherwise a request that happens to land on a
            // boundary would fire immediately and the music would lurch.
            const f64 index = std::floor(static_cast<f64>(playhead) / unit) + 1.0;
            return static_cast<u64>(index * unit);
        }
        case TransitionPoint::Count:
            break;
    }
    return playhead;
}

u32 advance_playlist(Span<const PlaylistEntry> entries, PlaylistOrder order,
                     PlaylistState& state) noexcept {
    if (entries.empty()) {
        state.finished = true;
        return kPlaylistEnd;
    }
    if (entries.size() == 1) {
        state.previous = state.current;
        state.current = 0;
        state.finished = (order == PlaylistOrder::Sequential) && (state.previous == 0);
        return state.finished ? kPlaylistEnd : 0U;
    }

    switch (order) {
        case PlaylistOrder::Sequential: {
            const u32 next = state.current + 1U;
            if (next >= static_cast<u32>(entries.size())) {
                state.finished = true;
                return kPlaylistEnd;
            }
            state.previous = state.current;
            state.current = next;
            return next;
        }
        case PlaylistOrder::Loop: {
            state.previous = state.current;
            state.current = (state.current + 1U) % static_cast<u32>(entries.size());
            return state.current;
        }
        case PlaylistOrder::Shuffle: {
            f32 total = 0.0F;
            for (usize index = 0; index < entries.size(); ++index) {
                // THE ENTRY JUST PLAYED IS EXCLUDED. A shuffle that can repeat is a shuffle players
                // complain about, and with one entry left the exclusion is dropped rather than
                // producing nothing.
                if (static_cast<u32>(index) == state.current) {
                    continue;
                }
                total += (entries[index].weight > 0.0F) ? entries[index].weight : 0.0F;
            }
            if (total <= 0.0F) {
                state.previous = state.current;
                state.current = (state.current + 1U) % static_cast<u32>(entries.size());
                return state.current;
            }
            const f32 pick =
                static_cast<f32>(next_random(state.seed) & 0xFFFFFFU) / 16777215.0F * total;
            f32 accumulated = 0.0F;
            for (usize index = 0; index < entries.size(); ++index) {
                if (static_cast<u32>(index) == state.current) {
                    continue;
                }
                accumulated += (entries[index].weight > 0.0F) ? entries[index].weight : 0.0F;
                if (pick <= accumulated) {
                    state.previous = state.current;
                    state.current = static_cast<u32>(index);
                    return state.current;
                }
            }
            state.previous = state.current;
            state.current = (state.current + 1U) % static_cast<u32>(entries.size());
            return state.current;
        }
    }
    return state.current;
}

void update_layers(Span<MusicLayer> layers, f32 parameter, f32 dt) noexcept {
    for (MusicLayer& layer : layers) {
        // The mapping is the LAYER's, so a composer changes how the music responds without gameplay
        // code changing — and a layer that fades out as the parameter rises is the same arithmetic
        // with its two ends swapped.
        f32 target = 0.0F;
        const f32 span = layer.full_at - layer.silent_at;
        if (std::fabs(span) < 1e-6F) {
            target = (parameter >= layer.full_at) ? 1.0F : 0.0F;
        } else {
            target = clampf((parameter - layer.silent_at) / span, 0.0F, 1.0F);
        }
        if (layer.fade_seconds <= 0.0F || dt <= 0.0F) {
            layer.gain = target;
            continue;
        }
        // A half-life again: the same fade over the same wall-clock time at any update rate.
        const f32 alpha = 1.0F - std::pow(0.5F, dt / layer.fade_seconds);
        layer.gain += (target - layer.gain) * alpha;
    }
}

Status schedule_transition(const MusicalClip& clip, u64 playhead,
                           MusicTransition& transition) noexcept {
    if (clip.sample_rate == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a clip needs a sample rate to schedule against", 0});
    }
    transition.at_sample = next_transition_point(clip, playhead, transition.point);
    transition.scheduled = true;
    return ok();
}

TransitionMix transition_mix(const MusicTransition& transition, const MusicalClip& clip,
                             u64 sample) noexcept {
    TransitionMix mix;
    if (!transition.scheduled || sample < transition.at_sample) {
        // BEFORE THE POINT the source owns the mix entirely. A transition scheduled for the next
        // bar changes nothing until the bar arrives, which is the whole content of the requirement.
        return mix;
    }
    const f64 fade_samples =
        static_cast<f64>(clip.sample_rate) * static_cast<f64>(transition.crossfade_seconds);
    if (fade_samples <= 0.0) {
        mix.from_gain = 0.0F;
        mix.to_gain = 1.0F;
        mix.complete = true;
        return mix;
    }
    const f64 elapsed = static_cast<f64>(sample - transition.at_sample);
    const f32 t = clampf(static_cast<f32>(elapsed / fade_samples), 0.0F, 1.0F);

    // EQUAL-POWER, not linear: two uncorrelated stems cross-faded linearly dip by three decibels in
    // the middle, which is audible as a hole exactly where the transition is meant to be seamless.
    constexpr f32 kQuarterTurn = std::numbers::pi_v<f32> * 0.5F;
    mix.from_gain = std::cos(t * kQuarterTurn);
    mix.to_gain = std::sin(t * kQuarterTurn);
    mix.complete = t >= 1.0F;
    if (!transition.segment.is_empty()) {
        // A TRANSITION SEGMENT takes the middle: it rises as the source falls and falls as the
        // destination rises, so the two never meet directly.
        mix.segment_gain = std::sin(t * std::numbers::pi_v<f32>);
        mix.in_segment = t > 0.1F && t < 0.9F;
        if (mix.in_segment) {
            mix.from_gain *= 1.0F - mix.segment_gain;
            mix.to_gain *= 1.0F - mix.segment_gain;
        }
    }
    return mix;
}

}  // namespace cy::audio
