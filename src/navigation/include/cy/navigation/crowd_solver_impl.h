// SPDX-License-Identifier: MIT
#pragma once
// The definitions of `BasicCrowd<Policy>` (crowd_solver.h): the sampled
// reciprocal-velocity-obstacle solver and the uniform grid that bounds its neighbour set, written
// once.
//
// Included by exactly the translation units that instantiate it — src/navigation/src/crowd.cpp for
// f32 and src/movement/src/crowd.cpp for `Fixed` — so a consumer of `Crowd` compiles none of it.
// The grid is a cell table sorted by key, so finding a cell is a binary search rather than a scan.
// The argument behind each decision is in crowd.h; the comments here say what each step computes.

#include <cy/navigation/crowd_solver.h>

#include <algorithm>

namespace cy::navigation {

/// The candidate lattice, per tier. Fixed sets rather than a random sample: determinism is a
/// requirement and a random sample would make one scenario produce two crowds.
inline constexpr u32 kCrowdCandidateRings[static_cast<usize>(CrowdTier::Count)] = {3, 2, 0};
inline constexpr u32 kCrowdCandidateSpokes[static_cast<usize>(CrowdTier::Count)] = {8, 4, 0};
inline constexpr u32 kCrowdTierNeighbours[static_cast<usize>(CrowdTier::Count)] = {6, 3, 3};

template <class Policy>
BasicCrowd<Policy>::BasicCrowd(Allocator& allocator, Scalar cell_size) noexcept
    // Every array in `allocator`; a cell size that is not positive is the policy's default.
    : agents_(allocator),
      cells_(allocator),
      cell_agents_(allocator),
      cell_keys_(allocator),
      neighbours_(allocator),
      neighbour_distance_(allocator),
      cell_size_(cell_size > Policy::zero() ? cell_size : Policy::default_cell_size()) {}

template <class Policy>
Expected<CrowdAgentId, Error> BasicCrowd<Policy>::add(Vec position, const Params& params) noexcept {
    for (usize index = 0; index < agents_.size(); ++index) {
        if (!agents_[index].active) {
            agents_[index] = Agent{};
            agents_[index].position = position;
            agents_[index].params = params;
            agents_[index].active = true;
            ++live_;
            return static_cast<CrowdAgentId>(index);
        }
    }
    Agent agent{};
    agent.position = position;
    agent.params = params;
    agent.active = true;
    if (Status pushed = agents_.push_back(agent); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++live_;
    return static_cast<CrowdAgentId>(agents_.size() - 1);
}

template <class Policy>
Status BasicCrowd<Policy>::remove(CrowdAgentId id) noexcept {
    if (id >= agents_.size() || !agents_[id].active) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such crowd agent"});
    }
    agents_[id].active = false;
    --live_;
    return ok();
}

template <class Policy>
Policy::Agent* BasicCrowd<Policy>::agent(CrowdAgentId id) noexcept {
    if (id >= agents_.size() || !agents_[id].active) {
        return nullptr;
    }
    return &agents_[id];
}

template <class Policy>
const Policy::Agent* BasicCrowd<Policy>::agent(CrowdAgentId id) const noexcept {
    if (id >= agents_.size() || !agents_[id].active) {
        return nullptr;
    }
    return &agents_[id];
}

template <class Policy>
void BasicCrowd<Policy>::set_desired_velocity(CrowdAgentId id, Vec velocity) noexcept {
    if (Agent* target = agent(id); target != nullptr) {
        target->desired_velocity = Policy::flatten(velocity);
    }
}

template <class Policy>
void BasicCrowd<Policy>::set_tier(CrowdAgentId id, CrowdTier tier) noexcept {
    if (Agent* target = agent(id); target != nullptr) {
        target->tier = tier;
    }
}

template <class Policy>
Status BasicCrowd<Policy>::rebuild_grid(CrowdReport& report) noexcept {
    cells_.clear();
    cell_agents_.clear();
    if (Status sized = cell_keys_.resize(agents_.size()); !sized) {
        return sized;
    }

    // A SORT, NOT A SCAN. Every active agent is listed once, ordered by (cell key, agent index),
    // and the cell table is the run-length encoding of that list — so it comes out ordered by key
    // and `gather_neighbours` finds a cell by binary search. Until this was a sort, each agent
    // found its cell by scanning the cell table, which made a step O(agents x cells): the 2,000
    // agent scale act spent most of its navigation tick there, and `samples/08-vertical-slice/`
    // had to size its cells to keep the table short. The agent index as the second key keeps the
    // order within a cell the ascending one the scan produced.
    for (usize index = 0; index < agents_.size(); ++index) {
        if (!agents_[index].active) {
            continue;
        }
        const Vec position = agents_[index].position;
        cell_keys_[index] =
            cell_key(cell_of(Policy::across(position)), cell_of(Policy::along(position)));
        if (Status pushed = cell_agents_.push_back(static_cast<u32>(index)); !pushed) {
            return pushed;
        }
    }
    const u64* keys = cell_keys_.data();
    std::sort(cell_agents_.data(), cell_agents_.data() + cell_agents_.size(),
              [keys](u32 a, u32 b) noexcept {
                  return keys[a] < keys[b] || (keys[a] == keys[b] && a < b);
              });

    for (usize slot = 0; slot < cell_agents_.size(); ++slot) {
        const u64 key = keys[cell_agents_[slot]];
        if (!cells_.empty() && cells_[cells_.size() - 1].key == key) {
            ++cells_[cells_.size() - 1].count;
            continue;
        }
        GridCell cell;
        cell.key = key;
        cell.first = static_cast<u32>(slot);
        cell.count = 1;
        if (Status pushed = cells_.push_back(cell); !pushed) {
            return pushed;
        }
    }
    report.grid_cells_used = static_cast<u32>(cells_.size());
    return ok();
}

template <class Policy>
i32 BasicCrowd<Policy>::cell_of(Scalar coordinate) const noexcept {
    return Policy::cell(coordinate, cell_size_);
}

template <class Policy>
u64 BasicCrowd<Policy>::cell_key(i32 x, i32 z) noexcept {
    // Row-major with z the major key, each coordinate biased into an unsigned range so that the
    // packed key orders the same way the signed pair does. A row of cells is then one contiguous,
    // ascending run of keys.
    const auto biased = [](i32 value) noexcept {
        return static_cast<u64>(static_cast<u32>(value) ^ 0x80000000U);
    };
    return (biased(z) << 32U) | biased(x);
}

template <class Policy>
void BasicCrowd<Policy>::gather_neighbours(CrowdAgentId id, u32 wanted,
                                           CrowdReport& report) noexcept {
    neighbours_.clear();
    neighbour_distance_.clear();
    if (wanted == 0) {
        return;
    }
    const Agent& self = agents_[id];
    const i32 x = cell_of(Policy::across(self.position));
    const i32 z = cell_of(Policy::along(self.position));
    const i32 span = Policy::cell_span(self.params.neighbour_distance, cell_size_);

    const GridCell* const first_cell = cells_.data();
    const GridCell* const last_cell = first_cell + cells_.size();
    for (i32 row = z - span; row <= z + span; ++row) {
        const u64 lowest = cell_key(x - span, row);
        const u64 highest = cell_key(x + span, row);
        const GridCell* cell =
            std::lower_bound(first_cell, last_cell, lowest,
                             [](const GridCell& c, u64 key) noexcept { return c.key < key; });
        for (; cell != last_cell && cell->key <= highest; ++cell) {
            ++report.cells_examined;
            consider_cell(id, *cell, wanted, report);
        }
    }
}

template <class Policy>
void BasicCrowd<Policy>::consider_cell(CrowdAgentId id, const GridCell& cell, u32 wanted,
                                       CrowdReport& report) noexcept {
    const Agent& self = agents_[id];
    const Wide range_squared = Policy::square(self.params.neighbour_distance);
    for (u32 slot = 0; slot < cell.count; ++slot) {
        const u32 other = cell_agents_[cell.first + slot];
        if (other == id) {
            continue;
        }
        ++report.neighbour_tests;
        const Vec offset = Policy::flatten(agents_[other].position - self.position);
        const Wide distance_squared = Policy::length_squared(offset);
        if (distance_squared > range_squared) {
            continue;
        }
        // An insertion sort into a list of at most `wanted`, ordered by (distance, id). The
        // second key is what makes two agents at the same distance resolve the same way in
        // every run, and what makes the result independent of the order cells are visited in.
        const auto after = [](const Wide& distance_a, u32 id_a, const Wide& distance_b,
                              u32 id_b) noexcept {
            return distance_a > distance_b || (distance_a == distance_b && id_a > id_b);
        };
        if (neighbours_.size() == wanted &&
            !after(neighbour_distance_[wanted - 1], neighbours_[wanted - 1], distance_squared,
                   other)) {
            continue;
        }
        if (neighbours_.size() < wanted) {
            if (!neighbours_.push_back(other) || !neighbour_distance_.push_back(distance_squared)) {
                return;
            }
        }
        usize slot_index = neighbours_.size() - 1;
        while (slot_index > 0 && after(neighbour_distance_[slot_index - 1],
                                       neighbours_[slot_index - 1], distance_squared, other)) {
            neighbours_[slot_index] = neighbours_[slot_index - 1];
            neighbour_distance_[slot_index] = neighbour_distance_[slot_index - 1];
            --slot_index;
        }
        neighbours_[slot_index] = other;
        neighbour_distance_[slot_index] = distance_squared;
    }
}

/// Time until two discs of combined radius `radius` touch, or the policy's infinity when they never
/// do.
///
/// `relative_position` is from this agent to the other; `relative_velocity` is this agent's minus
/// the other's. So the pair closes when the two point the SAME way — `w . v > 0` — which is the
/// sign that has to be right: with it inverted the solver answers "never" for every approach and
/// "now" for every retreat, which looks exactly like a solver that is switched off.
///
/// The standard ray-versus-disc solve on the plane: the future separation is `w - v t`, so
/// `|w - v t| = r` gives `t^2 |v|^2 - 2 t (w.v) + |w|^2 - r^2 = 0`.
template <class Policy>
Policy::Scalar BasicCrowd<Policy>::time_to_collision(Vec relative_position, Vec relative_velocity,
                                                     Scalar radius) noexcept {
    const Wide c = Policy::length_squared(relative_position) - Policy::square(radius);
    if (c < Policy::wide_zero()) {
        return Policy::zero();  // already overlapping
    }
    const Wide a = Policy::length_squared(relative_velocity);
    if (a < Policy::still_speed_squared()) {
        return Policy::infinity();
    }
    const Wide b = Policy::dot(relative_position, relative_velocity);
    if (b <= Policy::wide_zero()) {
        return Policy::infinity();  // separating, or moving across
    }
    const Wide discriminant = Policy::product(b, b) - Policy::product(a, c);
    if (discriminant <= Policy::wide_zero()) {
        return Policy::infinity();  // it passes wide
    }
    return (Policy::narrow(b) - Policy::sqrt(discriminant)) / Policy::narrow(a);
}

/// The push away from every neighbour, whatever they are doing. Computed at every tier, including
/// `Minimal`: a crowd standing still has no velocities to avoid, and without it a bottleneck
/// compacts into one point.
template <class Policy>
Policy::Vec BasicCrowd<Policy>::separation(const Agent& self) const noexcept {
    const Scalar two = Policy::two();
    Vec push = Policy::zero_vec();
    for (const u32 neighbour : neighbours_.span()) {
        const Agent& other = agents_[neighbour];
        const Vec offset = Policy::flatten(self.position - other.position);
        const Scalar distance = Policy::length(offset);
        const Scalar touching = self.params.radius + other.params.radius;
        if (distance < Policy::separation_floor() || distance > touching * two) {
            continue;
        }
        push += offset * (((touching * two) - distance) / (distance * touching * two));
    }
    return push * self.params.separation_weight * self.params.max_speed;
}

/// When `candidate` first collides with any neighbour, under the reciprocal assumption.
template <class Policy>
Policy::Scalar BasicCrowd<Policy>::soonest_collision(const Agent& self,
                                                     Vec candidate) const noexcept {
    Scalar soonest = Policy::infinity();
    for (const u32 neighbour : neighbours_.span()) {
        const Agent& other = agents_[neighbour];
        const Scalar share = Policy::responsibility(self.params.priority, other.params.priority);
        // THE RECIPROCAL ASSUMPTION, written out. The agent takes `share` of the correction and
        // expects the neighbour to take the rest, so the velocity it must test against the obstacle
        // is not the candidate itself but the one the pair would present between them:
        // (v' - (1 - share) * v) / share, minus the neighbour's. With an even share that is the
        // textbook `2v' - v - vOther`.
        const Vec reciprocal =
            (candidate - (self.velocity * (Policy::one() - share))) * (Policy::one() / share);
        const Vec relative_velocity = reciprocal - other.velocity;
        // THE SIDE BIAS. Without it, a perfectly head-on pair never resolves, and the failure is
        // not a rounding artefact: two agents on one line, with equal radii and opposite
        // velocities, present the solver with a scoring function exactly symmetric about that
        // line, so both choose the same side and the geometry is unchanged. Real crowds break the
        // symmetry with floating-point noise, which is precisely what a deterministic simulation
        // must not rely on — and a `Fixed` crowd has none to rely on. So each agent perceives its
        // neighbour displaced along the PERPENDICULAR OF THE VECTOR TO IT. That vector points
        // opposite ways for the two agents, so the bias sends them to opposite sides of the line,
        // with no identity comparison, no shared state and no random source. It is a fraction of
        // a radius, so it changes nothing about a pair that is not head-on.
        Vec towards = Policy::flatten(other.position - self.position);
        towards += Policy::perpendicular(towards) * Policy::side_bias();
        const Scalar when =
            time_to_collision(towards, relative_velocity, self.params.radius + other.params.radius);
        soonest = Policy::lesser(soonest, when);
    }
    return soonest;
}

template <class Policy>
Policy::Vec BasicCrowd<Policy>::solve(const Agent& self, Scalar dt, CrowdReport& report) noexcept {
    const auto tier = static_cast<usize>(self.tier);
    const Scalar max_speed = self.params.max_speed;
    const Vec preferred = Policy::clamp_speed(self.desired_velocity, max_speed);
    const Vec pushed = preferred + separation(self);

    const u32 rings = kCrowdCandidateRings[tier];
    const u32 spokes = kCrowdCandidateSpokes[tier];
    if (rings == 0 || spokes == 0 || neighbours_.empty()) {
        return Policy::clamp_speed(pushed, max_speed);
    }

    // The candidate set: the preferred velocity, then a polar lattice around it. Scored on
    // (deviation from preferred) + (penalty for colliding soon), the standard sampled-RVO
    // objective, with the reciprocal assumption folded into the relative velocity.
    Vec best = pushed;
    Scalar best_score = Policy::infinity();
    for (u32 ring = 0; ring <= rings; ++ring) {
        const u32 count = (ring == 0) ? 1u : spokes;
        const Scalar magnitude = max_speed * Policy::ratio(ring, rings);
        for (u32 spoke = 0; spoke < count; ++spoke) {
            const Vec candidate = (ring == 0) ? Policy::clamp_speed(pushed, max_speed)
                                              : Policy::lattice(spoke, count, magnitude);
            ++report.candidates_scored;
            const Scalar soonest = soonest_collision(self, candidate);

            const Vec deviation = candidate - pushed;
            Scalar score = Policy::sqrt(Policy::length_squared(deviation));
            if (soonest < self.params.time_horizon) {
                // A hyperbola rather than a step: a candidate that collides in a moment is much
                // worse than one that collides near the horizon, and a step would make the solver
                // flip between two candidates on either side of it.
                score += max_speed *
                         (self.params.time_horizon / Policy::greater(soonest, dt * Policy::half()));
            }
            if (score < best_score) {
                best_score = score;
                best = candidate;
            }
        }
    }
    return Policy::clamp_speed(best, max_speed);
}

template <class Policy>
Status BasicCrowd<Policy>::step(Scalar dt, CrowdReport& report) noexcept {
    report = CrowdReport{};
    if (dt <= Policy::zero()) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "the step must be positive"});
    }
    if (Status built = rebuild_grid(report); !built) {
        return built;
    }

    for (usize index = 0; index < agents_.size(); ++index) {
        if (!agents_[index].active) {
            continue;
        }
        Agent& self = agents_[index];
        ++report.agents;
        ++report.agents_by_tier[static_cast<usize>(self.tier)];

        const u32 wanted = std::min(self.params.max_neighbours,
                                    kCrowdTierNeighbours[static_cast<usize>(self.tier)]);
        gather_neighbours(static_cast<CrowdAgentId>(index), wanted, report);
        const Vec target = solve(self, dt, report);

        // The acceleration limit is what keeps a solved velocity from being a teleport, and it is
        // applied here rather than inside the solver so that the solver's answer stays a statement
        // about avoidance rather than about how fast this agent can change its mind.
        const Vec change = target - self.velocity;
        const Scalar magnitude = Policy::length(Policy::flatten(change));
        const Scalar allowed = self.params.max_acceleration * dt;
        self.velocity = (magnitude > allowed && magnitude > Policy::change_floor())
                            ? self.velocity + (change * (allowed / magnitude))
                            : target;
        const Vec difference =
            self.velocity - Policy::clamp_speed(self.desired_velocity, self.params.max_speed);
        if (Policy::length(Policy::flatten(difference)) > Policy::adjusted_floor()) {
            ++report.agents_adjusted;
        }
    }
    return ok();
}

template <class Policy>
void BasicCrowd<Policy>::integrate(Scalar dt) noexcept {
    for (Agent& self : agents_.span()) {
        if (self.active) {
            self.position += self.velocity * dt;
        }
    }
}

}  // namespace cy::navigation
