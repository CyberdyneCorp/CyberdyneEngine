// SPDX-License-Identifier: MIT
#include <cy/rendering/gi/emissive.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::rendering::gi {
namespace {

/// Promoted lights' ids: a tag in the high word, then the cell, then the card's index in it.
constexpr u64 kEmitterLightTag = 0x454D495400000000ULL;  // "EMIT"

[[nodiscard]] bool is_emissive(const Surfel& surfel) noexcept {
    return surfel.emission.x > 0.0F || surfel.emission.y > 0.0F || surfel.emission.z > 0.0F;
}

[[nodiscard]] usize find_group(Span<const EmitterRecord> groups, const Surfel& surfel) noexcept {
    for (usize index = 0; index < groups.size(); ++index) {
        if (groups[index].instance_id == surfel.instance_id &&
            groups[index].material_id == surfel.material_id) {
            return index;
        }
    }
    return groups.size();
}

/// One record per (instance, material) that emits, with its cards, area and power summed.
[[nodiscard]] Status group_emitters(Span<const Surfel> surfels, u64 cell,
                                    Array<EmitterRecord>& groups) noexcept {
    for (const Surfel& surfel : surfels) {
        if (!is_emissive(surfel)) {
            continue;
        }
        usize index = find_group(groups.span(), surfel);
        if (index == groups.size()) {
            EmitterRecord fresh;
            fresh.cell = cell;
            fresh.instance_id = surfel.instance_id;
            fresh.material_id = surfel.material_id;
            if (Status pushed = groups.push_back(fresh); !pushed) {
                return pushed;
            }
        }
        EmitterRecord& group = groups[index];
        group.cards += 1;
        group.area_m2 += std::max(surfel.area, 0.0F);
        group.power += emitted_power(surfel);
    }
    return ok();
}

/// The class the measurement gives one emitter, before the light budget.
[[nodiscard]] EmitterClass measured_class(const EmitterRecord& group,
                                          const EmissiveSettings& settings) noexcept {
    const bool promoted =
        group.power >= settings.promotion_power && group.area_m2 <= settings.promotion_max_area_m2;
    return promoted ? EmitterClass::PromotedLight : EmitterClass::CacheOnly;
}

/// The class before the light budget: the rule's where one matches, the measurement's elsewhere.
[[nodiscard]] EmitterClass ruled_class(const EmitterRecord& group,
                                       const EmissiveSettings& settings) noexcept {
    switch (group.override_mode) {
        case EmitterOverride::CacheOnly:
            return EmitterClass::CacheOnly;
        case EmitterOverride::Promote:
            return EmitterClass::PromotedLight;
        case EmitterOverride::Off:
            return EmitterClass::Disabled;
        case EmitterOverride::Automatic:
        case EmitterOverride::Count:
            break;
    }
    return measured_class(group, settings);
}

/// Promote by power and area (or by rule), then fit the promoted emitters into the light budget
/// strongest first; the rest go back to the cache as `Demoted`.
[[nodiscard]] Status assign_classes(Array<EmitterRecord>& groups, const EmissiveSettings& settings,
                                    Span<const EmitterRule> rules) noexcept {
    Array<u32> candidates;
    for (u32 index = 0; index < groups.size(); ++index) {
        EmitterRecord& group = groups[index];
        group.override_mode = emitter_override_for(rules, group.instance_id, group.material_id);
        group.classification = ruled_class(group, settings);
        if (group.classification == EmitterClass::PromotedLight) {
            if (Status pushed = candidates.push_back(index); !pushed) {
                return pushed;
            }
        }
    }
    std::ranges::sort(candidates, [&groups](u32 a, u32 b) {
        if (groups[a].power != groups[b].power) {
            return groups[a].power > groups[b].power;
        }
        return a < b;
    });
    u32 used = 0;
    for (const u32 index : candidates) {
        if (used + groups[index].cards <= settings.max_promoted_lights) {
            used += groups[index].cards;
        } else {
            groups[index].classification = EmitterClass::Demoted;
        }
    }
    return ok();
}

/// How specific a rule that matches (instance, material) is, or -1 when it does not match.
[[nodiscard]] i32 rule_specificity(const EmitterRule& rule, u32 instance_id,
                                   u32 material_id) noexcept {
    const bool any_instance = rule.instance_id == kAnyEmitter;
    const bool any_material = rule.material_id == kAnyEmitter;
    if ((!any_instance && rule.instance_id != instance_id) ||
        (!any_material && rule.material_id != material_id)) {
        return -1;
    }
    // Both named, then the instance alone, then the material alone, then neither.
    return (any_instance ? 0 : 2) + (any_material ? 0 : 1);
}

[[nodiscard]] GiLight promoted_light(const Surfel& surfel, u64 id) noexcept {
    GiLight light;
    light.position = surfel.position;
    light.emitter_normal = normalized_or(surfel.normal, Vec3{0.0F, 1.0F, 0.0F});
    light.emitter_area = surfel.area;
    light.colour = surfel.emission;
    light.intensity = 1.0F;
    light.range = 0.0F;
    light.id = id;
    return light;
}

void accumulate(EmissiveReport& report, const EmitterRecord& record) noexcept {
    report.emitters += 1;
    report.total_power += record.power;
    switch (record.classification) {
        case EmitterClass::CacheOnly:
            report.cached_emitters += 1;
            report.cached_power += record.power;
            break;
        case EmitterClass::PromotedLight:
            report.promoted_emitters += 1;
            report.promoted_power += record.power;
            report.promoted_lights += record.cards;
            break;
        case EmitterClass::Demoted:
            report.demoted_emitters += 1;
            report.demoted_power += record.power;
            break;
        case EmitterClass::Disabled:
            report.disabled_emitters += 1;
            report.disabled_power += record.power;
            break;
    }
}

}  // namespace

