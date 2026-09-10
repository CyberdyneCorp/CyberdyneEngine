// The batched perception pass. See cy/ai/perception.h for the argument; this file is the five
// steps that specification lists, in that order.

#include <cy/ai/perception.h>

#include <cmath>
#include <numbers>

namespace cy::ai {
namespace {

[[nodiscard]] f32 distance_squared_xz(Vec3 a, Vec3 b) noexcept {
    const f32 dx = b.x - a.x;
    const f32 dz = b.z - a.z;
    return (dx * dx) + (dz * dz);
}

/// The confidence a sighting carries at `distance`, given the sensor's range and acuity falloff.
/// One at the acute end, `minimum` at the limit.
[[nodiscard]] f32 sight_confidence(const PerceptionSensors& sensors, f32 distance,
                                   f32 minimum) noexcept {
    const f32 acute = sensors.sight_range * sensors.acuity_falloff;
    if (distance <= acute || sensors.sight_range <= acute) {
        return 1.0F;
    }
    const f32 t = (distance - acute) / (sensors.sight_range - acute);
    return 1.0F - (t * (1.0F - minimum));
}

[[nodiscard]] bool within_view(Vec3 forward, Vec3 offset, f32 field_of_view_degrees) noexcept {
    const f32 forward_length = std::sqrt((forward.x * forward.x) + (forward.z * forward.z));
    const f32 offset_length = std::sqrt((offset.x * offset.x) + (offset.z * offset.z));
    if (forward_length < 1e-5F || offset_length < 1e-5F) {
        return true;  // an observer with no facing, or a target on top of it, sees it
    }
    const f32 cosine =
        ((forward.x * offset.x) + (forward.z * offset.z)) / (forward_length * offset_length);
    return cosine >= std::cos(field_of_view_degrees * 0.5F * (std::numbers::pi_v<f32> / 180.0F));
}

/// "No query answers this request." A named constant rather than a literal repeated in three
/// places, one of which would eventually be a different literal.
constexpr u32 kNoQuery = 0xFFFFFFFFu;

}  // namespace

PerceptionScheduler::PerceptionScheduler(Allocator& allocator,
                                         const PerceptionParams& params) noexcept
    : params_(params),
      requests_(allocator),
      queries_(allocator),
      results_(allocator),
      request_query_(allocator) {}

Status PerceptionScheduler::sense_unbatched(usize observer, const ObserverColumns& observers,
                                            const PerceptionTarget& target, f32 distance_squared,
                                            u32 tick, PerceptionReport& report) const noexcept {
    // Hearing needs no line of sight and no query, so it is resolved here and never reaches the
    // batch. `ai-system` lists it as its own sensor kind for exactly that reason: a sound is not a
    // raycast. Proximity is the same argument at arm's length.
    const PerceptionSensors& sensor = observers.sensors[observer];
    if (sensor.hearing && target.loudness > 0.0F) {
        const f32 audible = sensor.hearing_range * target.loudness;
        if (distance_squared <= audible * audible) {
            const Status heard = observers.stores[observer]->perceive(
                target.entity, target.position, target.velocity, SenseKind::Hearing,
                params_.minimum_confidence, target.relevance, tick);
            if (!heard) {
                return heard;
            }
            ++report.sounds;
        }
    }
    if (sensor.proximity && distance_squared <= sensor.proximity_range * sensor.proximity_range) {
        const Status touched = observers.stores[observer]->perceive(
            target.entity, target.position, target.velocity, SenseKind::Proximity, 1.0F,
            target.relevance, tick);
        if (!touched) {
            return touched;
        }
    }
    return ok();
}

Status PerceptionScheduler::gather(u32 tick, const ObserverColumns& observers,
                                   Span<const PerceptionTarget> targets,
                                   PerceptionReport& report) noexcept {
    // Steps 1 to 3: the sensors due on this tick, the broad phase, and the cheap rejections.
    //
    // The rotation is a function of the tick and the agent's own index, never of a clock and never
    // of how long the last tick took. `ai-system`: "a deterministic rotation keyed on tick number
    // and stable agent ordering within each tier".
    for (usize index = 0; index < observers.size(); ++index) {
        // `Minimal` and `Statistical` have no sensors at all: their knowledge comes from shared
        // channels, which is the tier table's own row and the whole of the saving.
        const AiTier tier = observers.tiers[index];
        if (tier == AiTier::Minimal || tier == AiTier::Statistical) {
            continue;
        }
        const u32 interval = tier_think_interval(tier);
        const auto offset = static_cast<u32>(index % (interval == 0 ? 1 : interval));
        if (interval > 1 && (tick % interval) != offset) {
            continue;
        }
        ++report.sensors_due;

        const PerceptionSensors& sensor = observers.sensors[index];
        for (u32 target_index = 0; target_index < targets.size(); ++target_index) {
            const PerceptionTarget& target = targets[target_index];
            const bool interesting = target.entity != observers.entities[index] &&
                                     (sensor.factions_of_interest & target.faction) != 0 &&
                                     target.faction != sensor.own_faction;
            if (!interesting) {
                continue;
            }
            const f32 distance_sq =
                distance_squared_xz(observers.positions[index], target.position);
            if (Status sensed =
                    sense_unbatched(index, observers, target, distance_sq, tick, report);
                !sensed) {
                return sensed;
            }
            if (!sensor.vision) {
                continue;
            }
            if (distance_sq > sensor.sight_range * sensor.sight_range) {
                ++report.rejected_range;
                continue;
            }
            if (!within_view(observers.forward[index], target.position - observers.positions[index],
                             sensor.field_of_view_degrees)) {
                ++report.rejected_angle;
                continue;
            }

            Request request;
            request.observer = static_cast<u32>(index);
            request.target = target_index;
            request.importance = observers.importance[index];
            request.confidence =
                sight_confidence(sensor, std::sqrt(distance_sq), params_.minimum_confidence);
            request.cell_x =
                static_cast<i32>(std::floor(observers.positions[index].x / params_.sharing_cell));
            request.cell_z =
                static_cast<i32>(std::floor(observers.positions[index].z / params_.sharing_cell));
            request.observer_bits = observers.entities[index].bits();
            if (Status pushed = requests_.push_back(request); !pushed) {
                return pushed;
            }
            ++report.candidates;
        }
    }
    return ok();
}

void PerceptionScheduler::order_by_importance() noexcept {
    // Ordered by importance, then by the observer's own bits, then by the target. The second and
    // third keys are what make "prioritised by agent importance" a total order rather than a
    // partial one — without them two agents of equal importance are ordered by whatever the query
    // happened to visit first. An insertion sort: the array is what one tick's candidates fit in.
    for (usize index = 1; index < requests_.size(); ++index) {
        const Request candidate = requests_[index];
        usize position = index;
        while (position > 0) {
            const Request& previous = requests_[position - 1];
            const bool same_weight = previous.importance == candidate.importance;
            const bool same_observer = previous.observer_bits == candidate.observer_bits;
            const bool after =
                (previous.importance < candidate.importance) ||
                (same_weight && (previous.observer_bits > candidate.observer_bits ||
                                 (same_observer && previous.target > candidate.target)));
            if (!after) {
                break;
            }
            requests_[position] = previous;
            --position;
        }
        requests_[position] = candidate;
    }
}

u32 PerceptionScheduler::shared_query_for(const Request& request) const noexcept {
    for (usize existing = 0; existing < queries_.size(); ++existing) {
        const Request& owner = requests_[queries_[existing].first_observer];
        if (owner.target == request.target && owner.cell_x == request.cell_x &&
            owner.cell_z == request.cell_z) {
            return static_cast<u32>(existing);
        }
    }
    return kNoQuery;
}

Status PerceptionScheduler::build_batch(const ObserverColumns& observers,
                                        Span<const PerceptionTarget> targets,
                                        PerceptionReport& report) noexcept {
    // Step 4: sharing, then the budget.
    if (Status sized = request_query_.resize(requests_.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < requests_.size(); ++index) {
        request_query_[index] = kNoQuery;
        const Request& request = requests_[index];

        if (params_.share_queries) {
            // Two observers in one cell asking about one target are one query. The scan is over the
            // queries already built this tick, which is bounded by the budget.
            const u32 shared = shared_query_for(request);
            if (shared != kNoQuery) {
                request_query_[index] = shared;
                ++report.queries_shared;
                continue;
            }
        }

        if (queries_.size() >= params_.query_budget) {
            // Deferred, not dropped: the agent's `last_sense_tick` is left where it was, so the
            // rotation offers it again. `ai-system` requires the deferral to be reported.
            report.budget_exceeded = true;
            ++report.deferred;
            continue;
        }
        VisibilityQuery query;
        query.from = observers.positions[request.observer];
        query.to = targets[request.target].position;
        query.first_observer = static_cast<u32>(index);
        query.target = request.target;
        if (Status pushed = queries_.push_back(query); !pushed) {
            return pushed;
        }
        request_query_[index] = static_cast<u32>(queries_.size() - 1);
    }
    return ok();
}

Status PerceptionScheduler::trace(VisibilityHost& host, PerceptionReport& report) noexcept {
    if (Status sized = results_.resize(queries_.size()); !sized) {
        return sized;
    }
    for (bool& result : results_.span()) {
        result = false;
    }
    if (!queries_.empty()) {
        host.trace_batch(queries_.span(), results_.span());
    }
    report.queries_issued = static_cast<u32>(queries_.size());
    return ok();
}

Status PerceptionScheduler::deliver(u32 tick, const ObserverColumns& observers,
                                    Span<const PerceptionTarget> targets,
                                    PerceptionReport& report) noexcept {
    // Step 5: the results, into knowledge.
    for (usize index = 0; index < requests_.size(); ++index) {
        const u32 query = request_query_[index];
        if (query == kNoQuery || !results_[query]) {
            continue;
        }
        const Request& request = requests_[index];
        const PerceptionTarget& target = targets[request.target];
        const Status seen = observers.stores[request.observer]->perceive(
            target.entity, target.position, target.velocity, SenseKind::Vision, request.confidence,
            target.relevance, tick);
        if (!seen) {
            return seen;
        }
        ++report.sightings;
        observers.sensors[request.observer].last_sense_tick = tick;
    }
    return ok();
}

Status PerceptionScheduler::update(u32 tick, const ObserverColumns& observers,
                                   Span<const PerceptionTarget> targets, VisibilityHost& host,
                                   PerceptionReport& report) noexcept {
    report = PerceptionReport{};
    if (!observers.consistent()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "the observer columns handed to perception are not the same length"});
    }

