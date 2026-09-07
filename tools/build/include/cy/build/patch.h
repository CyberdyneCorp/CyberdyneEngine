#ifndef CY_BUILD_PATCH_H
#define CY_BUILD_PATCH_H
// Chunk-level patching, and an installation that survives being interrupted. M6 tasks 7.5 and 7.6.
//
// `build-and-packaging` — "Chunk-level patching" and "Patch application is staged and atomic":
// "Patching SHALL operate on **content-addressed chunks**, not whole packages. A patch SHALL
// transfer only chunks whose hashes changed, plus a manifest … Patch application SHALL: download
// required chunks, **verify** them against their hashes, stage the new manifest, and switch
// atomically. A failed or interrupted patch SHALL leave the previous build intact and playable.
// Content SHALL NEVER be overwritten destructively in place."
//
// M6's exit criterion is the second half of that sentence and it is stated as a test rather than an
// argument: **a patch applies atomically and rolls back cleanly WHEN INTERRUPTED.** So interruption
// is a parameter of `apply` rather than a hazard it hopes not to meet, and
// `integration.build_patch` interrupts at every stage — including by killing the process outright,
// which is the only version of "interrupted" that proves nothing depends on a destructor running.
//
// --- WHY THE SWITCH IS ONE RENAME ----------------------------------------------------------------
//
// An installation is a chunk store, a directory of manifests, and a single file naming the manifest
// in force. Chunks are content-addressed, so adding one can never damage the build in force —
// different content has a different name. The whole of the atomicity is therefore the last step:
// one `write_atomic` of the `current` file, which is a write to a temporary and a rename, and
// rename is atomic on every filesystem the engine targets.
//
// That is why an interruption at any earlier stage is harmless rather than recoverable: there is
// nothing to recover. Staged chunks are unreferenced content and are discarded on the next `open`,
// exactly as `fs::discard_temporaries` discards an interrupted save's leftovers.
//
// --- WHAT A PATCH MANIFEST RECORDS ---------------------------------------------------------------
//
// "the build it produces, the builds it may be applied to, added and removed chunks with hashes and
// sizes, and bundle membership." All five, and the applicability check is enforced rather than
// documented: a patch offered to an installation it does not name is refused before a byte moves.

#include <cy/build/artefact_store.h>
#include <cy/build/package.h>
#include <cy/core/base/expected.h>

#include <string>
#include <vector>

namespace cy::build {

/// Where a patch's chunks come from.
///
/// An interface rather than an `ArtefactStore` because a patch's chunks arrive from a DOWNLOAD, and
/// the specification is precise about the order: "download required chunks, **verify** them against
/// their hashes, stage the new manifest, and switch atomically". A source that verified on the way
/// out would move the verification to the wrong side of the wire and make "WHEN a downloaded chunk
/// fails verification THEN the patch SHALL abort and report the chunk" untestable — which is
/// exactly how a check that never fires comes to be believed.
class ChunkSource {
public:
    ChunkSource() = default;
    ChunkSource(const ChunkSource&) = delete;
    ChunkSource& operator=(const ChunkSource&) = delete;
    virtual ~ChunkSource() = default;

    /// The bytes filed under a digest, WITHOUT checking that they still hash to it. The patcher
    /// checks; that is its job.
    [[nodiscard]] virtual Status fetch(const assets::ContentHash& digest, Array<u8>& out) const = 0;
};

/// A `ChunkSource` over an artefact store — the ordinary case, where the patch is built on the same
/// machine that cooked it.
class StoreChunkSource final : public ChunkSource {
public:
    explicit StoreChunkSource(const ArtefactStore& store) : store_(&store) {}

    [[nodiscard]] Status fetch(const assets::ContentHash& digest, Array<u8>& out) const override;

private:
    const ArtefactStore* store_;
};

/// One chunk a patch adds or removes.
struct PatchChunk {
    std::string name;
    assets::ContentHash digest{};
    u64 size = 0;
    std::string bundle;
};

/// The patch itself.
struct PatchManifest {
    /// The build this patch produces.
    std::string produces;
    /// The builds it may be applied to. Enforced by `Installation::apply`.
    std::vector<std::string> applies_to;
    std::vector<PatchChunk> added;
    std::vector<PatchChunk> removed;
    /// The manifest of the build being produced, carried so that applying a patch does not require
    /// fetching the new package manifest separately.
    PackageSet target;

