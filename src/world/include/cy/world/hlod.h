#pragma once
// World HLOD, and the representation tiers that stand in for content nobody is looking at. Task
// 3.6.
//
// --- WORLD HLOD IS NOT GEOMETRIC LOD ------------------------------------------------------------
//
// `world-partition-and-streaming` is explicit, and the distinction is worth restating because
// conflating the two is how an engine ends up with one system that does neither well:
//
//     virtual geometry reduces triangle detail WITHIN an object;
//     world HLOD replaces MANY OBJECTS with one aggregate.
//
// "Both SHALL exist and SHALL NOT be conflated." Two thousand buildings viewed from far away are
// one proxy, and neither their entities nor their assets are resident. That is a statement about
// what EXISTS, which is this layer's business; how many triangles the proxy draws is
// `virtual-geometry`'s.
//
// --- THE SWAP IS NOT A FADE ---------------------------------------------------------------------
//
// "The transition between an HLOD proxy and its real cells SHALL be driven by the same streaming
// state machine, and the proxy SHALL remain visible until the cells are published." And: "WHEN
// cells behind a proxy finish activating, THEN the proxy SHALL be replaced in the same frame the
// cells are published."
//
// So proxy visibility is a PURE FUNCTION of cell state — `visible == !activated` — evaluated in the
// same tick that publishes. There is no separate fade timer to get out of step with activation, and
// `swaps()` counts the transitions so a test can assert there was never a tick in which neither the
// proxy nor the cells were there.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/asset_id.h>
#include <cy/world/cell.h>

namespace cy::world {

/// An aggregate standing in for the content of one or more cells.
struct HlodProxy {
    /// The cell the proxy is filed under — typically a coarse-level cell whose region covers the
    /// finer cells it stands in for.
    CellId cell;
    AssetId asset;
    /// The proxy's own hierarchy level. Reported, not used for selection: selection is by state.
    u8 level = 0;
    bool visible = false;
    /// The covered cells' range in the registry's pool.
    u32 first_covered = 0;
    u32 covered_count = 0;
};

/// The proxies, and the rule that decides which are showing.
class HlodRegistry {
public:
    explicit HlodRegistry(Allocator& allocator) noexcept;

    /// Declare a proxy for `cell`, standing in for `covered` when they are not activated. A proxy
    /// with no covered cells stands in for its own cell, which is the common case.
    [[nodiscard]] Status declare(CellId cell, AssetId asset, u8 level,
                                 Span<const CellId> covered = {}) noexcept;

    /// Recompute visibility from cell state. `activated(cell)` answers whether a cell's entities
    /// are published; the proxy shows exactly while some cell it covers is not.
    ///
    /// Passed as a callable rather than reading a streaming object, so that the rule can be tested
    /// with no streaming system at all — which is what makes "the swap is not visible" a unit test.
    template <class ActivatedFn>
    [[nodiscard]] Status refresh(ActivatedFn&& activated) noexcept {
        for (HlodProxy& proxy : proxies_.span()) {
            bool all_activated = activated(proxy.cell);
            for (u32 index = 0; index < proxy.covered_count; ++index) {
                all_activated = all_activated && activated(covered_[proxy.first_covered + index]);
            }
            const bool should_show = !all_activated;
            if (should_show != proxy.visible) {
                proxy.visible = should_show;
                ++swaps_;
            }
        }
        return ok();
    }

    [[nodiscard]] const HlodProxy* find(CellId cell) const noexcept;
    [[nodiscard]] bool is_visible(CellId cell) const noexcept;
    [[nodiscard]] Span<const HlodProxy> proxies() const noexcept { return proxies_.span(); }
    [[nodiscard]] u32 visible_count() const noexcept;
    /// How many times a proxy has appeared or disappeared. The number a test asserts on.
    [[nodiscard]] u64 swaps() const noexcept { return swaps_; }

private:
    Array<HlodProxy> proxies_;
    Array<CellId> covered_;
    u64 swaps_ = 0;
};

/// How much of a thing exists. `world-partition-and-streaming`, "Representation tiers".
enum class RepresentationTier : u8 {
    /// Full entities.
    Full = 0,
    /// One entity representing a group.
    Aggregate,
    /// State without entities.
    Statistical,
};

[[nodiscard]] const char* representation_tier_name(RepresentationTier tier) noexcept;

/// A group that exists at some tier. The world owns WHETHER content exists and in what form;
/// `ai-system` owns HOW MUCH SIMULATION that form receives, and this struct carries nothing about
/// the second — the division is in the data, not only in the prose.
struct Representation {
    PersistentId id;
    RepresentationTier tier = RepresentationTier::Statistical;
    WorldPosition position;
    /// Gameplay-relevant state that promotion and demotion must preserve. A distant army that is
    /// demoted and later promoted has not silently changed strength.
    u32 population = 0;
    u64 gameplay_state = 0;
    /// How many individuals have been materialised from this group at `Full`.
    u32 materialised = 0;
};

/// Promotion and demotion between tiers, owned by the world.
class RepresentationTable {
public:
    explicit RepresentationTable(Allocator& allocator) noexcept;

    [[nodiscard]] Status add(const Representation& representation) noexcept;
    [[nodiscard]] const Representation* find(PersistentId id) const noexcept;

    /// Change a group's tier. Identity and gameplay-relevant state are carried across unchanged;
    /// only `materialised` moves, because that is what the tier MEANS.
    [[nodiscard]] Status set_tier(PersistentId id, RepresentationTier tier) noexcept;

    /// Where a group is now. Statistical groups still move — an army marching across an unloaded
    /// continent has a position and a destination without having a single entity.
    [[nodiscard]] Status set_position(PersistentId id, const WorldPosition& position) noexcept;

    [[nodiscard]] Span<const Representation> all() const noexcept { return groups_.span(); }

private:
    [[nodiscard]] Representation* mutable_find(PersistentId id) noexcept;

    Array<Representation> groups_;
};

}  // namespace cy::world
