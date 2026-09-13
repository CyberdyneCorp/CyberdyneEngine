#pragma once
// Which of two copies of one save is the one to keep. M10 task 6.2.
//
// `save-and-persistence` — "Storage backends and the cloud boundary": "Conflict resolution between
// local and remote saves SHALL use **logical metadata** — generation, campaign identity, simulation
// point, progress markers, content version — and SHALL NOT be decided by file modification
// timestamps." Its scenario is "a conflict is decided on meaning".
//
// THE RULE IS ENFORCED BY THE TYPE, NOT BY A CONVENTION. `SaveSummary` below holds the five pieces
// of metadata that sentence names and NOTHING ELSE: there is no field for a modification time, an
// upload time, a wall clock or a device clock, so a decision made through this header cannot be
// made on one. A timestamp is not merely discouraged here — there is nothing to decide with. Two
// devices whose clocks disagree by an hour, a cloud service that rewrites a file on download, and a
// player who moves a save between machines are all cases where the newer file is the older game.
//
// WHY DOMINANCE RATHER THAN A PRIORITY ORDER. Three of the five markers are ordered — the
// generation counts commits, the simulation point counts ticks played, and the progress marker
// counts how far the player has got. A save is kept only when it is at or ahead of the other on ALL
// THREE. When they disagree — this device saved more often, that device played further — the two
// are BRANCHES OF ONE CAMPAIGN and neither is the answer: `resolve_conflict` reports `Divergent`
// and the game asks the player. Picking the larger of one marker would be the engine inventing
// which of two real play sessions the player meant to keep, and the specification's closing rule
// for this capability is that the engine "SHALL NEVER invent authoritative state".
//
// WHAT THIS HEADER DOES NOT DECIDE. It does not fetch, upload, merge or delete anything: "cloud
// transport, quotas, and account association SHALL NOT be owned by this capability. It SHALL
// produce artefacts and the metadata a platform service needs." This is that metadata and the
// comparison over it; the platform layer acts on the answer.

#include <cy/core/assets/hash.h>
#include <cy/core/base/types.h>
#include <cy/core/values/asset_id.h>
#include <cy/save/container.h>

namespace cy::save {

/// The logical metadata one save is compared with another by. Built from a `Manifest` — which is
/// the only part of a save a conflict decision ever needs to read, and which a cloud backend can
/// fetch without the chunks it names.
struct SaveSummary {
    AssetId project;
    AssetId save;
    AssetId campaign;

    /// Commits behind this save. Counts writes, not play.
    u32 generation = 0;
    /// The tick at whose commit boundary the state was captured. Counts play, not writes.
    u64 simulation_point = 0;
    /// How far the player has got, as the project defines it. See `Manifest::progress`.
    u64 progress = 0;

    /// The cooked content the save is a delta against. Reported by a resolution, never decisive —
    /// see `ConflictResolution::content_version_differs`.
    assets::ContentHash content_version;
};

/// Read the metadata a conflict is decided on out of a manifest. `SaveArchive::read_manifest`
/// supplies the local one without loading a chunk; a cloud backend supplies the remote one the same
/// way.
[[nodiscard]] SaveSummary summarise(const Manifest& manifest) noexcept;

/// What comparing two summaries concluded.
enum class ConflictOutcome : u8 {
    /// Every ordered marker agrees: the two are the same point in the same campaign, and either
    /// copy may be used — subject to `content_version_differs`, which a caller checks against the
    /// content it actually has installed. Not a conflict.
    Identical = 0,
    /// The local save is at or ahead of the remote one on every ordered marker, and ahead on one.
    KeepLocal,
    /// The remote save is, by the same rule.
    KeepRemote,
    /// One campaign, two branches: the markers disagree about which is further on. The engine does
    /// not choose; the game asks the player.
    Divergent,
    /// Not two copies of one save at all — a different project, save slot or campaign. Both are
    /// kept, and nothing is overwritten.
    Unrelated,
};

[[nodiscard]] const char* conflict_outcome_name(ConflictOutcome outcome) noexcept;

/// The answer, and what produced it.
struct ConflictResolution {
    ConflictOutcome outcome = ConflictOutcome::Identical;

    /// Which metadata decided it, as a literal a log line and a player-facing prompt can both use.
    /// Never null and never empty.
    const char* reason = "";

    /// The two saves are deltas against different cooked content. REPORTED, NEVER DECISIVE: a save
    /// whose content this build does not have installed is refused by `check_compatibility` with
    /// `LoadFailure::MissingContent`, which is a load-time fact about one save rather than a reason
    /// to prefer the other. Deciding it here would duplicate that policy in a second place and get
    /// it subtly different.
    bool content_version_differs = false;
};

/// Compare two copies of one save and say which to keep.
///
/// Pure: the same two summaries always produce the same answer, on any machine, at any time of day,
/// whatever either file's modification time is.
[[nodiscard]] ConflictResolution resolve_conflict(const SaveSummary& local,
                                                  const SaveSummary& remote) noexcept;

/// The same comparison over two manifests, for the common case where both have just been read.
[[nodiscard]] ConflictResolution resolve_conflict(const Manifest& local,
                                                  const Manifest& remote) noexcept;

}  // namespace cy::save
