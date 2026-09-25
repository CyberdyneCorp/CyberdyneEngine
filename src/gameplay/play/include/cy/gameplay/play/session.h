#pragma once
// Play mode: the authored world, simulated, and put back exactly. M8.a tasks 5.1 and 5.2.
//
// ================================================================================================
// WHAT WAS MISSING, AND WHAT THIS IS
// ================================================================================================
//
// design.md §4: *"Pressing play must simulate the world the editor authored. Today it reports
// `hosting: NoRuntime`. The editor already survives its runtime being killed and already speaks to
// it over the ABI, so what is missing is the world crossing that boundary."*
//
// The world crossed that boundary in section 1: `cy::scene::serialization::World` IS the editor's
// document, read and written by the engine, with the editor's own identities on its nodes. What was
// still missing is the thing that simulates it. This is that thing:
//
//     enter()  the authored world becomes an ECS world — one entity per live node, its placement,
//              and the physics components the file declares — and `cy::physics::PhysicsBridge`
//              creates the bodies
//     tick()   one fixed step in the `Physics` stage, and the simulated placements are written back
//              into the authored world so that whatever draws it draws the simulation
//     stop()   every placement is put back the way it was, and the world is compared BYTE FOR BYTE
//              against what it was before play
//
// ================================================================================================
// TASK 5.2 IS THE HARD ONE AND THIS IS HOW IT IS KEPT RATHER THAN CLAIMED
// ================================================================================================
//
// *"Stop restores EXACTLY — no residue in the document, the scene or the persistence overlay. A
// play session that leaves any makes undo a lie."*
//
// Three mechanisms, in increasing order of how much they would cost to fool:
//
//   1. **Play writes ONE thing.** The only write into the authored world during a session is a
//      node's transform, through `set_transform`. There is no other mutating call in session.cpp
//      and the world is otherwise reached through `const` references.
//   2. **The pre-play placement of every node is kept**, and `stop()` writes each one back. Not
//      "reset to identity", not "recompute" — the value that was there, restored into the same
//      field of the same component.
//   3. **The whole file is snapshotted at `enter()` and rewritten at `stop()`, and the two byte
//      strings are compared.** `PlayReport::restored_exactly` is that comparison, and
//      `restored_difference` is the first offset at which they differ. A future change that starts
//      writing something else during play — a body handle, a velocity, a spawned node — turns this
//      false on the first run rather than being discovered as an undo that lies.
//
// When the comparison fails, the session does not shrug: it re-reads the snapshot over the world,
// which is the total restore, and reports that it had to. A caller that sees `restored_exactly ==
// false` has found a defect and has a world that is nonetheless correct.
//
// ================================================================================================
// WHY THE PHYSICS COMPONENTS ARE READ BY NAME OUT OF THE FILE'S OWN TYPE SECTION
// ================================================================================================
//
// `resolve_against` matches a `.cyworld`'s declared type names against the engine's
// `AuthoringSchema`, which is built from `reflect::TypeRegistry` — and physics' eight components
// are registered with `register_builtin`, by name, with no reflected type behind them
// (src/servers/physics/include/cy/servers/physics/components.h says why the layouts are where they
// are; nothing has assigned them manifest identifiers). So a `RigidBody` in a world file resolves
// to no `reflect::TypeId`, exactly as `MeshRenderer` does for `cy_editor_services::primitives`.
//
// It is read by name here instead, out of the file's own `type` section, which is the section
// `serialization-and-prefabs` requires precisely so that data whose type this build does not know
// survives a round trip. The names are the ones the editor's `scene.add-body` writes, and the two
// are pinned to each other by a golden world in each language's tests — the same arrangement
// `.cyprim` uses across the same boundary.
//
// **The day physics' components are reflected, `authored_body_of` becomes a lookup through
// `engine_type` and nothing else changes.** That is the one line of follow-up this design owes.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/clock.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>
#include <cy/gameplay/play/mode.h>
#include <cy/gameplay/play/spawn.h>
#include <cy/physics/bridge.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/scene/tree.h>

#include <string_view>

