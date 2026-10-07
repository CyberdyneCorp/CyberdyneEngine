// Local avoidance and the crowd. See cy/navigation/crowd.h for the argument; this file is the
// sampled reciprocal-velocity-obstacle solver and the uniform grid that bounds its neighbour set.
// The grid is a cell table sorted by key, so finding a cell is a binary search rather than a scan.

#include <cy/core/base/assert.h>
#include <cy/navigation/crowd.h>
#include <cy/navigation/follow.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cy::navigation {
namespace {

/// The candidate lattice, per tier. Fixed sets rather than a random sample: determinism is a
/// requirement and a random sample would make one scenario produce two crowds.
constexpr u32 kCandidateRings[static_cast<usize>(CrowdTier::Count)] = {3, 2, 0};
constexpr u32 kCandidateSpokes[static_cast<usize>(CrowdTier::Count)] = {8, 4, 0};
constexpr u32 kTierNeighbours[static_cast<usize>(CrowdTier::Count)] = {6, 3, 3};

[[nodiscard]] Vec3 flatten(Vec3 v) noexcept {
    return Vec3{v.x, 0.0F, v.z};
}

[[nodiscard]] Vec3 clamp_speed(Vec3 v, f32 max_speed) noexcept {
    const f32 speed_squared = (v.x * v.x) + (v.z * v.z);
    if (speed_squared <= max_speed * max_speed || speed_squared < 1e-12F) {
        return v;
    }
    const f32 scale = max_speed / std::sqrt(speed_squared);
    return Vec3{v.x * scale, v.y, v.z * scale};
}

/// Time until two discs of combined radius `radius` touch, or infinity when they never do.
///
/// `relative_position` is from this agent to the other; `relative_velocity` is this agent's minus
/// the other's. So the pair closes when the two point the SAME way — `w . v > 0` — which is the
/// sign that has to be right and the one a reader should check first: with it inverted the solver
/// answers "never" for every approach and "now" for every retreat, which looks exactly like a
/// solver that is switched off.
///
/// The standard ray-versus-disc solve, in XZ: the future separation is `w - v t`, so
/// `|w - v t| = r` gives `t^2 |v|^2 - 2 t (w.v) + |w|^2 - r^2 = 0`.
[[nodiscard]] f32 time_to_collision(Vec3 relative_position, Vec3 relative_velocity,
                                    f32 radius) noexcept {
    const f32 c = (relative_position.x * relative_position.x) +
                  (relative_position.z * relative_position.z) - (radius * radius);
    if (c < 0.0F) {
        return 0.0F;  // already overlapping
    }
    const f32 a =
        (relative_velocity.x * relative_velocity.x) + (relative_velocity.z * relative_velocity.z);
    if (a < 1e-9F) {
        return math::kInfinity;
    }
    const f32 b =
        (relative_position.x * relative_velocity.x) + (relative_position.z * relative_velocity.z);
    if (b <= 0.0F) {
        return math::kInfinity;  // separating, or moving across
    }
    const f32 discriminant = (b * b) - (a * c);
    if (discriminant <= 0.0F) {
        return math::kInfinity;  // it passes wide
    }
    return (b - std::sqrt(discriminant)) / a;
}

/// How much of the avoidance this agent takes. Half between equals, tilted by priority so the
/// lower-priority agent yields, and never 0 or 1 — see the header.
[[nodiscard]] f32 responsibility(u8 mine, u8 theirs) noexcept {
    if (mine == theirs) {
        return 0.5F;
    }
    return (mine < theirs) ? 0.85F : 0.15F;
}

/// The XZ perpendicular, rotated a quarter turn.
[[nodiscard]] Vec3 perpendicular(Vec3 v) noexcept {
    return Vec3{v.z, 0.0F, -v.x};
}

/// How far to one side an agent perceives a neighbour that is exactly ahead of it.
///
/// WITHOUT THIS, A PERFECTLY HEAD-ON PAIR NEVER RESOLVES, and the failure is not a rounding
/// artefact: two agents on one line, with equal radii and opposite velocities, present the solver
/// with a scoring function that is exactly symmetric about that line, so both choose the same side
/// and the geometry is unchanged. Real crowds break the symmetry with floating-point noise, which
/// is precisely what a deterministic simulation must not rely on.
///
/// So each agent perceives its neighbour displaced along the PERPENDICULAR OF THE VECTOR TO IT.
/// That vector points opposite ways for the two agents, so the bias sends them to opposite sides of
/// the line — the rule a corridor full of people follows — and it needs no identity comparison, no
/// shared state and no random source. It is a fraction of a radius, so it changes nothing about a
/// pair that is not head-on.
constexpr f32 kSideBias = 0.15F;

}  // namespace

