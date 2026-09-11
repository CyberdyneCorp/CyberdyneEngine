#pragma once
// Playback: the service, the instances, seeking, skipping, capture and restore, and the preload
// plan published ahead of need. M8.c tasks 3.2, 3.5 and 3.6.
//
// ================================================================================================
// A SERVICE, NOT A SINGLETON
// ================================================================================================
//
// `sequencing-and-cinematics` — "Playback control": a sequence "SHALL be playable through a
// **service scoped to a world, session, local player, or editor preview** — not a global
// singleton". So `SequencePlayer` carries its scope, four of them coexist, and a camera flourish
// played on one local player's service reaches that player only. There is no `instance()` on this
// class and there never will be.
//
// ================================================================================================
// SEEKING DOES NOT REPLAY FROM ZERO, AND THE INDEX IS WHY
// ================================================================================================
//
// "Seeking SHALL locate the target time through the index, restore or reconstruct subsystem state,
// and resume — **without replaying the sequence from its start**." `seek()` performs exactly three
// steps: `Program::active_at()` for the destination, a difference against the sections that were
// active, and `Program::events_between()` for what the jump crossed. Its cost is the same for a
// four-minute jump as for a frame, and `tests/test_playback.cpp` measures that as a claim rather
// than describing it.
//
// The seek MODE decides what happens to the events the jump crossed, and the four modes are not
// interchangeable: an author scrubbing must not fire a mission unlock, a replay must not fire
// anything because the log already carries it, and a reconstructing seek must apply the outcome
// without pretending the moment happened.
//
// ================================================================================================
// SKIPPING IS NOT STOPPING
// ================================================================================================
//
// "**Skipping a sequence that carries gameplay consequence SHALL apply its required authoritative
// outcomes** — its declared events, commands, and final state — before advancing presentation to
// the end. Stopping playback SHALL NOT be the implementation of skipping."
//
// `skip()` emits the declared outcomes into the batch and then completes. The door opens. And the
// half of that requirement that keeps it honest is in the compiler, not here: a sequence with
// authoritative tracks and no declared outcomes cannot be marked skippable, so `skip()` can never
// be asked to guess what the outcomes were.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/handle.h>
#include <cy/core/values/name.h>
#include <cy/sequencing/adapters.h>
#include <cy/sequencing/dispatch.h>
#include <cy/sequencing/program.h>