namespace cy::gameplay {

/// Where a session is. The same three `cy_editor_viewport::play::PlayState` has, because a fourth
/// here would be one the editor could not show.
enum class PlayState : u8 {
    /// Authoring. Nothing is simulating and the authored world is the only world.
    Editing = 0,
    Playing,
    /// Simulating, but stopped where it is. What is in the world is still the simulation's.
    Paused,
};

/// The spelling the protocol carries and the editor shows. Never null.
[[nodiscard]] const char* play_state_name(PlayState state) noexcept;
/// The state a word names, or nothing when this build has no such state.
[[nodiscard]] Expected<PlayState, Error> play_state_of(std::string_view name) noexcept;

/// What a session needs that it does not own.
class PlaySession;

struct PlayConfiguration {
    /// Where this session's runtime runs. M11.b task 3.1.
    ///
    /// The mode is a property of the SESSION rather than of the caller, because `enter` is where a
    /// mode that is not available has to be refused: a request that reached `build()` and then
    /// discovered it could not have a transport would already have spawned a world. See
    /// `cy/gameplay/play/mode.h` for why availability is a query about the build rather than a
    /// constant, and `enter`'s own comment for the refusal.
    PlayMode mode = PlayMode::InEditor;
    /// What this build can do, which is what decides whether `mode` is available. Supplied rather
    /// than read inside `enter` so that a test can ask for the OTHER answer and watch the refusal
    /// go the other way — a refusal that cannot be made to stop refusing is untested.
    PlayModeSupport support = play_mode_support();
    /// The solver. Required, and NOT created here: which backend a project simulates with is the
    /// host's decision, exactly as it is for `cy::physics::PhysicsBridge`.
    physics::PhysicsServer* physics = nullptr;
    /// The rate the simulation clock runs at.
    determinism::TickRate rate;
    Vec3 gravity{0.0F, -9.81F, 0.0F};
    /// Frames per second, as an exact rational, for `step_frame`. Separate from `rate` because a
    /// frame and a tick are different units and a session may be asked to step either.
    determinism::TickRate frame_rate{60, 1};
    /// How many bodies the physics world is sized for. A world with more nodes than this still
    /// enters play; the bodies beyond it are refused by the server, counted, and named.
    u32 body_capacity = 1024;
    /// The engine schema, so that a TOTAL restore — the fallback when the byte comparison fails —
    /// leaves the world resolved. Optional: a session over a world nobody resolved does not need
    /// one, and a total restore is not expected to happen at all.
    const scene::serialization::AuthoringSchema* schema = nullptr;
    /// Optional project gameplay step, after physics and before scene propagation.
    Status (*gameplay_tick)(void* user, PlaySession& session, f32 dt) noexcept = nullptr;
    void* gameplay_user = nullptr;
};

/// What one session did.
struct PlayReport {
    /// The mode this session entered in. Reported rather than remembered by the caller, because a
    /// report read out of a log is the only record of which mode produced it.
    PlayMode mode = PlayMode::InEditor;
    /// Ticks run by an explicit step rather than by the host's loop. A session driven only by
    /// `step_tick`/`step_frame` has `ticks == stepped_ticks`; the two differing is how a reader
    /// tells a stepped session from a running one.
    u64 stepped_ticks = 0;
    /// Frames advanced by `step_frame`.
    u64 stepped_frames = 0;
    /// Entities spawned from the authored world's live nodes.
    u32 entities = 0;
    /// Nodes that carried a body component the bridge could act on.
    u32 bodies = 0;
    u32 colliders = 0;
    /// Nodes whose declared body component this build does not know. Counted rather than dropped.
    u32 unknown_components = 0;
    u64 ticks = 0;
    /// Placements written back into the authored world over the session.
    u64 placements_published = 0;
    /// Placements restored at `stop()`.
    u32 restored = 0;
    /// Whether the world's bytes at `stop()` were IDENTICAL to its bytes at `enter()`. See the
    /// header: this is task 5.2 measured rather than asserted.
    bool restored_exactly = false;
    /// The first byte at which they differed, when they did. `restored_length_before` and
    /// `restored_length_after` are the two lengths, so a report names what changed and by how much.
    u64 restored_difference = 0;
    u64 restored_length_before = 0;
    u64 restored_length_after = 0;
    /// Set when the byte comparison failed and the whole snapshot had to be re-read over the world.
    bool total_restore = false;
};

/// One play session over one authored world.
///
/// Not thread-safe: it is driven from the runtime's own loop, on the simulation thread.
class PlaySession {
public:
    PlaySession(Allocator& allocator, scene::serialization::World& authored) noexcept;
    ~PlaySession();

    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;
    PlaySession(PlaySession&&) = delete;
    PlaySession& operator=(PlaySession&&) = delete;

