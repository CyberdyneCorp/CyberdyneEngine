// Region sets, dirty-set dilation, the dependency ledger, and the record of what a region read.
// See include/cy/pcg/invalidation.h.

#include <cy/pcg/invalidation.h>

namespace cy::pcg {

const char* dirty_cause_name(DirtyCause cause) noexcept {
    switch (cause) {
        case DirtyCause::AuthoredEdit:
            return "authored-edit";
        case DirtyCause::FieldChange:
            return "field-change";
        case DirtyCause::UpstreamStage:
            return "upstream-stage";
        case DirtyCause::DeclaredReach:
            return "declared-reach";
        case DirtyCause::RecordedRead:
            return "recorded-read";
        case DirtyCause::FixedPointExpansion:
            return "fixed-point-expansion";
        case DirtyCause::ProgramChanged:
            return "program-changed";
        case DirtyCause::kCount:
            break;
    }
    return "unknown";
}

// --- RegionSet --------------------------------------------------------------------------------

Status RegionSet::resize() noexcept {
    const i64 cells = extent_.count();
    if (cells <= 0) {
        return Status{make_unexpected(Error{ErrorCode::InvalidArgument,
                                            "pcg: a region extent must contain at least one "
                                            "region; max is inclusive of min"})};
    }
    if (Status sized = bits_.resize(static_cast<usize>(cells)); !sized) {
        return sized;
    }
    clear();
    return ok();
}

i64 RegionSet::index_of(const RegionCoord& region) const noexcept {
    if (!extent_.contains(region)) {
        return -1;
    }
    return (static_cast<i64>(region.z) - extent_.min_z) * extent_.width() +
           (static_cast<i64>(region.x) - extent_.min_x);
}

bool RegionSet::contains(const RegionCoord& region) const noexcept {
    const i64 index = index_of(region);
    return index >= 0 && static_cast<usize>(index) < bits_.size() &&
           bits_[static_cast<usize>(index)] != 0;
}

Status RegionSet::add(const RegionCoord& region) noexcept {
    const i64 index = index_of(region);
    if (index < 0 || static_cast<usize>(index) >= bits_.size()) {
        // Outside the extent is not an error: a dilation at the world edge legitimately reaches
        // past it, and silently clamping is what "clamped to the extent" means. A caller that needs
        // to know asks `extent().contains()` first.
        return ok();
    }
    bits_[static_cast<usize>(index)] = 1;
    return ok();
}

void RegionSet::remove(const RegionCoord& region) noexcept {
    const i64 index = index_of(region);
    if (index >= 0 && static_cast<usize>(index) < bits_.size()) {
        bits_[static_cast<usize>(index)] = 0;
    }
}

void RegionSet::clear() noexcept {
    for (u8& bit : bits_) {
        bit = 0;
    }
}

Status RegionSet::members(Array<RegionCoord>& out) const noexcept {
    out.clear();
    // Row-major over the extent, which is a canonical order and therefore a function of the SET
    // rather than of the history that built it. A traversal order that depended on insertion would
    // put the whole of this module's reproducibility at the mercy of the caller's loop.
    for (i64 z = extent_.min_z; z <= extent_.max_z; ++z) {
        for (i64 x = extent_.min_x; x <= extent_.max_x; ++x) {
            const RegionCoord region{static_cast<i32>(x), static_cast<i32>(z), extent_.level};
            if (!contains(region)) {
                continue;
            }
            if (Status pushed = out.push_back(region); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

usize RegionSet::size() const noexcept {
    usize total = 0;
    for (u8 bit : bits_) {
        total += bit != 0 ? 1U : 0U;
    }
    return total;
}

Expected<usize, Error> RegionSet::unite(const RegionSet& other) noexcept {
    if (other.bits_.size() != bits_.size()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "pcg: two region sets over different extents cannot be united"});
    }
    usize added = 0;
    for (usize index = 0; index < bits_.size(); ++index) {
        if (other.bits_[index] != 0 && bits_[index] == 0) {
            bits_[index] = 1;
            ++added;
        }
    }
    return added;
}

Expected<RegionSet, Error> RegionSet::clone() const noexcept {
    RegionSet copy(bits_.allocator(), extent_);
    if (Status sized = copy.bits_.resize(bits_.size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (usize index = 0; index < bits_.size(); ++index) {
        copy.bits_[index] = bits_[index];
    }
    return copy;
}

Expected<RegionSet, Error> dilate(Allocator& allocator, const RegionSet& set, u8 radius) noexcept {
    Expected<RegionSet, Error> grown = set.clone();
    if (!grown || radius == 0) {
        return grown;
    }
    Array<RegionCoord> seeds(allocator);
    if (Status listed = set.members(seeds); !listed) {
        return make_unexpected(listed.error());
    }
    const i32 reach = static_cast<i32>(radius);
    for (const RegionCoord& seed : seeds) {
        for (i32 dz = -reach; dz <= reach; ++dz) {
            for (i32 dx = -reach; dx <= reach; ++dx) {
                const RegionCoord neighbour{seed.x + dx, seed.z + dz, seed.level};
                if (Status added = grown->add(neighbour); !added) {
                    return make_unexpected(added.error());
                }
            }
        }
    }
    return grown;
}

// --- InvalidationLedger -----------------------------------------------------------------------

Status InvalidationLedger::record(const DirtyReason& reason) noexcept {
    if (!enabled_) {
        // Checked BEFORE the record is built, so a shipping build pays neither the memory nor the
        // store. diagnostics.h makes the same argument for provenance and the suite measures it.
        return ok();
    }
    return reasons_.push_back(reason);
}

Status InvalidationLedger::why_dirty(const RegionCoord& region,
                                     Array<DirtyReason>& out) const noexcept {
    out.clear();
    for (const DirtyReason& reason : reasons_) {
        if (reason.region == region) {
            if (Status pushed = out.push_back(reason); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

u32 InvalidationLedger::count_of(DirtyCause cause) const noexcept {
    u32 total = 0;
    for (const DirtyReason& reason : reasons_) {
        total += reason.cause == cause ? 1U : 0U;
    }
    return total;
}

// --- ReadLedger -------------------------------------------------------------------------------

u64 ReadLedger::key_of(u8 stage, const RegionCoord& region) noexcept {
    // A packing is legitimate here and not in `region_key()`: this key indexes a map inside one
    // process for the life of one run, so it has to be unique rather than stable, and a collision
    // would be a wrong read record rather than a wrong instance identity. The coordinates are
    // masked to 24 bits, which covers a world 16 million regions across.
    const u64 x = static_cast<u64>(static_cast<u32>(region.x)) & 0xFFFFFFULL;
    const u64 z = static_cast<u64>(static_cast<u32>(region.z)) & 0xFFFFFFULL;
    return (x << 40U) | (z << 16U) | (static_cast<u64>(region.level) << 8U) |
           static_cast<u64>(stage);
}

usize ReadLedger::find(u8 stage, const RegionCoord& region) const noexcept {
    const usize* slot = keys_.find(key_of(stage, region));
    return slot != nullptr ? *slot : kNoOpen;
}

Status ReadLedger::begin(u8 stage, const RegionCoord& region) noexcept {
    const usize existing = find(stage, region);
    if (existing != kNoOpen) {
        reads_[existing].sources.clear();
        open_ = existing;
        return ok();
    }
    Expected<Record*, Error> slot = reads_.emplace_back(reads_.allocator());
    if (!slot) {
        return Status{make_unexpected(slot.error())};
    }
    (*slot)->stage = stage;
    (*slot)->region = region;
    const usize index = reads_.size() - 1;
    if (Expected<usize*, Error> inserted = keys_.insert(key_of(stage, region), index); !inserted) {
        return Status{make_unexpected(inserted.error())};
    }
    open_ = index;
    return ok();
}

Status ReadLedger::read(const RegionCoord& source) noexcept {
    if (open_ == kNoOpen) {
        return ok();
    }
    Array<RegionCoord>& sources = reads_[open_].sources;
    for (const RegionCoord& existing : sources) {
        if (existing == source) {
            return ok();
        }
    }
    return sources.push_back(source);
}

Span<const RegionCoord> ReadLedger::reads_of(u8 stage, const RegionCoord& region) const noexcept {
    const usize index = find(stage, region);
    if (index == kNoOpen) {
        return {};
    }
    return reads_[index].sources.span();
}

bool ReadLedger::has_record(u8 stage, const RegionCoord& region) const noexcept {
    return find(stage, region) != kNoOpen;
}

void ReadLedger::clear() noexcept {
    reads_.clear();
    keys_.clear();
    open_ = kNoOpen;
}

}  // namespace cy::pcg