namespace cy::sequencing {

/// What the service is scoped to. Carried rather than inferred: a world service and an editor
/// preview service differ in what they are allowed to do, and the difference is checked.
enum class PlaybackScope : u8 {
    World = 0,
    Session,
    LocalPlayer,
    EditorPreview,
    Count,
};

[[nodiscard]] const char* playback_scope_name(PlaybackScope scope) noexcept;

/// The instance lifecycle, complete. "Instance lifecycle SHALL be explicit."
enum class InstanceState : u8 {
    Created = 0,
    Preparing,
    Ready,
    Playing,
    Paused,
    Seeking,
    Completing,
    Completed,
    Stopped,
    Failed,
    Count,
};

[[nodiscard]] const char* instance_state_name(InstanceState state) noexcept;

/// Why an instance failed. "Failure is diagnosable": "the reason SHALL be structured — unresolved
/// binding, unready asset, incompatible clock, or unsupported track."
enum class FailureReason : u8 {
    None = 0,
    UnresolvedBinding,
    UnreadyAsset,
    IncompatibleClock,
    UnsupportedTrack,
    Count,
};

[[nodiscard]] const char* failure_reason_name(FailureReason reason) noexcept;

enum class PlaybackMode : u8 {
    Once = 0,
    Loop,
    PingPong,
    /// Play once and hold the final instant, still evaluating. A held shot.
    Hold,
    /// Advance only when the host says so. The editor scrubber's mode.
    Manual,
    Count,
};

[[nodiscard]] const char* playback_mode_name(PlaybackMode mode) noexcept;

/// What a seek does with the events it crossed.
enum class SeekMode : u8 {
    /// An author scrubbing. Irreversible events are suppressed.
    Preview = 0,
    /// A game seeking at run time. Everything crossed fires.
    Runtime,
    /// A replay seeking. Nothing fires: the log already carries what happened.
    Replay,
    /// Catching up. Idempotent and confirmed-only outcomes are APPLIED as state — flagged
    /// `applied_by_skip` — and reversible presentation events are not, because a shake that
    /// happened four minutes ago should not happen now.
    Reconstruct,
    Count,
};

[[nodiscard]] const char* seek_mode_name(SeekMode mode) noexcept;

/// What a project chooses when a sequence is played before its content is resident. "A project
/// SHALL be able to choose between failing if not ready, starting with fallbacks, or waiting for
/// required content."
enum class PreparePolicy : u8 {
    FailIfNotReady = 0,
    StartWithFallbacks,
    WaitForContent,
    Count,
};

struct ParameterOverride {
    u32 stable_id = 0;
    ChannelValue value;
};

CY_HANDLE_TAG(SequenceInstance);
using InstanceId = Handle<SequenceInstanceTag>;

struct PlayRequest {
    Span<const BindingResolution> bindings;
    Span<const ParameterOverride> parameters;
    PlaybackMode mode = PlaybackMode::Once;
    PlayRate rate = PlayRate::normal();
    SequenceTime start;
    PreparePolicy prepare = PreparePolicy::StartWithFallbacks;
    /// The exclusive group this instance takes part in. Starting one instance in a group suspends
    /// the others in it — "Exclusive groups SHALL be supported".
    Name exclusive_group;
    /// Arbitration priority for the whole instance, added to each section's own.
    i32 priority = 0;
};

/// What a host may ask about one instance. A value: nothing here is a pointer into the player.
struct InstanceStatus {
    InstanceState state = InstanceState::Created;
    FailureReason failure = FailureReason::None;
    SequenceTime time;
    PlaybackMode mode = PlaybackMode::Once;
    PlayRate rate = PlayRate::normal();
    u32 active_sections = 0;
    /// The binding or asset the failure names, so "failure is diagnosable" is a value rather than a
    /// message: an unresolved required binding puts its stable id here.
    u32 failure_detail = 0;
    u32 program_generation = 0;
    bool suspended = false;
};

/// One asset the plan wants resident, published ahead of need.
struct PreloadRequest {
    u64 asset = 0;
    /// Ticks from the instance's current time until the asset is required. Negative means it is
    /// required already, which is a preload MISS unless the host has it.
    i64 lead_ticks = 0;
    f32 priority = 1.0F;
    InstanceId instance;
};

/// "Preload misses SHALL be reported, naming the asset, the deadline, and what was substituted."
struct PreloadMiss {
    u64 asset = 0;
    i64 deadline_ticks = 0;
    u64 substituted = 0;
    InstanceId instance;
};

/// What a playing sequence tells the streaming layer about its FUTURE. The differentiator: no
/// reactive predictor can know where the camera will be in six seconds, and this does.
///
/// Identities and deadlines rather than world bounds, because `src/sequencing/` names no world —
/// the camera bridge turns these into `cy::camera` cuts and the camera's own streaming source
/// carries them to residency. See README.md, "what this module may not name".
struct FutureShot {
    InstanceId instance;
    u64 rig = 0;
    u64 framing_target = 0;
    /// Ticks from now until the shot begins.
    i64 lead_ticks = 0;
    bool cut = false;
    f32 importance = 1.0F;
};

/// The playback service.
///
/// NOT THREAD-SAFE, and for the reason `CameraServer` gives: it is advanced where the frame is
/// driven and read where batches are consumed, which is one thread. Parallel EVALUATION is a
/// separate question and is answered in dispatch.h — the ordering key is the identity, so
/// evaluating instances on workers and merging by `stable_less()` produces the same batch this
/// class does.
class SequencePlayer {
public:
    SequencePlayer(Allocator& allocator, const AdapterRegistry& registry, PlaybackScope scope,
                   u32 scope_id) noexcept;
    ~SequencePlayer();

    SequencePlayer(const SequencePlayer&) = delete;
    SequencePlayer& operator=(const SequencePlayer&) = delete;

    [[nodiscard]] PlaybackScope scope() const noexcept { return scope_; }
    [[nodiscard]] u32 scope_id() const noexcept { return scope_id_; }

