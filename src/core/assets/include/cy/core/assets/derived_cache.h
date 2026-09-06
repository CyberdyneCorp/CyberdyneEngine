#ifndef CY_CORE_ASSETS_DERIVED_CACHE_H
#define CY_CORE_ASSETS_DERIVED_CACHE_H
// The derived data cache — one cache for everything the build produces from something else. M5 task
// 5.1.
//
// `asset-import-pipeline` — "Cook cache": "Cooked outputs SHALL be stored in the engine's derived
// data cache: content-addressed by derivation key, shared between developers and continuous
// integration, with local, shared read-only remote, and CI-writable remote tiers. **The import
// cache SHALL NOT be a separate mechanism from the cache used for shaders, material programs,
// geometry and texture pages, and other derived data; there SHALL be one cache covering all derived
// data.**"
//
// That last sentence is why this file is in `cy::assets` at layer 0 rather than in the importer at
// layer 7. An import cache that lived with the importer would be a second mechanism the moment the
// shader toolchain wanted one, and the two would then disagree about eviction, about tiers, and
// about what a hit means — which is the state every engine that grew three caches got into by
// adding them one at a time.
//
// --- THE CACHE IS DISPOSABLE, AND THAT IS AN INVARIANT RATHER THAN A HOPE
// -------------------------
//
// "Deleting it SHALL never lose project content; the only consequence SHALL be a slower next
// build." Two things follow, and both are enforced here:
//
//   * Nothing is stored here that is not reproducible from the project plus the code. A `.meta`
//     sidecar is authoritative metadata and belongs in source control (see `identity.h`); a cooked
//     payload is derived and belongs here.
//   * A record this cache cannot read is DELETED rather than reported as a build failure. A
//     corrupted cache entry that stopped a build would make the cache a thing that can break you,
//     and the whole argument for content addressing is that it cannot.
//
// --- WHY A MISS IS NOT AN ERROR ------------------------------------------------------------------
//
// `lookup` returns a `CacheResult` and never an `Expected`. A miss is the ordinary case on a first
// build and on every fresh checkout, and a cache whose miss path returned an error would put an
// error check in front of the most common thing that happens. Failure to READ a tier — a permission
// problem, a disk that filled, a share that is not mounted — is reported as a miss carrying the
// reason, because the caller's action is the same in every case: cook it.
//
// --- DEPENDENCY TRACKING, AND WHY IT IS NOT ONLY THE KEY -----------------------------------------
//
// "Import SHALL record the dependencies of each cooked asset ... A change to any dependency SHALL
// invalidate the cooked output."
//
// The derivation key already covers everything the producer knew BEFORE it ran: the source's
// content, the options, the versions, the platform. It cannot cover what the producer DISCOVERED
// while running — the includes a shader pulled in, the textures a glTF material referenced —
// because those are not known until the work is done, and a key that could only be computed after
// doing the work would never produce a hit.
//
// So each entry records the dependencies it discovered along with the digest each one had at the
// time, and `lookup` re-digests them through a caller-supplied resolver. A changed include is an
// `Invalidated` outcome naming the dependency, which is exactly the diagnostic
// `asset-import-pipeline` asks for: "the cache outcome (hit, miss, or invalidated with the
// reason)". The cost is one digest per dependency per lookup, which is a read of files the producer
// was about to read anyway on a miss.

#include <cy/core/assets/derivation.h>
#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::assets {

/// Which tier answered.
enum class CacheTier : u8 {
    /// Nothing answered.
    None = 0,
    /// This machine's own cache. Writable, and where a promoted hit lands.
    Local = 1,
    /// The team's shared cache. Read-only from a developer's machine.
    Shared = 2,
    /// The cache continuous integration writes.
    Remote = 3,
};

/// The enumerator's own spelling, for a diagnostic. Never null.
[[nodiscard]] const char* cache_tier_name(CacheTier tier) noexcept;

/// What a lookup found.
enum class CacheOutcome : u8 {
    /// A record was found and every dependency still digests the same.
    Hit = 0,
    /// No tier holds a record for this key.
    Miss = 1,
    /// A record exists and one of its dependencies changed. `stale` names which.
    Invalidated = 2,
    /// A record exists and could not be read. It has been deleted; the next store repopulates it.
    Corrupt = 3,
};

/// The enumerator's own spelling, for a diagnostic. Never null.
[[nodiscard]] const char* cache_outcome_name(CacheOutcome outcome) noexcept;

/// One input a producer discovered while it ran, and the digest that input had at the time.
///
/// `name` is whatever the producer will be able to re-resolve later: a virtual path, an asset id in
/// its text form, the name of an engine setting. This cache never interprets it — it hands it back
/// to the resolver — which is what lets one cache serve producers whose inputs are files, assets
/// and settings without knowing what any of those are.
struct DerivedDependency {
    std::string_view name;
    ContentHash hash;
};

