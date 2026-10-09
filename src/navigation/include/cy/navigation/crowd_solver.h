// SPDX-License-Identifier: MIT
#pragma once
// The crowd's sampled reciprocal-velocity-obstacle solver, written ONCE over a scalar policy.
// openspec/changes/add-deterministic-math task 6.2 (design §9.1, "Crowd").
//
// `navigation` requires local avoidance to be "written once and instantiated for both kinds of
// arithmetic, rather than maintained as two diverging implementations". This is that one text:
//
//   BasicCrowd<FloatCrowdPolicy>   `navigation::Crowd` (crowd.h): f32, `Vec3`, every float world.
//                                  Its bits are the ones the solver computed before it was a
//                                  template — the policy spells each operation exactly as the
//                                  f32 code did, in the same order.
//   BasicCrowd<FixedCrowdPolicy>   `movement::FixedCrowd` (src/movement/include/cy/movement/
//                                  crowd.h): `Fixed`, `FixedVec2`, exact `WideFixed` comparisons —
//                                  a `Lockstep` world's avoidance, the same on every architecture.
//
// WHY THIS HEADER NAMES NO FIXED TYPE. `cy::navigation` does not link `cy::core-detmath`: linking
// it defines `CY_DETERMINISM_MATH` for every consumer, and navigation's consumers include every f32
// world. So the policy carries the arithmetic, and the `Fixed` instantiation lives in
// `cy::movement`, which does link it. The algorithm, its grid, its neighbour order, its candidate
// lattice, its scoring and its acceleration limit are here and only here.
//
// WHAT A POLICY PROVIDES. Types — `Scalar`, `Vec` (the planar velocity space), `Wide` (what a dot
// product or a squared length is compared in), `Agent`, `Params` — and the primitive operations the
// algorithm is written in: projections onto the plane, products, square roots, the speed clamp, the
// candidate lattice's directions, cell coordinates, and the solver's named constants. Each is the
// smallest piece whose arithmetic differs between the two instantiations; everything that decides
// WHAT the crowd does is written once below. See `FloatCrowdPolicy` in crowd.h for the reference
// spelling of each.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::navigation {

/// An agent's slot in its crowd: stable while the agent lives, reused after it is removed.
using CrowdAgentId = u32;
/// No agent.
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

/// What one `step()` cost and what it decided, so a budget is a measurement.
struct CrowdReport {
    u32 agents = 0;
    u32 agents_by_tier[static_cast<usize>(CrowdTier::Count)] = {};
    u32 neighbour_tests = 0;
    u32 candidates_scored = 0;
    u32 agents_adjusted = 0;  ///< whose velocity differs from the desired one
    u32 grid_cells_used = 0;
    /// Grid cells the neighbour queries looked at, summed over every agent. Bounded by the block
    /// of cells one query's radius covers, times the agents — NOT by the cells the crowd occupies,
    /// which is what a grid that scanned its whole cell table per query would show here.
    u32 cells_examined = 0;
};

/// A crowd over `Policy`'s arithmetic: packed agents, a uniform grid for neighbour queries, and one
/// velocity per agent per step. See crowd.h for the argument behind every choice in it.
///
/// NOT a world. It holds no mesh and no field: `set_desired_velocity` is how a follower, a flow
/// field or a scripted order reaches it.
template <class Policy>
class BasicCrowd {
public:
    using Scalar = typename Policy::Scalar;
    using Vec = typename Policy::Vec;
    using Wide = typename Policy::Wide;
    using Agent = typename Policy::Agent;
    using Params = typename Policy::Params;

    BasicCrowd(Allocator& allocator, Scalar cell_size) noexcept;

    BasicCrowd(const BasicCrowd&) = delete;
    BasicCrowd& operator=(const BasicCrowd&) = delete;
    BasicCrowd(BasicCrowd&&) noexcept = default;
    BasicCrowd& operator=(BasicCrowd&&) noexcept = default;
    ~BasicCrowd() = default;

    /// An agent at `position`, reusing the lowest free slot. Its id is its slot.
    [[nodiscard]] Expected<CrowdAgentId, Error> add(Vec position, const Params& params) noexcept;
    [[nodiscard]] Status remove(CrowdAgentId id) noexcept;
    [[nodiscard]] u32 size() const noexcept { return live_; }
    [[nodiscard]] u32 capacity() const noexcept { return static_cast<u32>(agents_.size()); }

    [[nodiscard]] Agent* agent(CrowdAgentId id) noexcept;
    [[nodiscard]] const Agent* agent(CrowdAgentId id) const noexcept;
    /// The packed array, for a caller that iterates in bulk. Inactive slots carry `active = false`.
    [[nodiscard]] Span<const Agent> agents() const noexcept { return agents_.span(); }

    void set_desired_velocity(CrowdAgentId id, Vec velocity) noexcept;
    void set_tier(CrowdAgentId id, CrowdTier tier) noexcept;

    /// Resolve avoidance for every active agent and write its velocity. POSITIONS ARE NOT TOUCHED.
    [[nodiscard]] Status step(Scalar dt, CrowdReport& report) noexcept;

    /// Advance positions by the velocities `step()` produced: a stand-in for a controller.
    void integrate(Scalar dt) noexcept;

protected:
    /// One occupied cell. `cells_` is ordered by `key`, which is `cell_key(x, z)`.
    struct GridCell {
        u64 key = 0;
        u32 first = 0;  ///< into `cell_agents_`
        u32 count = 0;
    };

    [[nodiscard]] Status rebuild_grid(CrowdReport& report) noexcept;
    [[nodiscard]] i32 cell_of(Scalar coordinate) const noexcept;
    [[nodiscard]] static u64 cell_key(i32 x, i32 z) noexcept;
    void gather_neighbours(CrowdAgentId id, u32 wanted, CrowdReport& report) noexcept;
    void consider_cell(CrowdAgentId id, const GridCell& cell, u32 wanted,
                       CrowdReport& report) noexcept;
    [[nodiscard]] Vec solve(const Agent& self, Scalar dt, CrowdReport& report) noexcept;
    [[nodiscard]] Vec separation(const Agent& self) const noexcept;
    [[nodiscard]] Scalar soonest_collision(const Agent& self, Vec candidate) const noexcept;
    [[nodiscard]] static Scalar time_to_collision(Vec relative_position, Vec relative_velocity,
                                                  Scalar radius) noexcept;

    Array<Agent> agents_;
    Array<GridCell> cells_;
    /// Every active agent's index, ordered by (cell key, index); a cell is a run of it.
    Array<u32> cell_agents_;
    /// Scratch, reused every step: each agent's cell key, indexed by agent.
    Array<u64> cell_keys_;
    /// Scratch, reused every step: the neighbour set of the agent being solved, ordered by
    /// (distance, id) so the solver's input does not depend on the grid's iteration order.
    Array<u32> neighbours_;
    Array<Wide> neighbour_distance_;
    Scalar cell_size_;
    u32 live_ = 0;
};

}  // namespace cy::navigation
