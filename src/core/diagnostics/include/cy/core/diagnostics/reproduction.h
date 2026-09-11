#pragma once
// The reproduction artefact: a bug report that carries its own reproduction.
//
// `diagnostics-profiling-and-crash` — "Reproduction artefacts":
//
//   The engine SHALL support producing a **reproduction artefact** linking a crash or a reported
//   defect to the replay slice, checkpoints, external results, and state hashes recorded by
//   `replay-and-rollback`.
//
//   Given such an artefact, the engine SHALL be able to load it and advance to shortly before the
//   failure, so that a bug report carries its own reproduction.
//
//   The reproduction artefact SHALL be separate from the crash artefact and linked to it, since a
//   crash may have no reproduction and a reproduction may have no crash.
//
//   Where determinism is insufficient to reproduce exactly, the artefact SHALL state so rather than
//   implying fidelity it does not have.
//
// WHY IT IS A MANIFEST AND NOT A CONTAINER. `src/core/diagnostics/` is layer 0 and
// `replay-and-rollback` is layer 4: this module cannot hold a `RecordLog`, must not learn what one
// is, and would be the wrong owner of it if it could. What it CAN own — and what the requirement
// actually asks for — is the LINK: the identities, the tick window, the hashes and the honest
// statement of fidelity, in a small artefact that names the replay slice rather than containing it.
// The replay side writes the slice; this writes what ties the slice, the capture and the crash
// report together, and it is the only one of the three that every one of them can reference.
//
// THE FIDELITY FIELD IS THE POINT. An artefact that says "replay this" without saying what replay
// will reproduce is worse than no artefact: it sends someone to look for a divergence that was
// never promised. `Fidelity` is a required field with no default that means "probably fine", and
// anything short of `Exact` carries a reason.

#include <cy/core/diagnostics/prelude.h>

namespace cy::diag {

/// What replaying this window will actually reproduce. There is deliberately no value meaning
/// "assume the best".
enum class Fidelity : u8 {
    /// Nobody stated one. An artefact written with this is reporting its own ignorance, which is
    /// the honest thing to do and is NOT the same as Exact.
    Unstated = 0,
    /// The session's determinism profile guarantees the same state hashes from the same inputs.
    Exact = 1,
    /// The window replays and the outcome may differ — an unrecorded external result, a profile
    /// that permits reassociation, a subsystem outside the profile.
    Approximate = 2,
    /// Replaying it will not reproduce the failure, and the artefact says so rather than wasting
    /// somebody's afternoon.
    NotReproducible = 3,
};

const char* fidelity_name(Fidelity fidelity) noexcept;

/// Everything the artefact records. Every pointer must outlive the call; nothing here is owned.
struct Reproduction {
    // --- What it points at
    // -------------------------------------------------------------------------
    /// The replay slice `replay-and-rollback` wrote. Named, not contained.
    const char* replay_log_path = "";
    /// The trace capture taken around the failure, when there was one.
    const char* capture_path = "";
    /// The crash artefact this reproduces, when there was a crash. "A reproduction may have no
    /// crash", so an empty string is a legitimate value and not a missing field.
    const char* crash_report_path = "";

    // --- The window
    // --------------------------------------------------------------------------------
    u64 first_tick = 0;
    u64 last_tick = 0;
    /// The checkpoint a player of this artefact seeks to before advancing. "Advance to shortly
    /// before the failure" is this number.
    u64 checkpoint_tick = 0;
    u64 session_seed = 0;
    /// `RecordLog::hash()` over the slice, so a player can tell it was handed the right one.
    u64 log_hash = 0;
    /// The state hash at `last_tick`, which is what a replay compares against.
    u64 final_state_hash = 0;
    u32 external_result_count = 0;

    // --- The honesty
    // -------------------------------------------------------------------------------
    Fidelity fidelity = Fidelity::Unstated;
    /// Required whenever `fidelity` is not `Exact`: what stops it being exact. An empty reason with
    /// a non-Exact fidelity is refused by the writer.
    const char* fidelity_reason = "";
    /// The determinism profile the session required, so a reader knows what was promised.
    const char* profile = "";
    const char* build_identity = "";
};

/// Write the artefact. Refuses, rather than writing something misleading, when:
///   * there is no replay slice to point at;
///   * `fidelity` is not `Exact` and no reason is given.
Expected<u64, cy::Error> write_reproduction(const char* path, const Reproduction& record) noexcept;

/// Register the artefact so the CRASH report carries a reference to it. The path is copied into
/// fixed storage now, because the handler that reads it may not allocate.
///
/// This is the "linked to it" half: the crash report names the reproduction, and the reproduction
/// names the crash report, and either may exist without the other.
void set_reproduction_link(const char* path, Fidelity fidelity, u64 first_tick,
                           u64 last_tick) noexcept;

struct ReproductionLink {
    const char* path = "";
    Fidelity fidelity = Fidelity::Unstated;
    u64 first_tick = 0;
    u64 last_tick = 0;
    bool present = false;
};

/// Async-signal-safe: a fixed buffer and relaxed loads, so the crash handler may call it.
ReproductionLink reproduction_link() noexcept;

void clear_reproduction_link() noexcept;

}  // namespace cy::diag