const char* crowd_tier_name(CrowdTier tier) noexcept {
    switch (tier) {
        case CrowdTier::Full:
            return "Full";
        case CrowdTier::Reduced:
            return "Reduced";
        case CrowdTier::Minimal:
            return "Minimal";
        case CrowdTier::Count:
            break;
    }
    return "unknown";
}

Crowd::Crowd(Allocator& allocator, f32 cell_size) noexcept
    : agents_(allocator),
      cells_(allocator),
      cell_agents_(allocator),
      cell_keys_(allocator),
      neighbours_(allocator),
      neighbour_distance_(allocator),
      cell_size_(cell_size > 0.0F ? cell_size : 4.0F) {}

Expected<CrowdAgentId, Error> Crowd::add(Vec3 position, const AvoidanceParams& params) noexcept {
    for (usize index = 0; index < agents_.size(); ++index) {
        if (!agents_[index].active) {
            agents_[index] = CrowdAgent{};
            agents_[index].position = position;
            agents_[index].params = params;
            agents_[index].active = true;
            ++live_;
            return static_cast<CrowdAgentId>(index);
        }
    }
    CrowdAgent agent;
    agent.position = position;
    agent.params = params;
    agent.active = true;
    if (Status pushed = agents_.push_back(agent); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++live_;
    return static_cast<CrowdAgentId>(agents_.size() - 1);
}

Status Crowd::remove(CrowdAgentId id) noexcept {
    if (id >= agents_.size() || !agents_[id].active) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such crowd agent"});
    }
    agents_[id].active = false;
    --live_;
    return ok();
}

CrowdAgent* Crowd::agent(CrowdAgentId id) noexcept {
    if (id >= agents_.size() || !agents_[id].active) {
        return nullptr;
    }
    return &agents_[id];
}

const CrowdAgent* Crowd::agent(CrowdAgentId id) const noexcept {
    if (id >= agents_.size() || !agents_[id].active) {
        return nullptr;
    }
    return &agents_[id];
}

void Crowd::set_desired_velocity(CrowdAgentId id, Vec3 velocity) noexcept {
    if (CrowdAgent* target = agent(id); target != nullptr) {
        target->desired_velocity = flatten(velocity);
    }
}

void Crowd::set_tier(CrowdAgentId id, CrowdTier tier) noexcept {
    if (CrowdAgent* target = agent(id); target != nullptr) {
        target->tier = tier;
    }
}

void Crowd::steer_towards(CrowdAgentId id, Vec3 target, f32 arrival_distance) noexcept {
    CrowdAgent* subject = agent(id);
    if (subject == nullptr) {
        return;
    }
    const Vec3 offset = flatten(target - subject->position);
    const f32 distance = length(offset);
    if (distance <= arrival_distance || distance < 1e-5F) {
        subject->desired_velocity = Vec3{};
        return;
    }
    // Slow into the arrival radius rather than stopping dead on the boundary.
    const f32 speed = std::fmin(subject->params.max_speed,
                                subject->params.max_speed * (distance / (arrival_distance * 4.0F)));
    subject->desired_velocity = offset * (std::fmax(speed, 0.05F) / distance);
}

