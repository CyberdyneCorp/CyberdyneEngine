#pragma once
// The four attribution axes a memory report needs and did not have. M7 task 3.1.
//
// `core-memory-and-containers` — "Memory diagnostics": "Reporting SHALL be attributable along the
// axes that answer real questions: by domain, **by type, by thread, by world cell, and by asset** —
// so that 'why is this region consuming this much' is answerable."
//
// M1 built the domain axis (`domain.h`, `diagnostics.h`) and nothing else, and the tier record has
// said so since. The remaining four land here because M6 is the milestone that made the world cell
// exist: before it, "by world cell" had nothing to name.
//
// --- WHY THE AXES ARE OPAQUE INTEGERS, AND THIS IS NOT LAZINESS ----------------------------------
//
// This module is layer 0 and below `cy::core-values`, `cy::core-reflect` and `cy::world`, so it
// cannot name `AssetId`, a reflected `TypeId` or a `CellId` — and a memory layer that COULD would
// be a memory layer every one of those modules had to be built after. So an axis is a number, and
// the module that owns the identity is the one that pushes it. That is the same shape
// `AllocationTag` already has and the same shape `residency`'s declared levers have: the thing that
// knows declares.
//
// The asset axis is 128 bits because `cy::AssetId` is, and folding it to 64 would attribute two
// assets to one row at a rate nobody would ever notice was happening.
//
// --- THREAD IS CAPTURED, NOT DECLARED ------------------------------------------------------------
//
// The other three are things a caller knows and the allocator cannot. The thread is the reverse, so
// asking a caller to push it would be asking it to get it wrong.
//
// --- A SCOPE MERGES RATHER THAN REPLACES ---------------------------------------------------------
//
// A cell activation pushes a cell; the mesh loader inside it pushes an asset; the container inside
// that pushes a type. Each wants to add an axis without knowing what the other two are, so a field
// left zero INHERITS the enclosing scope's value and a non-zero one overrides it. Replacing
// wholesale would make the innermost scope responsible for restating everything above it, which is
// the version that goes wrong silently.

#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>

namespace cy {

/// Which axis a report groups by.
///
/// `Domain` is not here: `DomainStats` in `domain.h` already answers it for every allocation in
/// every build, and a second implementation of one axis is how two reports come to disagree.
enum class AttributionAxis : u8 {
    /// A reflected type's identity, as `cy::core-reflect` spells it.
    Type = 0,
    /// The thread that made the allocation. Captured, not declared.
    Thread = 1,
    /// A world cell's identifier, as `cy::world` spells it. Opaque here, by that module's own rule.
    WorldCell = 2,
    /// A `cy::AssetId`, both halves.
    Asset = 3,
};

[[nodiscard]] const char* attribution_axis_name(AttributionAxis axis) noexcept;

/// What a caller declares about the allocations made inside a scope.
///
/// Zero means "not stated", on every field. It is not a valid identity in any of the three modules
/// that supply one — a nil `AssetId`, a zero `CellId` and an unregistered type are all the absence
/// of the thing — so there is no value that is both a real key and a request to inherit.
struct MemoryAttribution {
    u64 type = 0;
    u64 world_cell = 0;
    u64 asset_high = 0;
    u64 asset_low = 0;

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return type == 0 && world_cell == 0 && asset_high == 0 && asset_low == 0;
    }

    /// This scope's values over `outer`'s, field by field. See the header's note on merging.
    [[nodiscard]] constexpr MemoryAttribution merged_over(
        const MemoryAttribution& outer) const noexcept {
        MemoryAttribution result = outer;
        if (type != 0) {
            result.type = type;
        }
        if (world_cell != 0) {
            result.world_cell = world_cell;
        }
        if (asset_high != 0 || asset_low != 0) {
            result.asset_high = asset_high;
            result.asset_low = asset_low;
        }
        return result;
    }

    friend constexpr bool operator==(const MemoryAttribution& a,
                                     const MemoryAttribution& b) noexcept {
        return a.type == b.type && a.world_cell == b.world_cell && a.asset_high == b.asset_high &&
               a.asset_low == b.asset_low;
    }
};

/// What is in force on this thread. Never a dangling reference: the value is a thread-local, and a
/// scope restores what it found.
[[nodiscard]] const MemoryAttribution& current_attribution() noexcept;

/// A small dense number for this thread, assigned on first use and stable for the thread's life.
///
/// A `std::thread::id` is neither small nor ordered in a way a report can group by, and the
/// operating system's thread id is not portable. This is one number per thread, and a report reads
/// as "thread 3 holds 40 MB" rather than as a pointer-shaped value.
[[nodiscard]] u32 current_thread_ordinal() noexcept;

/// How many thread ordinals have been handed out. The width of the thread axis.
[[nodiscard]] u32 thread_ordinal_count() noexcept;

/// Attribute every allocation made in this scope, merged over whatever was already in force.
class MemoryAttributionScope {
public:
    explicit MemoryAttributionScope(const MemoryAttribution& attribution) noexcept;
    ~MemoryAttributionScope();

    MemoryAttributionScope(const MemoryAttributionScope&) = delete;
    MemoryAttributionScope& operator=(const MemoryAttributionScope&) = delete;
    MemoryAttributionScope(MemoryAttributionScope&&) = delete;
    MemoryAttributionScope& operator=(MemoryAttributionScope&&) = delete;

private:
    MemoryAttribution previous_;
};

/// One row of a report: a key, and what is live under it.
struct MemoryAttributionRow {
    u64 key = 0;
    /// The second half of a 128-bit key. Zero on every axis but `Asset`.
    u64 key_high = 0;
    u64 live_bytes = 0;
    u64 live_allocations = 0;
};

/// What a report found, including what it could not fit.
///
/// `distinct_keys` against `rows` is how a caller knows its span was too small, and
/// `unreported_bytes` is what those keys held — because a report that quietly showed the biggest
/// eight rows and did not say there were forty is a report that answers "why is this region
/// consuming this much" with a number that does not add up.
struct MemoryAttributionSummary {
    u32 rows = 0;
    u32 distinct_keys = 0;
    u64 reported_bytes = 0;
    u64 unreported_bytes = 0;
    /// Live allocations whose key on this axis is zero — nobody declared it. Counted rather than
    /// dropped: "most of this is unattributed" is the most useful thing a first report can say.
    u64 unattributed_bytes = 0;
};

}  // namespace cy
