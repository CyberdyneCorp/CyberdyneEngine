#pragma once
// Lockstep frames and resynchronisation. M9 task 3.3.
//
// `replay-and-rollback` — "Lockstep frames": "for each tick, the set of participants' commands and
// a reference to a previously agreed state hash"; a configurable **input delay**, whose change
// "SHALL be coordinated and deterministic"; a participant missing its deadline handled by a
// **declared policy** whose "decision SHALL be authoritative and recorded, so replay reproduces
// it"; and state hashes "exchanged periodically at a configured frequency" so that "divergence
// SHALL be detected rather than accumulating silently".
//
// ================================================================================================
// THE DECISION IS RECORDED IN THE ONE LOG, AS AN `ExternalResult`, AND THAT IS NOT A STRETCH
// ================================================================================================
//
// "The decision SHALL be authoritative and recorded, so replay reproduces it." A replay that
// re-derived the decision would have to reproduce *the network* — whether a packet arrived by a
// deadline — which is the definition of something the simulation did not compute. That is what
// `RecordKind::ExternalResult` is: "something authoritative the simulation consumed and did not
// compute". So the deadline verdict goes into the same log as everything else, under a source id
// derived from the participant, and no reader of the log needs a sixth record type to see it.
//
// The *consequences* of the decision are ordinary commands. `RepeatPrevious`, `Predict` and
// `SubstituteAgent` all produce a command the simulation consumed, and a command the simulation
// consumed is recorded by `CommandStream`'s seam like any other — so a replay reproduces the
// substituted input by replaying it, not by re-running the substitution. `TreatAsNoCommand`,
// `PauseSession` and `RemoveParticipant` produce no command and are reproduced from the decision
// record alone.
//
// ================================================================================================
// CHANGING THE INPUT DELAY IS A SCHEDULED CHANGE, NOT A SETTER
// ================================================================================================
//
// "Changing the delay SHALL be **coordinated and deterministic**." A setter would take effect
// whenever each peer happened to call it, which is a desync with a stopwatch in it. So a change is
// scheduled *at a tick*: every peer applies the same delay at the same tick because every peer was
// told the same (tick, delay) pair, and `input_delay(tick)` is a pure function of the schedule.
// Refusing a change scheduled for a tick already past is the other half — a peer that received the
// message late must not apply it retroactively and diverge.
//
// ================================================================================================
// WHAT THIS FILE IS NOT
// ================================================================================================
//
// **No transport.** Nothing here opens a socket, and `CY_NETWORKING` does not gate it: a lockstep
// frame is a fact about a session's inputs whether they arrived over a wire, from four local
// producers or from a replay. `src/networking/` drives this; it does not reimplement it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/memory/array.h>
#include <cy/replay/log.h>
#include <cy/replay/record.h>

namespace cy::replay {

/// What to do about a participant whose commands did not arrive by the deadline. The six the
/// specification names, and no seventh: a policy the engine invented would be a policy no session
/// declared.
enum class LatePolicy : u8 {
    /// The participant issued nothing this tick.
    TreatAsNoCommand = 0,
    /// Re-issue whatever it issued last. Cheap and wrong in exactly one way — a held key looks the
    /// same as a lost packet — which is why it is a choice rather than the default.
    RepeatPrevious,
    /// Predict, and reconcile when the real input arrives.
    Predict,
    /// Stop the world until it arrives.
    PauseSession,
    /// An agent plays for it.
    SubstituteAgent,
    /// It is out of the session.
    RemoveParticipant,
};

const char* late_policy_name(LatePolicy policy) noexcept;

/// What a session does when peers have diverged. "Policy — whether to resynchronise, disconnect, or
/// continue — SHALL belong to the session's rules, and **the mechanism SHALL support all three**."
enum class DesyncPolicy : u8 {
    /// Request an authoritative snapshot, restore it, resume. Increments the epoch.
    Resynchronise = 0,
    /// End the peer's participation.
    Disconnect,
    /// Carry on regardless. Legitimate for a spectator and for a session whose divergence is
    /// cosmetic; recorded either way, so nobody has to guess later whether it was noticed.
    Continue,
};

const char* desync_policy_name(DesyncPolicy policy) noexcept;

/// The source id a deadline decision is recorded under, one per participant.
///
/// Derived rather than registered so a session that gains a participant mid-match does not have to
/// declare a source for it first — and mixed with a constant so it cannot collide with a genuine
/// external source's identity by being a bare participant handle.
[[nodiscard]] u64 late_decision_source(u64 participant) noexcept;

/// One tick's lockstep frame: the participants' commands, and a reference to an agreed state hash.
struct LockstepFrame {
    u64 tick = 0;
    determinism::Epoch epoch;

