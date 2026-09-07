#ifndef CY_BUILD_ARTEFACT_STORE_H
#define CY_BUILD_ARTEFACT_STORE_H
// Immutable, content-addressed artefacts. M6 task 7.2.
//
// `build-and-packaging` — "Artefacts are immutable": "A derived artefact SHALL be identified by the
// hash of its content. An artefact with a given identity SHALL NEVER change; different content
// receives a different identity … Artefacts SHALL therefore be safely shareable between concurrent
// builds, machines, and processes without locking the project. Locking SHALL be scoped to
// individual outputs and manifests, never to the project as a whole."
//
// design.md §5 states the consequence in one line: **a mutable artefact makes every downstream key
// a lie.** A downstream key holds its upstream's output digest (§1.5), so an artefact that changed
// under a digest would silently invalidate every key computed from it — and nothing would report
// it, because the key still matches.
//
// --- HOW IMMUTABILITY IS ENFORCED RATHER THAN ASSERTED -------------------------------------------
//
// Three mechanisms, and each catches a different way of breaking it:
//
//   1. The NAME IS THE DIGEST. `put` cannot overwrite with different content, because different
//      content lands under a different name. This is the whole of the design; the other two catch
//      what happens outside it.
//   2. A stored file is made READ-ONLY (0444). An accidental in-place write — a producer handed the
//      store's path, a script redirecting into it — is refused by the operating system rather than
//      discovered as a wrong artefact three milestones later.
//   3. `verify` re-digests. A deliberate mutation (chmod, then write) is caught by the only thing
//      that can catch it: reading the bytes back and hashing them. M6's adversarial pass mutates an
//      artefact on purpose, and this is what must report it.
//
// --- WRITES ARE ATOMIC AND CONCURRENCY-SAFE, WITH NO PROJECT LOCK --------------------------------
//
// Every write goes to a temporary in the same directory and is renamed into place, so a reader
// never observes a partial artefact and two builds producing the same artefact write the same bytes
// to the same name. That is the whole locking story: `fs::write_atomic` is the per-output lock the
// specification asks for, and there is no lock above it.

#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <atomic>
#include <string>

namespace cy::build {

/// How a store answered a `put`.
enum class StoreOutcome : u8 {
    /// The artefact was written.
    Written = 0,
    /// The artefact was already present under this digest, so nothing was written. The ordinary
    /// case on a rebuild and on a second builder producing identical content.
    AlreadyPresent = 1,
};

/// What a store has done, for the build report.
struct StoreStatistics {
    u64 writes = 0;
    u64 already_present = 0;
    u64 reads = 0;
    u64 bytes_written = 0;
    u64 bytes_read = 0;
};

/// The content-addressed artefact store: a directory of immutable files named by their digests.
///
/// Thread-safe for concurrent `put` and `get` from many threads of one process and from many
/// processes, without any lock held here — see the note above. Not thread-safe for `configure` and
/// `clear`, which are lifecycle operations.
class ArtefactStore {
public:
    ArtefactStore() = default;

    ArtefactStore(const ArtefactStore&) = delete;
    ArtefactStore& operator=(const ArtefactStore&) = delete;

    /// Point the store at a directory, creating it when it does not exist.
    [[nodiscard]] Status configure(std::string_view root);

    [[nodiscard]] bool is_configured() const noexcept { return !root_.empty(); }
    [[nodiscard]] const std::string& root() const noexcept { return root_; }

    /// Store bytes and answer their digest. Storing content that is already present writes nothing
    /// and answers the same digest, which is what makes a warm build cheap and a concurrent build
    /// safe.
    [[nodiscard]] Expected<assets::ContentHash, Error> put(const void* data, usize size,
                                                           StoreOutcome* outcome = nullptr);

    /// Read an artefact back. Fails with `NotFound` when it is absent, and with `Internal` when its
    /// content does not digest to its name — which is the mutation case, and is reported rather
    /// than served.
    [[nodiscard]] Status get(const assets::ContentHash& digest, Array<u8>& out) const;

    [[nodiscard]] bool contains(const assets::ContentHash& digest) const noexcept;
    [[nodiscard]] Expected<u64, Error> size_of(const assets::ContentHash& digest) const;

    /// Re-digest one artefact. `ok()` when the bytes still hash to the name.
    [[nodiscard]] Status verify(const assets::ContentHash& digest) const;

    /// Re-digest everything in the store. Answers how many artefacts were checked and writes the
    /// number that failed to `corrupt`, which the caller reports; a corrupt artefact is not deleted
    /// here, because a build that silently repaired the evidence would make the mutation
    /// undiagnosable.
    [[nodiscard]] Expected<u64, Error> verify_all(u64* corrupt) const;

    /// Delete the store's contents. The store is derived data: this loses nothing a build cannot
    /// reproduce, and `just` gates that want a genuinely cold build need exactly this.
    [[nodiscard]] Status clear();

    /// A snapshot of the counters. By value, not by reference: `put` and `get` run concurrently,
    /// so the counters are atomics and a reference to them would hand a caller a moving target.
    [[nodiscard]] StoreStatistics statistics() const noexcept;

    /// The path an artefact occupies, for a diagnostic and for the patcher, which moves chunks
    /// between stores.
    [[nodiscard]] std::string path_of(const assets::ContentHash& digest) const;

private:
    void count_read(u64 bytes) const noexcept;

    std::string root_;
    mutable std::atomic<u64> writes_{0};
    mutable std::atomic<u64> already_present_{0};
    mutable std::atomic<u64> reads_{0};
    mutable std::atomic<u64> bytes_written_{0};
    mutable std::atomic<u64> bytes_read_{0};
};

}  // namespace cy::build

#endif  // CY_BUILD_ARTEFACT_STORE_H
