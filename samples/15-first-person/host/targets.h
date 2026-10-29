// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_FIRST_PERSON_TARGETS_H
#define CY_SAMPLE_FIRST_PERSON_TARGETS_H
// targets.h — the four targets, driven by the engine's AI (`cy::ai`). They are the engine's, not
// the game's: the Swift game shoots them, and what a target does about it is decided here, by
// `ai-system`'s own machinery.
//
// ================================================================================================
// WHAT A TARGET IS
// ================================================================================================
//
// An AI agent with a perception sensor, a knowledge store and one shared behaviour program:
//
//   selector
//     sequence  [down]      fall            health gone: topple over, and leave every layer a
//                                           shot or a character uses — and get back up, solid
//                                           again, when the game gives the health back
//     sequence  [hit]       flinch          a hit felt this moment: stagger back from where it
//                                           came from
//     sequence  [contact]   engage          the player is known: face where it was last seen and
//                                           strafe across the lane
//     patrol                                walk between two points
//
// The tree is compiled by `cy::graph::behaviour` from a graph built in `build`, and every task
// succeeds the tick it runs, so the tree is walked from the root each tick and a target reacts the
// tick its knowledge changes.
//
// ================================================================================================
// WHAT IT KNOWS, AND FROM WHERE
// ================================================================================================
//
// SIGHT is `ai-system`'s batched perception: one `PerceptionScheduler::update` a tick over every
// standing target, its visibility batch answered by the PHYSICS WORLD — a ray from the target's
// eyes to the player's, which a wall, a pillar or the beam blocks (`ArenaSight`).
//
// A HIT is the game's: the Swift game owns each target's health, on a `Health` component it adds to
// the target's node and writes in its fixed step when its raycast hits (game/Targets.swift). The
// host reads the component once a tick, after the game's fixed step and before this one, and a
// count that moved is a `SenseKind::Damage` perception of the player at the shot's source. A
// target never reads the game's code, and the game never reads this one's.
//
// DETERMINISM: everything here is a function of the tick, the physics world and the game's
// components — no clock, no frame time — so two runs of one script produce one digest.

#include <cy/ai/knowledge.h>
#include <cy/ai/perception.h>
#include <cy/ai/runtime.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/ecs/entity.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_behaviour.h>
#include <cy/servers/physics/server.h>

#include <memory>
#include <vector>

#include "arena.h"
#include "bodies.h"

namespace sample::fps {

/// What a target is doing, as the tree decided it this tick.
enum class TargetMode : cy::u8 {
    Patrol = 0,
    Engage,
    Flinch,
    Down,
    Count,
};

[[nodiscard]] const char* target_mode_name(TargetMode mode) noexcept;

/// What the game wrote on a target this tick: its `Health` component, as the host read it.
struct TargetHealth {
    bool present = false;
    cy::f32 points = 0.0F;
    cy::f32 hits = 0.0F;
    /// Where the shot that hit it last came from.
    cy::Vec3 source{0.0F, 0.0F, 0.0F};
};

/// One target, as the renderer and the report see it.
struct TargetState {
    /// The feet, on the floor.
    cy::Vec3 feet{0.0F, 0.0F, 0.0F};
    /// Turning right of −Z, radians: the way it faces.
    cy::f32 yaw = 0.0F;
    /// 0 standing, 1 lying on its back.
    cy::f32 fall = 0.0F;
    TargetMode mode = TargetMode::Patrol;
    /// Ticks spent in each mode, and the hits it felt.
    cy::u32 ticks_in[static_cast<cy::usize>(TargetMode::Count)] = {};
    cy::u32 hits_felt = 0;
    /// The tick it first saw the player, or zero.
    cy::u32 first_sighting = 0;
    /// Seconds of stagger left.
    cy::f32 flinch = 0.0F;
    /// Where it last knew the player to be.
    cy::Vec3 known{0.0F, 0.0F, 0.0F};
    bool knows = false;
    cy::f32 hits_seen = 0.0F;
    /// Times it got back up, the game having given its health back.
    cy::u32 rises = 0;
};

/// The body's placement for a target's state: standing, turned to its yaw, or toppled about its
/// heels by `fall`. The renderer draws the same placement.
[[nodiscard]] cy::Transform target_placement(const TargetState& state) noexcept;
/// Where its head is drawn, from the body's placement.
[[nodiscard]] cy::Transform head_placement(const cy::Transform& body) noexcept;

/// The player, as the targets perceive it.
struct PlayerSighting {
    cy::ecs::Entity entity;
    /// The eyes, and the velocity of the body.
    cy::Vec3 eyes{0.0F, 0.0F, 0.0F};
    cy::Vec3 velocity{0.0F, 0.0F, 0.0F};
    cy::physics::BodyHandle body;
};

class Targets {
public:
    Targets(cy::Allocator& allocator, cy::physics::PhysicsServer& server,
            cy::physics::WorldHandle world, ArenaBodies& bodies) noexcept;
    ~Targets();

