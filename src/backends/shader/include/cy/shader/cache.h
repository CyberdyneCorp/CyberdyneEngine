#ifndef CY_SHADER_CACHE_H
#define CY_SHADER_CACHE_H
// The tiered content-addressed cache. Task 3.4.
//
// `shader-system` — "Shader compilation service and tiered cache": compiled outputs are stored in a
// tiered content-addressed cache — **local, a shared read-only remote, and a writable remote
// populated by CI** — the same tiering as the cook cache in `asset-import-pipeline`. Identical
// compilation work is not repeated when a tier already holds the result.
//
// --- WHY THE TIERS ARE AN INTERFACE AND THE FILESYSTEM IS ONE IMPLEMENTATION ----------------------
//
// A remote tier is an HTTP or object-store client, and there is no HTTP client in this engine at M3
// — `core-assets-and-io`'s `RemoteMount` is the seam that will hold one and it is a stub. Writing
// one here to satisfy the word "remote" would be the wrong order: the requirement that pays off now
// is the **lookup order and the promotion rule**, and those are independent of transport.
//
// So: `CacheTier` is the interface, `DirectoryCacheTier` is the implementation, and a tier that
// happens to be a directory on a network share is a legitimate shared read-only tier today. When a
// real client arrives it implements this interface and nothing above changes. What is *not*
// deferred is the behaviour the specification names, and it is tested:
//
//   * **lookup order** — local first, then each remote in the order it was added. The first hit
//     wins, and a hit in a remote tier is **promoted into the local tier** so the second lookup is
//     local. That promotion is the whole value of tiering: "CI populates, developers consume".
//   * **write policy** — a `put` goes to every writable tier. CI has the writable remote configured
//     and a developer does not, which is how the population direction is enforced by configuration
//     rather than by remembering.
//   * **the key** — `compile_cache_key()` in `compiler.h`. A tier never invents a key; it stores
//     bytes under one. That is what makes the cache content-addressed rather than merely a map.
//
// --- WHAT IS DELIBERATELY MISSING ------------------------------------------------------------------
//
// Eviction. A local shader cache is bounded by the number of variants a project has, not by time,
// and a cache that evicts under a size ceiling before anything has measured the ceiling is a source
// of unexplained recompilation. `stats()` reports the size so the decision can be made on numbers.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/shader/shader.h>

namespace cy::shader {

/// What a lookup did, so the build report can say where the work went.
enum class CacheOutcome : u8 {
    /// Found in the local tier.
    LocalHit = 0,
    /// Found in a remote tier and copied into the local one.
    RemoteHit = 1,
    /// Not found anywhere; the caller must compile.
    Miss = 2,
};

const char* cache_outcome_name(CacheOutcome outcome) noexcept;

struct CacheStats {
    u64 lookups = 0;
    u64 local_hits = 0;
    u64 remote_hits = 0;
    u64 misses = 0;
    u64 stores = 0;
    /// Entries promoted from a remote tier into the local one.
    u64 promotions = 0;
    u64 bytes_read = 0;
    u64 bytes_written = 0;
};

/// One tier of the cache. Content-addressed: the key is a hash of the inputs and the tier neither
/// computes nor interprets it.
class CacheTier {
public:
    virtual ~CacheTier() = default;

    CacheTier(const CacheTier&) = delete;
    CacheTier& operator=(const CacheTier&) = delete;
    CacheTier(CacheTier&&) = delete;
    CacheTier& operator=(CacheTier&&) = delete;

    /// The tier's name, for a report: `local`, `shared`, `ci`.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// False for a shared read-only tier. A `put` skips it rather than failing, because "this tier
    /// does not accept writes" is a configuration, not an error.
    [[nodiscard]] virtual bool writable() const noexcept = 0;

    /// Read an entry into `out`, replacing it. `NotFound` when the tier does not hold the key —
    /// which is an ordinary outcome and not a failure worth a diagnostic.
    [[nodiscard]] virtual Status get(const ContentHash& key, Array<u8>& out) noexcept = 0;

    /// Store an entry. Overwrites silently: the key is a content hash, so a second write of the same
    /// key is the same bytes, and refusing it would make two workers racing on one variant an error.
    [[nodiscard]] virtual Status put(const ContentHash& key, Span<const u8> bytes) noexcept = 0;

    /// True when the key is present, without reading the payload.
    [[nodiscard]] virtual bool contains(const ContentHash& key) noexcept = 0;

protected:
    CacheTier() = default;
};

/// A tier backed by a directory. Entries are files named by the key's hex form, sharded into 256
/// subdirectories by the first byte — a flat directory of a hundred thousand files is slow to
/// enumerate on every filesystem the engine targets and impossible to browse on any of them.
///
/// A write is atomic (`fs::write_atomic`), so a build interrupted mid-write leaves no truncated
/// entry for the next build to read as a valid artefact.
class DirectoryCacheTier final : public CacheTier {
public:
    /// `root` is created if it does not exist and `writable` is true; a read-only tier that does not
    /// exist is simply a tier that never hits.
    DirectoryCacheTier(const char* name, const char* root, bool writable,
                       Allocator& allocator) noexcept;

    [[nodiscard]] const char* name() const noexcept override { return name_; }
    [[nodiscard]] bool writable() const noexcept override { return writable_; }
    [[nodiscard]] Status get(const ContentHash& key, Array<u8>& out) noexcept override;
    [[nodiscard]] Status put(const ContentHash& key, Span<const u8> bytes) noexcept override;
    [[nodiscard]] bool contains(const ContentHash& key) noexcept override;

private:
    /// `<root>/<first two hex digits>/<64 hex digits>.cyshader` into `out`.
    void entry_path(const ContentHash& key, char* out, usize capacity) const noexcept;

    const char* name_;
    /// Owned: the caller's string is a local in the configuration code that built the tier.
    Array<char> root_;
    bool writable_;
};

/// The tiers in lookup order, with promotion.
class ShaderCache {
public:
    explicit ShaderCache(Allocator& allocator) noexcept;

    ShaderCache(const ShaderCache&) = delete;
    ShaderCache& operator=(const ShaderCache&) = delete;

    /// Tiers are consulted in the order they are added, so the local one is added first. The cache
    /// does not own a tier: a tier outlives the cache in every configuration that has one.
    [[nodiscard]] Status add_tier(CacheTier& tier) noexcept;

    /// Look up a key, promoting a remote hit into the first writable tier.
    ///
    /// `out` is replaced on a hit and left alone on a miss, so a caller can reuse one buffer across
    /// a whole cook without clearing it.
    [[nodiscard]] Expected<CacheOutcome, Error> get(const ContentHash& key,
                                                    Array<u8>& out) noexcept;

    /// Store into every writable tier. Succeeds when at least one accepted the bytes; fails only
    /// when every writable tier refused, because a cache that silently stores nothing is a cache
    /// that turns into a recompilation nobody can explain.
    [[nodiscard]] Status put(const ContentHash& key, Span<const u8> bytes) noexcept;

    [[nodiscard]] CacheStats stats() const noexcept { return stats_; }
    [[nodiscard]] u32 tier_count() const noexcept { return static_cast<u32>(tiers_.size()); }

private:
    Array<CacheTier*> tiers_;
    CacheStats stats_;
};

}  // namespace cy::shader

#endif  // CY_SHADER_CACHE_H
