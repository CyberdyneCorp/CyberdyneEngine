// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_RTS_HOST_H
#define CY_SAMPLE_RTS_API_RTS_HOST_H
// rts_host.h — the engine side of samples/13-rts-api. It brings up the servers, binds the six ABI
// 1.3 adapters, loads the level content and the Swift module, and runs frames. It decides nothing.
//
// ================================================================================================
// WHAT CHANGED SINCE samples/04-character, AND WHY THIS HOST IS SMALLER
// ================================================================================================
//
// 04-character's host carried the game's intent across eight components every tick, because at ABI
// 1.0 the Swift module could not reach input, physics, the camera or audio. This host carries
// nothing. It binds `InputAdapter`, `CameraAdapter`, `PhysicsQueryAdapter`, `NavigationAdapter`,
// `AudioAdapter` and `SpawnAdapter` onto the ABI host once, and from then on the game calls the
// engine itself: `Input.pointer()`, `Camera.ray(under:)`, `Physics.raycast`, `NavAgent.move(to:)`,
// `Audio.play`, `Prefab.instantiate`.
//
// ================================================================================================
// ONE FRAME, IN ORDER
// ================================================================================================
//
//   scene tree pump                      (Swift tree callbacks: onEnterTree, onReady — phase N)
//   for the frame's one fixed tick:
//     clock.tick = tick
//     input adapter observes the pending events;  input server resolves the tick
//     behaviours' onFixedUpdate          (Swift, phase F)
//     the fixed stages of the schedule   (Swift `trainUnits` and the native roll, phase F)
//     navigation adapter update          (orders, paths, crowd; moves the scene nodes)
//     unit bodies follow their nodes;    physics step
//   input adapter begins the frame;      camera rig evaluates
//   interface: layout, the pointer routed, its events delivered
//                                        (Swift `onUIEvent` and button actions, phase U)
//   behaviours' onUpdate                 (Swift, phase U; the HUD is written here)
//   the frame stages of the schedule     (phase U; nothing in them yet)
//   interface layout                     (what a renderer would flatten)
//   audio adapter and audio server update; the null device mixes the frame
//
// ================================================================================================
// WHAT ABI 1.5 ADDED TO THIS HOST, AND WHY IT IS STILL NOT GAMEPLAY
// ================================================================================================
//
// Three more bindings — `PhysicsBodyAdapter`, `CharacterAdapter` and the `ScriptSceneBridge` — an
// `ecs::Schedule` with `cy::abi::ScriptSystems` in it, and a level of named nodes: `/Level` with
// `Barracks`, `Crate` (a dynamic body), `Commander` and `Scout`. The two script behaviours are
// attached to their nodes through the bridge, so the tree's pump drives their tree callbacks and
// their `@Node` paths resolve against the level. What the scout's hero does and how hard the crate
// is kicked are Swift's; the host builds the crate and reads where it went.
//
// The rig is evaluated BEFORE `onUpdate`, so what Swift reads from the camera is the view this
// frame shows. A `Camera.setTarget` from `onUpdate` shows on the next frame.
//
// ================================================================================================
// WHAT ABI 1.6 ADDED: AN INTERFACE THE GAME BUILDS
// ================================================================================================
//
// A CyberUI store with a screen root, a text painter on the built-in font, and
// `cy::game_backend::UiAdapter` bound on the ABI host. The commander mounts its HUD in it — the
// resource bar, the selection panel and the minimap of samples/13-rts-selection, plus a Build
// button — and writes it from the game every frame. The host lays the store out, routes the pointer
// through it, sets the input adapter's OVER_UI from what the pointer is over, and delivers the
// clicks to the behaviours that own the elements. The host is headless, so nothing is drawn here;
// `render.rts_api_hud` draws the same HUD on a device.
//
// ================================================================================================
// THE HAND
// ================================================================================================
//
// `press_key`, `move_pointer` and `press_button` inject synthetic events into the input server,
// the same door a recorded replay uses. `project` answers where a world point is on screen through
// the adapter the game reads, which is what the scripted player (script.h) uses to aim a click the
// way a person aims with their eyes. They are for a driver and a test; the game never sees them.

