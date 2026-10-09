#pragma once
// Local avoidance and the crowd that runs it in bulk. M8.b task 6.2.
//
// ================================================================================================
// AVOIDANCE PRODUCES A VELOCITY, NOT A POSITION — AND THE INTERFACE SAYS SO
// ================================================================================================
//
// `navigation` is explicit: "Avoidance SHALL compute a DESIRED VELOCITY ADJUSTMENT, not a position:
// the agent's controller remains responsible for movement and for physics collision", and its
// scenario adds "avoidance SHALL NOT be relied on to prevent interpenetration".
//
// So `Crowd::step()` writes `CrowdAgent::velocity` and touches no position. Advancing an agent is
// `integrate()`, a separate call that exists because a test and a sample need a controller and the
// character controller lives in `cy::physics`, above this module. A project that has one calls
// `step()` and feeds `velocity` to it; a project that does not calls both. The split is the
// specification's, not a convenience.
//
// ================================================================================================
// THE FORMULATION: SAMPLED RECIPROCAL VELOCITY OBSTACLES
// ================================================================================================
//
// `navigation` names the family — "using a RECIPROCAL VELOCITY OBSTACLE formulation" — and not the
// solver. This one samples: a fixed lattice of candidate velocities is scored against the time to
// collision with each neighbour under the RECIPROCAL assumption that the neighbour will take its
// share of the avoidance, and the best-scoring candidate wins. It is chosen over an ORCA linear
// program for three reasons that are all about this engine rather than about geometry:
//
//   1. IT IS DETERMINISTIC BY CONSTRUCTION. The candidate set is fixed, the neighbour set is sorted
//      by (distance, agent id), and the tie-break is the candidate index. `ai-system` requires AI
//      decisions to survive replay and reconciliation, and an agent's velocity is one.
//   2. ITS COST IS A CONSTANT PER AGENT — candidates times neighbours — which is what makes a tier
//      budget mean something. A linear program's cost depends on how many constraints happen to be
//      active.
//   3. IT DEGRADES BY SAMPLING LESS, which is exactly what `navigation`'s "agents at reduced tiers
//      SHALL use cheaper avoidance" asks for, with the fidelity difference documented: see
//      `CrowdTier` below, whose rows say how many candidates and neighbours each tier gets.
//
// WRITTEN ONCE, FOR TWO KINDS OF ARITHMETIC. The solver is `BasicCrowd<Policy>`
// (crowd_solver.h, defined in crowd_solver_impl.h). `Crowd` below is its f32 instantiation, and
// computes the bits the solver computed before it was a template; `cy::movement::FixedCrowd` is the
// same text over `Fixed`, a `Lockstep` world's avoidance (openspec/changes/add-deterministic-math
// task 6.2).
//
// RECIPROCITY AND PRIORITY ARE THE SAME NUMBER. Each agent takes a share of the avoidance; two
// equals take half each, which is what stops the oscillation two agents each dodging fully would
// produce. `navigation`'s "lower-priority agents SHALL yield, avoiding deadlock" is that share
// tilted by priority — the yielder takes more of it — and the share is never 0 or 1, because an
// agent that took none of the responsibility would walk through the other.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/crowd_solver.h>
#include <cy/navigation/flow_field.h>
#include <cy/navigation/query.h>

namespace cy::navigation {

/// `navigation`'s agent parameters, by name: "radius, height, maximum speed, neighbour distance,
/// maximum neighbours, time horizon for agents, and time horizon for obstacles".
struct AvoidanceParams {
    f32 radius = 0.5F;
    f32 height = 2.0F;
    f32 max_speed = 3.5F;
    f32 max_acceleration = 8.0F;
    f32 neighbour_distance = 4.0F;
    u32 max_neighbours = 6;
    f32 time_horizon = 2.0F;
    f32 time_horizon_obstacles = 1.0F;
    /// Higher yields less. `navigation`: "priority so that important agents are less likely to
    /// yield".
    u8 priority = 0;
    /// How hard the agent pushes away from its neighbours regardless of where they are heading.
    /// Separate from avoidance because a stationary crowd has no velocities to avoid.
    f32 separation_weight = 0.6F;
};

/// One agent, as the crowd stores it. A plain aggregate in a packed array: `navigation` requires
/// "a scheduled, data-oriented system operating over agent components in BULK, not per-agent object
/// updates", and this is the storage that sentence describes.
struct CrowdAgent {
    Vec3 position;
    Vec3 velocity;
    /// What the agent would do with no neighbours — from its corridor, its flow field, or set
    /// directly. `step()` reads it and never writes it.
    Vec3 desired_velocity;
    AvoidanceParams params;
    CrowdTier tier = CrowdTier::Full;
    bool active = false;
};

/// The f32 arithmetic of `BasicCrowd` (crowd_solver.h). Each operation is spelled exactly as the
/// solver spelled it before it was a template, in the same order, so a float world's crowd computes
/// the bits it always did. Defined in crowd.cpp, the one translation unit that instantiates it.
struct FloatCrowdPolicy {
    using Scalar = f32;
    using Vec = Vec3;
    /// Squared lengths and dot products are compared in f32 itself.
    using Wide = f32;
    using Agent = CrowdAgent;
    using Params = AvoidanceParams;

