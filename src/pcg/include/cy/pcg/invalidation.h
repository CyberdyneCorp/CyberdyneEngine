#pragma once
// Regions, dirty sets, and the fixed point. M10 task 4.3, and the half of it the spike decided.
//
// `procedural-content-generation` — "Dependencies and spatial invalidation": "An edit SHALL produce
// a dirty set: the regions of each dependent generator whose inputs the edit touched, expanded by
// each consumer's declared sampling radius. Regeneration SHALL process only the dirty set ...
// The dependency graph SHALL be inspectable, and THE REASON A REGION IS DIRTY SHALL BE REPORTABLE."
//
// ================================================================================================
// THE DECLARED RADIUS IS NOT ENOUGH, AND THE SPIKE MEASURED HOW MUCH NOT ENOUGH
// ================================================================================================
//
// design.md §1.2. Twelve trials, six edit sites, two edit shapes, a different seed each time,
// against a full regeneration of the same seed compared for output AND for generated identity:
//
//   invalidation expanded to a FIXED POINT   reproduces 12 of 12
//   the DECLARED edges applied once          reproduces  7 of 12
//
// Seven of twelve is the dangerous cell and the reason this file exists in the shape it does: a
// one-edit, one-seed test would have called a statically-closed invalidation sound MORE OFTEN THAN
// NOT. So the only closure this module offers is the fixed point —
// `Generator::expand_after_sweep()` in execute.cpp, built on the sets below: a region whose output
// digest CHANGED re-dirties everything that reads it, and that repeats until nothing new changes.
// `tests/test_invalidation.cpp` is written against the transitive edge specifically —
// `NodeKind::Propagate`, where a region declares a radius of one and water crosses the world — and
// it was watched to go red with the re-expansion removed.
//
// ================================================================================================
// AND THE COST IS IN THE LONG-RANGE GATHER, NOT IN THE TRANSITIVE EDGE
// ================================================================================================
//
// design.md §1.3, which moved the worry the risk register had recorded. Mean regions of 576, over
// twelve trials, per node: the TRANSITIVE edge changed 19 regions and cost 40 to find them, which
// is a normal price for a sound invalidation. The LONG-RANGE GATHER changed ONE and cost 179 — 31%
// of the world — because a declared reach of 2 composed with a declared reach of 3 dilates the
// dirty set by 5 in every direction before anything is evaluated.
//
// That is a fact about long-range gathers rather than about one node, and the answer is a
// PROVENANCE RECORD OF WHAT EACH REGION ACTUALLY READ rather than a wider radius. `ReadLedger`
// below is that record: a region's evaluation reports the regions it truly touched, and the next
// invalidation dilates by the recorded reads where it has them and by the declared reach where it
// does not. The declared reach stays the conservative bound for a region never yet evaluated, which
// is what keeps the first generation correct.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash.h>
#include <cy/core/memory/hash_map.h>
#include <cy/pcg/identity.h>

namespace cy::pcg {

/// A region's integer coordinate at a generation level. Level 0 is the finest, matching
/// `world::CellCoord` and `foliage::ClusterCoord` — but the SIZE of a region is the generator's own
/// declaration and need not match a world cell, which is the specification's requirement in as many
/// words.
struct RegionCoord {
    i32 x = 0;
    i32 z = 0;
    u8 level = 0;

    friend constexpr bool operator==(const RegionCoord&, const RegionCoord&) noexcept = default;
};

/// The generated world's bounds, in regions.
///
/// A bounded extent is not a convenience: the fixed point below terminates because the region set
/// it expands over is finite, and an unbounded world would need a different termination argument.
/// A planet-scale generator declares a large extent and pays nothing for it — the sets are sparse.
struct RegionExtent {
    i32 min_x = 0;
    i32 min_z = 0;
    i32 max_x = 0;  ///< Inclusive.
    i32 max_z = 0;
    u8 level = 0;

    [[nodiscard]] constexpr bool contains(const RegionCoord& region) const noexcept {
        return region.level == level && region.x >= min_x && region.x <= max_x &&
               region.z >= min_z && region.z <= max_z;
    }
    [[nodiscard]] constexpr i64 width() const noexcept {
        return static_cast<i64>(max_x) - static_cast<i64>(min_x) + 1;
    }
    [[nodiscard]] constexpr i64 depth() const noexcept {
        return static_cast<i64>(max_z) - static_cast<i64>(min_z) + 1;
    }
    [[nodiscard]] constexpr i64 count() const noexcept { return width() * depth(); }
};

/// A set of regions over one extent. A byte per region, which for a 576-region world is 576 bytes
/// and for a hundred-kilometre world at 256 m regions is 152 KiB — cheap enough that the fixed
/// point's repeated unions are memory traffic rather than allocation.
class RegionSet {
public:
    RegionSet(Allocator& allocator, const RegionExtent& extent) noexcept
        : bits_(allocator), extent_(extent) {}

    RegionSet(const RegionSet&) = delete;
    RegionSet& operator=(const RegionSet&) = delete;
    RegionSet(RegionSet&&) noexcept = default;
    RegionSet& operator=(RegionSet&&) noexcept = default;

    [[nodiscard]] Status resize() noexcept;

    [[nodiscard]] bool contains(const RegionCoord& region) const noexcept;
    [[nodiscard]] Status add(const RegionCoord& region) noexcept;
    void remove(const RegionCoord& region) noexcept;
    void clear() noexcept;

    /// Every member, in a canonical order: row-major over the extent. Deterministic regardless of
    /// insertion order, which is what lets a regeneration's traversal be a function of the set
    /// rather than of the history that built it.
    [[nodiscard]] Status members(Array<RegionCoord>& out) const noexcept;

    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] const RegionExtent& extent() const noexcept { return extent_; }