    /// Create an instance against a program the CALLER owns. Programs are immutable and shared;
    /// a hundred instances of one cinematic hold one of these pointers each and nothing more.
    [[nodiscard]] Expected<InstanceId, Error> create(const Program& program,
                                                     const PlayRequest& request) noexcept;

    /// Prepare without playing: the guarantee a critical cinematic needs. Publishes the preload
    /// plan and moves to `Ready` when every asset the host has confirmed is resident.
    [[nodiscard]] Status prepare(InstanceId id) noexcept;
    /// The host confirms an asset is resident. What moves a `Preparing` instance to `Ready`.
    [[nodiscard]] Status asset_ready(InstanceId id, u64 asset) noexcept;
    /// The host could not deliver an asset in time and says what it used instead.
    [[nodiscard]] Status report_preload_miss(InstanceId id, u64 asset, u64 substituted) noexcept;

    [[nodiscard]] Status play(InstanceId id) noexcept;
    [[nodiscard]] Status pause(InstanceId id) noexcept;
    [[nodiscard]] Status resume(InstanceId id) noexcept;
    /// Stop and apply completion policies — "Interruption is not a special case".
    [[nodiscard]] Status stop(InstanceId id, DispatchBatches& out) noexcept;
    [[nodiscard]] Status set_rate(InstanceId id, PlayRate rate) noexcept;
    [[nodiscard]] Status set_parameter(InstanceId id, u32 stable_id,
                                       const ChannelValue& value) noexcept;
    [[nodiscard]] Status jump_to_marker(InstanceId id, Name marker, SeekMode mode,
                                        DispatchBatches& out) noexcept;
    [[nodiscard]] Status seek(InstanceId id, SequenceTime time, SeekMode mode,
                              DispatchBatches& out) noexcept;
    /// Fast-forward is DISTINCT from skipping: the sequence keeps playing, faster, and adapters may
    /// simplify while it is active.
    [[nodiscard]] Status set_fast_forward(InstanceId id, bool active) noexcept;
    [[nodiscard]] bool fast_forwarding(InstanceId id) const noexcept;

    /// Apply what the skip skips, then complete. See the header comment.
    [[nodiscard]] Status skip(InstanceId id, DispatchBatches& out) noexcept;

    /// Advance every playing instance by `delta_nanoseconds` and fill `out`.
    ///
    /// `out` is NOT cleared: a host advancing two services into one batch is the ordinary case in
    /// split screen, and clearing here would make the second service erase the first.
    [[nodiscard]] Status advance(i64 delta_nanoseconds, DispatchBatches& out) noexcept;
    /// The `Manual` mode's advance: a host stepping a scrubber supplies the instant itself.
    [[nodiscard]] Status step_to(InstanceId id, SequenceTime time, DispatchBatches& out) noexcept;

    [[nodiscard]] Expected<InstanceStatus, Error> status(InstanceId id) const noexcept;
    [[nodiscard]] u32 live_instances() const noexcept;

    /// Publish the preload plan for everything playing, out to `horizon`. Cleared and refilled.
    [[nodiscard]] Status publish_preload(SequenceTime horizon,
                                         Array<PreloadRequest>& out) const noexcept;
    /// The upcoming shots, out to `horizon`, with anticipated cuts flagged.
    [[nodiscard]] Status publish_future_shots(SequenceTime horizon,
                                              Array<FutureShot>& out) const noexcept;
    [[nodiscard]] Span<const PreloadMiss> preload_misses() const noexcept { return misses_.span(); }

    /// A new program generation for an instance already running — hot reload. `policy` is what the
    /// specification calls the declared policy; `ContinueAtEquivalentTime` is refused with a reason
    /// when the structural change makes it impossible, which is "the editor SHALL explain why
    /// rather than silently restarting".
    enum class ReloadPolicy : u8 { ContinueAtEquivalentTime = 0, Restart, KeepPrevious, Stop };
    [[nodiscard]] Status reload(InstanceId id, const Program& program, ReloadPolicy policy,
                                DispatchBatches& out) noexcept;

private:
    struct CaptureEntry {
        u64 target = 0;
        ResolvedProperty property;
        ChannelValue value;
        u32 segment = 0;
    };