    /// The XZ plane: y dropped.
    [[nodiscard]] static Vec3 flatten(Vec3 v) noexcept;
    [[nodiscard]] static f32 across(Vec3 v) noexcept { return v.x; }
    [[nodiscard]] static f32 along(Vec3 v) noexcept { return v.z; }
    [[nodiscard]] static f32 length_squared(Vec3 v) noexcept;
    [[nodiscard]] static f32 dot(Vec3 a, Vec3 b) noexcept;
    [[nodiscard]] static f32 square(f32 s) noexcept { return s * s; }
    [[nodiscard]] static f32 product(f32 a, f32 b) noexcept { return a * b; }
    [[nodiscard]] static f32 narrow(f32 w) noexcept { return w; }
    [[nodiscard]] static f32 sqrt(f32 w) noexcept;
    [[nodiscard]] static f32 length(Vec3 v) noexcept;
    [[nodiscard]] static Vec3 clamp_speed(Vec3 v, f32 max_speed) noexcept;
    /// The XZ perpendicular, rotated a quarter turn.
    [[nodiscard]] static Vec3 perpendicular(Vec3 v) noexcept;
    /// The `spoke`-th of `count` directions around the circle, at `magnitude`.
    [[nodiscard]] static Vec3 lattice(u32 spoke, u32 count, f32 magnitude) noexcept;
    [[nodiscard]] static f32 ratio(u32 numerator, u32 denominator) noexcept;
    [[nodiscard]] static f32 lesser(f32 a, f32 b) noexcept;
    [[nodiscard]] static f32 greater(f32 a, f32 b) noexcept;
    [[nodiscard]] static i32 cell(f32 coordinate, f32 size) noexcept;
    [[nodiscard]] static i32 cell_span(f32 range, f32 size) noexcept;
    [[nodiscard]] static f32 responsibility(u8 mine, u8 theirs) noexcept;
    [[nodiscard]] static Vec3 zero_vec() noexcept { return Vec3{}; }
    [[nodiscard]] static f32 zero() noexcept { return 0.0F; }
    [[nodiscard]] static f32 wide_zero() noexcept { return 0.0F; }
    [[nodiscard]] static f32 one() noexcept { return 1.0F; }
    [[nodiscard]] static f32 two() noexcept { return 2.0F; }
    [[nodiscard]] static f32 half() noexcept { return 0.5F; }
    [[nodiscard]] static f32 infinity() noexcept;
    [[nodiscard]] static f32 default_cell_size() noexcept { return 4.0F; }
    /// How far to one side an agent perceives a neighbour that is exactly ahead of it, as a
    /// fraction of the distance — the side bias, explained in `soonest_collision`.
    [[nodiscard]] static f32 side_bias() noexcept { return 0.15F; }
    /// Closer than this, two agents have no direction to separate along.
    [[nodiscard]] static f32 separation_floor() noexcept { return 1e-4F; }
    /// A relative speed squared below this never closes.
    [[nodiscard]] static f32 still_speed_squared() noexcept { return 1e-9F; }
    /// A velocity change below this is applied whole, whatever the acceleration limit.
    [[nodiscard]] static f32 change_floor() noexcept { return 1e-6F; }
    /// A velocity this far from the desired one counts as adjusted in the report.
    [[nodiscard]] static f32 adjusted_floor() noexcept { return 1e-3F; }
};

/// A crowd: packed agents, a uniform grid for neighbour queries, and one velocity per agent per
/// step — `BasicCrowd` over f32. The `Fixed` instantiation is `cy::movement::FixedCrowd`.
class Crowd : public BasicCrowd<FloatCrowdPolicy> {
public:
    Crowd(Allocator& allocator, f32 cell_size) noexcept
        : BasicCrowd<FloatCrowdPolicy>(allocator, cell_size) {}

    /// Steer every agent toward the point it is heading for, along `field` or along `corridor`'s
    /// next point. A convenience over `set_desired_velocity` and not a requirement of `step()`.
    void steer_towards(CrowdAgentId id, Vec3 target, f32 arrival_distance) noexcept;
};

extern template class BasicCrowd<FloatCrowdPolicy>;

/// Follow a straightened path: the desired velocity that carries an agent toward the next point,
/// and the index of the point it should now be aiming at.
///
/// Free rather than a member because `navigation` separates "path following" from "avoidance" and
/// an agent on a flow field uses one without the other.
[[nodiscard]] Vec3 follow_path(Span<const PathPoint> path, Vec3 position, f32 speed,
                               f32 arrival_distance, u32& cursor) noexcept;

/// The desired velocity a flow field gives an agent standing at `position`.
[[nodiscard]] Vec3 follow_field(const FlowField& field, Vec3 position, f32 speed) noexcept;

}  // namespace cy::navigation