#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/abi/systems.h>
#include <cy/animation/animation_system.h>
#include <cy/core/base/expected.h>
#include <cy/core/determinism/clock.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/animation_backend.h>
#include <cy/game_backend/audio_backend.h>
#include <cy/game_backend/camera_backend.h>
#include <cy/game_backend/character_backend.h>
#include <cy/game_backend/input_backend.h>
#include <cy/game_backend/navigation_backend.h>
#include <cy/game_backend/physics_backend.h>
#include <cy/game_backend/scene_bridge.h>
#include <cy/game_backend/spawn_backend.h>
#include <cy/game_backend/ui_backend.h>
#include <cy/navigation/components.h>
#include <cy/navigation/navmesh.h>
#include <cy/navigation/query.h>
#include <cy/scene/tree.h>
#include <cy/servers/audio/backend.h>
#include <cy/servers/audio/server.h>
#include <cy/servers/camera/server.h>
#include <cy/servers/input/server.h>
#include <cy/servers/physics/server.h>
#include <cy/servers/text/server.h>
#include <cy/ui/store.h>
#include <cy/ui/text/text_painter.h>

#include "units.h"
#include "worker_rig.h"

#include <memory>

namespace sample::rts {

/// How the host runs. Engine choices only.
struct HostOptions {
    const char* module_library = "";
    const char* module_manifest = "";
    /// THE NEGATIVE CONTROL. False loads the module but attaches no `Commander` and no `Scout`, so
    /// every C++ path runs with no game behind it: no units, no camera moves, no sound, no hero.
    bool behaviours = true;
    /// The scheduler's control. False registers the module's systems with the engine but never
    /// installs them in the schedule, so `trainUnits` runs only if the SCHEDULER runs it.
    bool systems = true;
    /// The interface's control (ABI 1.6). False brings up no interface: the game finds none, mounts
    /// no HUD, and every click is the world's.
    bool ui = true;
};

/// What the game and the engine say happened. Engine facts are read from the servers and the
/// adapters; `game` is the `RtsReport` component Swift writes, read and never acted on.
struct Observation {
    struct Game {
        bool found = false;
        CyEntity selected = CY_ENTITY_NULL;
        cy::f32 orders = 0.0F;
        cy::f32 arrivals = 0.0F;
        cy::f32 cues = 0.0F;
        cy::f32 spawns = 0.0F;
    };
    Game game;

    /// ABI 1.5. The commander's tree callbacks, from `RtsReport`.
    struct Tree {
        cy::f32 entered = 0.0F;
        cy::f32 readied = 0.0F;
        cy::f32 barracks_found = 0.0F;
    };
    Tree tree;
    /// The scout's `ScoutReport`, and where the crate went, from the physics server.
    struct Scout {
        bool found = false;
        cy::f32 crate_found = 0.0F;
        cy::f32 hero_x = 0.0F;
        cy::f32 hero_ground = -1.0F;
        cy::f32 airborne = 0.0F;
        cy::f32 kicks = 0.0F;
        cy::f32 crate_speed = 0.0F;
        cy::f32 crate_moved = 0.0F;
    };
    Scout scout;
    /// The schedule: script systems installed, `trainUnits`' runs, what the native roll read, and
    /// whether the scheduler ordered the roll against `trainUnits` by their declarations.
    struct Systems {
        cy::u32 installed = 0;
        cy::u64 runs = 0;
        cy::u32 rows = 0;
        cy::f32 most = 0.0F;
        bool ordered = false;
    };
    Systems systems;
    /// ABI 1.6. The HUD as the store holds it — the texts and the layout the game's writes produced
    /// — and what the commander reported about it.
    struct Hud {
        cy::u32 elements = 0;  ///< module elements in the store
        cy::u32 clicks = 0;    ///< clicks the adapter routed
        cy::f32 mounted = 0.0F;
        cy::f32 builds = 0.0F;  ///< workers the Build button built
        cy::f32 heard = 0.0F;   ///< clicks the commander's `onUIEvent` saw
        char gold[16] = {};
        char wood[16] = {};
        char food[16] = {};
        char title[48] = {};
        char health[16] = {};  ///< the first selection row's
        cy::u32 rows = 0;      ///< selection rows shown
        cy::u32 dots = 0;      ///< minimap dots shown
        cy::f32 fill = 0.0F;   ///< the first health bar's fill, in pixels
        bool button = false;
    };
    Hud hud;
    /// Issue #76 stage 4: the workers' animators, as the engine ran them, and what the game heard.
    struct Animation {
        /// Units carrying an animator: what the game attached.
        cy::u32 animated = 0;
        /// Units seen in, or blending into, `walk` while their agent followed a path.
        cy::u32 walked = 0;
        /// Units seen in `cheer`, the one-shot the game plays on arrival.
        cy::u32 cheered = 0;
        /// Units back in `idle` after their cheer, which the game does on the cheer's event.
        cy::u32 idle_after = 0;
        /// Animated units that never left `idle`.
        cy::u32 only_idle = 0;
        /// The largest change, over the run, of any unit's skinning matrices from the pose it was
        /// first published with. Zero for a unit that stayed in its reference pose.
        cy::f32 departure = 0.0F;
        /// Events the game read from `Animation.events`: footfalls, and cheers finished.
        cy::f32 footsteps = 0.0F;
        cy::f32 cheer_events = 0.0F;
    };
    Animation animation;
    /// Navigation agents with a body, which is every unit the game configured.
    cy::u32 units = 0;
    /// Agents the navigation adapter drives.
    cy::u32 agents = 0;
    /// Nodes named "Worker" alive in the scene: what the spawn adapter instantiated.
    cy::u32 workers = 0;
    /// The audio server's active voices after the last update.
    cy::u32 active_voices = 0;
};

class RtsHost {
public:
    RtsHost(cy::Allocator& allocator, const HostOptions& options) noexcept;
    ~RtsHost();