/// What a producer asks the cache to store.
struct DerivedArtefact {
    DerivedKind kind = DerivedKind::Unknown;
    /// The producer's own name, recorded so a human reading the cache can tell what made an entry.
    std::string_view producer;
    Span<const u8> payload;
    Span<const DerivedDependency> dependencies;
};

/// A record read back out of the cache. Owns its bytes; the spans point into them.
class DerivedEntry {
public:
    DerivedEntry() noexcept = default;

    DerivedEntry(const DerivedEntry&) = delete;
    DerivedEntry& operator=(const DerivedEntry&) = delete;
    DerivedEntry(DerivedEntry&&) noexcept = default;
    DerivedEntry& operator=(DerivedEntry&&) noexcept = default;

    [[nodiscard]] bool is_empty() const noexcept { return record_.empty(); }
    [[nodiscard]] DerivedKind kind() const noexcept { return kind_; }
    [[nodiscard]] const DerivationKey& key() const noexcept { return key_; }
    [[nodiscard]] std::string_view producer() const noexcept;

    /// The cooked bytes. This is what the caller came for.
    [[nodiscard]] Span<const u8> payload() const noexcept;

    /// The digest the payload had when it was stored, checked on read.
    [[nodiscard]] const ContentHash& payload_hash() const noexcept { return payload_hash_; }

    [[nodiscard]] u32 dependency_count() const noexcept { return dependency_count_; }
    /// One recorded dependency. `index` must be below `dependency_count()`.
    [[nodiscard]] DerivedDependency dependency(u32 index) const noexcept;

private:
    friend class DerivedCache;

    Array<u8> record_;
    DerivationKey key_{};
    ContentHash payload_hash_{};
    DerivedKind kind_ = DerivedKind::Unknown;
    u32 dependency_count_ = 0;
    u32 producer_offset_ = 0;
    u32 producer_size_ = 0;
    u32 dependencies_offset_ = 0;
    u64 payload_offset_ = 0;
    u64 payload_size_ = 0;
};

/// What a lookup answered, and why.
struct CacheResult {
    CacheOutcome outcome = CacheOutcome::Miss;
    CacheTier tier = CacheTier::None;
    /// One line for the import report. Never null; empty on a plain hit.
    const char* reason = "";
    /// The dependency whose change invalidated the entry. Points into `entry`, and is empty unless
    /// the outcome is `Invalidated`.
    std::string_view stale;
    /// The record, valid only when the outcome is `Hit`.
    DerivedEntry entry;

    [[nodiscard]] bool is_hit() const noexcept { return outcome == CacheOutcome::Hit; }
};

/// Where the tiers live, and what this process may write.
///
/// The paths are native filesystem paths rather than `VirtualPath`s, deliberately: a shared cache
/// is on a network mount and a remote cache is somewhere continuous integration decided, and
/// neither belongs in the project's virtual namespace. Nothing in a cache is addressed by project
/// path.
struct DerivedCacheTiers {
    /// This machine's cache. Empty disables caching entirely, which is a supported configuration:
    /// a clean-build gate wants exactly that.
    const char* local = "";
    /// The team's shared cache, read-only from here. Empty when there is none.
    const char* shared = "";
    /// The cache continuous integration populates. Empty when there is none.
    const char* remote = "";
    /// Whether this process may WRITE the remote tier. A developer must not; CI must.
    bool write_remote = false;
    /// Copy a hit from a read-only tier into the local one, so the second read is local. The cost
    /// is one write per first use; the benefit is that a cold machine warms itself.
    bool promote_hits = true;
    /// Verify the payload's recorded digest on every read.
    ///
    /// On by default, and it is the check that turns a truncated network read into a `Corrupt`
    /// outcome and a re-cook rather than into a cooked asset with a hole in it. A shipping cook
    /// that trusts its own local disk can turn it off; nothing else should.
    bool verify_payload = true;
};

/// How the cache has behaved. `asset-import-pipeline` requires the outcome to be reportable per
/// asset; this is the project-level summary the same report ends with.
struct CacheStatistics {
    u64 hits_local = 0;
    u64 hits_shared = 0;
    u64 hits_remote = 0;
    u64 misses = 0;
    u64 invalidated = 0;
    u64 corrupt = 0;
    u64 stores = 0;
    u64 promotions = 0;
    u64 bytes_read = 0;
    u64 bytes_written = 0;

    [[nodiscard]] u64 hits() const noexcept { return hits_local + hits_shared + hits_remote; }
    [[nodiscard]] u64 lookups() const noexcept { return hits() + misses + invalidated + corrupt; }
};