    /// Build the simulation from the authored world and start it.
    ///
    /// Refused when a session is already running: entering play twice is a caller that has lost
    /// track, and `cy_editor_services::authoring`'s `play.enter` says the same thing at its own
    /// end. Leaves nothing behind on failure — a session that failed halfway through building
    /// would be residue of exactly the kind task 5.2 is about, so `enter` tears itself down.
    [[nodiscard]] Status enter(const PlayConfiguration& configuration) noexcept;

    /// One fixed step. A no-op that succeeds when the session is paused or not playing, because a
    /// host's loop calls this every tick and should not have to ask first.
    [[nodiscard]] Status tick() noexcept;

    [[nodiscard]] Status pause() noexcept;
    [[nodiscard]] Status resume() noexcept;

    /// Advance exactly one simulation tick, while paused. M11.b task 3.1.
    ///
    /// `live-editing` requires *"pause, single frame step, and single simulation tick step in every
    /// mode where the runtime permits"*, and M11.b's delta makes "where the runtime permits"
    /// answerable by a query: this refuses, naming the mode, when
    /// `capabilities_of(mode).step_tick` is false, rather than stepping anyway or silently doing
    /// nothing.
    ///
    /// Refused when the session is not paused. Stepping a running simulation would race the host's
    /// own loop, and a step that quietly became "one extra tick this frame" is not a step.
    [[nodiscard]] Status step_tick() noexcept;

    /// Advance exactly one frame's worth of simulation, while paused.
    ///
    /// A frame is `PlayConfiguration::frame_rate` of a second, and the ticks it contains are
    /// computed from the two rational rates rather than assumed to be one: at a 60 Hz tick rate and
    /// a 30 Hz frame rate a frame is two ticks, and a step that ran one would be a frame step in
    /// name only. At least one tick always runs, so a frame faster than the tick rate steps the
    /// simulation rather than standing still.
    [[nodiscard]] Status step_frame() noexcept;

    /// How many ticks one frame contains at this session's configured rates. Zero outside a
    /// session. Public because the editor shows it beside the step buttons.
    [[nodiscard]] u32 ticks_per_frame() const noexcept;

    /// The mode this session is in, and what it can be asked to do. Queried rather than assumed:
    /// see `step_tick`.
    [[nodiscard]] PlayMode mode() const noexcept { return mode_; }
    [[nodiscard]] PlayModeCapabilities capabilities() const noexcept;

    /// Leave play, restoring the authored world exactly. Idempotent.
    [[nodiscard]] Status stop() noexcept;

    [[nodiscard]] PlayState state() const noexcept { return state_; }
    [[nodiscard]] const PlayReport& report() const noexcept { return report_; }
    /// The simulation's own world, for an inspector and for a test. Null outside a session.
    [[nodiscard]] ecs::World* world() noexcept { return world_.get(); }
    [[nodiscard]] scene::SceneTree* tree() noexcept { return tree_.get(); }
    [[nodiscard]] physics::PhysicsBridge* bridge() noexcept { return bridge_.get(); }
    [[nodiscard]] const determinism::SimulationClock& clock() const noexcept { return clock_; }
    /// The entity an authored node's identity is simulating as, or a null entity.
    [[nodiscard]] ecs::Entity entity_for(u64 identity) const noexcept;

    /// The configuration this session entered with, so a restart can re-enter with the same one.
    /// Meaningless outside a session.
    [[nodiscard]] const PlayConfiguration& configuration() const noexcept { return configuration_; }