    RtsHost(const RtsHost&) = delete;
    RtsHost& operator=(const RtsHost&) = delete;
    RtsHost(RtsHost&&) = delete;
    RtsHost& operator=(RtsHost&&) = delete;

    /// Bring everything up. `detail` names what failed where the failure has a name.
    [[nodiscard]] cy::Status start(const char** detail) noexcept;
    /// One frame. See the header.
    [[nodiscard]] cy::Status frame() noexcept;
    /// Tear down in the one safe order. Idempotent; the destructor calls it.
    void shutdown() noexcept;

    // --- The hand ------------------------------------------------------------------------------
    [[nodiscard]] cy::Status press_key(cy::input::Key key, bool down) noexcept;
    [[nodiscard]] cy::Status move_pointer(cy::f32 x, cy::f32 y) noexcept;
    [[nodiscard]] cy::Status press_button(cy::input::MouseControl button, bool down) noexcept;
    /// Where `point` lands on screen through the primary camera, in window pixels.
    [[nodiscard]] bool project(cy::Vec3 point, cy::f32& out_x, cy::f32& out_y) noexcept;
    /// The centre of the first interface element named `name`, as the last layout placed it.
    [[nodiscard]] bool ui_centre(const char* name, cy::f32& out_x, cy::f32& out_y) const noexcept;

    // --- The eyes ------------------------------------------------------------------------------
    [[nodiscard]] cy::u32 unit_count() const noexcept { return bodies_->count(); }
    [[nodiscard]] CyEntity unit(cy::u32 index) const noexcept { return bodies_->entity(index); }
    /// The unit's feet, from its scene node.
    [[nodiscard]] cy::Vec3 unit_position(CyEntity unit) const noexcept;
    /// The unit's navigation status, as the adapter reports it.
    [[nodiscard]] CyNavPathStatus unit_status(CyEntity unit) const noexcept;
    /// The camera's position as last evaluated.
    [[nodiscard]] cy::Vec3 camera_position() const noexcept;
    [[nodiscard]] Observation observe() noexcept;
    [[nodiscard]] cy::u64 frames() const noexcept { return frame_; }
    [[nodiscard]] cy::u32 behaviours() const noexcept { return runtime_.live_instances(); }

private:
    [[nodiscard]] cy::Status start_input() noexcept;
    [[nodiscard]] cy::Status start_camera() noexcept;
    [[nodiscard]] cy::Status start_physics() noexcept;
    [[nodiscard]] cy::Status start_navigation() noexcept;
    [[nodiscard]] cy::Status start_audio() noexcept;
    [[nodiscard]] cy::Status start_scene() noexcept;
    [[nodiscard]] cy::Status start_props() noexcept;
    [[nodiscard]] cy::Status start_ui() noexcept;
    [[nodiscard]] cy::Status ui_frame() noexcept;
    [[nodiscard]] cy::Status ui_layout() noexcept;
    void observe_hud(Observation& seen) noexcept;
    [[nodiscard]] cy::Status start_animation() noexcept;
    /// After every frame: what each unit's animator was doing, for the report.
    [[nodiscard]] cy::Status track_animation() noexcept;
    [[nodiscard]] cy::Status load_module(const char** detail) noexcept;
    [[nodiscard]] cy::Status start_systems() noexcept;
    [[nodiscard]] cy::Status attach_behaviours(const char** detail) noexcept;
    [[nodiscard]] cy::Status run_stages(cy::ecs::Stage first, cy::ecs::Stage last) noexcept;
    [[nodiscard]] cy::Status fixed_tick() noexcept;
    void observe_scout(Observation& seen) noexcept;
    void update_audio() noexcept;
    [[nodiscard]] cy::Nanoseconds event_time() noexcept;