    struct Instance {
        explicit Instance(Allocator& allocator) noexcept
            : bindings(allocator),
              parameters(allocator),
              active(allocator),
              previous(allocator),
              captures(allocator),
              spawned(allocator),
              pending_assets(allocator) {}

        const Program* program = nullptr;
        u32 generation = 0;
        InstanceState state = InstanceState::Created;
        FailureReason failure = FailureReason::None;
        PlaybackMode mode = PlaybackMode::Once;
        PlayRate rate = PlayRate::normal();
        PreparePolicy prepare_policy = PreparePolicy::StartWithFallbacks;
        Name exclusive_group;
        i32 priority = 0;
        bool suspended = false;
        bool fast_forward = false;
        bool reverse_leg = false;  ///< PingPong's second leg.
        i64 ticks = 0;
        TimeAccumulator accumulator;
        u32 id = 0;
        u32 failure_detail = 0;
        /// Assets the host has not yet confirmed resident. Empty means `Ready`.
        u32 slot = 0;

        Array<BindingResolution> bindings;
        Array<ParameterOverride> parameters;
        Array<u32> active;
        Array<u32> previous;
        Array<CaptureEntry> captures;
        /// The segments whose spawn actually happened, so that what is cleaned up on completion is
        /// what was spawned rather than everything the program COULD have spawned.
        Array<u32> spawned;
        Array<u64> pending_assets;
    };

    [[nodiscard]] Instance* find(InstanceId instance) noexcept;
    [[nodiscard]] const Instance* find(InstanceId instance) const noexcept;
    [[nodiscard]] static u64 target_for(const Instance& instance, u32 binding) noexcept;
    [[nodiscard]] static bool binding_resolved(const Instance& instance, u32 binding) noexcept;

    /// Move one instance to `ticks`, emitting everything the move implies. The single path every
    /// public mover funnels through — playing, seeking, stepping and skipping — which is what makes
    /// "the same time reached by playing, seeking, stepping, or replaying SHALL be the same
    /// instant" a property of the code rather than a claim about it.
    [[nodiscard]] Status move_to(Instance& instance, i64 target_ticks, SeekMode mode, bool crossing,
                                 DispatchBatches& out) noexcept;
    [[nodiscard]] Status advance_one(Instance& instance, i64 delta_ticks,
                                     DispatchBatches& out) noexcept;
    [[nodiscard]] static Status emit_active(Instance& instance, DispatchBatches& out) noexcept;
    [[nodiscard]] Status capture_section(Instance& instance, u32 segment_index) noexcept;
    [[nodiscard]] Status restore_section(Instance& instance, u32 segment_index,
                                         DispatchBatches& out) noexcept;
    [[nodiscard]] static Status apply_outcome(Instance& instance, const RequiredOutcome& outcome,
                                              DispatchBatches& out) noexcept;
    /// `from` is the instant the instance was at before the move: a section is ENTERED when the
    /// move crosses into its range, which is not the same question as whether it is in the active
    /// set. A section with a pre-roll is in the active set long before it starts, and the first
    /// version of this code asked only the second question — so the pre-roll fired and the section
    /// itself never did. The capture found it; `tests/test_playback.cpp` keeps it found.
    [[nodiscard]] Status enter_sections(Instance& instance, i64 from,
                                        DispatchBatches& out) noexcept;
    [[nodiscard]] Status leave_sections(Instance& instance, DispatchBatches& out) noexcept;
    [[nodiscard]] static Status emit_events(Instance& instance, i64 from, i64 to, SeekMode mode,
                                            DispatchBatches& out) noexcept;
    [[nodiscard]] Status complete(Instance& instance, DispatchBatches& out) noexcept;
    void suspend_group(Name group, u32 except) noexcept;

    Allocator* allocator_;
    const AdapterRegistry* registry_;
    PlaybackScope scope_;
    u32 scope_id_ = 0;
    u32 next_id_ = 1;
    Array<Instance*> instances_;
    Array<u32> generations_;
    Array<PreloadMiss> misses_;
    Array<u32> scratch_;
};

}  // namespace cy::sequencing
