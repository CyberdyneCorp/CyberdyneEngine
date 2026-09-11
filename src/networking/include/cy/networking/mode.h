#pragma once
// The three network modes, their prerequisites, and the compatibility scope. M9 task 4.2.
//
// ================================================================================================
// A MODE IS AN ARCHITECTURE, AND ITS PREREQUISITES ARE CHECKED RATHER THAN ASSUMED
// ================================================================================================
//
// `networking-and-replication` — "Network modes": three modes, "genuinely different architectures,
// not quality settings", and "the engine SHALL verify at startup and at session start that the
// declared mode's prerequisites hold, failing with a clear diagnostic rather than desyncing later".
//
// The verification is not this module's own invention. `simulation-and-determinism` already has the
// registry that knows what each subsystem can hold to and the refusal that names it —
// `determinism::DeterminismConfiguration::require()`, M9 task 2.2 — and a second mechanism here
// would be a second thing to keep in step. So `verify_mode()` maps the mode to the determinism
// profile it needs, asks that registry, and adds only what is genuinely networking's:
//
//   * the **excluded set** — a subsystem the specification names non-deterministic (VFX, non-pinned
//     inference, adaptive budget controllers) that has nevertheless been declared authoritative;
//   * the **session declarations** a determinism registry cannot see — a fixed timestep, seeded
//     random state carried in simulation state, deterministic system ordering;
//   * the **compatibility scope**, below, which is the whole of why lockstep works at all.
//
// ================================================================================================
// LOCKSTEP IS ONLY AS TRUE AS THE SPIKE SAYS IT IS
// ================================================================================================
//
// `networking-and-replication` states it plainly and this header does not soften it:
// **cross-platform lockstep is not supported.** M9's spike (design.md §1) measured the engine's own
// primitives bit-identical between clang 18 and GCC 13 at all four optimisation levels — on ONE
// machine, ONE architecture and ONE libm. Nothing in this tree has compared a state hash between
// two architectures, so nothing here claims one converges with another.
//
// What follows from that is `CompatibilityScope`: a lockstep session declares platform,
// architecture and binary build identity, and `join_verdict()` refuses a peer that differs in any
// of them, **naming which**. That is the honest form of the guarantee — not "lockstep is
// cross-platform", but "lockstep participants are the same build on the same architecture, and the
// engine checks it at join time rather than discovering it as a desync at minute forty".

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/profile.h>
#include <cy/core/memory/array.h>