    [[nodiscard]] u64 transferred_bytes() const noexcept;
};

/// The chunks that differ between two builds. Only content whose hash changed, which is the whole
/// point: "WHEN one texture changes in a large game THEN the patch SHALL contain the affected pages
/// and a manifest, not the containing package."
[[nodiscard]] PatchManifest diff(const PackageSet& from, const PackageSet& to);

[[nodiscard]] std::string write_patch(const PatchManifest& patch);
[[nodiscard]] Expected<PatchManifest, Error> read_patch(std::string_view document);

/// Where a patch may be interrupted. Ordered as application proceeds.
enum class PatchStage : u8 {
    /// Before anything is fetched.
    Begin = 0,
    /// Chunks are being fetched into the staging directory.
    Fetch = 1,
    /// Staged chunks are being re-digested against the manifest.
    Verify = 2,
    /// Verified chunks are being moved into the installation's content-addressed store.
    Commit = 3,
    /// The new manifest has been written and the `current` pointer is about to be switched.
    Switch = 4,
    /// Done. Not an interruption point.
    Complete = 5,
};

[[nodiscard]] const char* patch_stage_name(PatchStage stage) noexcept;
[[nodiscard]] Expected<PatchStage, Error> patch_stage_from_name(std::string_view name) noexcept;

/// How, and where, to interrupt an application. `Complete` means "do not".
struct PatchInterrupt {
    PatchStage stage = PatchStage::Complete;
    /// Kill the process at that point with `_exit`, rather than returning an error.
    ///
    /// The difference matters and is the reason both exist: a returned error still unwinds and
    /// still runs the rollback, so it tests the rollback path; `_exit` runs no destructor and
    /// closes no file, so it tests that the installation is intact **without** anything having
    /// tidied up. Only the second one proves the property the specification states, and it is why
    /// `integration.build_patch` forks.
    bool hard = false;
};

/// What an application did.
struct PatchResult {
    PatchStage reached = PatchStage::Begin;
    bool applied = false;
    /// The chunk whose fetch or verification failed, empty otherwise.
    ///
    /// A failure here is a REPORTED RESULT rather than an `Error`, because "Verification failure
    /// SHALL abort the patch and report which chunk failed" needs the chunk's name to reach the
    /// caller, and an error code cannot carry one.
    std::string failed_chunk;
    u64 fetched_chunks = 0;
    u64 fetched_bytes = 0;
};

/// An installed build: a chunk store, its manifests, and the one file that says which is in force.
class Installation {
public:
    Installation() = default;

    Installation(const Installation&) = delete;
    Installation& operator=(const Installation&) = delete;

    /// Open, creating the layout when it does not exist.
    ///
    /// Discards anything left in `staging/` by an interrupted application. That is not cleanup for
    /// tidiness: it is the second half of "a failed or interrupted patch SHALL leave the previous
    /// build intact", and it is the same mechanism `fs::discard_temporaries` provides for saves.
    [[nodiscard]] Status open(std::string root);

    /// The build in force, or `NotFound` when nothing is installed.
    [[nodiscard]] Expected<std::string, Error> current_build() const;

    /// The manifest in force.
    [[nodiscard]] Expected<PackageSet, Error> current_package() const;

    /// Install a build from an artefact store. The initial installation; a patch is `apply`.
    [[nodiscard]] Status install(const PackageSet& packages, const ArtefactStore& source);

    /// Apply a patch, staging, verifying and switching atomically.
    ///
    /// Fails with `InvalidArgument` when the patch does not name the installed build, and with
    /// `Internal` naming the chunk when verification fails. In both cases the installation is
    /// unchanged.
    [[nodiscard]] Expected<PatchResult, Error> apply(const PatchManifest& patch,
                                                     const ChunkSource& source,
                                                     PatchInterrupt interrupt = {});

    /// Discard a staging directory an interrupted application left. Idempotent, and called by
    /// `open`, so a caller never has to remember it.
    [[nodiscard]] Status rollback();

    /// Every chunk the build in force names is present and digests correctly.
    ///
    /// This is what "playable" means for a test: not that a game started, but that every byte the
    /// manifest promises is there and is the byte it promised.
    [[nodiscard]] Status verify() const;

    /// Read one installed chunk by its logical name.
    [[nodiscard]] Status read(std::string_view name, Array<u8>& out) const;

    [[nodiscard]] const std::string& root() const noexcept { return root_; }
    [[nodiscard]] const ArtefactStore& chunks() const noexcept { return chunks_; }

private:
    [[nodiscard]] std::string staging_root() const;
    [[nodiscard]] std::string manifest_path(std::string_view build_id) const;
    [[nodiscard]] std::string current_path() const;

    std::string root_;
    ArtefactStore chunks_;
};

}  // namespace cy::build

#endif  // CY_BUILD_PATCH_H
