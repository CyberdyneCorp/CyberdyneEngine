#pragma once
// Agents as ECS entities: the components `ai-system` names, the level-of-detail tiers that decide
// how much thinking each one gets, and the registration that gives them ids in one world.
// M8.b task 6.3.
//
// ================================================================================================
// THERE IS NO PER-AGENT UPDATE OBJECT, AND THE SHAPE OF THIS FILE IS WHY
// ================================================================================================
//
// `ai-system`: "Agent behaviour SHALL execute as scheduled systems over queries, in bulk. The
// engine SHALL NOT provide a per-agent virtual update object", and "Agents sharing a graph SHALL
// share one compiled program and differ only in per-entity data."
//
// So an agent is four plain aggregates in archetype chunks, and the thing that runs is
// `cy::ai::AiRuntime::think()` — one loop over packed columns against one immutable
// `cy::graph::behaviour::BehaviourProgram`. Nothing here has a virtual function, nothing here owns
// an allocation, and nothing here knows about a graph.
//
// ================================================================================================
// WHERE THE EXECUTION STACK LIVES, AND WHY IT IS NOT IN THE CHUNK
// ================================================================================================
//
// `ai-system` asks for "`AIState` (program counter, stack, timers)" and "`Blackboard`". Both are
// here. What is NOT here is `cy::graph::behaviour::AgentState`, which is the type the compiled
// program actually executes against — because it owns a growable blackboard array, and an ECS chunk
// holding one allocation per row is the shape `ecs-core` exists to avoid.
//
// The resolution is that `AIState::slot` indexes a PACKED SIDE TABLE the runtime owns, and the
// fields beside it — the resumed instruction, the last status, the tick it last thought on — are
// the same values MIRRORED into the chunk so that a query, a debugger and a save can read them
// without reaching into the runtime. The mirror is written by `think()` and read by everyone else;
// nothing writes it twice.
//
// The blackboard is the other way round: it IS in the chunk, as a fixed block of slots, and the
// runtime copies it into the execution state around a think. `ai-system` requires blackboard keys
// to be "resolved to indices at compile time" with "no string lookup at runtime", which the
// compiler already does — `AiInstruction::blackboard` is an index — so the chunk-side form is an
// array and the copy is sixty-four bytes each way for an agent that actually thought.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/values/name.h>
#include <cy/ecs/world.h>
#include <cy/graph/lower_behaviour.h>

