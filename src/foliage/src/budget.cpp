// The allocation, and the declared order reduction walks it in. See budget.h for why there is no
// `BudgetSubsystem::Foliage` and why the protection is a place the walk does not reach.

#include <cy/foliage/budget.h>

#include <cy/core/math/scalar.h>

namespace cy::foliage {

const char* budget_axis_name(BudgetAxis axis) noexcept {
    switch (axis) {
        case BudgetAxis::InstanceCounts:
            return "instance-counts";
        case BudgetAxis::GroundCoverDensity:
            return "ground-cover-density";
        case BudgetAxis::GroundCoverDistance:
            return "ground-cover-distance";
        case BudgetAxis::DetailThresholds:
            return "detail-thresholds";
        case BudgetAxis::WindDetail:
            return "wind-detail";
        case BudgetAxis::InteractionResolution:
            return "interaction-resolution";
        case BudgetAxis::kCount:
            break;
    }
    return "unknown";
}

u32 FoliageSettings::allowed(SpeciesClass klass, u32 generated) const noexcept {
    const u32 index = static_cast<u32>(klass);
    if (index >= kSpeciesClassCount) {
        return generated;
    }
    const f32 fraction = math::clamp(instance_fraction[index], 0.0F, 1.0F);
    return static_cast<u32>(static_cast<f32>(generated) * fraction);
}

f32 FoliageLadder::cost_at(u8 position) const noexcept {
    const u8 usable = last_position();
    const f32 cost = relative_cost[position < usable ? position : usable];
    return cost > 0.0F ? cost : 1.0F;
}

u8 FoliageLadder::last_position() const noexcept {
    const u8 count = positions > 0 ? positions : 1U;
    const u8 capped = count < kMaxBudgetPositions ? count : static_cast<u8>(kMaxBudgetPositions);
    return static_cast<u8>(capped - 1U);
}

namespace {

/// What each axis looks like at each position. The whole of the reduction, in one table, so a
/// reader can see the ladder rather than reconstruct it from a chain of `if`s.
///
/// The numbers are multipliers on the RICHEST settings, not absolutes: a project that halves its
/// grass distance gets a ladder that still describes its own world.
struct AxisCurve {
    f32 scale[kMaxBudgetPositions];
};

constexpr AxisCurve kGroundCoverDistance{{1.0F, 0.7F, 0.45F, 0.25F, 0.12F}};
constexpr AxisCurve kGroundCoverDensity{{1.0F, 0.8F, 0.55F, 0.3F, 0.15F}};
constexpr AxisCurve kInteractionResolution{{1.0F, 1.0F, 0.5F, 0.5F, 0.25F}};
constexpr AxisCurve kDetailThresholds{{1.0F, 1.0F, 1.25F, 1.6F, 2.2F}};
/// Instance counts are the LAST thing to move, so the first three positions leave them alone.
constexpr AxisCurve kInstanceCounts{{1.0F, 1.0F, 1.0F, 0.75F, 0.5F}};

/// The wind ceiling per position. Not a scale: detail is an enumerator.
constexpr WindDetail kWindCeiling[kMaxBudgetPositions] = {
    WindDetail::Leaf, WindDetail::Leaf, WindDetail::Branch, WindDetail::Branch, WindDetail::Sway};

[[nodiscard]] f32 scale_at(const AxisCurve& curve, u8 position) noexcept {
    const u8 index = position < kMaxBudgetPositions ? position : kMaxBudgetPositions - 1;
    return curve.scale[index];
}

/// The positions at which an axis actually changes. What `steps()` reports, and what a test asserts
/// the ORDER of.
[[nodiscard]] bool axis_moves(BudgetAxis axis, u8 from, u8 to) noexcept {
    switch (axis) {
        case BudgetAxis::GroundCoverDistance:
            return scale_at(kGroundCoverDistance, from) != scale_at(kGroundCoverDistance, to);
        case BudgetAxis::GroundCoverDensity:
            return scale_at(kGroundCoverDensity, from) != scale_at(kGroundCoverDensity, to);
        case BudgetAxis::InteractionResolution:
            return scale_at(kInteractionResolution, from) != scale_at(kInteractionResolution, to);
        case BudgetAxis::DetailThresholds:
            return scale_at(kDetailThresholds, from) != scale_at(kDetailThresholds, to);
        case BudgetAxis::InstanceCounts:
            return scale_at(kInstanceCounts, from) != scale_at(kInstanceCounts, to);
        case BudgetAxis::WindDetail:
            return kWindCeiling[from < kMaxBudgetPositions ? from : kMaxBudgetPositions - 1] !=
                   kWindCeiling[to < kMaxBudgetPositions ? to : kMaxBudgetPositions - 1];
        case BudgetAxis::kCount:
            break;
    }
    return false;
}

}  // namespace

FoliageBudget::FoliageBudget(const FoliageSettings& richest) noexcept
    : richest_(richest), settings_(richest) {}

bool FoliageBudget::protected_from_reduction(const SpeciesDeclaration& species) noexcept {
    // `foliage` — "cover that matters tactically SHALL NOT vanish because the frame is busy." One
    // comparison, and `rebuild()` never reaches a protected species because `fraction_for()` is
    // what a publisher asks and it answers 1 before it looks at the class.
    return species.importance == GameplayImportance::Cover;
}

f32 FoliageBudget::fraction_for(const SpeciesDeclaration& species) const noexcept {
    if (protected_from_reduction(species)) {
        return 1.0F;
    }
    return static_cast<f32>(settings_.allowed(species.klass, 1000)) / 1000.0F;
}

void FoliageBudget::set_position(u8 position) noexcept {
    const u8 last = static_cast<u8>(kMaxBudgetPositions - 1);
    position_ = position < last ? position : last;
    rebuild();
}

namespace {

/// The settings one ladder position produces from a project's richest ones. Factored out because
/// BOTH the actuator and the PRICE are functions of it: a ladder whose prices were written as
/// constants beside the curves would describe some other project's world the moment this one
/// changed its grass distance, and `ladder()`'s own comment would stop being true.
[[nodiscard]] FoliageSettings settings_for(const FoliageSettings& richest, u8 position) noexcept {
    FoliageSettings settings = richest;
    settings.grass.distance_metres =
        richest.grass.distance_metres * scale_at(kGroundCoverDistance, position);
    settings.grass.density_scale =
        richest.grass.density_scale * scale_at(kGroundCoverDensity, position);
    settings.tier_threshold_scale =
        richest.tier_threshold_scale * scale_at(kDetailThresholds, position);
    settings.wind.ceiling =
        kWindCeiling[position < kMaxBudgetPositions ? position : kMaxBudgetPositions - 1];
    const f32 resolution = scale_at(kInteractionResolution, position);
    settings.interaction.trail_cells = static_cast<u32>(
        math::max(8.0F, static_cast<f32>(richest.interaction.trail_cells) * resolution));
    const f32 instances = scale_at(kInstanceCounts, position);
    for (u32 index = 0; index < kSpeciesClassCount; ++index) {
        settings.instance_fraction[index] = richest.instance_fraction[index] * instances;
    }
    return settings;
}

/// What one position's settings cost, relative to nothing. Ground cover dominates and scales with
/// AREA — halving the distance quarters the blades — which is why the first two rungs of the ladder
/// buy so much and why they are the first two the reduction order walks.
[[nodiscard]] f32 priced(const FoliageSettings& settings) noexcept {
    const f32 grass = settings.grass.distance_metres * settings.grass.distance_metres *
                      settings.grass.density_scale;
    f32 instances = 0.0F;
    for (const f32 fraction : settings.instance_fraction) {
        instances += fraction;
    }
    instances /= static_cast<f32>(kSpeciesClassCount);
    const f32 thresholds =
        settings.tier_threshold_scale > 0.0F ? 1.0F / settings.tier_threshold_scale : 1.0F;
    const f32 resolution = static_cast<f32>(settings.interaction.trail_cells);
    return (0.55F * grass) + (0.30F * instances) + (0.10F * thresholds) + (0.05F * resolution);
}

}  // namespace

void FoliageBudget::rebuild() noexcept {
    settings_ = settings_for(richest_, position_);

    steps_.count = 0;
    for (u8 step = 0; step < position_; ++step) {
        // `kReductionOrder` is walked at every step, so the report reads as the specification's
        // sentence does: ground cover first, instance counts last.
        for (BudgetAxis axis : kReductionOrder) {
            if (!axis_moves(axis, step, static_cast<u8>(step + 1))) {
                continue;
            }
            if (steps_.count >= kBudgetAxisCount * kMaxBudgetPositions) {
                break;
            }
            steps_.items[steps_.count] = ReductionStep{axis, step, static_cast<u8>(step + 1)};
            ++steps_.count;
        }
    }
}

FoliageLadder FoliageBudget::ladder() const noexcept {
    FoliageLadder ladder;
    ladder.positions = kMaxBudgetPositions;
    // The prices are DERIVED from the settings each position actually produces, against THIS
    // project's richest ones — not written as constants beside the curves. A project that halves
    // its ground-cover distance gets a ladder that still describes its own world, which is what
    // `rendering::QualityLadder`'s "relative to position 0" has to mean for the arbiter to allocate
    // against it.
    const f32 reference = priced(settings_for(richest_, 0));
    for (u8 position = 0; position < kMaxBudgetPositions; ++position) {
        const f32 cost = priced(settings_for(richest_, position));
        ladder.relative_cost[position] =
            reference > 0.0F ? math::max(0.02F, cost / reference) : 1.0F;
    }
    // Position 0 is 1.0 by construction, which the arbiter's contract requires of it.
    ladder.relative_cost[0] = 1.0F;
    return ladder;
}

}  // namespace cy::foliage
