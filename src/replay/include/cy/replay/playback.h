#pragma once
// Playback, seeking, and presentation tracks. M9 task 3.1.
//
// Section 1 built the record and the cursors over it. **This file is the thing that hands a
// cursor's output back to a running session**, which is what section 1's README lists as
// deliberately absent: "what does not exist here is the thing that hands their output to
// `CommandStream`".
//
// ================================================================================================
// PLAYBACK IS A CONTROL SOURCE, AND THAT IS WHY `produce()` TAKES A `CommandStream`
// ================================================================================================
//
// `replay-and-rollback`: "Replay playback SHALL be a **control source** producing recorded
// commands, so playback exercises the same simulation path as live play." The temptation is to hand
// a replay's commands straight to the systems that execute them and skip the merge; that would be a
// second execution path, and the first divergence between a replay and its session would be
// invisible because the two never went through the same code.
//
// So `PlaybackDriver::produce()` writes into `gameplay::CommandBuffer`s through
// `CommandStream::producer()`, exactly as a keyboard does, and the session's own `commit()` merges,
// validates, logs and publishes them. Nothing here calls a system.
//
// ================================================================================================
// THE PRODUCER TOPOLOGY IS BOUND BY THE CALLER, AND THE REASON IS THE BIT-EXACT CLAIM
// ================================================================================================
//
// `CommandStream::commit()` merges on `(producer order, record order)` and
// `CommandBuffer::record()` stamps its own per-producer sequence number. A replay that fed four
// participants' commands through ONE producer would therefore reproduce the same *order* and a
// different set of `sequence` values — the state hash would match and the log's bytes would not,
// and "recorded and replayed bit-exactly" would be a claim about half the record.
//
// `bind_participant()` is how a caller reproduces the recording's topology: one producer per
// participant, opened in the recording's order. `unbound_participants()` counts what fell through
// to the default producer, so a session that could not reproduce the topology says so rather than
// quietly weakening the claim. `tests/test_bitexact.cpp` asserts the whole record —
// `RecordLog::hash()` over the replay equals the original's — and that is only reachable because of
// this.
//
// ================================================================================================
// SPEED CHANGES WHICH TICKS ARE PRESENTED. IT NEVER CHANGES THE STEP.
// ================================================================================================
//
// `PlaybackClock` is integer arithmetic over an exact rational tick rate and an exact rational
// speed; there is no `float` anywhere in it. `tick_micros()` is a function of the tick rate ALONE
// and `set_speed()` cannot reach it — which is the structural half of "ticks SHALL remain fixed and
// only presentation frequency SHALL change", and what `tests/test_playback_clock.cpp` checks by
// reading `tick_micros()` either side of a speed change.
//
// ================================================================================================
// BACKWARD SEEKING SEEKS BACKWARD AND SIMULATES FORWARD
// ================================================================================================
//
// "**Backward simulation SHALL NOT be attempted.** Reverse playback SHALL seek an earlier
// checkpoint and replay forward." `plan_seek()` therefore returns the same shape whichever
// direction the target lies in: a checkpoint to restore, a first tick, and a count of ticks to
// fast-forward. `backward` is reported for the diagnostic and changes nothing about the plan,
// because there is nothing it could change — there is only one mechanism.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/gameplay/command.h>
#include <cy/replay/log.h>
#include <cy/replay/readers.h>
#include <cy/replay/record.h>

namespace cy::replay {

/// Whether a tick's output reaches a renderer, an audio device or an effect system.
///
/// `replay-and-rollback`: "with presentation output suppressed during fast-forward so it runs as
/// fast as the processor allows". This module cannot reach a renderer — see the CMake dependency
/// list — so what it produces is the *decision*, and the runtime honours it.
enum class Presentation : u8 {
    Present = 0,
    Suppressed,
};

const char* presentation_name(Presentation value) noexcept;

/// Playback speed as an exact rational. Half speed is `{1, 2}`, three times is `{3, 1}`.
///
/// Never a float, for `CompatibilityManifest::tick_rate_numerator`'s reason: a speed of 0.1 is not
/// representable, and a clock that accumulated it would drift away from the tick grid over a long
/// session — which would make "the same replay, watched twice" present different ticks.
struct PlaybackSpeed {
    u32 numerator = 1;
    u32 denominator = 1;

    friend constexpr bool operator==(PlaybackSpeed, PlaybackSpeed) noexcept = default;
};

/// The fixed-step clock playback advances on.
///
/// Integer throughout: real microseconds in, whole ticks out, with the remainder carried in the
/// accumulator so no time is lost or invented across calls.
class PlaybackClock {
public:
    /// The tick rate as the manifest records it. Refuses nothing here; `valid()` reports a rate
    /// that cannot be used, because a constructor that could fail would make every caller handle
    /// it.
    PlaybackClock(u32 tick_rate_numerator, u32 tick_rate_denominator) noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return rate_numerator_ != 0 && rate_denominator_ != 0;
    }