Status Crowd::rebuild_grid(CrowdReport& report) noexcept {
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
        cell_keys_[index] =
            cell_key(cell_of(agents_[index].position.x), cell_of(agents_[index].position.z));
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

i32 Crowd::cell_of(f32 coordinate) const noexcept {
    return static_cast<i32>(std::floor(coordinate / cell_size_));
}

u64 Crowd::cell_key(i32 x, i32 z) noexcept {
    // Row-major with z the major key, each coordinate biased into an unsigned range so that the
    // packed key orders the same way the signed pair does. A row of cells is then one contiguous,
    // ascending run of keys.
    const auto biased = [](i32 value) noexcept {
        return static_cast<u64>(static_cast<u32>(value) ^ 0x80000000U);
    };
    return (biased(z) << 32U) | biased(x);
}

void Crowd::gather_neighbours(CrowdAgentId id, u32 wanted, CrowdReport& report) noexcept {
    neighbours_.clear();
    neighbour_distance_.clear();
    if (wanted == 0) {
        return;
    }
    const CrowdAgent& self = agents_[id];
    const f32 range = self.params.neighbour_distance;
    const i32 x = cell_of(self.position.x);
    const i32 z = cell_of(self.position.z);
    const i32 span = static_cast<i32>(std::ceil(range / cell_size_));

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

void Crowd::consider_cell(CrowdAgentId id, const GridCell& cell, u32 wanted,
                          CrowdReport& report) noexcept {
    const CrowdAgent& self = agents_[id];
    const f32 range = self.params.neighbour_distance;
    for (u32 slot = 0; slot < cell.count; ++slot) {
        const u32 other = cell_agents_[cell.first + slot];
        if (other == id) {
            continue;
        }
        ++report.neighbour_tests;
        const Vec3 offset = flatten(agents_[other].position - self.position);
        const f32 distance_squared = (offset.x * offset.x) + (offset.z * offset.z);
        if (distance_squared > range * range) {
            continue;
        }
        // An insertion sort into a list of at most `wanted`, ordered by (distance, id). The
        // second key is what makes two agents at the same distance resolve the same way in
        // every run, and what makes the result independent of the order cells are visited in —
        // see the header.
        const auto after = [](f32 distance_a, u32 id_a, f32 distance_b, u32 id_b) noexcept {
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

Vec3 Crowd::solve(const CrowdAgent& self, f32 dt, CrowdReport& report) noexcept {
    const auto tier = static_cast<usize>(self.tier);
    const Vec3 preferred = clamp_speed(self.desired_velocity, self.params.max_speed);

    // Separation is computed at every tier, including `Minimal`: a crowd standing still has no
    // velocities to avoid, and without it a bottleneck compacts into one point.
    Vec3 separation{};
    for (const u32 neighbour : neighbours_.span()) {
        const CrowdAgent& other = agents_[neighbour];
        const Vec3 offset = flatten(self.position - other.position);
        const f32 distance = length(offset);
        const f32 touching = self.params.radius + other.params.radius;
        if (distance < 1e-4F || distance > touching * 2.0F) {
            continue;
        }
        separation += offset * (((touching * 2.0F) - distance) / (distance * touching * 2.0F));
    }
    separation = separation * self.params.separation_weight * self.params.max_speed;

    const u32 rings = kCandidateRings[tier];
    const u32 spokes = kCandidateSpokes[tier];
    if (rings == 0 || spokes == 0 || neighbours_.empty()) {
        return clamp_speed(preferred + separation, self.params.max_speed);
    }

    // The candidate set: the preferred velocity, then a polar lattice around it. Scored on
    // (deviation from preferred) + (penalty for colliding soon), the standard sampled-RVO
    // objective, with the reciprocal assumption folded into the relative velocity.
    Vec3 best = preferred + separation;
    f32 best_score = math::kInfinity;
    for (u32 ring = 0; ring <= rings; ++ring) {
        const u32 count = (ring == 0) ? 1u : spokes;
        const f32 magnitude =
            self.params.max_speed * (static_cast<f32>(ring) / static_cast<f32>(rings));
        for (u32 spoke = 0; spoke < count; ++spoke) {
            const f32 angle = (2.0F * std::numbers::pi_v<f32> * static_cast<f32>(spoke)) /
                              static_cast<f32>(count);
            const Vec3 candidate =
                (ring == 0) ? clamp_speed(preferred + separation, self.params.max_speed)
                            : Vec3{std::cos(angle) * magnitude, 0.0F, std::sin(angle) * magnitude};
            ++report.candidates_scored;

            f32 soonest = math::kInfinity;
            for (const u32 neighbour : neighbours_.span()) {
                const CrowdAgent& other = agents_[neighbour];
                const f32 share = responsibility(self.params.priority, other.params.priority);
                // THE RECIPROCAL ASSUMPTION, written out. The agent takes `share` of the
                // correction and expects the neighbour to take the rest, so the velocity it must
                // test against the obstacle is not the candidate itself but the one the pair would
                // present between them: (v' - (1 - share) * v) / share, minus the neighbour's.
                // With an even share that is the textbook `2v' - v - vOther`.
                const Vec3 reciprocal =
                    (candidate - (self.velocity * (1.0F - share))) * (1.0F / share);
                const Vec3 relative_velocity = reciprocal - other.velocity;
                Vec3 towards = flatten(other.position - self.position);
                towards += perpendicular(towards) * kSideBias;
                const f32 when = time_to_collision(towards, relative_velocity,
                                                   self.params.radius + other.params.radius);
                soonest = std::fmin(soonest, when);
            }

            const Vec3 deviation = candidate - (preferred + separation);
            f32 score = std::sqrt((deviation.x * deviation.x) + (deviation.z * deviation.z));
            if (soonest < self.params.time_horizon) {
                // A hyperbola rather than a step: a candidate that collides in a moment is much
                // worse than one that collides near the horizon, and a step would make the solver
                // flip between two candidates on either side of it.
                score += self.params.max_speed *
                         (self.params.time_horizon / std::fmax(soonest, dt * 0.5F));
            }
            if (score < best_score) {
                best_score = score;
                best = candidate;
            }
        }
    }
    return clamp_speed(best, self.params.max_speed);
}

Status Crowd::step(f32 dt, CrowdReport& report) noexcept {
    report = CrowdReport{};
    if (dt <= 0.0F) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "the step must be positive"});
    }
    if (Status built = rebuild_grid(report); !built) {
        return built;
    }

    for (usize index = 0; index < agents_.size(); ++index) {
        if (!agents_[index].active) {
            continue;
        }
        CrowdAgent& self = agents_[index];
        ++report.agents;
        ++report.agents_by_tier[static_cast<usize>(self.tier)];

        const u32 wanted =
            std::min(self.params.max_neighbours, kTierNeighbours[static_cast<usize>(self.tier)]);
        gather_neighbours(static_cast<CrowdAgentId>(index), wanted, report);
        const Vec3 target = solve(self, dt, report);

        // The acceleration limit is what keeps a solved velocity from being a teleport, and it is
        // applied here rather than inside the solver so that the solver's answer stays a statement
        // about avoidance rather than about how fast this agent can change its mind.
        const Vec3 change = target - self.velocity;
        const f32 magnitude = length(flatten(change));
        const f32 allowed = self.params.max_acceleration * dt;
        self.velocity = (magnitude > allowed && magnitude > 1e-6F)
                            ? self.velocity + (change * (allowed / magnitude))
                            : target;
        const Vec3 difference =
            self.velocity - clamp_speed(self.desired_velocity, self.params.max_speed);
        if (length(flatten(difference)) > 1e-3F) {
            ++report.agents_adjusted;
        }
    }
    return ok();
}

void Crowd::integrate(f32 dt) noexcept {
    for (CrowdAgent& self : agents_.span()) {
        if (self.active) {
            self.position += self.velocity * dt;
        }
    }
}

namespace {

/// `follow_points`' float instantiation: the XZ offset, its length against the arrival distance,
/// and full speed along it.
struct FloatFollowPolicy {
    using Vec = Vec3;
    using Scalar = f32;

    [[nodiscard]] static Vec3 position(const PathPoint& point) noexcept { return point.position; }
    [[nodiscard]] static Vec3 offset(Vec3 to, Vec3 from) noexcept {
        return Vec3{to.x - from.x, 0.0F, to.z - from.z};
    }
    [[nodiscard]] static bool beyond(Vec3 offset, f32 arrival) noexcept {
        return length(offset) > arrival;
    }
    [[nodiscard]] static Vec3 toward(Vec3 offset, f32 speed) noexcept {
        return offset * (speed / std::fmax(length(offset), 1e-5F));
    }
};

}  // namespace

Vec3 follow_path(Span<const PathPoint> path, Vec3 position, f32 speed, f32 arrival_distance,
                 u32& cursor) noexcept {
    return follow_points(FloatFollowPolicy{}, path, position, speed, arrival_distance, cursor);
}

Vec3 follow_field(const FlowField& field, Vec3 position, f32 speed) noexcept {
    const Vec3 direction = field.direction_at(position);
    return direction * speed;
}

}  // namespace cy::navigation
