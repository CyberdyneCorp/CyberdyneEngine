#pragma once
// Derivation keys and the region cache. M10 task 4.3.
//
// `procedural-content-generation` — "Caching and distribution": "Each region's generation SHALL
// have a derivation key hashing: the compiled program, the generator version, region identity,
// input dataset hashes, field versions, parameters, and platform where relevant. Results SHALL be
// stored in the derived data cache defined in `build-and-packaging`, so that identical generation
// is never repeated and generation distributes across build workers without additional mechanism."
//
// ================================================================================================
// A BUDGETED RESULT IS NOT CACHEABLE, AND THIS IS WHERE THAT IS ENFORCED
// ================================================================================================
//
// design.md §1.2, third condition. An iterative solve run to a SWEEP BUDGET rather than to
// convergence reproduced a full regeneration in **0 of 12 trials, in all 12 of its configurations**
// — the only axis of the spike's matrix with no survivor anywhere — because a partial run sweeps a
// different set of regions a different number of times, and a budgeted result is a function of
// exactly that.
//
// The answer is not to forbid the budget: it is a legitimate runtime lever, and a regrowth
// generator that must not hitch will use one. The answer is to STOP RECORDING IT AS IF IT WERE
// REPRODUCIBLE. `Program::cacheable()` is false whenever any stage iterates to a budget, and
// `RegionCache::store()` refuses such a region with `CacheRefusal::NotCacheable` rather than
// storing something a later run would serve as though it were the converged answer.
//
// ================================================================================================
// WHY THIS IS AN INTERFACE AND A MEMORY-RESIDENT IMPLEMENTATION, NOT A DISC CACHE
// ================================================================================================
//
// "Results SHALL be stored in the derived data cache defined in `build-and-packaging`", and that
// cache is a build-system artefact store with its own key space, its own eviction and its own
// remote fetch. What this row owes is the KEY — a number that is a complete function of everything
// a region's generation depends on — and a store that honours it. `DerivationKey` is that number
// and `RegionCache` is the in-process store the runtime uses; binding it to the derived data cache
// so that continuous integration's result is fetched rather than regenerated is a
// `build-and-packaging` integration and is recorded as a gap in this module's README rather than
// claimed here.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/hash_map.h>
#include <cy/pcg/dataset.h>
#include <cy/pcg/invalidation.h>

namespace cy::pcg {

class Program;
struct RegionState;

/// Everything one region's generation depends on, as one number.
///
/// It is a complete function of its inputs by construction: `derivation_key()` takes each
/// contribution as an argument and there is no ambient state it could read instead. A key that
/// omitted the field versions, say, would serve a cached forest for a region whose moisture had
/// changed, and would do it silently — which is why the inputs are parameters rather than fields
/// somebody remembers to set.
struct DerivationKey {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(DerivationKey, DerivationKey) noexcept = default;
};

/// The versions of the fields a region's generation read. Ordered by field identifier, so that two
/// runs that sampled the same fields produce the same key whatever order they sampled them in.
struct FieldVersions {
    explicit FieldVersions(Allocator& allocator) noexcept : entries(allocator) {}

    FieldVersions(const FieldVersions&) = delete;
    FieldVersions& operator=(const FieldVersions&) = delete;
    FieldVersions(FieldVersions&&) noexcept = default;
    FieldVersions& operator=(FieldVersions&&) noexcept = default;

    struct Entry {
        u64 field = 0;
        u64 version = 0;
    };

    Array<Entry> entries;

