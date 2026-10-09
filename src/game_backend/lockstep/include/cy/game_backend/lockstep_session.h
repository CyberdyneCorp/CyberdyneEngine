// SPDX-License-Identifier: MIT
#pragma once
// A lockstep session of fixed-point units, driven by orders in a command stream.
// openspec/changes/add-deterministic-math stage 8 — what ABI 1.8's `lockstep_*` entries reach.
//
// THE PIECES, EACH ALREADY ITS OWN SUBSYSTEM, PUT TOGETHER THE WAY A LOCKSTEP RTS PUTS THEM:
//
//   world      a baked navigation mesh, converted once into a `Fixed` world (`FixedNavMesh`)
//   units      the kinematic mover (`KinematicMover`): positions, velocities, headings in `Fixed`
//   orders     a `cy::gameplay::CommandStream` under `Lockstep`, whose order payload declares its
//              fields — `kind` and `group` integers, `target_x` and `target_z` raw `Fixed` — so a
//              float payload would be refused at declaration
//   paths      A* and the funnel in the `Fixed` world, one per unit, to its slot in a formation
//              around the order's target
//   avoidance  navigation's crowd solver instantiated over `Fixed` (`movement::FixedCrowd`)
//
// The session refuses to start unless `DeterminismConfiguration::require(Lockstep)` admits it: the
// mover, a `Fixed` navigation world without runtime rebuilds, and the payload-checked stream.
//
// ONE TICK. Commit the stream's commands for the tick; execute each order (plan paths, or stop);
// set every unit's desired velocity from its path; run avoidance; step the mover; note who finished
// its path; hash. The state hash folds the mover's state and every unit's progress along its path;
// the digest folds every tick's state hash in order, starting from the world hash.
//
// TWO PEERS. `record()` is the issuing side. `receive()` is every other peer: it records exactly
// the commands the issuer's log holds for the tick about to run, and nothing else. Two sessions
// built separately and driven by one log agree on every tick's hash — and, because every quantity
// is integer arithmetic, on every architecture (`determinism.cross_leg` publishes one such digest).

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/vec.h>
#include <cy/core/memory/array.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/movement/crowd.h>
#include <cy/movement/mover.h>
#include <cy/movement/nav_mesh.h>
#include <cy/movement/path.h>
#include <cy/navigation/navmesh.h>

namespace cy::jobs {
class JobSystem;
}  // namespace cy::jobs

namespace cy::game_backend {

using detmath::Fixed;
using detmath::FixedVec2;

/// How a session runs. Every field is part of the simulation's definition and of its world hash.
struct LockstepConfig {
    /// Folded into the world hash: peers configured differently disagree before the first tick.
    u64 seed = 0;
    movement::MoverParams mover{};
    /// Crowd avoidance between units; off, units only separate when they overlap.
    bool avoidance = true;
    movement::FixedAvoidanceParams avoidance_params{};
    /// A path point is reached within this distance.
    Fixed arrival = Fixed::from_raw(Fixed::kOneRaw / 4);
    /// The distance between neighbours in a group's formation. Beyond the crowd's separation reach
    /// — twice the two radii, 2 m at the default half-metre radius — so units that have reached
    /// their slots stop pushing each other out of them.
    Fixed formation_spacing = Fixed::from_raw((Fixed::kOneRaw * 9) / 4);
    /// The polygons one A* query may expand.
    u32 node_budget = 4096;
};

/// A unit, as `enlist` takes it. Zero radius and speed are the mover's defaults.
struct LockstepUnitSpec {
    u32 group = 0;
    /// Presentation only: the scene entity that draws the unit. Not hashed.
    u64 entity = 0;
    FixedVec2 position;
    Fixed radius;
    Fixed max_speed;
};

/// What an order tells a group: `CyLockstepOrderKind`, value for value.
enum class LockstepOrderKind : u32 { Move = 0, Stop = 1 };

/// The order command's payload: integers and raw `Fixed`, described field by field to the stream.
struct LockstepOrderPayload {
    u32 kind = 0;
    u32 group = 0;
    i64 target_x = 0;
    i64 target_z = 0;
};

/// The order command's stable identity, "ORDR".
inline constexpr u32 kLockstepOrderStableId = 0x4F524452U;

/// One unit's state, as the last tick left it.
struct LockstepUnitState {
    u32 group = 0;
    u64 entity = 0;
    FixedVec2 position;
    FixedVec2 velocity;
    Fixed height;
    detmath::Angle heading;
    bool moving = false;
    bool just_arrived = false;
};

class LockstepSession {
public:
    LockstepSession(Allocator& allocator, const LockstepConfig& config) noexcept;

