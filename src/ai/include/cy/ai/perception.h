#pragma once
// Batched perception: one scheduler, a bounded set of queries per tick, and results written into
// knowledge stores. M8.b task 6.4.
//
// ================================================================================================
// WHY THERE IS A SCHEDULER AT ALL
// ================================================================================================
//
// `ai-system`: "Perception SHALL be scheduled globally. Agents declare sensors; they SHALL NOT
// issue their own per-agent queries during normal operation", and the scenario is a number — "WHEN
// 10,000 agents have vision sensors THEN the scheduler SHALL issue a BOUNDED, BATCHED set of
// queries rather than one or more raycasts per agent per tick."
//
// A raycast per agent per target is the thing this file exists to not do. The pipeline is the one
// the specification lists, in its order:
//
//   1. gather the sensors due on this tick (the rotation, keyed on tick and a stable agent order);
//   2. broad-phase by declared filter — faction, range, layer — over a spatial grid;
//   3. cheap rejections — distance squared, view angle, cached occlusion;
//   4. batch what survives into ONE visibility query set handed to the host;
//   5. write the results into knowledge stores.
//
// ================================================================================================
// THE HOST IS AN INTERFACE, AND THAT IS THE DEPENDENCY DECISION
// ================================================================================================
//
// Step 4 says "against the physics server", and `cy::servers::physics` is at layer 2 while this
// module is at layer 4. Reaching down would be legal and would still be wrong: it would make an AI
// suite need a physics world, and it would make perception untestable without one. So the batch
// goes to `VisibilityHost`, a pure interface with one method that takes a WHOLE BATCH — not one
// query — because an interface that takes one query is an interface a caller will loop over, which
// is the shape this file exists to prevent.
//
// ================================================================================================
// SHARING, BUDGET AND DEFERRAL ARE THREE PARTS OF ONE MECHANISM
// ================================================================================================
//
//   * SHARING. "Query results SHALL be shareable between agents where the query is equivalent." Two
//     agents close together asking about the same target are one query: the batch is keyed on
//     (observer cell, target), and `PerceptionReport::queries_shared` counts what that saved.
//   * BUDGET. "The scheduler SHALL enforce a per-tick query budget, prioritising by agent
//     importance." Requests are ordered by importance and then by entity, and the tail past the
//     budget is not issued.
//   * DEFERRAL. "queries SHALL be prioritised by agent importance and the remainder DEFERRED to
//     subsequent ticks DETERMINISTICALLY, with the deferral REPORTED." A deferred agent's
//     `last_sense_tick` is not advanced, so the rotation offers it again next tick, and
//     `PerceptionReport::deferred` is the number that says so.
//
// LATENCY IS DOCUMENTED RATHER THAN HIDDEN. `ai-system`: "WHEN a target becomes visible between
// perception updates THEN the agent SHALL perceive it at the next scheduled update, and this
// latency SHALL be documented rather than treated as a defect." At `Reduced` that is up to six
// ticks.

#include <cy/ai/agent.h>
#include <cy/ai/knowledge.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::ai {

/// One thing that can be perceived. The scheduler is handed these rather than reading a world,
/// because what is perceivable is a gameplay question — a corpse, a noise, a vehicle — and
/// `ai-system` puts the answer in gameplay's hands rather than the AI runtime's.
struct PerceptionTarget {
    Entity entity;
    Vec3 position;
    Vec3 velocity;
    u64 faction = 1;
    /// How loud it is, for hearing. Zero is silent.
    f32 loudness = 0.0F;
    /// What noticing it is worth, before confidence. Feeds `KnowledgeEntry::relevance`.
    f32 relevance = 1.0F;
};

/// One visibility question, as the host receives it.
struct VisibilityQuery {
    Vec3 from;
    Vec3 to;
    /// Which observer asked. Several observers may share one query; `first_observer` is the one the
    /// batch was keyed on, and the scheduler fans the answer back out itself.
    u32 first_observer = 0;
    u32 target = 0;
};

/// What the physics server, or a test, answers a batch with.
///
/// ONE CALL PER TICK, not one per query — see the header. `results` is written in place, one entry
/// per query, and the implementation may run the batch in parallel.
class VisibilityHost {
public:
    VisibilityHost() = default;
    virtual ~VisibilityHost() = default;
    VisibilityHost(const VisibilityHost&) = delete;
    VisibilityHost& operator=(const VisibilityHost&) = delete;
    VisibilityHost(VisibilityHost&&) = delete;
    VisibilityHost& operator=(VisibilityHost&&) = delete;

    /// `results[i]` is true when nothing blocks `queries[i]`.
    virtual void trace_batch(Span<const VisibilityQuery> queries, Span<bool> results) = 0;
};

struct PerceptionParams {
    /// The most visibility queries one tick may issue. `ai-system`'s per-tick query budget.
    u32 query_budget = 512;
    /// The grid cell used to decide that two observers' queries are equivalent. Larger shares more
    /// and is less exact; the fidelity difference is that two agents up to this far apart see the
    /// same answer.
    f32 sharing_cell = 4.0F;
    /// How much of an agent's sight range is fully acute before falloff starts is
    /// `PerceptionSensors::acuity_falloff`; this is the least confidence a sighting can carry.
    f32 minimum_confidence = 0.25F;
    /// Whether sharing is on at all. Off makes every query its own, which is what a project
    /// debugging a perception difference wants.
    bool share_queries = true;
};