    /// Insert or update, keeping the array sorted by field. Small and linear on purpose: a region
    /// reads a handful of fields, and a hash map would have to be walked in a canonical order
    /// anyway to produce a stable digest.
    [[nodiscard]] Status observe(u64 field, u64 version) noexcept;
    [[nodiscard]] u64 digest() const noexcept;
    void clear() noexcept { entries.clear(); }
};

/// Which machine's arithmetic produced a result.
///
/// `procedural-content-generation` asks the key to hash "platform where relevant", and it is
/// relevant exactly here: design.md §1.5 refuses to claim that a region generated on one
/// architecture reproduces on another, because this host has one architecture. So the platform tag
/// participates in the key, and a cache populated on one target is a miss on another rather than a
/// silently-wrong hit. When the cross-architecture criterion goes green in continuous integration,
/// the honest change is to REMOVE this contribution, not to add one.
[[nodiscard]] u64 platform_tag() noexcept;

/// The key for one region.
///
/// `input_digest` is the digest of the datasets this region's generation consumed that are not
/// covered by the program, the seed or the fields — a neighbour's candidate list, an upstream
/// generator's output. Zero where there are none.
[[nodiscard]] DerivationKey derivation_key(u64 program_digest, u32 generator_version, u64 seed,
                                           const RegionCoord& region, u64 input_digest,
                                           const FieldVersions& fields, u64 params_digest) noexcept;

/// Why a store was refused.
enum class CacheRefusal : u8 {
    None = 0,
    /// The program contains an iterative stage running to a BUDGET. See the header comment.
    NotCacheable,
    /// The key is zero, which means a caller assembled it from nothing.
    InvalidKey,
    /// The cache is at its declared capacity and the entry was not admitted.
    OverCapacity,
};

[[nodiscard]] const char* cache_refusal_name(CacheRefusal refusal) noexcept;

/// One cached region: the digests its stages produced and the accepted points.
///
/// The RASTER is not cached. It is an intermediate — every stage that reads it is re-run anyway
/// when the region is dirty, and a region that is not dirty does not need it — so caching it would
/// multiply the store's size by the number of channels for no hit it could serve. What a hit has to
/// supply is the OUTPUT and the digests the next stage's invalidation compares, and that is what is
/// here.
struct CachedRegion {
    explicit CachedRegion(Allocator& allocator) noexcept
        : points(allocator), stage_digests(allocator) {}

    CachedRegion(const CachedRegion&) = delete;
    CachedRegion& operator=(const CachedRegion&) = delete;
    CachedRegion(CachedRegion&&) noexcept = default;
    CachedRegion& operator=(CachedRegion&&) noexcept = default;

    DerivationKey key;
    RegionCoord region;
    PointSet points;
    Array<u64> stage_digests;
    f32 macro_density = 0.0F;
    f32 macro_height = 0.0F;

    [[nodiscard]] u64 bytes() const noexcept;
};

/// The in-process region cache.
class RegionCache {
public:
    RegionCache(Allocator& allocator, u64 capacity_bytes) noexcept
        : allocator_(&allocator),
          entries_(allocator),
          index_(allocator),
          capacity_(capacity_bytes) {}

    RegionCache(const RegionCache&) = delete;
    RegionCache& operator=(const RegionCache&) = delete;

    /// Store a region's result. `cacheable` is `Program::cacheable()`, passed rather than read off
    /// a program so that the refusal is visible at the call site — a cache that reached into a
    /// program to decide would put the rule somewhere a caller does not look.
    [[nodiscard]] Expected<const CachedRegion*, Error> store(DerivationKey key,
                                                             const RegionCoord& region,
                                                             const PointSet& points,
                                                             Span<const u64> stage_digests,
                                                             bool cacheable) noexcept;

    [[nodiscard]] const CachedRegion* find(DerivationKey key) const noexcept;

    /// Drop everything. What a generator-version change does, because a version is in every key and
    /// nothing under the old one can ever hit again.
    void clear() noexcept;

    [[nodiscard]] CacheRefusal last_refusal() const noexcept { return refusal_; }
    [[nodiscard]] u64 hits() const noexcept { return hits_; }
    [[nodiscard]] u64 misses() const noexcept { return misses_; }
    [[nodiscard]] u64 refusals() const noexcept { return refusals_; }
    [[nodiscard]] u64 bytes() const noexcept { return bytes_; }
    [[nodiscard]] usize size() const noexcept { return entries_.size(); }

private:
    Allocator* allocator_;
    Array<CachedRegion> entries_;
    HashMap<u64, usize> index_;
    u64 capacity_ = 0;
    u64 bytes_ = 0;
    mutable u64 hits_ = 0;
    mutable u64 misses_ = 0;
    u64 refusals_ = 0;
    CacheRefusal refusal_ = CacheRefusal::None;
};

}  // namespace cy::pcg
