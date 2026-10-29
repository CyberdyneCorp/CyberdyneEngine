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
// COMBAT (lockstep_combat.h, ABI 1.16). Armed units and fortified structures fight: between the
// orders and the avoidance a tick thinks — targets, chases, blows and launches — and after the
// mover's step its projectiles fly and land. A session nothing was armed in is the session before.
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
#include <cy/game_backend/lockstep_combat.h>
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

/// What an order tells a group: `CyLockstepOrderKind`, value for value. The two attacks are for
/// armed units (lockstep_combat.h): an attack-move goes to the point fighting what it meets, an
/// attack goes for the enemy standing nearest the point, within `kAttackPickRadius` of its
/// footprint — and is an attack-move to the point when none does.
enum class LockstepOrderKind : u32 { Move = 0, Stop = 1, AttackMove = 2, Attack = 3 };

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
    /// Give unit `index` health and a weapon (lockstep_combat.h). Before `start()`;
    /// PermissionDenied after, NotFound for a unit never enlisted, InvalidArgument for a spec with
    /// no health, no damage, no cooldown, a negative reach, or a ranged weapon with no speed.
    [[nodiscard]] Status arm(u32 index, const LockstepCombatSpec& spec) noexcept;
    /// A structure: a static target and an obstacle. Before `start()`, as `arm`; its index is its
    /// identity. InvalidArgument for no health or no footprint.
    [[nodiscard]] Expected<u32, Error> fortify(const LockstepStructureSpec& spec) noexcept;
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

    // --- Combat (lockstep_combat.h) ------------------------------------------------------------
    /// Whether anything was armed or fortified: the combat state is part of the hashes.
    [[nodiscard]] bool combat() const noexcept { return combat_; }
    /// Unit `index`'s combat state. NotFound for a unit never enlisted.
    [[nodiscard]] Expected<LockstepCombatantState, Error> combatant(u32 index) const noexcept;
    [[nodiscard]] u32 structures() const noexcept { return static_cast<u32>(structures_.size()); }
    [[nodiscard]] Expected<LockstepStructureState, Error> structure(u32 index) const noexcept;
    /// The last tick's events, in the order they happened.
    [[nodiscard]] Span<const LockstepEvent> events() const noexcept { return events_.span(); }
    /// Events made since the session started.
    [[nodiscard]] u64 event_count() const noexcept { return next_event_; }
    /// The projectiles in flight, in launch order.
    [[nodiscard]] Span<const LockstepProjectile> projectiles() const noexcept {
        return projectiles_.span();
    }

private:
    [[nodiscard]] Status admit_profile() noexcept;
    [[nodiscard]] Status record_command(gameplay::Command command) noexcept;
    [[nodiscard]] Status execute(const gameplay::Command& command) noexcept;
    [[nodiscard]] Status move_group(u32 group, FixedVec2 target) noexcept;
    [[nodiscard]] FixedVec2 slot(u32 k, u32 group_size) const noexcept;
    [[nodiscard]] u64 compute_state_hash() const noexcept;
    [[nodiscard]] u64 compute_world_hash() const noexcept;

    // Combat, in lockstep_combat.cpp.
    struct Combatant {
        LockstepCombatSpec spec;
        LockstepCombatantState state;
        FixedVec2 destination;
        u64 repath_at = 0;
        /// It left its way to fight: once it has nothing to fight, it takes its way up again.
        bool diverted = false;
    };
    struct Structure {
        LockstepStructureSpec spec;
        u32 health = 0;
    };
    [[nodiscard]] Status order_attack(u32 group, FixedVec2 point, bool pick) noexcept;
    void arrive(u32 unit) noexcept;
    [[nodiscard]] Status think() noexcept;
    [[nodiscard]] Status think_unit(u32 unit) noexcept;
    [[nodiscard]] Status fly() noexcept;
    [[nodiscard]] Status land(const LockstepProjectile& shot) noexcept;
    [[nodiscard]] Status fire(u32 unit) noexcept;
    [[nodiscard]] Status deal(u32 attacker, u32 team, LockstepTarget target, u32 damage) noexcept;
    [[nodiscard]] Status emit(LockstepEvent event) noexcept;
    [[nodiscard]] Status plan(u32 unit, FixedVec2 to) noexcept;
    void halt(u32 unit) noexcept;
    [[nodiscard]] LockstepTarget acquire(u32 unit) const noexcept;
    [[nodiscard]] LockstepTarget nearest_enemy(u32 team, FixedVec2 point,
                                               Fixed within) const noexcept;
    [[nodiscard]] bool standing(LockstepTarget target) const noexcept;
    [[nodiscard]] FixedVec2 where(LockstepTarget target) const noexcept;
    [[nodiscard]] Fixed footprint(LockstepTarget target) const noexcept;
    [[nodiscard]] Fixed gap(u32 unit, LockstepTarget target) const noexcept;
    [[nodiscard]] u64 fold_combat_state(u64 fold) const noexcept;
    [[nodiscard]] u64 fold_combat_world(u64 fold) const noexcept;

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

    Array<Combatant> combatants_;
    Array<Structure> structures_;
    Array<LockstepProjectile> projectiles_;
    Array<LockstepEvent> events_;
    u64 next_event_ = 0;
    u32 next_projectile_ = 0;
    bool combat_ = false;

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
