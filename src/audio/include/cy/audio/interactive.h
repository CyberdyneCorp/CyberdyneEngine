#ifndef CY_AUDIO_INTERACTIVE_H
#define CY_AUDIO_INTERACTIVE_H
// Playlists, layered music, and transitions at musically meaningful points. M8.b task 10.2.
//
// `audio`: "The engine SHALL support: playlists with ordering and transition rules, layered music
// where stems fade with gameplay parameters, and transitions that occur at musically meaningful
// points (immediate, next beat, next bar, next marker, end of clip) with optional transition
// segments."
//
// --- THE TRANSITION POINT IS ARITHMETIC ON THE AUDIO CLOCK ---------------------------------------
//
// "WHEN combat begins and the transition is set to 'next bar' THEN the music SHALL switch at the
// next bar boundary, optionally through a transition stem." A transition scheduled for "the next
// bar" is a SAMPLE NUMBER, computed from the tempo, the time signature and where the playhead is —
// not "soon". `next_transition_point()` returns that sample, and everything else here is
// bookkeeping around it.
//
// The clock is samples rather than seconds because `audio`'s timing requirement is explicit about
// it: "Playback SHALL be schedulable at a future audio time, so sounds can be started exactly on a
// beat regardless of frame timing." A frame boundary is not a musical boundary.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::audio {

/// Where a transition may happen.
enum class TransitionPoint : u8 {
    /// At the next callback. What a stinger wants.
    Immediate = 0,
    NextBeat,
    NextBar,
    /// At the next authored marker, whatever it is for.
    NextMarker,
    /// When the current clip ends. What a playlist does between tracks.
    EndOfClip,
    Count,
};

[[nodiscard]] const char* transition_point_name(TransitionPoint point) noexcept;

/// A clip's musical shape. Everything a transition needs to know about where it is.
struct MusicalClip {
    Name name;
    /// Total length in samples, which is what the audio clock counts.
    u64 length_samples = 0;
    u32 sample_rate = 48000;
    f32 tempo_bpm = 120.0F;
    /// Beats per bar. Four is a bar of four four.
    u32 beats_per_bar = 4;
    /// Authored markers, in samples from the clip's start, ascending.
    Span<const u64> markers;
    /// Whether it loops when nothing takes over.
    bool looping = true;
};

/// Samples per beat, which every other computation here is built from.
[[nodiscard]] f64 samples_per_beat(const MusicalClip& clip) noexcept;

/// The sample at which a transition may begin, at or after `playhead`.
///
/// Returns the clip's end for `EndOfClip`, and `playhead` itself for `Immediate` — so a caller
/// schedules against one number whichever rule is in force, and there is no branch at the call
/// site.
[[nodiscard]] u64 next_transition_point(const MusicalClip& clip, u64 playhead,
                                        TransitionPoint point) noexcept;

/// One entry in a playlist.
struct PlaylistEntry {
    Name clip;
    /// Relative weight for `Shuffle`. Ignored by the other orders.
    f32 weight = 1.0F;
};

/// How a playlist chooses what comes next.
enum class PlaylistOrder : u8 {
    Sequential = 0,
    Loop,
    /// Weighted random, and it does NOT repeat the entry it just played unless the playlist has one
    /// entry — a shuffle that can play the same track twice is a shuffle players complain about.
    Shuffle,
};

struct PlaylistState {
    u32 current = 0;
    u32 previous = 0xFFFFFFFFU;
    /// The deterministic stream a shuffle draws from. Seeded by the caller, so a replay of a
    /// session picks the same tracks — which `simulation-and-determinism` requires of anything that
    /// consumes randomness.
    u64 seed = 0x9E3779B97F4A7C15ULL;
    bool finished = false;
};

/// Choose the next entry. Returns the index, or `kPlaylistEnd` when a sequential playlist is done.
inline constexpr u32 kPlaylistEnd = 0xFFFFFFFFU;
[[nodiscard]] u32 advance_playlist(Span<const PlaylistEntry> entries, PlaylistOrder order,
                                   PlaylistState& state) noexcept;

// --- Layered music
// ----------------------------------------------------------------------------------

/// One stem of a layered piece, and the gameplay parameter that brings it in.
struct MusicLayer {
    Name name;
    /// The parameter value at which this layer is silent, and the one at which it is full. A layer
    /// that fades OUT as the parameter rises simply has `full` below `silent`.
    f32 silent_at = 0.0F;
    f32 full_at = 1.0F;
    /// Seconds the layer takes to reach a new gain. A layer that snapped would be a click.
    f32 fade_seconds = 0.5F;
    /// The gain in force, advanced by `update_layers`.
    f32 gain = 0.0F;
};

/// Advance every layer toward the gain the parameter implies.
///
/// The parameter is gameplay's — combat intensity, depth, health — and the mapping is the layer's,
/// so a composer changes the music's response without gameplay code changing.
void update_layers(Span<MusicLayer> layers, f32 parameter, f32 dt) noexcept;

// --- The transition
// ----------------------------------------------------------------------------------

/// A scheduled transition.
struct MusicTransition {
    Name from;
    Name to;
    /// The optional stem played between the two. Empty means the two cross-fade directly.
    Name segment;
    TransitionPoint point = TransitionPoint::NextBar;
    /// Seconds the cross-fade takes.
    f32 crossfade_seconds = 0.5F;
    /// The sample the transition begins at, filled in by `schedule_transition`.
    u64 at_sample = 0;
    bool scheduled = false;
};

/// Schedule a transition against a clip and a playhead.
[[nodiscard]] Status schedule_transition(const MusicalClip& clip, u64 playhead,
                                         MusicTransition& transition) noexcept;

/// What the mix should be doing at `sample`, for a scheduled transition.
struct TransitionMix {
    f32 from_gain = 1.0F;
    f32 to_gain = 0.0F;
    f32 segment_gain = 0.0F;
    /// True once the transition has finished and the destination owns the mix.
    bool complete = false;
    /// True while the transition segment is the thing playing.
    bool in_segment = false;
};

/// The gains at a sample. Before the transition point the source owns the mix; through the
/// cross-fade the two overlap; a transition segment, when there is one, takes the middle.
[[nodiscard]] TransitionMix transition_mix(const MusicTransition& transition,
                                           const MusicalClip& clip, u64 sample) noexcept;

}  // namespace cy::audio

#endif  // CY_AUDIO_INTERACTIVE_H
