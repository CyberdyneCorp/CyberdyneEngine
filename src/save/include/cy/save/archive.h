#pragma once
// Generations, the journal, and the write sequence that survives being killed. Tasks 6.2 and 6.4.
//
// `save-and-persistence` — "Atomic writes and generations": write new chunks, verify them, write
// the new manifest, and switch, with durability applied appropriately for the platform. **Existing
// save data SHALL NEVER be overwritten in place.** A crash, power loss or storage error at any
// point SHALL leave the previous save valid and loadable, and multiple generations SHALL be
// retained by policy so that a corrupted newest save falls back rather than losing progress.
//
// THE SEQUENCE, AND WHERE A KILL AT EACH POINT LEAVES THE ARCHIVE. This is the whole design, and
// M6's exit criterion is the middle column being true at every row.
//
//   phase             what it does                        killed here, the store holds
//   ----------------  ----------------------------------  -----------------------------------------
//   ChunksWritten     new chunks, under their own hashes  the previous generation, plus chunks
//                                                         nothing references — collected later
//   ChunksVerified    reads them back and re-hashes them  the same
//   ManifestWritten   the new generation's manifest       the same; the manifest is unreferenced
//   PointerSwitched   rewrites `current` — ONE atomic     the NEW generation, whole
//                     write, and the only one that
//                     changes what loads
//   Pruned            drops generations past the policy   the new generation, plus whatever the
//                     and unreferenced chunks             interrupted collection did not remove
//
// There is exactly one instant at which the active save changes, and it is a single atomic write of
// one small object. Everything before it is additive — new chunks are new NAMES, because a chunk is
// stored under the hash of its own bytes — so nothing an interrupted save wrote can be mistaken for
// part of the previous one. That is why "never overwritten in place" is a property of the layout
// here rather than a rule somebody has to remember.
//
// THE PHASE OBSERVER IS NOT DEBUG SCAFFOLDING. "Testing SHALL include transactional tests
// simulating failure after every write phase and verifying the previous save remains valid." A test
// cannot simulate that without a way to stop the sequence at a chosen phase, and a hook that exists
// only in a test build is one the shipping path does not have. It doubles as the progress report a
// user interface needs while a large save is written.
//
// THE JOURNAL IS THE CHEAP HALF. "Persistent change MAY be recorded incrementally in a journal
// appended as change occurs, with a base checkpoint periodically compacted from base plus journal."
// An append writes one object per dirty region and nothing else — no manifest, no switch — so a
// checkpoint every few seconds costs what changed. `compact()` folds base and journal into a new
// generation through the sequence above, which is why "compaction SHALL be atomic and SHALL leave
// the previous base valid until it completes" needs no separate mechanism.

#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/save/container.h>
#include <cy/save/overlay.h>
#include <cy/save/storage.h>

namespace cy::save {

/// Which of the five phases of a commit has completed.
enum class WritePhase : u8 {
    ChunksWritten = 0,
    ChunksVerified = 1,
    ManifestWritten = 2,
    /// The instant the active save changes. Everything before this is invisible to a loader.
    PointerSwitched = 3,
    Pruned = 4,
};

const char* write_phase_name(WritePhase phase) noexcept;

/// Called after each completed phase. Returning is the only thing it may do that the archive
/// depends on; a test that never returns from one is a save killed at exactly that phase.
using PhaseObserver = void (*)(void* user, WritePhase phase) noexcept;

/// What a save declares about itself and about what produced it.
struct SaveIdentity {
    /// The build writing the save. Compared on load per the project's compatibility policy.
    const char* build_id = "";
    AssetId project;
    AssetId save;
    AssetId campaign;
    u64 session_seed = 0;
    /// The cooked content and installed plugin set this save is a delta against.
    assets::ContentHash content_version;
    assets::ContentHash plugin_version;
};

struct ArchiveConfig {
    /// How many generations to keep. One is enough to be atomic; more is what makes a corrupted
    /// newest generation a fallback rather than lost progress. Zero is refused.
    u32 retained_generations = 3;
    /// Read each chunk back and re-hash it before the manifest is written. This is the verify step
    /// of "write new chunks, verify them, write the new manifest, and switch".
    bool verify_on_write = true;
    /// Re-hash each chunk as it is read. Off, a corrupt chunk is detected by the decoder failing
    /// rather than by its hash, which is later and less precise.
    bool verify_on_load = true;
};

/// The generation store. Move-only; holds a reference to a backend it does not own.
class SaveArchive {
public:
    explicit SaveArchive(Allocator& allocator = current_allocator()) noexcept
        : allocator_(&allocator), scratch_(allocator) {}

