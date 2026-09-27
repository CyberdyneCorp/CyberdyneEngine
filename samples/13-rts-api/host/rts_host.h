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
//   for the frame's one fixed tick:
//     clock.tick = tick
//     input adapter observes the pending events;  input server resolves the tick
//     behaviours' onFixedUpdate          (Swift, phase F)
//     navigation adapter update          (orders, paths, crowd; moves the scene nodes)
//     unit bodies follow their nodes;    physics step
//   input adapter begins the frame;      camera rig evaluates
//   behaviours' onUpdate                 (Swift, phase U)
//   audio adapter and audio server update; the null device mixes the frame
//
// The rig is evaluated BEFORE `onUpdate`, so what Swift reads from the camera is the view this
// frame shows. A `Camera.setTarget` from `onUpdate` shows on the next frame.
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
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/audio_backend.h>
#include <cy/game_backend/camera_backend.h>
#include <cy/game_backend/input_backend.h>
#include <cy/game_backend/navigation_backend.h>
#include <cy/game_backend/physics_backend.h>
#include <cy/game_backend/spawn_backend.h>
#include <cy/navigation/components.h>
#include <cy/navigation/navmesh.h>
#include <cy/navigation/query.h>
#include <cy/scene/tree.h>
#include <cy/servers/audio/backend.h>
#include <cy/servers/audio/server.h>
#include <cy/servers/camera/server.h>
#include <cy/servers/input/server.h>
#include <cy/servers/physics/server.h>

#include "units.h"

namespace sample::rts {

/// How the host runs. Engine choices only.
struct HostOptions {
    const char* module_library = "";
    const char* module_manifest = "";
    /// THE NEGATIVE CONTROL. False loads the module but creates no `Commander`, so every C++ path
    /// runs with no game behind it: no units, no camera moves, no sound.
    bool behaviours = true;
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
    [[nodiscard]] cy::Status load_module(const char** detail) noexcept;
    [[nodiscard]] cy::Status fixed_tick() noexcept;
    void update_audio() noexcept;
    [[nodiscard]] cy::Nanoseconds event_time() noexcept;

    cy::Allocator& allocator_;
    HostOptions options_;

    // The world and the scene over it.
    cy::ecs::World world_;
    cy::scene::SceneTree tree_;

    // The ABI, innermost first: the runtime destroys instances through the host, which reaches the
    // world through the binding — reverse destruction order is the safe one.
    cy::abi::World binding_;
    cy::abi::Host host_;
    cy::abi::BehaviourRuntime runtime_;
    cy::Array<char> manifest_text_;
    cy::abi::ModuleManifest manifest_;

    // Servers.
    cy::input::InputServer input_;
    cy::camera::CameraServer camera_;
    cy::physics::PhysicsServer* physics_ = nullptr;
    cy::physics::WorldHandle physics_world_;
    cy::audio::NullAudioBackend audio_device_;
    cy::audio::AudioServer audio_;

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

    // Content.
    cy::Array<cy::f32> click_samples_;
    cy::Array<cy::f32> mix_scratch_;
    cy::physics::ShapeHandle ground_shape_;
    cy::physics::BodyHandle ground_;
    cy::camera::RigHandle rig_;
    cy::input::DeviceId keyboard_;
    cy::input::DeviceId mouse_;
    cy::ecs::Entity player_;

    cy::u64 frame_ = 0;
    cy::u32 events_this_frame_ = 0;
    bool started_ = false;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_RTS_HOST_H
