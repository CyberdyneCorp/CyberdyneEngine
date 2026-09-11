// The batches, their stable order, and arbitration.

#include <cy/sequencing/dispatch.h>

#include <algorithm>
#include <cmath>

namespace cy::sequencing {
namespace {

[[nodiscard]] bool same_slot(const ValueRequest& a, const ValueRequest& b) noexcept {
    return a.subsystem == b.subsystem && a.target == b.target &&
           a.property.adapter == b.property.adapter && a.property.property == b.property.property;
}

/// The total order every buffer is sorted by. Identity only — never a pointer, never a position,
/// never a worker. See dispatch.h.
[[nodiscard]] bool value_less(const ValueRequest& a, const ValueRequest& b) noexcept {
    if (a.subsystem != b.subsystem) {
        return a.subsystem < b.subsystem;
    }
    if (a.target != b.target) {
        return a.target < b.target;
    }
    if (a.property.adapter != b.property.adapter) {
        return a.property.adapter < b.property.adapter;
    }
    if (a.property.property != b.property.property) {
        return a.property.property < b.property.property;
    }
    if (a.priority != b.priority) {
        return a.priority < b.priority;
    }
    return stable_less(a.provenance, b.provenance);
}

}  // namespace

bool stable_less(const Provenance& a, const Provenance& b) noexcept {
    if (a.instance != b.instance) {
        return a.instance < b.instance;
    }
    if (a.track_stable_id != b.track_stable_id) {
        return a.track_stable_id < b.track_stable_id;
    }
    if (a.section_stable_id != b.section_stable_id) {
        return a.section_stable_id < b.section_stable_id;
    }
    return a.order < b.order;
}

DispatchBatches::DispatchBatches(Allocator& allocator) noexcept
    : values(allocator),
      cameras(allocator),
      prepares(allocator),
      time_scales(allocator),
      commands(allocator),
      events(allocator),
      spawns(allocator),
      releases(allocator) {}

void DispatchBatches::clear() noexcept {
    values.clear();
    cameras.clear();
    prepares.clear();
    time_scales.clear();
    commands.clear();
    events.clear();
    spawns.clear();
    releases.clear();
}

void DispatchBatches::sort() noexcept {
    std::sort(values.data(), values.data() + values.size(), value_less);
    std::sort(cameras.data(), cameras.data() + cameras.size(),
              [](const CameraRequest& a, const CameraRequest& b) noexcept {
                  if (a.binding != b.binding) {
                      return a.binding < b.binding;
                  }
                  if (a.priority != b.priority) {
                      return a.priority < b.priority;
                  }
                  return stable_less(a.provenance, b.provenance);
              });
    std::sort(prepares.data(), prepares.data() + prepares.size(),
              [](const PrepareRequest& a, const PrepareRequest& b) noexcept {
                  return stable_less(a.provenance, b.provenance);
              });
    std::sort(time_scales.data(), time_scales.data() + time_scales.size(),
              [](const TimeScaleRequest& a, const TimeScaleRequest& b) noexcept {
                  return stable_less(a.provenance, b.provenance);
              });
    std::sort(commands.data(), commands.data() + commands.size(),
              [](const CommandRequest& a, const CommandRequest& b) noexcept {
                  return stable_less(a.provenance, b.provenance);
              });
    std::sort(events.data(), events.data() + events.size(),
              [](const EventRequest& a, const EventRequest& b) noexcept {
                  if (a.ticks != b.ticks) {
                      return a.ticks < b.ticks;
                  }
                  return stable_less(a.provenance, b.provenance);
              });
    std::sort(spawns.data(), spawns.data() + spawns.size(),
              [](const SpawnRequest& a, const SpawnRequest& b) noexcept {
                  return stable_less(a.provenance, b.provenance);
              });
    std::sort(releases.data(), releases.data() + releases.size(),
              [](const ReleaseRequest& a, const ReleaseRequest& b) noexcept {
                  return stable_less(a.provenance, b.provenance);
              });
}

Span<const ValueRequest> DispatchBatches::values_for(SubsystemId subsystem) const noexcept {
    usize begin = values.size();
    usize end = values.size();
    for (usize index = 0; index < values.size(); ++index) {
        if (values[index].subsystem == subsystem) {
            if (begin == values.size()) {
                begin = index;
            }
            end = index + 1;
        }
    }
    if (begin == values.size()) {
        return {};
    }
    return {values.data() + begin, end - begin};
}

void ArbitrationReport::clear() noexcept {
    values.clear();
    contributions.clear();
    cameras.clear();
}

namespace {

/// Where one priority level ends, and how much absolute weight it asks for.
struct Level {
    const ValueRequest* end = nullptr;
    f32 absolute_weight = 0.0F;
};

[[nodiscard]] Level level_of(const ValueRequest* first, const ValueRequest* last) noexcept {
    Level level;
    level.end = first;
    while (level.end != last && level.end->priority == first->priority) {
        if (level.end->blend == BlendMode::Absolute) {
            level.absolute_weight += std::max(level.end->weight, 0.0F);
        }
        ++level.end;
    }
    return level;
}

/// The absolute contributions of one level, mixed against EACH OTHER. Mixed here rather than each
/// blending the accumulated result separately toward itself: two shots at half weight are a
/// half-and-half mix, not one of them applied twice.
[[nodiscard]] ChannelValue mix_absolute(const ValueRequest* first,
                                        const ValueRequest* last) noexcept {
    ChannelValue mixed = first->value;
    f32 accumulated = 0.0F;
    for (const ValueRequest* entry = first; entry != last; ++entry) {
        if (entry->blend != BlendMode::Absolute) {
            continue;
        }
        const f32 weight = std::max(entry->weight, 0.0F);
        accumulated += weight;
        if (accumulated > 0.0F) {
            mixed = blend_values(mixed, entry->value, weight / accumulated);
        }
    }
    return mixed;
}

[[nodiscard]] Contribution contribution_of(const ValueRequest& entry,
                                           f32 absolute_weight) noexcept {
    Contribution contribution;
    contribution.provenance = entry.provenance;
    contribution.priority = entry.priority;
    contribution.requested_weight = entry.weight;
    // The weight it ACTUALLY had: normalised when the level asked for more than one weight's worth.
    contribution.effective_weight = (entry.blend == BlendMode::Absolute && absolute_weight > 1.0F)
                                        ? entry.weight / absolute_weight
                                        : entry.weight;
    contribution.blend = entry.blend;
    contribution.blend_group = entry.blend_group;
    contribution.value = entry.value;
    return contribution;
}

/// Apply one priority level over `value`, and record each of its contributions.
[[nodiscard]] Status apply_level(const ValueRequest* first, const ValueRequest* last,
                                 f32 absolute_weight, bool& seeded, ChannelValue& value,
                                 ArbitrationReport& out) noexcept {
    if (absolute_weight > 0.0F) {
        const ChannelValue mixed = mix_absolute(first, last);
        value = seeded ? blend_values(value, mixed, std::min(absolute_weight, 1.0F)) : mixed;
        seeded = true;
    }
    for (const ValueRequest* entry = first; entry != last; ++entry) {
        if (entry->blend == BlendMode::Override) {
            value =
                blend_values(value, entry->value, std::min(std::max(entry->weight, 0.0F), 1.0F));
            seeded = true;
        } else if (entry->blend == BlendMode::Additive) {
            value = add_values(value, entry->value, entry->weight);
            seeded = true;
        }
        if (Status pushed = out.contributions.push_back(contribution_of(*entry, absolute_weight));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// Resolve one slot — one property of one target — from the contributions `[first, last)`, which
/// arrive sorted by ascending priority.
///
/// LOWEST PRIORITY FIRST, and each level blends over what is below it. That is what makes the
/// result "a resolved result" rather than a last writer: a gameplay camera at priority 0 and a
/// cinematic at 100 produce one value, and the cinematic's weight decides how much of it is the
/// cinematic's.
[[nodiscard]] Status resolve_slot(const ValueRequest* first, const ValueRequest* last,
                                  ArbitrationReport& out) noexcept {
    ResolvedValue resolved;
    resolved.subsystem = first->subsystem;
    resolved.target = first->target;
    resolved.property = first->property;
    resolved.value = first->value;
    resolved.contribution_begin = static_cast<u32>(out.contributions.size());

    bool seeded = false;
    const ValueRequest* cursor = first;
    while (cursor != last) {
        const Level level = level_of(cursor, last);
        if (Status applied =
                apply_level(cursor, level.end, level.absolute_weight, seeded, resolved.value, out);
            !applied) {
            return applied;
        }
        cursor = level.end;
    }

    resolved.contribution_count =
        static_cast<u32>(out.contributions.size()) - resolved.contribution_begin;
    return out.values.push_back(resolved);
}

/// One winner per camera binding, plus a release for everything it displaced, plus the exclusive
/// groups. "Subsystems SHALL receive a **resolved result**."
[[nodiscard]] Status resolve_cameras(const DispatchBatches& batches,
                                     ArbitrationReport& out) noexcept {
    for (const CameraRequest& request : batches.cameras.span()) {
        if (request.release) {
            // A release is not a candidate: it is a section that has ENDED asking the stack to
            // blend it out, and it must survive arbitration untouched.
            if (Status pushed = out.cameras.push_back(request); !pushed) {
                return pushed;
            }
        }
    }
    for (usize index = 0; index < batches.cameras.size(); ++index) {
        const CameraRequest& candidate = batches.cameras[index];
        if (candidate.release) {
            continue;
        }
        const CameraRequest* winner = &candidate;
        bool displaced = false;
        for (usize other = 0; other < batches.cameras.size(); ++other) {
            const CameraRequest& rival = batches.cameras[other];
            if (other == index || rival.release) {
                continue;
            }
            const bool same_binding = rival.binding == candidate.binding;
            const bool same_group = !candidate.exclusive_group.is_empty() &&
                                    rival.exclusive_group == candidate.exclusive_group;
            if (!same_binding && !same_group) {
                continue;
            }
            const bool rival_wins = (rival.priority > winner->priority) ||
                                    (rival.priority == winner->priority &&
                                     stable_less(winner->provenance, rival.provenance));
            if (rival_wins) {
                winner = &rival;
                displaced = true;
            }
        }
        CameraRequest resolved = candidate;
        if (displaced) {
            // Displaced rather than dropped: the stack blends it out with its own declared blend,
            // which is what "the gameplay camera's contribution SHALL be blended or suspended as
            // declared" asks for.
            resolved.release = true;
        }
        if (Status pushed = out.cameras.push_back(resolved); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace

Status arbitrate(const DispatchBatches& batches, ArbitrationReport& out) noexcept {
    out.clear();

    // A local copy, sorted: `arbitrate()` takes its input by const reference because a caller may
    // arbitrate the same batch twice — once for a preview and once for the frame — and a function
    // that reordered its argument would make the second call's answer depend on the first.
    Array<ValueRequest> sorted(out.values.allocator());
    if (Status reserved = sorted.reserve(batches.values.size()); !reserved) {
        return reserved;
    }
    for (const ValueRequest& request : batches.values.span()) {
        if (Status pushed = sorted.push_back(request); !pushed) {
            return pushed;
        }
    }
    std::sort(sorted.data(), sorted.data() + sorted.size(), value_less);

    usize begin = 0;
    while (begin < sorted.size()) {
        usize end = begin + 1;
        while (end < sorted.size() && same_slot(sorted[begin], sorted[end])) {
            ++end;
        }
        if (Status resolved = resolve_slot(sorted.data() + begin, sorted.data() + end, out);
            !resolved) {
            return resolved;
        }
        begin = end;
    }

    return resolve_cameras(batches, out);
}

}  // namespace cy::sequencing
