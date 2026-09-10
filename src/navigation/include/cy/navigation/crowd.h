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
// RECIPROCITY AND PRIORITY ARE THE SAME NUMBER. Each agent takes a share of the avoidance; two
// equals take half each, which is what stops the oscillation two agents each dodging fully would
// produce. `navigation`'s "lower-priority agents SHALL yield, avoiding deadlock" is that share
// tilted by priority — the yielder takes more of it — and the share is never 0 or 1, because an
// agent that took none of the responsibility would walk through the other.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/flow_field.h>
#include <cy/navigation/query.h>

namespace cy::navigation {

using CrowdAgentId = u32;
inline constexpr CrowdAgentId kInvalidCrowdAgent = 0xFFFFFFFFu;

/// How much thinking one agent gets. `navigation` requires crowd cost to be "tiered consistently
/// with AI LOD", and `ai-system`'s tier table is the one these three mirror.
///
/// | Tier      | Candidates | Neighbours | Steering |
/// |-----------|-----------:|-----------:|----------|
/// | `Full`    |         25 |          6 | full sampled RVO |
/// | `Reduced` |          9 |          3 | sampled RVO over a coarser lattice |
/// | `Minimal` |          0 |          3 | separation only — no time-to-collision term |
enum class CrowdTier : u8 { Full = 0, Reduced, Minimal, Count };

[[nodiscard]] const char* crowd_tier_name(CrowdTier tier) noexcept;

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

/// What one `step()` cost and what it decided, so a budget is a measurement.
struct CrowdReport {
    u32 agents = 0;
    u32 agents_by_tier[static_cast<usize>(CrowdTier::Count)] = {};
    u32 neighbour_tests = 0;
    u32 candidates_scored = 0;
    u32 agents_adjusted = 0;  ///< whose velocity differs from the desired one
    u32 grid_cells_used = 0;
};

/// A crowd: packed agents, a uniform grid for neighbour queries, and one velocity per agent per
/// step.
///
/// NOT a world. It holds no mesh and no field: `set_desired_velocity` is how a follower, a flow
/// field or a scripted order reaches it, and that keeps the avoidance solver testable without a
/// navigation mesh in existence.
class Crowd {
public:
    Crowd(Allocator& allocator, f32 cell_size) noexcept;

    Crowd(const Crowd&) = delete;
    Crowd& operator=(const Crowd&) = delete;
    Crowd(Crowd&&) noexcept = default;
    Crowd& operator=(Crowd&&) noexcept = default;

    [[nodiscard]] Expected<CrowdAgentId, Error> add(Vec3 position,
                                                    const AvoidanceParams& params) noexcept;
    [[nodiscard]] Status remove(CrowdAgentId id) noexcept;
    [[nodiscard]] u32 size() const noexcept { return live_; }
    [[nodiscard]] u32 capacity() const noexcept { return static_cast<u32>(agents_.size()); }

    [[nodiscard]] CrowdAgent* agent(CrowdAgentId id) noexcept;
    [[nodiscard]] const CrowdAgent* agent(CrowdAgentId id) const noexcept;
    /// The packed array, for a caller that iterates in bulk. Inactive slots carry `active = false`.
    [[nodiscard]] Span<const CrowdAgent> agents() const noexcept { return agents_.span(); }

    void set_desired_velocity(CrowdAgentId id, Vec3 velocity) noexcept;
    void set_tier(CrowdAgentId id, CrowdTier tier) noexcept;

    /// Steer every agent toward the point it is heading for, along `field` or along `corridor`'s
    /// next point. A convenience over `set_desired_velocity` and not a requirement of `step()`.
    void steer_towards(CrowdAgentId id, Vec3 target, f32 arrival_distance) noexcept;

    /// Resolve avoidance for every active agent and write `velocity`. POSITIONS ARE NOT TOUCHED.
    [[nodiscard]] Status step(f32 dt, CrowdReport& report) noexcept;

    /// Advance positions by the velocities `step()` produced.
    ///
    /// THIS IS A STAND-IN FOR THE CHARACTER CONTROLLER, not part of avoidance — see the header.
    /// A project with a controller feeds `velocity` to it and never calls this.
    void integrate(f32 dt) noexcept;

private:
    struct GridCell {
        i32 x = 0;
        i32 z = 0;
        u32 first = 0;  ///< into `cell_agents_`
        u32 count = 0;
    };

    [[nodiscard]] Status rebuild_grid(CrowdReport& report) noexcept;
    void gather_neighbours(CrowdAgentId id, u32 wanted, CrowdReport& report) noexcept;
    [[nodiscard]] Vec3 solve(const CrowdAgent& self, f32 dt, CrowdReport& report) noexcept;

    Array<CrowdAgent> agents_;
    Array<GridCell> cells_;
    Array<u32> cell_agents_;
    /// Scratch, reused every step: the neighbour set of the agent being solved, ordered by
    /// (distance, id) so the solver's input does not depend on the grid's iteration order.
    Array<u32> neighbours_;
    Array<f32> neighbour_distance_;
    f32 cell_size_ = 4.0F;
    u32 live_ = 0;
};

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