    LockstepSession(const LockstepSession&) = delete;
    LockstepSession& operator=(const LockstepSession&) = delete;
    LockstepSession(LockstepSession&&) = delete;
    LockstepSession& operator=(LockstepSession&&) = delete;
    ~LockstepSession() = default;

    /// Convert the baked mesh into this session's `Fixed` world. Before `start()`.
    [[nodiscard]] Status load(const navigation::NavMesh& baked) noexcept;
    /// A unit, in processing order: its index is its identity. Before `start()`; PermissionDenied
    /// after.
    [[nodiscard]] Expected<u32, Error> enlist(const LockstepUnitSpec& spec) noexcept;
    /// Declare the order command, pass the `Lockstep` profile check, open the participant, and
    /// compute the world hash. Called by `advance()` when not yet called.
    [[nodiscard]] Status start() noexcept;
    [[nodiscard]] bool started() const noexcept { return started_; }

    /// The issuing peer: record `order` for the next tick to run. NotFound for a group no unit is
    /// in; InvalidArgument for an unknown kind.
    [[nodiscard]] Status record(const LockstepOrderPayload& order) noexcept;
    /// Every other peer: record what `log` holds for the next tick to run, from `cursor` on.
    [[nodiscard]] Status receive(const gameplay::CommandLog& log, u32& cursor) noexcept;
    /// Run the next tick. With `jobs`, the mover's passes run on workers, with the same bits.
    [[nodiscard]] Status advance(jobs::JobSystem* jobs = nullptr) noexcept;

    /// Ticks run so far; the next tick to run has this number.
    [[nodiscard]] u64 tick() const noexcept { return tick_; }
    [[nodiscard]] u32 units() const noexcept { return mover_.size(); }
    [[nodiscard]] Expected<LockstepUnitState, Error> unit(u32 index) const noexcept;
    [[nodiscard]] bool has_group(u32 group) const noexcept;
    /// The last tick's state hash; the world hash before the first tick.
    [[nodiscard]] u64 state_hash() const noexcept { return state_hash_; }
    /// The world hash, then every tick's state hash, folded in order.
    [[nodiscard]] u64 digest() const noexcept { return digest_; }
    [[nodiscard]] u64 world_hash() const noexcept { return world_hash_; }
    [[nodiscard]] const gameplay::CommandLog& log() const noexcept { return commands_.log(); }
    [[nodiscard]] const movement::KinematicMover& mover() const noexcept { return mover_; }
    /// Orders executed, and paths planned and found by them.
    [[nodiscard]] u32 orders_executed() const noexcept { return orders_executed_; }
    [[nodiscard]] u32 paths_planned() const noexcept { return paths_planned_; }
    [[nodiscard]] u32 paths_found() const noexcept { return paths_found_; }
    /// Authoritative subsystems the profile check admitted.
    [[nodiscard]] u32 admitted_subsystems() const noexcept { return admitted_subsystems_; }

private:
    [[nodiscard]] Status admit_profile() noexcept;
    [[nodiscard]] Status record_command(gameplay::Command command) noexcept;
    [[nodiscard]] Status execute(const gameplay::Command& command) noexcept;
    [[nodiscard]] Status move_group(u32 group, FixedVec2 target) noexcept;
    [[nodiscard]] FixedVec2 slot(u32 k, u32 group_size) const noexcept;
    [[nodiscard]] u64 compute_state_hash() const noexcept;
    [[nodiscard]] u64 compute_world_hash() const noexcept;

    Allocator& allocator_;
    LockstepConfig config_;
    movement::FixedNavMesh mesh_;
    movement::KinematicMover mover_;
    movement::FixedCrowd crowd_;
    movement::FixedPathSearch search_;
    Array<movement::FixedPolyIndex> corridor_;
    gameplay::GameSession session_;
    gameplay::ControlRegistry control_;
    gameplay::CommandStream commands_;
    gameplay::CommandTypeId order_type_ = gameplay::kInvalidCommandType;
    gameplay::ParticipantId participant_;
    u32 producer_ = 0;

    Array<u32> groups_;
    Array<u64> entities_;
    Array<Array<FixedVec2>> paths_;
    Array<u32> cursors_;
    Array<u8> moving_;
    Array<u8> arrived_;

    bool loaded_ = false;
    bool started_ = false;
    u64 tick_ = 0;
    u64 world_hash_ = 0;
    u64 state_hash_ = 0;
    u64 digest_ = 0;
    u32 orders_executed_ = 0;
    u32 paths_planned_ = 0;
    u32 paths_found_ = 0;
    u32 admitted_subsystems_ = 0;
};

}  // namespace cy::game_backend
