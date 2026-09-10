// Importance scoring, tier assignment with hysteresis and budgets, and virtualisation.
// M8.b task 10.2.

#include <cy/audio/tiers.h>

#include <algorithm>
#include <cmath>

namespace cy::audio {
namespace {

[[nodiscard]] f32 clampf(f32 value, f32 low, f32 high) noexcept {
    return std::clamp(value, low, high);
}

/// The rank at which one tier ends and the next begins.
[[nodiscard]] u32 boundary_rank(SimulationTier tier, const TierBudgets& budgets) noexcept {
    switch (tier) {
        case SimulationTier::FullAcoustic:
            return budgets.full_acoustic;
        case SimulationTier::Spatialised:
            return budgets.full_acoustic + budgets.spatialised;
        case SimulationTier::Simple:
            return budgets.full_acoustic + budgets.spatialised + budgets.simple;
        case SimulationTier::Virtual:
        case SimulationTier::Count:
            break;
    }
    return 0xFFFFFFFFU;
}

/// A tier's own budget, or "unlimited" for `Virtual`.
[[nodiscard]] u32 budget_of(SimulationTier tier, const TierBudgets& budgets) noexcept {
    switch (tier) {
        case SimulationTier::FullAcoustic:
            return budgets.full_acoustic;
        case SimulationTier::Spatialised:
            return budgets.spatialised;
        case SimulationTier::Simple:
            return budgets.simple;
        case SimulationTier::Virtual:
        case SimulationTier::Count:
            break;
    }
    return 0xFFFFFFFFU;
}

[[nodiscard]] SimulationTier tier_for_rank(u32 rank, const TierBudgets& budgets) noexcept {
    if (rank < budgets.full_acoustic) {
        return SimulationTier::FullAcoustic;
    }
    if (rank < budgets.full_acoustic + budgets.spatialised) {
        return SimulationTier::Spatialised;
    }
    if (rank < budgets.full_acoustic + budgets.spatialised + budgets.simple) {
        return SimulationTier::Simple;
    }
    return SimulationTier::Virtual;
}

}  // namespace

const char* simulation_tier_name(SimulationTier tier) noexcept {
    switch (tier) {
        case SimulationTier::FullAcoustic:
            return "full-acoustic";
        case SimulationTier::Spatialised:
            return "spatialised";
        case SimulationTier::Simple:
            return "simple";
        case SimulationTier::Virtual:
            return "virtual";
        case SimulationTier::Count:
            break;
    }
    return "unknown";
}

f32 score_source(const SourceScoring& source, const ScoringListener& listener) noexcept {
    const Vec3 to_source = source.position - listener.position;
    const f32 distance = length(to_source);
    const f32 audible = (listener.audible_distance > 0.0F) ? listener.audible_distance : 1.0F;
    if (distance >= audible) {
        // BEYOND AUDIBLE IS ZERO, whatever the source's volume: a distant explosion is not more
        // important than a nearby footstep because it is loud.
        return 0.0F;
    }

    // Distance is the dominant term and falls off quadratically, which matches how loudness does.
    const f32 nearness = 1.0F - (distance / audible);
    f32 score = nearness * nearness;

    // Orientation: a source behind the listener matters slightly less, and only slightly — a sound
    // behind you is still a sound.
    const Vec3 direction = normalized_or(to_source, listener.forward);
    const f32 facing = dot(direction, normalized_or(listener.forward, Vec3{0.0F, 0.0F, -1.0F}));
    score *= 0.75F + (0.25F * clampf((facing + 1.0F) * 0.5F, 0.0F, 1.0F));

    score *= clampf(source.volume, 0.0F, 4.0F);
    score *= clampf(source.priority, 0.0F, 8.0F);
    score *= clampf(source.category_weight, 0.0F, 8.0F);
    // Occlusion reduces it but never to nothing: a muffled sound behind a door is still worth
    // spatialising, which is why this is a scale rather than a gate.
    score *= 1.0F - (0.5F * clampf(source.occlusion, 0.0F, 1.0F));
    if (source.gameplay_important) {
        // GAMEPLAY'S OWN FLAG, and it is a large multiplier rather than an override: a line of
        // dialogue two hundred metres away is still not the most important sound in the mix, and a
        // flag that forced it to be would make the pinning mechanism pointless.
        score *= 4.0F;
    }
    return clampf(score, 0.0F, 1.0F);
}

Status assign_tiers(Span<SourceScoring> sources, const ScoringListener& listener,
                    const TierBudgets& budgets, f32 dt, Array<TierAssignment>& out,
                    TierReport& report) noexcept {
    out.clear();
    report = TierReport{};
    report.scored = static_cast<u32>(sources.size());
    if (sources.empty()) {
        return ok();
    }

    struct Ranked {
        u32 index = 0;
        f32 score = 0.0F;
    };
    Array<Ranked> ranked(out.allocator());
    if (Status sized = ranked.resize(sources.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < sources.size(); ++index) {
        ranked[index].index = static_cast<u32>(index);
        ranked[index].score = score_source(sources[index], listener);
    }
    // Stable, so two sources with identical scores keep their order between frames rather than
    // swapping tiers because a sort was unstable.
    Span<Ranked> span = ranked.span();
    std::ranges::stable_sort(
        span, [](const Ranked& a, const Ranked& b) noexcept { return a.score > b.score; });

    if (Status sized = out.resize(sources.size()); !sized) {
        return sized;
    }

    for (usize rank = 0; rank < ranked.size(); ++rank) {
        SourceScoring& source = sources[ranked[rank].index];
        const SimulationTier wanted = tier_for_rank(static_cast<u32>(rank), budgets);
        SimulationTier tier = wanted;
        bool held = false;

        source.seconds_in_tier += (dt > 0.0F) ? dt : 0.0F;

        // HYSTERESIS, AND IT IS ABOUT THE BOUNDARY RATHER THAN ABOUT EVERY CHANGE. A source deep
        // inside a tier moves freely; one hovering at the rank where two tiers meet is held,
        // because that is the source that would otherwise change tier every frame and be audible as
        // a flutter. A source that has never been assigned is not held at all — a sound that has
        // just started should be heard now.
        if (source.assigned && tier != source.previous_tier) {
            const bool dwelt = source.seconds_in_tier >= budgets.minimum_dwell;
            if (!dwelt) {
                tier = source.previous_tier;
                held = true;
            } else {
                const SimulationTier upper =
                    (static_cast<u8>(tier) < static_cast<u8>(source.previous_tier))
                        ? tier
                        : source.previous_tier;
                const u32 crossing = boundary_rank(upper, budgets);
                const u32 band =
                    (crossing == 0xFFFFFFFFU)
                        ? 0U
                        : static_cast<u32>(static_cast<f32>(crossing) *
                                           clampf(budgets.promotion_margin, 0.0F, 1.0F));
                const u32 slots = (band == 0U) ? 1U : band;
                const u32 rank_index = static_cast<u32>(rank);
                const bool inside_band = (crossing != 0xFFFFFFFFU) &&
                                         (rank_index + slots > crossing) &&
                                         (rank_index < crossing + slots);
                // AND A HOLD MAY NOT BREAK A BUDGET. Hysteresis exists to stop flapping, not to
                // let a tier exceed the count a project configured — "the cost of audio is bounded
                // by configuration rather than by content", and a hold that overran the budget
                // would make that false by a source or two per boundary.
                const u32 occupancy = report.counts[static_cast<usize>(source.previous_tier)];
                if (inside_band && occupancy < budget_of(source.previous_tier, budgets)) {
                    tier = source.previous_tier;
                    held = true;
                }
            }
        }

        // PINNING WINS OVER EVERYTHING, including the budget: "it SHALL never be demoted below that
        // tier regardless of distance or budget pressure."
        if (static_cast<u8>(tier) > static_cast<u8>(source.minimum_tier)) {
            tier = source.minimum_tier;
            ++report.pinned;
        }

        TierAssignment& assignment = out[ranked[rank].index];
        assignment.source = source.source;
        assignment.tier = tier;
        assignment.score = ranked[rank].score;
        assignment.changed = tier != source.previous_tier;
        assignment.held = held;
        if (assignment.changed) {
            if (static_cast<u8>(tier) < static_cast<u8>(source.previous_tier)) {
                ++report.promotions;
            } else {
                ++report.demotions;
            }
            source.seconds_in_tier = 0.0F;
        }
        if (held) {
            ++report.held_by_hysteresis;
        }
        source.previous_tier = tier;
        source.assigned = true;
        ++report.counts[static_cast<usize>(tier)];
    }
    return ok();
}

void advance_virtual(VirtualVoice& voice, f32 dt) noexcept {
    if (voice.stopped || dt <= 0.0F) {
        return;
    }
    // THE WHOLE COST OF A VIRTUAL SOURCE. No mixing, no filtering, no panning: one multiply and one
    // add, so eight thousand of them cost what eight thousand multiplies cost.
    voice.position += static_cast<f64>(dt) * static_cast<f64>(voice.pitch);
    if (voice.looping && voice.length > 0.0) {
        while (voice.position >= voice.length) {
            voice.position -= voice.length;
        }
    } else if (voice.length > 0.0 && voice.position > voice.length) {
        voice.position = voice.length;
    }
}

ResumePlan plan_resume(const VirtualVoice& voice, f32 fade_in_seconds) noexcept {
    ResumePlan plan;
    plan.fade_in = (fade_in_seconds > 0.0F) ? fade_in_seconds : 0.0F;
    if (voice.stopped) {
        // Stopped is stopped. A voice gameplay stopped does not come back when the listener walks
        // past it again.
        return plan;
    }
    // WHERE IT WOULD HAVE BEEN, which for a loop is inside the loop rather than at its start.
    plan.position = voice.position;
    plan.resumable = true;
    return plan;
}

}  // namespace cy::audio