    /// Refuses a zero numerator or denominator. A speed of zero is `pause()`, which is a different
    /// thing from a clock that cannot advance.
    [[nodiscard]] Status set_speed(PlaybackSpeed speed) noexcept;
    [[nodiscard]] PlaybackSpeed speed() const noexcept { return speed_; }

    void pause() noexcept { paused_ = true; }
    void resume() noexcept { paused_ = false; }
    [[nodiscard]] bool paused() const noexcept { return paused_; }

    /// Advance by `micros` of real time and return the whole ticks to simulate.
    ///
    /// The remainder stays in the accumulator, so time is neither lost nor invented across calls: a
    /// hundred calls of 10 000 us at 60 Hz simulate exactly sixty ticks, where an implementation
    /// that divided per call and dropped the remainder would simulate none at all — every one of
    /// those calls is shorter than a tick.
    [[nodiscard]] u32 advance(u64 micros) noexcept;

    /// The duration of one simulation tick. **A function of the tick rate and of nothing else** —
    /// there is no path from `set_speed()` to this number, which is the structural half of "speed
    /// SHALL affect which ticks are presented, never the step size".
    [[nodiscard]] u64 tick_micros() const noexcept;

    /// Ticks the clock has issued since construction.
    [[nodiscard]] u64 ticks_issued() const noexcept { return issued_; }

    void reset() noexcept;

private:
    u64 accumulator_ = 0;
    u64 issued_ = 0;
    u32 rate_numerator_ = 60;
    u32 rate_denominator_ = 1;
    PlaybackSpeed speed_;
    bool paused_ = false;
};

/// What a seek to a tick amounts to. Produced by `PlaybackDriver::plan_seek()` and executed by the
/// session, because restoring a capture needs a world and this module is handed one per call rather
/// than holding one.
struct SeekPlan {
    /// False when the log cannot serve the target at all — a tick past everything recorded.
    bool valid = false;

    /// The checkpoint to restore first. False means the window starts at the session's beginning
    /// and the fast-forward runs from tick zero, which is a fact worth stating rather than
    /// implying: a seek two hours in with no checkpoint is an expensive surprise.
    bool has_checkpoint = false;
    u64 checkpoint_tick = 0;
    /// `LogRecord::value` of the checkpoint record — the address in the checkpoint store. This
    /// module does not own the store and deliberately does not look inside it.
    u64 checkpoint_address = 0;

    /// The first tick to simulate after the restore.
    u64 first_tick = 0;
    u64 target_tick = 0;
    /// Ticks between the restore and the target. What the fast-forward costs.
    u64 fast_forward_ticks = 0;

    /// Presentation during the fast-forward. Always `Suppressed`: "rendering, audio, and effects
    /// SHALL be suppressed while ticks are simulated".
    Presentation fast_forward_presentation = Presentation::Suppressed;

    /// The target was behind the current tick. Diagnostics only — the plan is the same either way,
    /// because backward simulation is not attempted.
    bool backward = false;
};

/// Reader 1's driver: the control source that produces recorded commands.
class PlaybackDriver {
public:
    PlaybackDriver(Allocator& allocator, const RecordLog& log) noexcept
        : cursor_(log), log_(&log), bindings_(allocator) {}

    PlaybackDriver(const PlaybackDriver&) = delete;
    PlaybackDriver& operator=(const PlaybackDriver&) = delete;

    /// Route a recorded participant's commands to one producer, reproducing the recording's
    /// topology. Refuses a duplicate participant — two producers for one participant would make the
    /// merge order depend on which binding was found first.
    [[nodiscard]] Status bind_participant(u64 participant, u32 producer_order) noexcept;

    /// Where a command whose participant has no binding goes. Zero by default.
    void set_default_producer(u32 producer_order) noexcept { default_producer_ = producer_order; }

    [[nodiscard]] u32 binding_count() const noexcept { return static_cast<u32>(bindings_.size()); }
    /// Commands produced for a participant with no binding. Non-zero means the replay could not
    /// reproduce the recording's producer topology, so its per-producer sequence numbers will
    /// differ from the recording's even when every value agrees.
    [[nodiscard]] u64 unbound_participants() const noexcept { return unbound_; }

    /// Plan a seek to `target_tick` from `current_tick`.
    [[nodiscard]] SeekPlan plan_seek(u64 target_tick, u64 current_tick) noexcept;

    /// Produce every recorded command for `tick` into `stream`'s producers, in recorded order.
    ///
    /// Provenance is rewritten to `ControlSourceKind::Replay`, and that is safe precisely because
    /// `gameplay-framework` forbids provenance from affecting validation, ordering or execution —
    /// a replay says out loud where its commands came from and nothing downstream is allowed to
    /// care. Returns the number produced.
    [[nodiscard]] Expected<u32, Error> produce(gameplay::CommandStream& stream, u64 tick) noexcept;

    [[nodiscard]] u64 commands_produced() const noexcept { return produced_; }

    /// Rewind the cursor to the start of the log. The bindings survive.
    void rewind() noexcept;

private:
    [[nodiscard]] u32 producer_for(u64 participant) noexcept;

    struct Binding {
        u64 participant = 0;
        u32 producer_order = 0;
    };

