#pragma once
// The foliage allocation, and the five axes it is distributed across. M10 task 2.4.
//
// `foliage` — "Foliage budget": "Foliage SHALL hold an allocation from the renderer budget arbiter
// (see `rendering-architecture`) and DISTRIBUTE IT ACROSS: instance counts by species class, ground
// cover density and distance, detail tier thresholds, wind response detail, and interaction field
// resolution. Reduction SHALL proceed from LEAST TO MOST IMPORTANT, and species MAY declare a
// gameplay importance that protects them — cover that matters tactically SHALL NOT vanish because
// the frame is busy."
//
// ================================================================================================
// WHY THERE IS NO `BudgetSubsystem::Foliage`
// ================================================================================================
//
// `rendering::BudgetSubsystem` has seven members and they are `rendering-architecture`'s own list —
// "geometry, shadows, global illumination, reflections, material evaluation, VFX, post-processing".
// Adding an eighth would be a change to THAT capability's specification, made from inside a foliage
// module, which is not this milestone's change to make and is the same line `src/rendering/sky/`
// drew when it declined to add a ninth.
//
// So foliage is part of the GEOMETRY allocation and declares a PRICED LADDER — the shape
// `rendering::QualityLadder` takes — which `cy::foliage-render` hands to the arbiter as a composite
// position under `BudgetSubsystem::Geometry`. `virtual-shadows`' `ShadowLeverLadder` is the
// precedent named in `subsystem.h`: five levers folded into one composite position, because "which
// of its own levers a subsystem moves to fit an allocation is the subsystem's decision and nobody
// else can make it well".
//
// THIS FILE NAMES NO RENDERER TYPE. The ladder is `FoliageLadder`, the conversion is in cook.h, and
// that split is what keeps `cy::foliage` linkable in a dedicated server and in a cook.
//
// ================================================================================================
// "REDUCTION FROM LEAST TO MOST IMPORTANT" IS A DECLARED ORDER, NOT A HEURISTIC
// ================================================================================================
//
// `kReductionOrder` is the sequence, written once. Ground cover distance and density go first —
// "WHEN the foliage allocation is exceeded THEN ground cover distance and density SHALL be reduced
// BEFORE TREES ARE REMOVED" is the scenario, and it is the first two steps of the array. Instance
// counts go last, and within them `GameplayImportance::Cover` is never reduced at all: `reduce()`
// walks the classes from `Decorative` upward and stops before `Cover`, so the protection is a place
// the walk does not reach rather than a condition inside it.

#include <cy/core/base/types.h>
#include <cy/foliage/grass.h>
#include <cy/foliage/interaction.h>
#include <cy/foliage/species.h>
#include <cy/foliage/wind.h>

namespace cy::foliage {

/// The axes an allocation is distributed across. `foliage`'s own list, in its order.
enum class BudgetAxis : u8 {
    /// Instance counts by species class.
    InstanceCounts = 0,
    /// Ground cover density.
    GroundCoverDensity,
    /// Ground cover distance.
    GroundCoverDistance,
    /// Detail tier thresholds — the pixel sizes at which a tier gives way.
    DetailThresholds,
    /// Wind response detail.
    WindDetail,
    /// Interaction field resolution.
    InteractionResolution,
    kCount,
};

inline constexpr u32 kBudgetAxisCount = static_cast<u32>(BudgetAxis::kCount);

[[nodiscard]] const char* budget_axis_name(BudgetAxis axis) noexcept;

/// The order reduction walks. Least important first; see the header note.
inline constexpr BudgetAxis kReductionOrder[kBudgetAxisCount] = {
    BudgetAxis::GroundCoverDistance,   BudgetAxis::GroundCoverDensity,
    BudgetAxis::InteractionResolution, BudgetAxis::WindDetail,
    BudgetAxis::DetailThresholds,      BudgetAxis::InstanceCounts,
};

/// How many positions each axis has. Position 0 is the richest.
inline constexpr u32 kMaxBudgetPositions = 5;

/// The settings one budget position produces. What the rest of the module actually reads.
struct FoliageSettings {
    /// Fraction of a species class's generated instances that are drawn, per class. 1 draws all.
    f32 instance_fraction[kSpeciesClassCount] = {1.0F, 1.0F, 1.0F, 1.0F};
    GrassBudget grass;
    WindTuning wind;
    InteractionBounds interaction;
    /// Multiplier on every species' `tier_pixels`. Above 1 coarsens sooner.
    f32 tier_threshold_scale = 1.0F;

    /// The maximum instances a species class may draw given how many exist.
    [[nodiscard]] u32 allowed(SpeciesClass klass, u32 generated) const noexcept;
};

/// The priced ladder foliage declares. `relative_cost[p]` is what foliage costs at position `p` as
/// a fraction of what it costs at position 0 — the currency `rendering::QualityLadder` speaks, in
/// this module's own type so this module names no renderer.
struct FoliageLadder {
    u8 positions = 1;
    f32 relative_cost[kMaxBudgetPositions] = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F};

    [[nodiscard]] f32 cost_at(u8 position) const noexcept;
    [[nodiscard]] u8 last_position() const noexcept;
};

/// One step of reduction: which axis moved, from what position to what.
struct ReductionStep {
    BudgetAxis axis = BudgetAxis::GroundCoverDistance;
    u8 from = 0;
    u8 to = 0;
};

/// Holds the allocation and turns it into settings.
///
/// It does not measure frame time and has no way to: `rendering-architecture` forbids a controller
/// to, and there is no setter here through which one could arrive. `set_position()` is what the
/// arbiter grants.
class FoliageBudget {
public:
    explicit FoliageBudget(const FoliageSettings& richest) noexcept;

    /// The declared ladder. Five positions, priced by what each step actually removes; the prices
    /// are derived from the settings rather than guessed, so a project that changes the richest
    /// settings gets a ladder that still describes them.
    [[nodiscard]] FoliageLadder ladder() const noexcept;

    /// Move to a ladder position. Positions beyond the last are clamped, and the clamp is reported
    /// rather than silent — a caller asking for position 9 of 5 has a mapping bug.
    void set_position(u8 position) noexcept;
    [[nodiscard]] u8 position() const noexcept { return position_; }

    /// The settings at the current position.
    [[nodiscard]] const FoliageSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] const FoliageSettings& richest() const noexcept { return richest_; }

    /// The steps taken between position 0 and the current position, in `kReductionOrder`. What a
    /// diagnostic shows and what a test asserts the ORDER of.
    [[nodiscard]] Span<const ReductionStep> steps() const noexcept { return steps_.span(); }

    /// Whether a species is protected from budget pressure. `GameplayImportance::Cover` always is.
    [[nodiscard]] static bool protected_from_reduction(const SpeciesDeclaration& species) noexcept;

    /// The instance fraction a species gets: the class's fraction, or 1 when the species is
    /// protected. Public because "gameplay-relevant cover persists" is a claim about exactly this
    /// function and a test should be able to ask it about one species.
    [[nodiscard]] f32 fraction_for(const SpeciesDeclaration& species) const noexcept;

private:
    void rebuild() noexcept;

    FoliageSettings richest_;
    FoliageSettings settings_;
    u8 position_ = 0;
    /// Fixed capacity: the walk takes at most one step per axis per position, and a heap
    /// allocation inside a per-frame budget update would be a per-frame allocation.
    struct StepArray {
        ReductionStep items[kBudgetAxisCount * kMaxBudgetPositions];
        u32 count = 0;

        [[nodiscard]] Span<const ReductionStep> span() const noexcept { return {items, count}; }
    };
    StepArray steps_;
};

}  // namespace cy::foliage