namespace cy::ai {

using ecs::ComponentTypeId;
using ecs::Entity;
using ecs::kInvalidComponent;
using ecs::World;
using graph::behaviour::BtStatus;

/// `ai-system`'s AI LOD table, by name: "`Full`, `Reduced`, `Minimal`, `Statistical`".
///
/// | Tier | Think rate | Perception | Navigation | Reasoning |
/// |---|---|---|---|---|
/// | `Full` | up to every tick | all sensors, full queries | individual pathfinding | full graph |
/// | `Reduced` | 5–10 Hz | cheap sensors, cached visibility | flow field | costly nodes skipped |
/// | `Minimal` | 0.2–1 Hz | none; knowledge from shared channels | macro movement | coarse state |
/// | `Statistical` | aggregate | none | group-level | population model, not per agent |
enum class AiTier : u8 { Full = 0, Reduced, Minimal, Statistical, Count };

[[nodiscard]] const char* ai_tier_name(AiTier tier) noexcept;

/// How many ticks may pass between two thinks at a tier. `ai-system`: "the rotation SHALL guarantee
/// that every agent thinks within a bounded number of ticks for its tier", so this is the guarantee
/// and not a target — the scheduler is checked against it.
[[nodiscard]] u32 tier_think_interval(AiTier tier) noexcept;

/// The blackboard slots one agent carries. A fixed block, in the chunk; see the header.
inline constexpr u32 kBlackboardSlots = 16;

/// "This agent has no execution state." IT IS ZERO, AND THAT IS THE WHOLE POINT: an ECS chunk is
/// zero-initialised when an entity is created, so a sentinel of `0xFFFFFFFF` would make a
/// freshly-created agent look like the holder of slot zero — and it would then execute another
/// agent's program counter, stack and timers, silently and only for whichever agent was unlucky.
/// Slot handles are therefore ONE-BASED: `AiRuntime::reserve_slots()` hands out `1..n` and
/// `AiRuntime::state()` subtracts.
inline constexpr u32 kInvalidSlot = 0;

/// Which graph an agent runs, how much it matters, and what tier it is at.
struct AIAgent {
    /// The behaviour graph. Resolved to a `BehaviourProgram` by the runtime, once per graph.
    Name graph;
    /// `ai-system`: "Tier SHALL be derived from agent importance, distance to the nearest observer,
    /// visibility, and gameplay-critical flags." This is the first of those.
    f32 importance = 1.0F;
    /// The seed of this agent's own random stream. Part of simulation state, never a clock.
    u64 seed = 0;
    /// What the policy decided last. Written by `AiRuntime::update_tiers`.
    AiTier tier = AiTier::Full;
    /// `ai-system`: "Agents SHALL be pinnable to a minimum tier." A quest-critical agent pinned to
    /// `Full` is never demoted, however far away or however hard the budget is pressing.
    AiTier pinned = AiTier::Statistical;
    /// A gameplay-critical flag, which the tier policy reads directly.
    bool critical = false;
};

/// One agent's execution state, as the chunk sees it. The authoritative copy is the runtime's; see
/// the header for why, and for which of these fields are mirrors.
struct AIState {
    /// A one-based handle into the runtime's packed state table; zero until the runtime has given
    /// this agent one. See `kInvalidSlot` for why zero rather than a high sentinel.
    u32 slot = kInvalidSlot;
    /// The tick this agent last thought on. The whole of "no starvation" is a comparison against
    /// it, so it is simulation state and never a timestamp.
    u32 last_think_tick = 0;
    /// The instruction execution resumed at, mirrored from the runtime.
    u16 running = 0xFFFFU;
    /// How deep the execution stack was, mirrored.
    u8 stack_depth = 0;
    BtStatus last_status = BtStatus::Running;
};

/// `ai-system`: "typed named values readable and writable by graph nodes, with declared keys,
/// types, and default values", resolved to indices at compile time.
struct Blackboard {
    f32 value[kBlackboardSlots] = {};
};

/// What an agent can sense. Declared here and read by the perception scheduler; see perception.h.
struct PerceptionSensors {
    /// Vision.
    f32 sight_range = 20.0F;
    f32 field_of_view_degrees = 110.0F;
    /// `ai-system`'s "acuity falloff": the fraction of `sight_range` beyond which confidence in a
    /// sighting starts to drop.
    f32 acuity_falloff = 0.6F;
    /// Hearing.
    f32 hearing_range = 15.0F;
    /// Proximity and touch, which need no line of sight.
    f32 proximity_range = 2.0F;
    /// Which factions this agent reports. A broad-phase filter, applied before any query is issued.
    u64 factions_of_interest = ~u64{0};
    u64 own_faction = 1;
    /// The tick this agent's sensors were last updated on, so the scheduler's deferral is a
    /// comparison rather than a queue.
    u32 last_sense_tick = 0;
    bool vision = true;
    bool hearing = true;
    bool proximity = true;
};

inline constexpr const char* kAIAgentComponentName = "cy.ai.AIAgent";
inline constexpr const char* kAIStateComponentName = "cy.ai.AIState";
inline constexpr const char* kBlackboardComponentName = "cy.ai.Blackboard";
inline constexpr const char* kPerceptionSensorsComponentName = "cy.ai.PerceptionSensors";

/// AI's component ids in ONE world. Ids are per world (`ecs-core`), so this is a value the runtime
/// holds and never a static.
///
/// THE ORDER IS FIXED, AND IT IS THE SERIALIZED DESCRIPTOR TABLE'S ORDER — the same rule
/// `cy::physics::PhysicsComponents` and `cy::navigation::NavComponents` state, for the same reason.
struct AiComponents {
    ComponentTypeId agent = kInvalidComponent;
    ComponentTypeId state = kInvalidComponent;
    ComponentTypeId blackboard = kInvalidComponent;
    ComponentTypeId sensors = kInvalidComponent;

    [[nodiscard]] static Expected<AiComponents, Error> register_all(World& world) noexcept;

    [[nodiscard]] bool registered() const noexcept {
        return agent != kInvalidComponent && state != kInvalidComponent &&
               blackboard != kInvalidComponent && sensors != kInvalidComponent;
    }
};

}  // namespace cy::ai