    /// Union in place. Returns how many regions were newly added, which is the fixed point's own
    /// termination test.
    [[nodiscard]] Expected<usize, Error> unite(const RegionSet& other) noexcept;

    [[nodiscard]] Expected<RegionSet, Error> clone() const noexcept;

private:
    [[nodiscard]] i64 index_of(const RegionCoord& region) const noexcept;

    Array<u8> bits_;
    RegionExtent extent_;
};

/// Grow `set` by `radius` regions in the Chebyshev metric, clamped to the extent.
[[nodiscard]] Expected<RegionSet, Error> dilate(Allocator& allocator, const RegionSet& set,
                                                u8 radius) noexcept;

// --- Why is this region dirty --------------------------------------------------------------------

/// What put a region in the dirty set. `procedural-content-generation`: "WHEN a region regenerates
/// THEN the tooling SHALL name the change and the dependency path that reached it."
enum class DirtyCause : u8 {
    /// An authored edit's own footprint. The root of every dependency path.
    AuthoredEdit = 0,
    /// A field this stage reads changed here.
    FieldChange,
    /// An upstream stage's output changed in this region.
    UpstreamStage,
    /// A neighbour within this stage's DECLARED reach changed.
    DeclaredReach,
    /// A neighbour this region was RECORDED to have read changed — the provenance path, narrower
    /// than the declared reach. §1.3's answer to the long-range gather.
    RecordedRead,
    /// The FIXED POINT re-expanded: this region reads a region whose output turned out to change
    /// after it had already been evaluated. The 7-of-12 cell's missing edge.
    FixedPointExpansion,
    /// The generator version or the compiled program changed, so nothing cached applies.
    ProgramChanged,
    kCount,
};

[[nodiscard]] const char* dirty_cause_name(DirtyCause cause) noexcept;

/// One link of a dependency path.
struct DirtyReason {
    RegionCoord region;
    /// The stage that was dirtied. `Program::kNoStage` for a cause that precedes every stage.
    u8 stage = 0xFF;
    const char* stage_name = "";
    DirtyCause cause = DirtyCause::AuthoredEdit;
    /// The region the cause came from. Equal to `region` for an authored edit.
    RegionCoord source;
    /// How many links from the authored edit. Zero for the edit itself.
    u16 hop = 0;
};

/// The record of one regeneration's invalidation, and the answer to "why is this dirty".
///
/// Retained in development builds and strippable: `enabled` false records nothing and allocates
/// nothing, which is the same arrangement `diagnostics.h` uses for provenance and for the same
/// requirement — a shipping build pays for neither.
class InvalidationLedger {
public:
    explicit InvalidationLedger(Allocator& allocator) noexcept : reasons_(allocator) {}

    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    [[nodiscard]] Status record(const DirtyReason& reason) noexcept;
    void clear() noexcept { reasons_.clear(); }

    /// Every reason recorded for `region`, oldest first — the dependency path that reached it.
    [[nodiscard]] Status why_dirty(const RegionCoord& region,
                                   Array<DirtyReason>& out) const noexcept;

    [[nodiscard]] Span<const DirtyReason> reasons() const noexcept { return reasons_.span(); }
    [[nodiscard]] usize size() const noexcept { return reasons_.size(); }

    /// How many reasons of each cause were recorded. The number §1.3's finding is read off:
    /// `DeclaredReach` far above `RecordedRead` is a generator whose dirty sets are dominated by a
    /// declared radius nothing actually reads.
    [[nodiscard]] u32 count_of(DirtyCause cause) const noexcept;

private:
    Array<DirtyReason> reasons_;
    bool enabled_ = true;
};

// --- What a region actually read --------------------------------------------------------------

/// The regions one region's evaluation of one stage truly touched.
///
/// design.md §1.3's answer to the long-range gather: a declared reach of 2 composed with a declared
/// reach of 3 invalidates 179 regions of 576 to find one that changed, and the fix is not a wider
/// or narrower declaration but a record of what was read. A region with no record yet falls back to
/// the declared reach, which is what keeps a first generation correct.
class ReadLedger {
public:
    explicit ReadLedger(Allocator& allocator) noexcept : reads_(allocator), keys_(allocator) {}

    /// Begin recording for (`stage`, `region`). Clears whatever was recorded for it before.
    [[nodiscard]] Status begin(u8 stage, const RegionCoord& region) noexcept;
    /// Record that the region being evaluated read `source`.
    [[nodiscard]] Status read(const RegionCoord& source) noexcept;
    void end() noexcept { open_ = kNoOpen; }

    /// The regions recorded for (`stage`, `region`), or an empty span when nothing was recorded —
    /// which the caller must read as "fall back to the declared reach", never as "read nothing".
    [[nodiscard]] Span<const RegionCoord> reads_of(u8 stage,
                                                   const RegionCoord& region) const noexcept;
    [[nodiscard]] bool has_record(u8 stage, const RegionCoord& region) const noexcept;

    void clear() noexcept;
    [[nodiscard]] usize records() const noexcept { return keys_.size(); }

private:
    static constexpr usize kNoOpen = static_cast<usize>(-1);

    struct Record {
        u8 stage = 0;
        RegionCoord region;
        Array<RegionCoord> sources;

        explicit Record(Allocator& allocator) noexcept : sources(allocator) {}
    };

    [[nodiscard]] static u64 key_of(u8 stage, const RegionCoord& region) noexcept;
    [[nodiscard]] usize find(u8 stage, const RegionCoord& region) const noexcept;

    Array<Record> reads_;
    HashMap<u64, usize> keys_;
    usize open_ = kNoOpen;
};

}  // namespace cy::pcg