    // --- What a live edit needs, and nothing more ------------------------------------------------
    //
    // M11.b task 3.2. `cy::gameplay::live::LiveEditCompiler` translates an authoring change into a
    // runtime delta and then has to APPLY it, and two of the six policies — `ReinitializeComponent`
    // and `RecreateEntity` — are operations only the code that built the simulation can perform.
    // They are exposed here rather than reimplemented there, because a second implementation of
    // "build this node's physics from the authored world" would be a second place for the two to
    // disagree.

    /// Rebuild the physics components of one simulated node from the authored world.
    ///
    /// What `ReinitializeComponent` costs for the components a play session creates: the collider
    /// and body components are removed and re-added from what the `.cyworld` says NOW, and the
    /// bridge recreates the solver body on its next sync. The entity, its identity and its place in
    /// the scene tree are untouched.
    ///
    /// Refused when no live node carries `identity`, rather than succeeding over nothing.
    [[nodiscard]] Status reinitialize_physics(u64 identity) noexcept;

    /// Destroy and respawn the entity simulating one authored node, keeping its identity.
    ///
    /// What `RecreateEntity` means: *"the entity is recreated, preserving identity"*. The authored
    /// node's identity is the identity; the `ecs::Entity` behind it is a new one, which is how a
    /// caller can tell a recreate from a reinitialise without being told.
    [[nodiscard]] Status recreate_entity(u64 identity) noexcept;

    /// The world `stop()` will restore, as text.
    ///
    /// `stop()` puts the authored world back to the bytes it had at `enter()`. An authoring edit
    /// made DURING play is an edit to the document, so restoring the pre-play bytes over it would
    /// discard a designer's work — which is why the restore target is readable and replaceable
    /// rather than private. `cy::gameplay::live` applies an authoring change to this text as well
    /// as to the running world, and `restored_exactly` keeps its exact meaning: the bytes at
    /// `stop()` equal the bytes the authoring state implies.
    [[nodiscard]] std::string_view restore_target() const noexcept;
    [[nodiscard]] Status set_restore_target(std::string_view text) noexcept;

private:
    /// One authored node, its entity, and the placement it had before play.
    struct Simulated {
        u64 identity = 0;
        u32 node = 0;
        ecs::Entity entity;
        Transform before;
    };

    [[nodiscard]] Status build(const PlayConfiguration& configuration) noexcept;
    [[nodiscard]] Status spawn_authored(const scene::serialization::WorldNode& node,
                                        u32 index) noexcept;
    [[nodiscard]] Status attach_physics(const scene::serialization::WorldNode& node,
                                        ecs::Entity entity) noexcept;
    [[nodiscard]] Status publish_placements() noexcept;
    /// The `simulated_` entry for an authored identity, or null.
    [[nodiscard]] Simulated* simulated_for(u64 identity) noexcept;
    /// Remove the physics components this session added to an entity, if any.
    [[nodiscard]] Status detach_physics(ecs::Entity entity) noexcept;
    /// One fixed step, whatever the state. `tick()` and the two step entry points share it so that
    /// a stepped tick and a looped one cannot drift apart.
    [[nodiscard]] Status advance_one() noexcept;
    void release() noexcept;

    scene::serialization::World* authored_;
    Allocator* allocator_;

    UniquePtr<ecs::World> world_;
    UniquePtr<scene::SceneTree> tree_;
    UniquePtr<SpawnService> spawns_;
    UniquePtr<physics::PhysicsBridge> bridge_;
    UniquePtr<ecs::Schedule> schedule_;
    physics::PhysicsComponents components_;
    physics::PhysicsServer* server_ = nullptr;
    PlayConfiguration configuration_;
    physics::WorldHandle physics_world_;
    determinism::SimulationClock clock_;
    const scene::serialization::AuthoringSchema* schema_ = nullptr;

    Array<Simulated> simulated_;
    /// The whole file as it was at `enter()`. See the header, mechanism 3.
    Array<char> snapshot_;
    Array<char> rewritten_;

    PlayState state_ = PlayState::Editing;
    PlayMode mode_ = PlayMode::InEditor;
    determinism::TickRate frame_rate_{60, 1};
    PlayReport report_;
};

}  // namespace cy::gameplay