/// What one tick of perception did. Every field is a measurement; `ai-system` requires the budget,
/// the sharing and the deferral each to be reported rather than inferred.
struct PerceptionReport {
    u32 sensors_due = 0;
    u32 candidates = 0;  ///< survived the broad phase
    u32 rejected_range = 0;
    u32 rejected_angle = 0;
    u32 queries_issued = 0;
    u32 queries_shared = 0;  ///< observers served by another observer's query
    u32 deferred = 0;        ///< agents whose turn came and whose queries did not fit
    u32 sightings = 0;
    u32 sounds = 0;
    bool budget_exceeded = false;
};

/// The parallel columns one perception pass reads, as one value.
///
/// It is a struct rather than seven parameters because seven parallel spans at a call site is seven
/// chances to pass them in the wrong order, and because `consistent()` is then one check in one
/// place rather than a condition every caller writes out.
struct ObserverColumns {
    Span<const Entity> entities;
    Span<const Vec3> positions;
    Span<const Vec3> forward;
    Span<PerceptionSensors> sensors;
    Span<const f32> importance;
    Span<const AiTier> tiers;
    /// `stores[i]` receives what `entities[i]` perceived. Never null for a live observer.
    Span<KnowledgeStore*> stores;

    [[nodiscard]] usize size() const noexcept { return entities.size(); }

    [[nodiscard]] bool consistent() const noexcept {
        const usize count = entities.size();
        return positions.size() == count && forward.size() == count && sensors.size() == count &&
               importance.size() == count && tiers.size() == count && stores.size() == count;
    }
};

/// The global perception pass.
///
/// It holds its scratch across ticks so that a tick allocates nothing, which is the other half of
/// "bounded": a budget on queries and an unbounded allocation would still be an unbounded frame.
class PerceptionScheduler {
public:
    PerceptionScheduler(Allocator& allocator, const PerceptionParams& params) noexcept;

    PerceptionScheduler(const PerceptionScheduler&) = delete;
    PerceptionScheduler& operator=(const PerceptionScheduler&) = delete;

    [[nodiscard]] const PerceptionParams& params() const noexcept { return params_; }

    /// One tick, over every observer.
    ///
    /// The direct, unbatched path `ai-system` also requires is `sense_one()` below; this is the one
    /// every agent uses.
    [[nodiscard]] Status update(u32 tick, const ObserverColumns& observers,
                                Span<const PerceptionTarget> targets, VisibilityHost& host,
                                PerceptionReport& report) noexcept;

    /// The direct path. `ai-system`: "A direct, unbatched query path SHALL remain available for
    /// cases needing exact instantaneous results, DOCUMENTED AS EXPENSIVE."
    ///
    /// EXPENSIVE: it issues one query per target, immediately, outside the budget and outside the
    /// rotation. It exists for the handful of cases that cannot wait a tick — a scripted reveal, a
    /// cinematic hand-off — and a system that called it per agent per tick would be exactly the
    /// per-agent raycasting this scheduler was built to remove.
    [[nodiscard]] Status sense_one(u32 tick, Entity observer, Vec3 position, Vec3 forward,
                                   const PerceptionSensors& sensors,
                                   Span<const PerceptionTarget> targets, KnowledgeStore& store,
                                   VisibilityHost& host, PerceptionReport& report) noexcept;

private:
    struct Request {
        u32 observer = 0;
        u32 target = 0;
        f32 importance = 0.0F;
        f32 confidence = 0.0F;
        i32 cell_x = 0;
        i32 cell_z = 0;
        u64 observer_bits = 0;
    };

    /// The five steps of `update()`, in the order `ai-system` lists them. Split because the whole
    /// pass in one function was past the complexity this project holds systems code to, and because
    /// each of these is separately the subject of a case in `integration.ai_runtime`.
    [[nodiscard]] Status gather(u32 tick, const ObserverColumns& observers,
                                Span<const PerceptionTarget> targets,
                                PerceptionReport& report) noexcept;
    [[nodiscard]] Status sense_unbatched(usize observer, const ObserverColumns& observers,
                                         const PerceptionTarget& target, f32 distance_squared,
                                         u32 tick, PerceptionReport& report) const noexcept;
    void order_by_importance() noexcept;
    [[nodiscard]] Status build_batch(const ObserverColumns& observers,
                                     Span<const PerceptionTarget> targets,
                                     PerceptionReport& report) noexcept;
    /// The query already built this tick that answers `request` too, or `0xFFFFFFFF`.
    [[nodiscard]] u32 shared_query_for(const Request& request) const noexcept;
    [[nodiscard]] Status trace(VisibilityHost& host, PerceptionReport& report) noexcept;
    [[nodiscard]] Status deliver(u32 tick, const ObserverColumns& observers,
                                 Span<const PerceptionTarget> targets,
                                 PerceptionReport& report) noexcept;

    PerceptionParams params_;
    Array<Request> requests_;
    Array<VisibilityQuery> queries_;
    Array<bool> results_;
    /// For each request, the query that answered it. Sharing is what makes this a many-to-one map.
    Array<u32> request_query_;
};

}  // namespace cy::ai