    requests_.clear();
    queries_.clear();
    results_.clear();
    request_query_.clear();

    if (Status gathered = gather(tick, observers, targets, report); !gathered) {
        return gathered;
    }
    order_by_importance();
    if (Status batched = build_batch(observers, targets, report); !batched) {
        return batched;
    }
    if (Status traced = trace(host, report); !traced) {
        return traced;
    }
    return deliver(tick, observers, targets, report);
}

Status PerceptionScheduler::sense_one(u32 tick, Entity observer, Vec3 position, Vec3 forward,
                                      const PerceptionSensors& sensors,
                                      Span<const PerceptionTarget> targets, KnowledgeStore& store,
                                      VisibilityHost& host, PerceptionReport& report) noexcept {
    report = PerceptionReport{};
    report.sensors_due = 1;
    queries_.clear();
    results_.clear();

    Array<u32> subjects(requests_.allocator());
    for (u32 index = 0; index < targets.size(); ++index) {
        const PerceptionTarget& target = targets[index];
        if (target.entity == observer || target.faction == sensors.own_faction ||
            (sensors.factions_of_interest & target.faction) == 0) {
            continue;
        }
        const f32 distance_sq = distance_squared_xz(position, target.position);
        if (distance_sq > sensors.sight_range * sensors.sight_range) {
            ++report.rejected_range;
            continue;
        }
        if (!within_view(forward, target.position - position, sensors.field_of_view_degrees)) {
            ++report.rejected_angle;
            continue;
        }
        VisibilityQuery query;
        query.from = position;
        query.to = target.position;
        query.first_observer = 0;
        query.target = index;
        if (Status pushed = queries_.push_back(query); !pushed) {
            return pushed;
        }
        if (Status pushed = subjects.push_back(index); !pushed) {
            return pushed;
        }
        ++report.candidates;
    }

    if (Status sized = results_.resize(queries_.size()); !sized) {
        return sized;
    }
    for (bool& result : results_.span()) {
        result = false;
    }
    if (!queries_.empty()) {
        host.trace_batch(queries_.span(), results_.span());
    }
    report.queries_issued = static_cast<u32>(queries_.size());

    for (usize index = 0; index < subjects.size(); ++index) {
        if (!results_[index]) {
            continue;
        }
        const PerceptionTarget& target = targets[subjects[index]];
        const f32 distance = std::sqrt(distance_squared_xz(position, target.position));
        const Status seen =
            store.perceive(target.entity, target.position, target.velocity, SenseKind::Vision,
                           sight_confidence(sensors, distance, params_.minimum_confidence),
                           target.relevance, tick);
        if (!seen) {
            return seen;
        }
        ++report.sightings;
    }
    return ok();
}

}  // namespace cy::ai