const char* emitter_class_name(EmitterClass classification) noexcept {
    switch (classification) {
        case EmitterClass::CacheOnly:
            return "CacheOnly";
        case EmitterClass::PromotedLight:
            return "PromotedLight";
        case EmitterClass::Demoted:
            return "Demoted";
        case EmitterClass::Disabled:
            return "Disabled";
    }
    return "Unknown";
}

const char* emitter_override_name(EmitterOverride mode) noexcept {
    switch (mode) {
        case EmitterOverride::Automatic:
            return "Automatic";
        case EmitterOverride::CacheOnly:
            return "CacheOnly";
        case EmitterOverride::Promote:
            return "Promote";
        case EmitterOverride::Off:
            return "Off";
        case EmitterOverride::Count:
            break;
    }
    return "Unknown";
}

EmitterOverride emitter_override_for(Span<const EmitterRule> rules, u32 instance_id,
                                     u32 material_id) noexcept {
    EmitterOverride chosen = EmitterOverride::Automatic;
    i32 best = -1;
    for (const EmitterRule& rule : rules) {
        const i32 specificity = rule_specificity(rule, instance_id, material_id);
        // `>=`: among equally specific rules the last given decides.
        if (specificity >= 0 && specificity >= best) {
            best = specificity;
            chosen = rule.mode;
        }
    }
    return chosen;
}

f32 emission_luminance(Vec3 emission) noexcept {
    return (0.2126F * emission.x) + (0.7152F * emission.y) + (0.0722F * emission.z);
}

f32 emitted_power(const Surfel& surfel) noexcept {
    return math::kPi * std::max(emission_luminance(surfel.emission), 0.0F) *
           std::max(surfel.area, 0.0F);
}

f32 emitter_influence_radius(const GiLight& light, f32 threshold) noexcept {
    // On axis, far from the card, the form factor is A / (pi d^2).
    const f32 radiance = std::max(emission_luminance(light.colour) * light.intensity, 0.0F);
    const f32 reach = std::sqrt((radiance * std::max(light.emitter_area, 0.0F)) /
                                (math::kPi * std::max(threshold, 1.0e-9F)));
    return math::clamp(reach, 1.0F, 64.0F);
}

Status classify_emitters(Span<const Surfel> surfels, const EmissiveSettings& settings, u64 cell,
                         u64 light_id_base, Array<Surfel>& cache_surfels, Array<GiLight>& lights,
                         Array<EmitterRecord>& records, EmissiveReport& report) noexcept {
    return classify_emitters(surfels, settings, Span<const EmitterRule>{}, cell, light_id_base,
                             cache_surfels, lights, records, report);
}