    PlaybackCursor cursor_;
    const RecordLog* log_;
    Array<Binding> bindings_;
    u64 produced_ = 0;
    u64 unbound_ = 0;
    u32 default_producer_ = 0;
};

// --- Presentation tracks -------------------------------------------------------------------------
//
// `replay-and-rollback`: "A replay MAY carry **presentation tracks** — camera direction,
// annotations, director cuts, markers — separate from authoritative data. Removing or ignoring a
// presentation track SHALL NOT affect authoritative reconstruction."
//
// **They are a separate container from `RecordLog` and that is the whole mechanism.** A track is
// not a `LogRecord` kind, it is not in the log's index, it is not in `RecordLog::hash()`, and
// `PlaybackDriver` cannot see one. So "removing a track does not affect reconstruction" is a
// property of the type graph rather than a rule somebody follows, and `tests/test_playback.cpp`
// checks it the only way that means anything: it replays a session with tracks and without them and
// compares the log hash and the final state hash.

/// What a track directs.
enum class PresentationTrackKind : u8 {
    /// Overrides the camera reconstructed from simulation state, for directed playback.
    CameraDirection = 0,
    /// Text a viewer reads.
    Annotation,
    /// A cut to another viewpoint.
    DirectorCut,
    /// A point of interest in the timeline.
    Marker,
};

const char* presentation_track_kind_name(PresentationTrackKind kind) noexcept;

/// The largest inline payload one track sample carries. A camera pose fits; a video does not, and a
/// track carrying one is carrying content rather than direction.
inline constexpr u16 kMaxTrackPayload = 64;

/// One sample on one track.
struct PresentationSample {
    u64 tick = 0;
    /// A subject the sample is about — an entity to look at, a marker's identity. Zero for none.
    u64 subject = 0;
    u16 payload_size = 0;
    u8 payload[kMaxTrackPayload] = {};
};

/// One track. Tick-monotonic, for the same reason the log is: the order samples were authored in is
/// the order they are played in, and a track that sorted would have an opinion.
class PresentationTrack {
public:
    PresentationTrack(Allocator& allocator, PresentationTrackKind kind, const char* name) noexcept
        : samples_(allocator), kind_(kind), name_(name) {}

    PresentationTrack(const PresentationTrack&) = delete;
    PresentationTrack& operator=(const PresentationTrack&) = delete;

    [[nodiscard]] Status append(const PresentationSample& sample) noexcept;

    [[nodiscard]] PresentationTrackKind kind() const noexcept { return kind_; }
    [[nodiscard]] const char* name() const noexcept { return name_; }
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(samples_.size()); }
    [[nodiscard]] const PresentationSample& at(u32 index) const noexcept { return samples_[index]; }

    /// The sample in force at `tick` — the last one at or before it. False when the track has not
    /// started yet, which is what makes a track that begins half-way through a replay legal.
    [[nodiscard]] bool sample_at(u64 tick, PresentationSample& out) const noexcept;

    void clear() noexcept;

private:
    Array<PresentationSample> samples_;
    PresentationTrackKind kind_;
    const char* name_;
};

/// A replay's tracks. Optional in the strongest sense: nothing in the reconstruction path holds
/// one.
class PresentationTrackSet {
public:
    explicit PresentationTrackSet(Allocator& allocator) noexcept
        : tracks_(allocator), allocator_(&allocator) {}

    ~PresentationTrackSet();

    PresentationTrackSet(const PresentationTrackSet&) = delete;
    PresentationTrackSet& operator=(const PresentationTrackSet&) = delete;

    /// Refuses a duplicate name: two tracks called "director" would make `find()` order-dependent.
    ///
    /// **`name` is stored, not copied**, so it must be a literal or storage that outlives the set —
    /// the convention `determinism::HashNode::name` follows, and for its reason: a track name is
    /// metadata a person reads, and copying it would put a string allocation on a path that has no
    /// other reason to have one. A caller that reuses one buffer for every track gives every track
    /// the same name, and the second `add()` refuses it.
    [[nodiscard]] Expected<PresentationTrack*, Error> add(PresentationTrackKind kind,
                                                          const char* name) noexcept;
    [[nodiscard]] PresentationTrack* find(const char* name) noexcept;
    [[nodiscard]] const PresentationTrack* find(const char* name) const noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(tracks_.size()); }
    [[nodiscard]] const PresentationTrack& at(u32 index) const noexcept { return *tracks_[index]; }

    /// The camera for `tick`: the first `CameraDirection` track that has a sample in force wins,
    /// and `false` means there is none, so the caller reconstructs from simulation state.
    ///
    /// "By default cameras SHALL be reconstructed from recorded simulation state; an authored
    /// camera track SHALL be able to override that for directed playback." The default is the
    /// *absence* of an answer here rather than a value chosen here, because this module has no
    /// camera.
    [[nodiscard]] bool camera_override(u64 tick, PresentationSample& out) const noexcept;

    void clear() noexcept;

private:
    Array<PresentationTrack*> tracks_;
    Allocator* allocator_;
};

}  // namespace cy::replay