    Targets(const Targets&) = delete;
    Targets& operator=(const Targets&) = delete;

    /// Compile the tree and make the agents, one a target, owned by `entities`.
    [[nodiscard]] cy::Status build(const cy::ecs::Entity (&entities)[kTargetCount]) noexcept;

    /// One fixed tick: what the game wrote, what each target sees, what the tree decides, and the
    /// move it makes — its body placed for the physics step that follows.
    [[nodiscard]] cy::Status tick(cy::u32 tick, cy::f32 dt, const PlayerSighting& player,
                                  const TargetHealth (&health)[kTargetCount]) noexcept;

    [[nodiscard]] const TargetState& state(cy::u32 index) const noexcept { return states_[index]; }
    /// The tree's digest, and what perception and thinking did over the run.
    [[nodiscard]] cy::u64 program_digest() const noexcept;
    [[nodiscard]] cy::u32 sightings() const noexcept { return sightings_; }
    [[nodiscard]] cy::u32 traces() const noexcept { return traces_; }
    [[nodiscard]] cy::u64 think_hash() const noexcept { return think_hash_; }

private:
    class Sight;
    class Brain;

    [[nodiscard]] cy::Status compile() noexcept;
    void feel(cy::u32 index, cy::u32 tick, const TargetHealth& health,
              cy::ecs::Entity player) noexcept;
    void move(cy::u32 index, cy::u32 tick, cy::f32 dt) noexcept;
    void rise(cy::u32 index, cy::f32 dt) noexcept;

    cy::Allocator* allocator_;
    cy::physics::PhysicsServer* server_;
    cy::physics::WorldHandle world_;
    ArenaBodies* bodies_;

    cy::graph::NodeRegistry registry_;
    cy::graph::DiagnosticSink sink_;
    cy::graph::Graph graph_;
    std::unique_ptr<cy::graph::behaviour::BehaviourProgram> program_;
    std::unique_ptr<cy::ai::AiRuntime> runtime_;
    std::unique_ptr<cy::ai::PerceptionScheduler> perception_;
    std::unique_ptr<Brain> brain_;

    cy::ecs::Entity entities_[kTargetCount] = {};
    cy::ai::AIAgent agents_[kTargetCount] = {};
    cy::ai::AIState ai_states_[kTargetCount] = {};
    cy::ai::Blackboard blackboards_[kTargetCount] = {};
    cy::ai::PerceptionSensors sensors_[kTargetCount] = {};
    std::vector<cy::ai::KnowledgeStore> knowledge_;
    TargetState states_[kTargetCount] = {};
    cy::u32 sightings_ = 0;
    cy::u32 traces_ = 0;
    cy::u64 think_hash_ = 0;
};

}  // namespace sample::fps

#endif  // CY_SAMPLE_FIRST_PERSON_TARGETS_H