Status classify_emitters(Span<const Surfel> surfels, const EmissiveSettings& settings,
                         Span<const EmitterRule> rules, u64 cell, u64 light_id_base,
                         Array<Surfel>& cache_surfels, Array<GiLight>& lights,
                         Array<EmitterRecord>& records, EmissiveReport& report) noexcept {
    Array<EmitterRecord> groups;
    if (Status grouped = group_emitters(surfels, cell, groups); !grouped) {
        return grouped;
    }
    if (Status assigned = assign_classes(groups, settings, rules); !assigned) {
        return assigned;
    }

    u64 next_id = light_id_base;
    for (const Surfel& surfel : surfels) {
        Surfel stored = surfel;
        if (is_emissive(surfel)) {
            const EmitterRecord& group = groups[find_group(groups.span(), surfel)];
            if (group.classification == EmitterClass::PromotedLight) {
                if (Status pushed = lights.push_back(promoted_light(surfel, next_id)); !pushed) {
                    return pushed;
                }
                next_id += 1;
                // The light carries this card's energy now; leaving it in the cache too would count
                // it twice.
                stored.emission = Vec3{0.0F, 0.0F, 0.0F};
            } else if (group.classification == EmitterClass::Disabled) {
                // Switched off: the card is still a surface, and emits nothing.
                stored.emission = Vec3{0.0F, 0.0F, 0.0F};
            }
        }
        if (Status pushed = cache_surfels.push_back(stored); !pushed) {
            return pushed;
        }
    }
    for (const EmitterRecord& group : groups) {
        accumulate(report, group);
        if (Status pushed = records.push_back(group); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status EmissiveLights::set_rules(Span<const EmitterRule> rules) noexcept {
    for (const EmitterRule& rule : rules) {
        if (rule.mode >= EmitterOverride::Count) {
            return fail(ErrorCode::InvalidArgument,
                        "EmissiveLights::set_rules: an override mode out of range");
        }
    }
    Array<EmitterRule> copy;
    if (Status appended = copy.append(rules); !appended) {
        return appended;
    }
    rules_ = std::move(copy);
    return ok();
}

Status EmissiveLights::ingest(u64 cell, Span<const Surfel> surfels,
                              Array<Surfel>& cache_surfels) noexcept {
    changed_.clear();
    cache_surfels.clear();
    // Every promoted light has a record, so the records are the cells that have anything here.
    for (const EmitterRecord& record : records_) {
        if (record.cell == cell) {
            return fail(ErrorCode::InvalidArgument,
                        "EmissiveLights::ingest: the cell is already classified");
        }
    }
    const usize first_light = lights_.size();
    EmissiveReport unused;
    const u64 base = kEmitterLightTag | ((cell & 0xFFFFFFULL) << 16U);
    if (Status classified = classify_emitters(surfels, settings_, rules_.span(), cell, base,
                                              cache_surfels, lights_, records_, unused);
        !classified) {
        return classified;
    }
    for (usize index = first_light; index < lights_.size(); ++index) {
        if (Status pushed = light_cells_.push_back(cell); !pushed) {
            return pushed;
        }
        if (Status pushed = changed_.push_back(lights_[index]); !pushed) {
            return pushed;
        }
    }
    return ok();
}

void EmissiveLights::evict(u64 cell) noexcept {
    changed_.clear();
    for (usize index = lights_.size(); index > 0; --index) {
        if (light_cells_[index - 1] == cell) {
            (void)changed_.push_back(lights_[index - 1]);
            lights_.erase(index - 1);
            light_cells_.erase(index - 1);
        }
    }
    for (usize index = records_.size(); index > 0; --index) {
        if (records_[index - 1].cell == cell) {
            records_.erase(index - 1);
        }
    }
}

EmissiveReport EmissiveLights::report() const noexcept {
    EmissiveReport report;
    for (const EmitterRecord& record : records_) {
        accumulate(report, record);
    }
    return report;
}

}  // namespace cy::rendering::gi