    cy::Allocator& allocator_;
    HostOptions options_;

    // The world, the scene over it, and the schedule its systems run in.
    cy::ecs::World world_;
    cy::scene::SceneTree tree_;
    cy::ecs::Schedule schedule_;

    // The ABI, innermost first: the runtime destroys instances through the host, which reaches the
    // world through the binding — reverse destruction order is the safe one.
    cy::abi::World binding_;
    cy::abi::Host host_;
    cy::abi::BehaviourRuntime runtime_;
    cy::abi::ScriptSystems script_systems_;
    cy::game_backend::ScriptSceneBridge bridge_;
    cy::Array<char> manifest_text_;
    cy::abi::ModuleManifest manifest_;

    // Servers.
    cy::input::InputServer input_;
    cy::camera::CameraServer camera_;
    cy::physics::PhysicsServer* physics_ = nullptr;
    cy::physics::WorldHandle physics_world_;
    cy::audio::NullAudioBackend audio_device_;
    cy::audio::AudioServer audio_;

    // The interface (ABI 1.6): the store, its screen root, text on the built-in font, the adapter.
    cy::text::TextServer text_server_;
    cy::ui::TextPainter text_;
    cy::ui::ElementStore ui_store_;
    cy::ui::ElementId ui_root_;
    cy::UniquePtr<cy::game_backend::UiAdapter> ui_adapter_;

    // Navigation.
    cy::navigation::NavComponents nav_components_;
    cy::navigation::NavMesh nav_mesh_;
    cy::navigation::PathQueue nav_queue_;
    cy::navigation::NavWorlds nav_worlds_;

    // The adapters, and the plumbing two of them need.
    cy::game_backend::InputAdapter input_adapter_;
    cy::game_backend::CameraAdapter camera_adapter_;
    cy::game_backend::AudioAdapter audio_adapter_;
    cy::game_backend::SpawnAdapter spawn_adapter_;
    NodeMotion motion_;
    cy::UniquePtr<UnitBodies> bodies_;
    cy::UniquePtr<cy::game_backend::PhysicsQueryAdapter> physics_adapter_;
    cy::UniquePtr<cy::game_backend::NavigationAdapter> nav_adapter_;
    // ABI 1.5: characters, the one entity-to-body map, rigid-body writes, and the native observer.
    cy::UniquePtr<cy::game_backend::CharacterAdapter> characters_;
    cy::UniquePtr<LevelBodies> level_bodies_;
    cy::UniquePtr<cy::game_backend::PhysicsBodyAdapter> body_adapter_;
    VeterancyRoll roll_;
    cy::ecs::SystemId roll_id_ = cy::ecs::kInvalidSystem;
    // Issue #76 stage 4: the worker's rig, the engine's animation system in the schedule, and the
    // adapter the Swift game drives it through.
    WorkerRig worker_rig_;
    cy::determinism::SimulationClock animation_clock_;
    cy::ecs::ComponentTypeId animator_ = cy::ecs::kInvalidComponent;
    std::unique_ptr<cy::animation::AnimationSystem> animation_;
    std::unique_ptr<cy::game_backend::AnimationAdapter> animation_adapter_;
    /// What the report needs to know about each animated unit, across the run.
    struct UnitAnimation {
        CyEntity entity = CY_ENTITY_NULL;
        cy::Mat4 first[kWorkerJoints] = {};
        cy::f32 departure = 0.0F;
        bool walked = false;
        bool cheered = false;
        bool idle_after = false;
        bool left_idle = false;
    };
    cy::Array<UnitAnimation> unit_animation_;

    // Content.
    cy::Array<cy::f32> click_samples_;
    cy::Array<cy::f32> mix_scratch_;
    cy::physics::ShapeHandle ground_shape_;
    cy::physics::BodyHandle ground_;
    cy::camera::RigHandle rig_;
    cy::input::DeviceId keyboard_;
    cy::input::DeviceId mouse_;
    cy::physics::ShapeHandle crate_shape_;
    cy::physics::BodyHandle crate_body_;
    cy::f32 crate_start_z_ = 0.0F;
    // The level's nodes. The commander and the scout are where the two behaviours live.
    cy::scene::Node commander_;
    cy::scene::Node scout_;
    cy::scene::Node crate_;

    cy::u64 frame_ = 0;
    cy::u32 events_this_frame_ = 0;
    bool started_ = false;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_RTS_HOST_H
