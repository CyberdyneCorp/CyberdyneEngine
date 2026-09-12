// Derivation keys and the region cache. See include/cy/pcg/cache.h.

#include <cy/pcg/cache.h>

#include <cy/pcg/program.h>

#include <utility>

namespace cy::pcg {

const char* cache_refusal_name(CacheRefusal refusal) noexcept {
    switch (refusal) {
        case CacheRefusal::None:
            return "none";
        case CacheRefusal::NotCacheable:
            return "not-cacheable";
        case CacheRefusal::InvalidKey:
            return "invalid-key";
        case CacheRefusal::OverCapacity:
            return "over-capacity";
    }
    return "unknown";
}

Status FieldVersions::observe(u64 field, u64 version) noexcept {
    for (usize index = 0; index < entries.size(); ++index) {
        if (entries[index].field == field) {
            entries[index].version = version;
            return ok();
        }
        if (entries[index].field > field) {
            // Sorted by field, so the digest below is a function of the SET rather than of the
            // order the region happened to sample in. Inserting in place is O(n) over a handful of
            // fields and is what keeps that true without a sort at digest time.
            if (Status pushed = entries.push_back(Entry{}); !pushed) {
                return pushed;
            }
            for (usize shift = entries.size() - 1; shift > index; --shift) {
                entries[shift] = entries[shift - 1];
            }
            entries[index] = Entry{field, version};
            return ok();
        }
    }
    return entries.push_back(Entry{field, version});
}

u64 FieldVersions::digest() const noexcept {
    Digest digest;
    digest.u64_value(entries.size());
    for (const Entry& entry : entries) {
        digest.u64_value(entry.field);
        digest.u64_value(entry.version);
    }
    return digest.value();
}

u64 platform_tag() noexcept {
    // The architecture and the compiler, because those are what design.md §1.5 declines to claim
    // agree. A cache populated on one target is a MISS on another rather than a silently-wrong hit;
    // when `pcg-regeneration-cross-platform` goes green in continuous integration the honest change
    // is to delete this contribution, not to add a second one.
    Digest digest;
#if defined(__x86_64__) || defined(_M_X64)
    digest.u64_value(0x7836345f36345f36ULL);  // "x64_64_6"
#elif defined(__aarch64__) || defined(_M_ARM64)
    digest.u64_value(0x6172636836345f5fULL);  // "arch64__"
#else
    digest.u64_value(0x756e6b6e6f776e00ULL);  // "unknown"
#endif
#if defined(__clang__)
    digest.u32_value(__clang_major__);
#elif defined(__GNUC__)
    digest.u32_value(0x8000U | __GNUC__);
#else
    digest.u32_value(0);
#endif
    digest.u32_value(static_cast<u32>(sizeof(void*)));
    return digest.value();
}

DerivationKey derivation_key(u64 program_digest, u32 generator_version, u64 seed,
                             const RegionCoord& region, u64 input_digest,
                             const FieldVersions& fields, u64 params_digest) noexcept {
    // Every contribution the specification names, and each is an ARGUMENT rather than something
    // this function could read off ambient state: a key that omitted the field versions would serve
    // a cached forest for a region whose moisture had changed, silently.
    Digest digest;
    digest.u64_value(program_digest);
    digest.u32_value(generator_version);
    digest.u64_value(seed);
    digest.u32_value(static_cast<u32>(region.x));
    digest.u32_value(static_cast<u32>(region.z));
    digest.u32_value(region.level);
    digest.u64_value(input_digest);
    digest.u64_value(fields.digest());
    digest.u64_value(params_digest);
    digest.u64_value(platform_tag());
    const u64 value = digest.value();
    // Zero is the invalid key, and a digest is free to land on it. Mapping it to one is better than
    // a key that compares equal to "nothing was computed".
    return DerivationKey{value != 0 ? value : 1};
}

u64 CachedRegion::bytes() const noexcept {
    return points.bytes() + stage_digests.capacity() * sizeof(u64) + sizeof(CachedRegion);
}

Expected<const CachedRegion*, Error> RegionCache::store(DerivationKey key,
                                                        const RegionCoord& region,
                                                        const PointSet& points,
                                                        Span<const u64> stage_digests,
                                                        bool cacheable) noexcept {
    if (!cacheable) {
        // THE REFUSAL. An iterative solve run to a sweep budget is a function of exactly which
        // regions were swept and how many times, so its result is not a function of the key — the
        // spike's `budget2` reproduced a full regeneration in 0 of 12 trials in all 12 of its
        // configurations. Storing it would serve it later as though it were the converged answer.
        refusal_ = CacheRefusal::NotCacheable;
        ++refusals_;
        return make_unexpected(
            Error{ErrorCode::PermissionDenied,
                  "pcg: this program has an iterative stage running to a sweep budget rather than "
                  "to convergence, so its region results are not cacheable"});
    }
    if (!key.is_valid()) {
        refusal_ = CacheRefusal::InvalidKey;
        ++refusals_;
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "pcg: a derivation key of zero is not a key"});
    }
    if (const usize* existing = index_.find(key.value); existing != nullptr) {
        return &entries_[*existing];
    }
    Expected<PointSet, Error> copy = points.clone();
    if (!copy) {
        return make_unexpected(copy.error());
    }
    const u64 cost = copy->bytes();
    if (capacity_ != 0 && bytes_ + cost > capacity_) {
        // No eviction here on purpose. A derived data cache's eviction policy is
        // `build-and-packaging`'s, and inventing a second one inside PCG is the parallel mechanism
        // this project keeps refusing to build. Refused and reported; the caller regenerates.
        refusal_ = CacheRefusal::OverCapacity;
        ++refusals_;
        return make_unexpected(
            Error{ErrorCode::OutOfMemory, "pcg: the region cache is at its declared capacity"});
    }
    Expected<CachedRegion*, Error> slot = entries_.emplace_back(*allocator_);
    if (!slot) {
        return make_unexpected(slot.error());
    }
    (*slot)->key = key;
    (*slot)->region = region;
    (*slot)->points = std::move(*copy);
    for (u64 digest : stage_digests) {
        if (Status pushed = (*slot)->stage_digests.push_back(digest); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    const usize index = entries_.size() - 1;
    if (Expected<usize*, Error> inserted = index_.insert(key.value, index); !inserted) {
        return make_unexpected(inserted.error());
    }
    bytes_ += cost;
    refusal_ = CacheRefusal::None;
    return &entries_[index];
}

const CachedRegion* RegionCache::find(DerivationKey key) const noexcept {
    const usize* slot = index_.find(key.value);
    if (slot == nullptr) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    return &entries_[*slot];
}

void RegionCache::clear() noexcept {
    entries_.clear();
    index_.clear();
    bytes_ = 0;
    refusal_ = CacheRefusal::None;
}

}  // namespace cy::pcg