    /// Participants expected to contribute at this tick.
    u32 participants = 0;
    /// Participants whose commands arrived by the deadline.
    u32 arrived = 0;
    /// Participants the declared policy was applied to.
    u32 late = 0;

    /// **The reference to a previously agreed state hash.** Not the hash for this tick — this tick
    /// has not been simulated yet — but the last tick at which every peer's hash agreed, which is
    /// what makes a frame carry the evidence that the session was in step when it was built.
    u64 agreed_hash_tick = 0;
    u64 agreed_hash = 0;
    bool has_agreement = false;

    /// The frame could not be closed because the policy is `PauseSession` and somebody is late. The
    /// session does not advance; nothing is recorded as consumed, because nothing was.
    bool paused = false;
};

/// What a peer's hash said.
enum class DesyncVerdict : u8 {
    /// The hashes agree. The agreement point advances.
    Agreed = 0,
    /// They disagree. **Detected at the exchange, which is the requirement**: "divergence SHALL be
    /// detected rather than accumulating silently".
    Diverged,
    /// Nothing to compare: this peer has no local hash recorded at that tick.
    NotCompared,
};

const char* desync_verdict_name(DesyncVerdict verdict) noexcept;

struct LockstepConfiguration {
    /// Commands issued now are for `now + input_delay_ticks`. Zero is legal and means a session
    /// with no distribution latency budget at all — a local hot-seat match.
    u32 input_delay_ticks = 2;
    /// Exchange a state hash every this many ticks. Zero disables the exchange, which is a session
    /// that has chosen not to detect divergence and should have to say so.
    u32 hash_exchange_interval = 15;
    LatePolicy late_policy = LatePolicy::TreatAsNoCommand;
    DesyncPolicy desync_policy = DesyncPolicy::Resynchronise;
};

/// What a resynchronisation did.
struct ResyncReport {
    DesyncPolicy policy = DesyncPolicy::Resynchronise;
    bool performed = false;
    u64 at_tick = 0;
    /// The epoch entered. Zero-valued when the policy was not `Resynchronise`, because the timeline
    /// was not reset.
    determinism::Epoch epoch;
    /// The hash adopted from the authority.
    u64 authoritative_hash = 0;
};

/// The session's lockstep half. Holds no transport and no world.
class LockstepSession {
public:
    LockstepSession(Allocator& allocator, RecordLog& log,
                    const LockstepConfiguration& configuration) noexcept;

    LockstepSession(const LockstepSession&) = delete;
    LockstepSession& operator=(const LockstepSession&) = delete;

    [[nodiscard]] const LockstepConfiguration& configuration() const noexcept { return config_; }

    /// Refuses a duplicate. A participant counted twice would make every frame report a deadline it
    /// did not miss.
    [[nodiscard]] Status add_participant(u64 participant) noexcept;
    [[nodiscard]] Status remove_participant(u64 participant) noexcept;
    /// Participants still IN the session. A removed participant keeps its entry — it carries how
    /// many deadlines it missed, which is the explanation for why it left — and is not counted
    /// here.
    [[nodiscard]] u32 participant_count() const noexcept;
    /// Every entry, removed ones included, for a report that wants to say who left and why.
    [[nodiscard]] u32 entry_count() const noexcept {
        return static_cast<u32>(participants_.size());
    }
    [[nodiscard]] u64 participant_at(u32 index) const noexcept {
        return participants_[index].participant;
    }
    [[nodiscard]] bool participant_removed(u32 index) const noexcept {
        return participants_[index].removed;
    }
    [[nodiscard]] u32 participant_late_count(u32 index) const noexcept {
        return participants_[index].late_count;
    }

    // --- Input delay -----------------------------------------------------------------------------

    /// The tick a command issued at `now` is for.
    [[nodiscard]] u64 issue_tick(u64 now) const noexcept { return now + input_delay(now); }

    /// The delay in force at `tick`. A pure function of the schedule, so two peers given the same
    /// schedule compute the same number without exchanging anything further.
    [[nodiscard]] u32 input_delay(u64 tick) const noexcept;