/// Re-digest a dependency the producer recorded earlier.
///
/// Returns the dependency's current digest, and sets `*found` to false when it no longer exists —
/// which is itself an invalidation, and a different one from "it changed", because a missing
/// include is usually a mistake and a changed one usually is not.
using DependencyDigest = ContentHash (*)(void* user, std::string_view name, bool* found) noexcept;

/// The one cache, over its tiers.
///
/// Not thread-safe, and deliberately: import runs in parallel on the job system, and a cache with a
/// lock inside it would serialise every producer on one mutex for an operation that is mostly I/O.
/// The supported shape is one `DerivedCache` per worker over the same directories — the store is
/// content-addressed and written atomically, so two workers producing the same artefact write the
/// same bytes to the same name and neither can observe a half-written file.
class DerivedCache {
public:
    DerivedCache() noexcept = default;

    DerivedCache(const DerivedCache&) = delete;
    DerivedCache& operator=(const DerivedCache&) = delete;

    /// Point the cache at its tiers, creating the local one when it does not exist.
    ///
    /// Fails with `InvalidArgument` on a path longer than `kMaxRootLength`, and with the
    /// filesystem's own error when the local tier cannot be created. A shared or remote tier that
    /// does not exist is NOT an error: a developer with no access to the team's share must still be
    /// able to build, and it degrades to a miss.
    [[nodiscard]] Status configure(const DerivedCacheTiers& tiers) noexcept;

    /// Whether a local tier was configured. When false, every lookup misses and every store is a
    /// no-op that succeeds — the clean-build configuration.
    [[nodiscard]] bool is_enabled() const noexcept { return local_[0] != '\0'; }

    /// Look for a key across the tiers, in order: local, shared, remote.
    ///
    /// `resolver` may be null when the producer records no dependencies; a record that HAS
    /// dependencies and is looked up with no resolver is `Invalidated`, because a cache that
    /// silently skipped a check it was asked to make would be worse than no cache.
    [[nodiscard]] CacheResult lookup(const DerivationKey& key, DependencyDigest resolver,
                                     void* user) noexcept;

    /// Store an artefact under a key. Writes the local tier, and the remote one when this process
    /// may write it. Succeeds and does nothing when the cache is disabled.
    [[nodiscard]] Status store(const DerivationKey& key, const DerivedArtefact& artefact) noexcept;

    /// Whether the local tier holds this key, without reading the payload. For a report, and for a
    /// cook that wants to know what it is about to do before it does it.
    [[nodiscard]] bool contains(const DerivationKey& key) const noexcept;

    /// Delete the local tier's contents.
    ///
    /// The shared and remote tiers are NEVER touched: they are not this process's to delete, and a
    /// recipe that cleared the team's cache because a developer wanted a clean build would be the
    /// most expensive typo in the project. This is what makes "deleting the cache loses nothing"
    /// something a test can execute rather than something a comment claims.
    [[nodiscard]] Status clear_local() noexcept;

    [[nodiscard]] const CacheStatistics& statistics() const noexcept { return statistics_; }
    void reset_statistics() noexcept { statistics_ = {}; }

    /// The longest tier root this cache accepts. A cache path is a directory, two hex characters
    /// and a file name, and the whole thing must fit a fixed buffer because layer 0 has no string
    /// type.
    static constexpr usize kMaxRootLength = 240;

private:
    [[nodiscard]] static CacheResult read_tier(CacheTier tier, const char* root,
                                               const DerivationKey& key, DependencyDigest resolver,
                                               void* user, bool verify_payload) noexcept;

    /// The record's byte layout, in one place for both directions.
    ///
    /// Static because promotion encodes a record it has just decoded, and a member that needed the
    /// cache's own state to do that would make promotion depend on which tier answered.
    [[nodiscard]] static Status encode_record(const DerivationKey& key,
                                              const DerivedArtefact& artefact,
                                              Array<u8>& out) noexcept;

    /// Decode a record, taking ownership of `record` on success. `expected` is the key the file was
    /// filed under, and a record whose own key disagrees is refused rather than served.
    [[nodiscard]] static Status decode_record(Array<u8>& record, const DerivationKey& expected,
                                              DerivedEntry& out) noexcept;

    char local_[kMaxRootLength + 1] = {};
    char shared_[kMaxRootLength + 1] = {};
    char remote_[kMaxRootLength + 1] = {};
    bool write_remote_ = false;
    bool promote_hits_ = true;
    bool verify_payload_ = true;
    CacheStatistics statistics_{};
};

}  // namespace cy::assets

#endif  // CY_CORE_ASSETS_DERIVED_CACHE_H
