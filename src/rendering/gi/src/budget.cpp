#include <cy/rendering/gi/budget.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <ranges>

namespace cy::rendering::gi {
namespace {

/// The share of the allocation each lever's axis is modelled to cost at position 0. They sum to
/// one, which is what makes `predicted_cost_ms` a millisecond figure rather than an index.
constexpr f32 kLeverShare[kGiLeverCount] = {
    0.28F,  // ProbeUpdates
    0.24F,  // TracedRays
    0.14F,  // TracingResolution
    0.14F,  // SurfaceCacheRate
    0.06F,  // ProbeDensity
    0.08F,  // ReflectionResolution
    0.06F,  // DenoiserQuality
};

}  // namespace

const char* gi_lever_name(GiLever lever) noexcept {
    switch (lever) {
        case GiLever::ProbeUpdates:
            return "ProbeUpdates";
        case GiLever::TracedRays:
            return "TracedRays";
        case GiLever::TracingResolution:
            return "TracingResolution";
        case GiLever::SurfaceCacheRate:
            return "SurfaceCacheRate";
        case GiLever::ProbeDensity:
            return "ProbeDensity";
        case GiLever::ReflectionResolution:
            return "ReflectionResolution";
        case GiLever::DenoiserQuality:
            return "DenoiserQuality";
        case GiLever::Count:
            break;
    }
    return "Unknown";
}

GiLeverSchedule default_schedule(GiLever lever) noexcept {
    GiLeverSchedule schedule;
    schedule.declared = true;
    switch (lever) {
        case GiLever::ProbeUpdates:
            // Probes updated per frame. The largest single cost and the one with the most room.
            schedule.position_count = 4;
            schedule.value[0] = 256.0F;
            schedule.value[1] = 128.0F;
            schedule.value[2] = 64.0F;
            schedule.value[3] = 32.0F;
            schedule.relative_cost[0] = 1.00F;
            schedule.relative_cost[1] = 0.50F;
            schedule.relative_cost[2] = 0.25F;
            schedule.relative_cost[3] = 0.125F;
            schedule.reduction_order = 3;
            break;
        case GiLever::TracedRays:
            schedule.position_count = 4;
            schedule.value[0] = 64.0F;
            schedule.value[1] = 32.0F;
            schedule.value[2] = 16.0F;
            schedule.value[3] = 8.0F;
            schedule.relative_cost[0] = 1.00F;
            schedule.relative_cost[1] = 0.50F;
            schedule.relative_cost[2] = 0.25F;
            schedule.relative_cost[3] = 0.125F;
            schedule.reduction_order = 4;
            break;
        case GiLever::TracingResolution:
            schedule.position_count = 3;
            schedule.value[0] = 1.00F;
            schedule.value[1] = 0.71F;
            schedule.value[2] = 0.50F;
            schedule.relative_cost[0] = 1.00F;
            schedule.relative_cost[1] = 0.50F;
            schedule.relative_cost[2] = 0.25F;
            schedule.reduction_order = 5;
            break;
        case GiLever::SurfaceCacheRate:
            schedule.position_count = 3;
            schedule.value[0] = 512.0F;
            schedule.value[1] = 256.0F;
            schedule.value[2] = 128.0F;
            schedule.relative_cost[0] = 1.00F;
            schedule.relative_cost[1] = 0.50F;
            schedule.relative_cost[2] = 0.25F;
            schedule.reduction_order = 2;
            break;
        case GiLever::ProbeDensity:
            // Density is the last of the caches to move: a probe that is not there cannot be
            // updated later, so this one costs convergence rather than a frame.
            schedule.position_count = 2;
            schedule.value[0] = 1.00F;
            schedule.value[1] = 0.50F;
            schedule.relative_cost[0] = 1.00F;
            schedule.relative_cost[1] = 0.50F;
            schedule.reduction_order = 6;
            break;
        case GiLever::ReflectionResolution:
            schedule.position_count = 3;
            schedule.value[0] = 1.00F;
            schedule.value[1] = 0.50F;
            schedule.value[2] = 0.25F;
            schedule.relative_cost[0] = 1.00F;
            schedule.relative_cost[1] = 0.50F;
            schedule.relative_cost[2] = 0.25F;
            schedule.reduction_order = 1;
            break;
        case GiLever::DenoiserQuality:
            // First to reduce: it costs residual noise, which the temporal accumulation partly
            // hides, and it is the cheapest thing to give back.
            schedule.position_count = 4;
            schedule.value[0] = 0.0F;
            schedule.value[1] = 1.0F;
            schedule.value[2] = 2.0F;
            schedule.value[3] = 3.0F;
            schedule.relative_cost[0] = 1.00F;
            schedule.relative_cost[1] = 0.75F;
            schedule.relative_cost[2] = 0.18F;
            schedule.relative_cost[3] = 0.09F;
            schedule.reduction_order = 0;
            break;
        case GiLever::Count:
            schedule.declared = false;
            break;
    }
    return schedule;
}

GiBudget::GiBudget() noexcept {
    for (u32 index = 0; index < kGiLeverCount; ++index) {
        schedules_[index] = default_schedule(static_cast<GiLever>(index));
        positions_[index] = 0;
    }
}

void GiBudget::configure(const GiBudgetSettings& settings) noexcept {
    settings_ = settings;
}

Status GiBudget::declare(GiLever lever, const GiLeverSchedule& schedule) noexcept {
    if (schedule.position_count == 0 || schedule.position_count > kMaxLeverPositions) {
        return fail(ErrorCode::InvalidArgument,
                    "GiBudget::declare: a ladder has between one and kMaxLeverPositions positions");
    }
    if (schedule.declared && std::abs(schedule.relative_cost[0] - 1.0F) > 1.0e-4F) {
        return fail(ErrorCode::InvalidArgument,
                    "GiBudget::declare: position 0 is authored quality and costs 1.0 by "
                    "construction; every other cost is relative to it");
    }
    for (u32 position = 1; position < schedule.position_count; ++position) {
        if (schedule.relative_cost[position] > schedule.relative_cost[position - 1]) {
            return fail(ErrorCode::InvalidArgument,
                        "GiBudget::declare: the ladder is ordered coarsest last, so cost may not "
                        "rise along it");
        }
    }
    schedules_[static_cast<u32>(lever)] = schedule;
    positions_[static_cast<u32>(lever)] = 0;
    return ok();
}

const GiLeverSchedule& GiBudget::schedule(GiLever lever) const noexcept {
    return schedules_[static_cast<u32>(lever)];
}

void GiBudget::set_allocation_ms(f32 allocation_ms) noexcept {
    settings_.allocation_ms = std::max(allocation_ms, 0.0F);
}

f32 GiBudget::value(GiLever lever) const noexcept {
    const GiLeverSchedule& schedule = schedules_[static_cast<u32>(lever)];
    return schedule
        .value[std::min(positions_[static_cast<u32>(lever)], schedule.position_count - 1)];
}

f32 GiBudget::cost_at(GiLever lever, u32 position) const noexcept {
    const GiLeverSchedule& schedule = schedules_[static_cast<u32>(lever)];
    if (!schedule.declared) {
        return kLeverShare[static_cast<u32>(lever)] * settings_.allocation_ms;
    }
    const u32 clamped = std::min(position, schedule.position_count - 1);
    return kLeverShare[static_cast<u32>(lever)] * settings_.allocation_ms *
           schedule.relative_cost[clamped];
}

f32 GiBudget::predicted_cost_ms() const noexcept {
    f32 total = 0.0F;
    for (u32 index = 0; index < kGiLeverCount; ++index) {
        total += cost_at(static_cast<GiLever>(index), positions_[index]);
    }
    return total;
}

bool GiBudget::at_reserved_minimum() const noexcept {
    for (u32 index = 0; index < kGiLeverCount; ++index) {
        const GiLeverSchedule& schedule = schedules_[index];
        if (schedule.declared && positions_[index] + 1 < schedule.position_count) {
            return false;
        }
    }
    return true;
}

void GiBudget::reset_positions() noexcept {
    for (u32& position : positions_) {
        position = 0;
    }
    filter_primed_ = false;
    changed_once_ = false;
}

bool GiBudget::tighten_one(GiBudgetReport& report) noexcept {
    // Ascending reduction order, one step from the first lever that has one. Never a scale over
    // every lever — see the header, and design.md §2.3.
    u32 best = kGiLeverCount;
    u32 best_order = ~0U;
    for (u32 index = 0; index < kGiLeverCount; ++index) {
        const GiLeverSchedule& schedule = schedules_[index];
        if (!schedule.declared || positions_[index] + 1 >= schedule.position_count) {
            continue;
        }
        if (schedule.reduction_order < best_order) {
            best_order = schedule.reduction_order;
            best = index;
        }
    }
    if (best == kGiLeverCount) {
        return false;
    }
    positions_[best] += 1;
    report.levers_tightened += 1;
    report.last_lever = static_cast<GiLever>(best);
    return true;
}

bool GiBudget::relax_one(GiBudgetReport& report) noexcept {
    // The reverse walk: the lever that reduced last is the one that comes back first.
    u32 best = kGiLeverCount;
    u32 best_order = 0;
    bool found = false;
    for (u32 index = 0; index < kGiLeverCount; ++index) {
        const GiLeverSchedule& schedule = schedules_[index];
        if (!schedule.declared || positions_[index] == 0) {
            continue;
        }
        if (!found || schedule.reduction_order > best_order) {
            best_order = schedule.reduction_order;
            best = index;
            found = true;
        }
    }
    if (!found) {
        return false;
    }
    positions_[best] -= 1;
    report.levers_relaxed += 1;
    report.last_lever = static_cast<GiLever>(best);
    return true;
}

/// Cover `gain` of the deficit, one step per lever, walking the declared reduction order.
void GiBudget::tighten_to_cover(f32 deficit, u64 frame, GiBudgetReport& report) noexcept {
    f32 covered = 0.0F;
    const f32 target = deficit * math::clamp(settings_.gain, 0.01F, 2.0F);
    while (covered < target) {
        const f32 before = predicted_cost_ms();
        if (!tighten_one(report)) {
            break;
        }
        covered += before - predicted_cost_ms();
    }
    if (report.levers_tightened != 0) {
        last_change_frame_ = frame;
        changed_once_ = true;
    }
}

/// One step back up, all or nothing, and only when the step's predicted increase fits under the
/// relax margin.
///
/// A partial grant that cannot buy the step is the worst of both — design.md §2.5 item 3: it spends
/// the surplus and changes nothing, and the loop then creeps until the step fires at a moment
/// nothing chose. So a step that does not fit is put back.
void GiBudget::try_relax(u64 frame, GiBudgetReport& report) noexcept {
    const bool dwell_elapsed =
        !changed_once_ || frame >= last_change_frame_ + settings_.relax_dwell_frames;
    if (!dwell_elapsed) {
        return;
    }

    const f32 before = predicted_cost_ms();
    if (!relax_one(report)) {
        return;
    }
    const f32 predicted = filtered_ms_ + (predicted_cost_ms() - before);
    if (predicted > settings_.allocation_ms * settings_.relax_margin) {
        for (u32 index = 0; index < kGiLeverCount; ++index) {
            if (static_cast<GiLever>(index) == report.last_lever) {
                positions_[index] += 1;
                break;
            }
        }
        report.levers_relaxed = 0;
        report.last_lever = GiLever::Count;
        return;
    }
    last_change_frame_ = frame;
    changed_once_ = true;
}

/// True when a lever is down. The condition behind `relaxation_withheld`.
bool GiBudget::any_lever_reduced() const noexcept {
    return std::ranges::any_of(positions_, [](u32 position) { return position != 0; });
}

GiBudgetReport GiBudget::update(f32 measured_gi_ms, u64 frame) noexcept {
    GiBudgetReport report;
    report.measured_ms = measured_gi_ms;
    report.allocation_ms = settings_.allocation_ms;
    report.pinned = pinned_;

    const f32 alpha = math::clamp(settings_.filter_alpha, 0.01F, 1.0F);
    filtered_ms_ =
        filter_primed_ ? math::lerp(filtered_ms_, measured_gi_ms, alpha) : measured_gi_ms;
    filter_primed_ = true;
    report.filtered_ms = filtered_ms_;

    if (pinned_) {
        // Pinned is total. The overrun is reported and nothing moves; that is the mode, and a
        // controller that corrected here would be one that ignored the arbiter.
        report.predicted_ms = predicted_cost_ms();
        report.at_reserved_minimum = at_reserved_minimum();
        relaxation_permitted_ = false;
        return report;
    }

    const f32 allocation = settings_.allocation_ms;
    if (filtered_ms_ > allocation * settings_.tighten_margin) {
        tighten_to_cover(filtered_ms_ - allocation, frame, report);
    } else if (relaxation_permitted_) {
        try_relax(frame, report);
    } else if (filtered_ms_ < allocation * settings_.relax_margin && any_lever_reduced()) {
        // Under the allocation with a lever down and no permission to take it back. Reported,
        // never taken: the time a step up costs comes out of a frame this controller cannot see.
        report.relaxation_withheld = true;
    }

    relaxation_permitted_ = false;
    report.predicted_ms = predicted_cost_ms();
    report.at_reserved_minimum = at_reserved_minimum();
    return report;
}

f32 GiBudget::importance_weight(f32 screen_coverage, f32 object_importance) noexcept {
    const f32 coverage = math::clamp(screen_coverage, 0.0F, 1.0F);
    const f32 importance = math::clamp(object_importance, 0.0F, 4.0F);
    // Coverage alone is distance in disguise. Multiplying by the game's own importance is what
    // makes a gameplay-critical unit outrank the scenery behind it at the same size on screen, and
    // the floor keeps the scenery lit rather than switching it off.
    const f32 weight = std::sqrt(coverage) * (0.5F + (0.5F * importance));
    return math::clamp(weight, 0.15F, 1.0F);
}

Status GiBudget::set_foveation_mask(Span<const f32> mask, u32 width, u32 height) noexcept {
    if (mask.empty() || width == 0 || height == 0) {
        foveation_.clear();
        foveation_width_ = 0;
        foveation_height_ = 0;
        return ok();
    }
    if (mask.size() != static_cast<usize>(width) * height) {
        return fail(ErrorCode::InvalidArgument,
                    "GiBudget::set_foveation_mask: the mask does not cover width * height");
    }
    foveation_.clear();
    if (Status appended = foveation_.append(mask); !appended) {
        return appended;
    }
    foveation_width_ = width;
    foveation_height_ = height;
    return ok();
}

f32 GiBudget::foveation_at(f32 u, f32 v) const noexcept {
    if (foveation_width_ == 0 || foveation_height_ == 0) {
        return 1.0F;
    }
    const auto x =
        static_cast<u32>(math::clamp(u, 0.0F, 0.999F) * static_cast<f32>(foveation_width_));
    const auto y =
        static_cast<u32>(math::clamp(v, 0.0F, 0.999F) * static_cast<f32>(foveation_height_));
    return math::clamp(foveation_[(static_cast<usize>(y) * foveation_width_) + x], 0.0F, 1.0F);
}

}  // namespace cy::rendering::gi