namespace cy::net {

/// `networking-and-replication`'s table, in its own order.
enum class NetworkMode : u8 {
    /// Component state from the authority to interested peers. Most games.
    SnapshotAuthoritative = 0,
    /// Inputs plus authoritative state, with client-side snapshot, rollback and replay.
    Rollback,
    /// Commands only; every peer simulates the full world.
    Lockstep,
};

inline constexpr u32 kNetworkModeCount = 3;

const char* network_mode_name(NetworkMode mode) noexcept;

/// The determinism profile a mode cannot do without.
///
/// Written as a function rather than a table member so that the mapping is one expression a reader
/// can check against the specification's table in one glance:
///
///   SnapshotAuthoritative  the authority decides; a client's own arithmetic may differ from the
///                          server's without the session being wrong, because the server's answer
///                          overwrites it. What must hold is that the session is reconstructible.
///   Rollback               "client simulation must be deterministic and snapshot-restorable" — the
///                          same binary re-simulating the same inputs must reach the same state, or
///                          reconciliation corrects into a different wrong answer every time.
///   Lockstep               "all participating peers must produce bit-identical simulation", from
///                          commands alone — **on one platform**. See below.
///
/// **WHY LOCKSTEP ASKS FOR `SamePlatform` AND NOT FOR `DeterminismProfile::Lockstep`.** This is the
/// milestone's brief as a line of code, and it is a deliberate answer rather than an oversight.
///
/// `determinism::guarantees_of(DeterminismProfile::Lockstep)` carries
/// `cross_platform_reproducible`, and `DeterminismConfiguration::require()` refuses that obligation
/// in this tree — there is no deterministic math module, so every `CrossPlatform` or `Lockstep`
/// session is rejected with `DeterministicMathMissing`, which is the honest answer and
/// `simulation-and-determinism`'s own: "The engine SHALL NOT claim that arbitrary floating-point
/// code produces identical results across architectures, compilers, or vector widths."
///
/// `networking-and-replication` does not ask for that guarantee either, and says so in capitals:
/// "**Cross-platform lockstep SHALL NOT be supported.** The physics backend does not guarantee
/// cross-platform determinism, and the engine provides no fixed-point or soft-float simulation
/// path. A lockstep session SHALL therefore declare its **compatibility scope** — platform,
/// architecture, and binary build identifier — and the engine SHALL verify that every participant
/// matches before the session starts."
///
/// So the two halves of lockstep's contract are asked of the two things that can answer them.
/// `SamePlatform` — the same binary on the same architecture reproduces authoritative state
/// bit-exactly — is asked of the determinism registry, and it is exactly what M9's spike measured
/// (design.md §1.1: the engine's own primitives bit-identical across two compilers and four
/// optimisation levels, on one architecture). "From commands alone" is asked of *this* module, by
/// `replicates_state(Lockstep) == false`, and "every participant matches" is asked of
/// `join_verdict()`. Asking the determinism registry for a guarantee the networking specification
/// declines to make would make lockstep unconfigurable while claiming more than anyone measured.
[[nodiscard]] constexpr determinism::DeterminismProfile profile_required_by(
    NetworkMode mode) noexcept {
    switch (mode) {
        case NetworkMode::SnapshotAuthoritative:
            return determinism::DeterminismProfile::ReplayStable;
        case NetworkMode::Rollback:
        case NetworkMode::Lockstep:
            return determinism::DeterminismProfile::SamePlatform;
    }
    return determinism::DeterminismProfile::None;
}

/// Does the mode replicate component state at all? Lockstep does not — that is the whole of its
/// bandwidth argument, and `scheduler.h` refuses to schedule state for a lockstep session because
/// of this predicate rather than because of a comment.
[[nodiscard]] constexpr bool replicates_state(NetworkMode mode) noexcept {
    return mode != NetworkMode::Lockstep;
}

/// Does the mode send participant commands on the wire?
[[nodiscard]] constexpr bool replicates_commands(NetworkMode mode) noexcept {
    return mode != NetworkMode::SnapshotAuthoritative;
}

// --- What a session declares about itself -------------------------------------------------------

/// The prerequisites a determinism registry cannot see, because they are properties of how the
/// session is driven rather than of which subsystems are linked.
///
/// Every one of them defaults to **false**, and that is deliberate: a session that declares nothing
/// is refused for lockstep rather than waved through. A default of true would make
/// `verify_mode()` a check that passes on an empty struct, which is the defect class this
/// project's gates have found eighteen times.
struct SessionDeclarations {
    /// A fixed simulation timestep. `simulation-and-determinism`'s first requirement of any
    /// reproducible session.
    bool fixed_timestep = false;
    /// Random state is seeded and carried in simulation state, not drawn from a global.
    bool seeded_random_in_state = false;
    /// ECS system ordering is deterministic.
    bool deterministic_system_order = false;
    /// Animation root motion is deterministic.
    bool deterministic_root_motion = false;
    /// Every participating peer can restore a snapshot of the rollback set.
    bool snapshot_restorable = false;
};

/// A subsystem the session has excluded from authoritative simulation, with why.
///
/// `networking-and-replication`: lockstep requires "exclusion of every subsystem declared
/// non-deterministic (VFX, non-pinned ML inference, adaptive budget controllers)". A session names
/// them here; `verify_mode()` refuses when one of them is *also* declared authoritative in the
/// determinism registry, which is the contradiction that would otherwise be discovered as a desync.
struct ExcludedSubsystem {
    /// Matched against `determinism::SubsystemDeterminism::name` by exact spelling.
    const char* name = "";
    /// Required. An exclusion with no argument behind it is how a list of them grows silently.
    const char* reason = "";
};

// --- The compatibility scope --------------------------------------------------------------------

/// What a lockstep participant must match. See the header comment.
///
/// The strings are literals or storage outliving the session; they are compared by content, never
/// by pointer, because two builds of the same engine hold their own copies of the same spelling.
struct CompatibilityScope {
    const char* platform = "";
    const char* architecture = "";
    /// The binary's own identity — what `replay::CompatibilityManifest::engine_build` carries.
    u64 build_id = 0;
    /// The identity of the compiled replication schema set (`schema.h`). Verified at connection for
    /// every mode, not only lockstep: a peer with a different schema set misinterprets bytes rather
    /// than failing.
    u64 schema_set_hash = 0;
    /// The profile the session declared. Two peers that agree about everything else and disagree
    /// about this are two different claims about the same wire.
    determinism::DeterminismProfile profile = determinism::DeterminismProfile::None;
};

/// Why a peer was refused at join time. One enumerator per thing that can differ, because "you
/// cannot join this session" is a support ticket nobody can answer.
enum class JoinRefusal : u8 {
    None = 0,
    PlatformMismatch,
    ArchitectureMismatch,
    BuildMismatch,
    SchemaSetMismatch,
    ProfileMismatch,
};

const char* join_refusal_name(JoinRefusal refusal) noexcept;

/// May a peer declaring `candidate` join a session declaring `host` in `mode`?
///
/// **The rule is stricter for lockstep, and that is the point.** Under `SnapshotAuthoritative` the
/// authority decides the state, so a peer on another platform is legitimate and only the schema set
/// must match. Under `Rollback` and `Lockstep` the peer re-simulates, so platform, architecture and
/// build must match too — the specification's "cross-platform lockstep SHALL NOT be supported", as
/// a function rather than as a sentence in a document.
[[nodiscard]] JoinRefusal join_verdict(NetworkMode mode, const CompatibilityScope& host,
                                       const CompatibilityScope& candidate) noexcept;

// --- The refusal --------------------------------------------------------------------------------

enum class ModeRefusal : u8 {
    None = 0,
    /// The determinism registry refused the profile this mode needs. `determinism` carries which.
    DeterminismProfile,
    /// A session declaration the mode requires was not made.
    SessionDeclaration,
    /// A subsystem the session excluded is declared authoritative.
    ExcludedSubsystemIsAuthoritative,
    /// An exclusion was declared with no reason.
    ExclusionWithoutReason,
    /// The session's own compatibility scope is incomplete — an empty platform, a zero build id.
    IncompleteScope,
};

const char* mode_refusal_name(ModeRefusal refusal) noexcept;

/// One refusal, carrying everything the diagnostic prints and nothing it has to look up.
struct ModeRejection {
    ModeRefusal tag = ModeRefusal::None;
    NetworkMode mode = NetworkMode::SnapshotAuthoritative;
    /// The subsystem named, or "" when the refusal is about the session rather than a subsystem.
    const char* subsystem = "";
    /// What was missing, spelled: "a fixed simulation timestep", "cross-platform reproducibility".
    /// Never null.
    const char* requirement = "";
    /// The determinism registry's own rejection, when `tag` is `DeterminismProfile`. Carried whole
    /// so the caller prints one diagnostic rather than two.
    determinism::ProfileRejection determinism{};
};

/// What one verification examined. Reported on acceptance as well as on refusal, because a check
/// that looked at nothing and a check that found nothing must not read alike.
struct ModeReport {
    NetworkMode mode = NetworkMode::SnapshotAuthoritative;
    determinism::DeterminismProfile profile_required = determinism::DeterminismProfile::None;
    u32 subsystems_examined = 0;
    u32 authoritative_subsystems = 0;
    u32 exclusions_examined = 0;
    /// Declarations the mode required, and how many of them were made. Equal on acceptance; the
    /// pair is reported so that "this mode required nothing" is visible as 0/0.
    u32 declarations_required = 0;
    u32 declarations_met = 0;
};

/// **The startup refusal.** Called when a session is created, before a tick runs and before a peer
/// connects.
///
/// `configuration` is finalised by this call if it has not been, for the reason
/// `DeterminismConfiguration::require()` gives: a refusal validated against an unsorted list would
/// name a different subsystem on a different run.
[[nodiscard]] Expected<ModeReport, ModeRejection> verify_mode(
    NetworkMode mode, determinism::DeterminismConfiguration& configuration,
    const determinism::BuildConfiguration& build, const SessionDeclarations& declarations,
    Span<const ExcludedSubsystem> excluded, const CompatibilityScope& scope) noexcept;

}  // namespace cy::net