    /// Schedule a change. Refuses a tick at or before `now_tick` — a change applied retroactively
    /// is a desync — and refuses a tick already scheduled.
    [[nodiscard]] Status schedule_input_delay(u32 delay, u64 effective_tick, u64 now_tick) noexcept;
    [[nodiscard]] u32 scheduled_changes() const noexcept {
        return static_cast<u32>(delays_.size());
    }

    // --- Frames ----------------------------------------------------------------------------------

    /// A participant's commands for `tick` arrived.
    void arrived(u64 participant, u64 tick) noexcept;

    /// Close the frame for `tick`, applying the declared policy to every participant that did not
    /// arrive and **recording each decision in the log**.
    ///
    /// Refuses a tick before the last frame closed: frames are closed in order, and a session that
    /// closed tick 40 after tick 41 would record its decisions out of order.
    [[nodiscard]] Expected<LockstepFrame, Error> close_frame(u64 tick,
                                                             determinism::Epoch epoch) noexcept;

    [[nodiscard]] u32 frames_closed() const noexcept { return frames_closed_; }
    [[nodiscard]] u32 late_decisions() const noexcept { return late_decisions_; }

    // --- Hash exchange ---------------------------------------------------------------------------

    /// Is an exchange due at `tick`? False for every tick when the interval is zero.
    [[nodiscard]] bool exchange_due(u64 tick) const noexcept;

    /// Record this peer's own authoritative hash for `tick` and append it to the log.
    [[nodiscard]] Status publish_hash(u64 tick, u64 hash, determinism::Epoch epoch) noexcept;

    /// Compare a peer's hash against this peer's own at the same tick.
    [[nodiscard]] DesyncVerdict observe_peer_hash(u64 participant, u64 tick, u64 hash) noexcept;

    /// The last tick at which a peer's hash was compared and agreed.
    [[nodiscard]] bool agreement(u64& tick, u64& hash) const noexcept;
    [[nodiscard]] bool diverged() const noexcept { return diverged_; }
    [[nodiscard]] u64 first_diverging_tick() const noexcept { return first_diverging_; }
    /// The peer that reported the disagreeing hash. Zero when none has.
    [[nodiscard]] u64 diverging_participant() const noexcept { return diverging_participant_; }

    // --- Resynchronisation
    // -------------------------------------------------------------------------

    /// Apply the declared desync policy at `tick`.
    ///
    /// `Resynchronise` advances the epoch (`EpochReason::WorldReload` — the world is being replaced
    /// by the authority's, not restored from a capture this peer took) and records the fact in the
    /// log, so "replays and logs show that the timeline was reset". `Disconnect` and `Continue`
    /// record the decision and leave the epoch alone. **All three are recorded**, because a session
    /// that decided to carry on and one that never noticed read identically otherwise.
    [[nodiscard]] Status resynchronise(determinism::EpochCounter& epochs, u64 tick,
                                       u64 authoritative_hash, ResyncReport& out) noexcept;

    [[nodiscard]] u32 resynchronisations() const noexcept { return resyncs_; }

private:
    struct Participant {
        u64 participant = 0;
        /// The last tick this participant's commands arrived for. `kNever` until the first.
        u64 last_arrival = kNever;
        u32 late_count = 0;
        bool removed = false;

        static constexpr u64 kNever = ~0ULL;
    };

    struct DelayChange {
        u64 effective_tick = 0;
        u32 delay = 0;
    };

    struct PeerHash {
        u64 tick = 0;
        u64 hash = 0;
    };

    [[nodiscard]] Participant* find(u64 participant) noexcept;
    [[nodiscard]] Status record_decision(u64 participant, LatePolicy policy, u64 tick,
                                         determinism::Epoch epoch) noexcept;

    Array<Participant> participants_;
    Array<DelayChange> delays_;
    /// This peer's own hashes, for the comparison. Bounded by `kHashHistory` because a lockstep
    /// session runs for hours and nobody compares a hash from an hour ago.
    Array<PeerHash> own_hashes_;
    RecordLog* log_;
    LockstepConfiguration config_;
    u64 last_frame_tick_ = 0;
    u64 agreed_tick_ = 0;
    u64 agreed_hash_ = 0;
    u64 first_diverging_ = 0;
    u64 diverging_participant_ = 0;
    u32 frames_closed_ = 0;
    u32 late_decisions_ = 0;
    u32 resyncs_ = 0;
    bool any_frame_ = false;
    bool has_agreement_ = false;
    bool diverged_ = false;

    /// How many of this peer's own hashes are kept for the comparison.
    static constexpr u32 kHashHistory = 64;
};

}  // namespace cy::replay