    SaveArchive(const SaveArchive&) = delete;
    SaveArchive& operator=(const SaveArchive&) = delete;

    [[nodiscard]] Status open(SaveBackend& backend, const ArchiveConfig& config = {}) noexcept;
    [[nodiscard]] bool is_open() const noexcept { return backend_ != nullptr; }
    void close() noexcept { backend_ = nullptr; }

    /// Write a full checkpoint and make it the active generation. Returns its number.
    ///
    /// Does NOT clear the overlay's dirty flags: they are the caller's to clear once this has
    /// returned successfully, because a crash between the capture and the commit must leave the
    /// changes dirty rather than believed saved.
    [[nodiscard]] Expected<u32, Error> commit(const Overlay& overlay,
                                              const SaveIdentity& identity) noexcept;

    /// Append the overlay's DIRTY regions to the active generation's journal.
    ///
    /// Cheap and frequent: one atomic object per dirty region, no manifest, no switch. A kill
    /// during one loses at most the entries not yet written, and never the base.
    [[nodiscard]] Status append_journal(const Overlay& overlay) noexcept;

    /// Fold the active generation and its journal into a new generation, then drop that journal.
    [[nodiscard]] Expected<u32, Error> compact(const SaveIdentity& identity) noexcept;

    /// Load the active generation, its journal replayed over it, into `out`.
    ///
    /// On a verification or decode failure the next older generation is tried, and
    /// `report.generations_skipped` counts how many were passed over — a fallback the player is
    /// told about rather than one that happens quietly.
    [[nodiscard]] Status load(const LoadPolicy& policy, Overlay& out, LoadReport& report) noexcept;

    /// Read one generation's manifest without loading any chunk. The save inspector's entry point,
    /// and what a save-selection screen reads.
    [[nodiscard]] Status read_manifest(u32 generation, Manifest& out, LoadReport& report) noexcept;

    /// Every generation in the store, ascending. Reads the store, so it sees what another process
    /// wrote.
    [[nodiscard]] Status generations(Array<u32>& out) const noexcept;

    /// The generation `current` names, or zero when nothing has been committed.
    [[nodiscard]] Expected<u32, Error> active_generation() const noexcept;

    /// Drop generations past the retention policy and every chunk no retained manifest names.
    /// Safe to interrupt and safe to repeat.
    [[nodiscard]] Status prune() noexcept;

    void set_phase_observer(PhaseObserver observer, void* user) noexcept {
        observer_ = observer;
        observer_user_ = user;
    }

    [[nodiscard]] const ArchiveConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] Status write_chunk(Span<const u8> payload, ChunkRef& out) noexcept;
    [[nodiscard]] Status verify_chunk(const ChunkRef& chunk, LoadReport& report) noexcept;
    [[nodiscard]] Status write_manifest(const Manifest& manifest,
                                        assets::ContentHash& hash) noexcept;
    [[nodiscard]] Status write_pointer(u32 generation, const assets::ContentHash& hash) noexcept;
    [[nodiscard]] Status load_generation(u32 generation, const LoadPolicy& policy, Overlay& out,
                                         LoadReport& report) noexcept;
    [[nodiscard]] Status replay_journal(u32 generation, const LoadPolicy& policy, Overlay& out,
                                        LoadReport& report) noexcept;
    void notify(WritePhase phase) noexcept;

    Allocator* allocator_ = nullptr;
    SaveBackend* backend_ = nullptr;
    ArchiveConfig config_;
    PhaseObserver observer_ = nullptr;
    void* observer_user_ = nullptr;
    /// One reusable buffer for the chunk being encoded. A commit is not concurrent with itself.
    Array<u8> scratch_;
};

}  // namespace cy::save
