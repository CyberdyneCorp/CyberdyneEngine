// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_THIRD_PERSON_TPS_HOST_H
#define CY_SAMPLE_THIRD_PERSON_TPS_HOST_H
// tps_host.h — the engine side of samples/14-third-person. It brings up the servers, builds the
// level's colliders, binds the adapters the game calls, loads the Swift module and runs frames. It
// decides nothing.
//
// ================================================================================================
// WHAT THE GAME REACHES, AND THROUGH WHAT
// ================================================================================================
//
//   Input.action          InputAdapter        four actions, scripted or a keyboard (level.h)
//   CharacterController   CharacterAdapter    a capsule on the Jolt world: stairs, slopes, walls
//   Animator              AnimationAdapter    the engine's AnimationSystem over the `hero` rig —
//                                             its blend space and its foot placement, whose ground
//                                             is the same physics world (PhysicsGroundProbe)
//   Camera                CameraAdapter       a third-person rig on the camera server, whose
//                                             collision queries CameraCollisionResolver answers
//                                             against the physics world every frame
//   Audio                 AudioAdapter        six synthesised cues (sounds.h), a listener at the
//                                             camera
//   SceneTree / @Node     ScriptSceneBridge   /Level/Hero, /Level/Camera, /Level/Ambience, and
//                                             the two nodes the colliders belong to
//
// ================================================================================================
// ONE FRAME, IN ORDER (one fixed tick a frame, 60 a second)
// ================================================================================================
//
//   scene tree pump                       Swift tree callbacks (onReady, the @Node paths)
//   the fixed tick:
//     input resolves the tick
//     behaviours' onFixedUpdate           Swift: the hero moves its capsule, sets its blend
//                                         space's speed and its feet, plays its states
//     the schedule's fixed stages         the animation system advances its clocks
//     physics step
//     the hero's node follows its capsule, turned by the game's HeroPose
//   input begins the frame; the animation adapter snapshots the tick's events
//   the camera: the game's last request turned into the rig's intent, the rig evaluated, its
//   queries answered by the physics world
//   behaviours' onUpdate                  Swift: the camera director asks for a framing; the hero
//                                         hears its footfalls and reads its feet
//   the schedule's frame stages           the animation system poses the hero, feet placed
//   the GI, from the hero (when a renderer built it); the audio mixes the frame
//
// ================================================================================================
// THE CAMERA REQUEST, AND WHY THE HOST TRANSLATES IT
// ================================================================================================
//
// `Camera.setTarget` carries an ABSOLUTE yaw, pitch and distance; the rig's orbit node takes look
// DELTAS and a normalised zoom, which is `camera-system`'s intent model. `CameraAdapter` records
// the request as the rig's `Framing` and leaves the conversion to the host (camera_backend.h says
// why), so this host turns the difference between the framing asked for and the one last applied
// into the orbit's intent each frame. The conversion has no opinion: a request that does not
// change applies a zero delta.

#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/abi/systems.h>
#include <cy/animation/animation_system.h>
#include <cy/animation/ground_probes.h>
#include <cy/core/base/expected.h>
#include <cy/core/determinism/clock.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/animation_backend.h>
#include <cy/game_backend/audio_backend.h>
#include <cy/game_backend/camera_backend.h>
#include <cy/game_backend/camera_collision.h>
#include <cy/game_backend/character_backend.h>
#include <cy/game_backend/input_backend.h>
#include <cy/game_backend/scene_bridge.h>
#include <cy/scene/tree.h>
#include <cy/servers/audio/backend.h>
#include <cy/servers/audio/server.h>
#include <cy/servers/camera/server.h>
#include <cy/servers/input/server.h>
#include <cy/servers/physics/server.h>

#include <functional>
#include <memory>
#include <vector>

#include "hero_rig.h"
#include "illumination.h"
#include "map_format.h"

namespace sample::tps {

/// How the host runs. Engine choices only.
struct HostOptions {
    const char* module_library = "";
    const char* module_manifest = "";
    /// THE NEGATIVE CONTROL. False loads the module and builds the level, but attaches no
    /// behaviour: no hero, no camera direction, no sound — every C++ path with no game behind it.
    bool behaviours = true;
    /// The cooked level: its terrain and placements become colliders, and the GI's boxes.
    const sample::rts::map::MapContent* map = nullptr;
    /// Build the GI (a renderer lights its frame with it). Headless runs leave it out.
    bool illumination = false;
    /// Write every frame's mix here, interleaved stereo at 48 kHz, when not null.
    std::vector<cy::f32>* mix = nullptr;
    /// The scripted run's own keyboard, which `press_key` injects on. A played run's devices are
    /// the platform's (samples/play), so it makes none.
    bool scripted_devices = true;
    /// Walls round the terrain's edge, not drawn: a played hero may run anywhere on the hillside
    /// and off none of it. The scripted run never reaches the edge and has none.
    bool fenced = false;
    /// Services an embedder binds on the ABI host before the game starts — the play host's
    /// interface (ABI 1.6 and 1.12), which the game builds its pause menu through.
    std::function<void(cy::abi::Host&)> bind_services;
};

/// One frame's time, as a real-time loop hands it over. The scripted run's is one step.
struct FrameStep {
    /// The frame's seconds: what the behaviours' `onUpdate` is told.
    cy::f32 seconds = 1.0F / 60.0F;
    /// How far past the last tick the frame is (`CyTime::interpolation`): the hero is drawn
    /// carried forward from the last tick by that much of a step.
    cy::f64 interpolation = 0.0;
    /// The simulation is paused (`CY_TIME_PAUSED`): the camera's smoothing and the sound hold.
    bool paused = false;
};

/// What the game and the engine say happened. `game` is the `HeroReport` Swift wrote, read and
/// never acted on; the rest is read from the servers and the adapters.
struct Observation {
    struct Game {
        bool found = false;
        cy::f32 ticks = 0.0F;
        cy::f32 grounded = 0.0F;
        cy::f32 airborne = 0.0F;
        cy::f32 stepped_up = 0.0F;
        cy::f32 jumps = 0.0F;
        cy::f32 landings = 0.0F;
        cy::f32 footsteps = 0.0F;
        cy::f32 stone_steps = 0.0F;
        cy::f32 grass_steps = 0.0F;
        cy::f32 highest = 0.0F;
        cy::f32 fastest = 0.0F;
        cy::f32 run_ticks = 0.0F;
        cy::f32 walk_ticks = 0.0F;
        cy::f32 feet_solved = 0.0F;
        cy::f32 feet_adjusted = 0.0F;
        cy::f32 pelvis_drop = 0.0F;
        cy::f32 states = 0.0F;
        cy::f32 camera_pulled = 0.0F;
        cy::f32 camera_nearest = 1.0F;
        cy::f32 camera_yaw = 0.0F;
        cy::f32 ambience = 0.0F;
        cy::f32 dropped = 0.0F;
    };
    Game game;
    /// The hero, from its controller and its node.
    bool hero = false;
    cy::Vec3 hero_feet{0.0F, 0.0F, 0.0F};
    cy::f32 hero_facing = 0.0F;
    /// Frames the camera's resolver answered a collision for, and the nearest it answered.
    cy::u32 collision_frames = 0;
    cy::f32 nearest_fraction = 1.0F;
    cy::u64 camera_queries = 0;
    /// The animation system: the largest a skinning matrix moved from the hero's first pose, and
    /// the most the foot placement lowered the hips, from the system's own report.
    cy::f32 departure = 0.0F;
    cy::f32 deepest_pelvis = 0.0F;
    cy::u32 placed_frames = 0;
    /// Audio: the most voices playing after any frame, and voices the mixer started.
    cy::u32 peak_voices = 0;
    cy::u64 voices_started = 0;
    /// Everything that moves, folded over every frame: the digest two runs must agree on.
    cy::u64 digest = 0;
};

class ThirdPersonHost {
public:
    /// A frame's time, as `cy::play::HostedGame` hands it over.
    using Step = FrameStep;

    ThirdPersonHost(cy::Allocator& allocator, const HostOptions& options) noexcept;
    ~ThirdPersonHost();

    ThirdPersonHost(const ThirdPersonHost&) = delete;
    ThirdPersonHost& operator=(const ThirdPersonHost&) = delete;
    ThirdPersonHost(ThirdPersonHost&&) = delete;
    ThirdPersonHost& operator=(ThirdPersonHost&&) = delete;

    /// Bring everything up. `detail` names what failed where the failure has a name.
    [[nodiscard]] cy::Status start(const char** detail) noexcept;
    /// One frame of the scripted run — `pump`, one `tick`, `begin_frame`, `update` — and its wall
    /// time. See the header.
    [[nodiscard]] cy::Status frame() noexcept;

    // --- A frame's parts, for a real-time loop (samples/play) -----------------------------------
    [[nodiscard]] cy::Status pump() noexcept;
    /// One fixed tick, its input stamped `now` on the events' clock.
    [[nodiscard]] cy::Status tick(cy::Nanoseconds now) noexcept;
    /// A paused frame's input, resolved again at the last tick.
    [[nodiscard]] cy::Status drain(cy::Nanoseconds now) noexcept;
    /// The frame's pointer: the edges since the last frame, published.
    void begin_frame() noexcept;
    /// The rest of the frame: the camera, the behaviours' `onUpdate`, the pose, the GI, the mix.
    [[nodiscard]] cy::Status update(const FrameStep& step) noexcept;
    [[nodiscard]] cy::input::InputServer& input() noexcept { return input_; }
    [[nodiscard]] CyInputPointer pointer() noexcept;
    void set_pointer_over_ui(bool over) noexcept;
    void ui_event(const CyUiEvent& event) noexcept;
    /// Tear down in the one safe order. Idempotent; the destructor calls it.
    void shutdown() noexcept;

    // --- The hand --------------------------------------------------------------------------------
    [[nodiscard]] cy::Status press_key(cy::input::Key key, bool down) noexcept;

    // --- The eyes
    // ----------------------------------------------------------------------------------
    [[nodiscard]] Observation observe() noexcept;
    [[nodiscard]] cy::u64 frames() const noexcept { return frame_; }
    [[nodiscard]] cy::f64 last_frame_ms() const noexcept { return frame_ms_; }
    [[nodiscard]] cy::u32 behaviours() const noexcept { return runtime_.live_instances(); }
    /// The camera as last evaluated: where it is, where it looks, its vertical field of view.
    [[nodiscard]] cy::Vec3 camera_position() const noexcept;
    [[nodiscard]] cy::Vec3 camera_forward() const noexcept;
    [[nodiscard]] cy::f32 camera_fov() const noexcept;
    /// The hero's node: its feet on the ground, turned the way the game faces it.
    [[nodiscard]] cy::Transform hero_placement() const noexcept;
    /// What the frame DRAWS: the hero at the frame's own time, carried forward from the last tick
    /// by its motion over it (`FrameStep::interpolation`). On a tick, `hero_placement` exactly.
    [[nodiscard]] cy::Transform drawn_hero() const noexcept;
    /// The hero's skinning matrices as the animation system last published them, one a joint of
    /// the hero rig; empty without an animator (`--no-behaviours`).
    [[nodiscard]] cy::Span<const cy::Mat4> hero_pose() const noexcept;
    [[nodiscard]] TpsIllumination& illumination() noexcept { return *gi_; }
    /// One line of where everything is, for `--trace`.
    void print_trace() const noexcept;

private:
    [[nodiscard]] cy::Status start_scene() noexcept;
    [[nodiscard]] cy::Status start_input() noexcept;
    [[nodiscard]] cy::Status start_camera() noexcept;
    [[nodiscard]] cy::Status start_physics() noexcept;
    [[nodiscard]] cy::Status start_level() noexcept;
    [[nodiscard]] cy::Status start_audio() noexcept;
    [[nodiscard]] cy::Status start_animation() noexcept;
    [[nodiscard]] cy::Status start_illumination() noexcept;
    [[nodiscard]] cy::Status load_module(const char** detail) noexcept;
    [[nodiscard]] cy::Status attach_behaviours(const char** detail) noexcept;
    [[nodiscard]] cy::Status static_box(cy::Vec3 centre, cy::Vec3 half, cy::Quat rotation,
                                        CyEntity owner) noexcept;
    [[nodiscard]] cy::Status run_stages(cy::ecs::Stage first, cy::ecs::Stage last) noexcept;
    [[nodiscard]] cy::Status fixed_tick(cy::Nanoseconds now) noexcept;
    [[nodiscard]] cy::Status drive_camera(cy::f32 seconds) noexcept;
    [[nodiscard]] cy::Status fence() noexcept;
    void place_hero() noexcept;
    void update_audio(cy::f32 seconds) noexcept;
    void track() noexcept;
    [[nodiscard]] cy::Nanoseconds event_time() noexcept;
    [[nodiscard]] const cy::physics::CharacterController* hero_controller() const noexcept;

    cy::Allocator& allocator_;
    HostOptions options_;

    cy::ecs::World world_;
    cy::scene::SceneTree tree_;
    cy::ecs::Schedule schedule_;

    // The ABI, innermost first: reverse destruction order is the safe one.
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
    cy::audio::ListenerHandle listener_;

    // Adapters.
    cy::game_backend::InputAdapter input_adapter_;
    cy::game_backend::CameraAdapter camera_adapter_;
    cy::game_backend::AudioAdapter audio_adapter_;
    cy::UniquePtr<cy::game_backend::CharacterAdapter> characters_;
    std::unique_ptr<cy::game_backend::CameraCollisionResolver> camera_collision_;
    std::unique_ptr<cy::animation::PhysicsGroundProbe> ground_;

    // Animation.
    HeroRig hero_rig_;
    cy::determinism::SimulationClock animation_clock_;
    cy::ecs::ComponentTypeId animator_ = cy::ecs::kInvalidComponent;
    std::unique_ptr<cy::animation::AnimationSystem> animation_;
    std::unique_ptr<cy::game_backend::AnimationAdapter> animation_adapter_;

    std::unique_ptr<TpsIllumination> gi_;

    // Content.
    std::vector<cy::physics::ShapeHandle> shapes_;
    std::vector<cy::physics::BodyHandle> bodies_;
    std::vector<cy::f32> mix_scratch_;
    std::vector<std::vector<cy::f32>> sound_samples_;
    cy::camera::RigHandle rig_;
    cy::input::DeviceId keyboard_;
    cy::scene::Node hero_;
    cy::scene::Node camera_node_;
    cy::scene::Node ambience_;
    cy::scene::Node terrain_;
    cy::scene::Node courtyard_;

    // The camera request last applied to the rig: what the next delta is measured from.
    cy::u32 framing_revision_ = 0;
    cy::f32 applied_yaw_ = 0.0F;
    cy::f32 applied_pitch_ = 0.0F;
    bool framing_applied_ = false;

    // The hero's node, smoothed vertically over a stair's step.
    cy::f32 presented_feet_ = 0.0F;
    bool presented_ = false;

    // What the report reads across the run.
    cy::Mat4 first_pose_[kHeroJoints] = {};
    bool first_pose_taken_ = false;
    cy::f32 departure_ = 0.0F;
    cy::f32 deepest_pelvis_ = 0.0F;
    cy::u32 placed_frames_ = 0;
    cy::u32 collision_frames_ = 0;
    cy::f32 nearest_fraction_ = 1.0F;
    cy::u64 camera_queries_ = 0;
    cy::u32 peak_voices_ = 0;
    cy::u64 digest_ = 14695981039346656037ULL;

    cy::u64 frame_ = 0;
    /// Ticks run: one a frame in the scripted run, where it is `frame_ + 1` inside a frame.
    cy::u64 tick_ = 0;
    /// The hero's node before the last tick, and how far past it the frame is.
    cy::Transform hero_before_ = cy::Transform::identity();
    cy::f64 interpolation_ = 0.0;
    cy::f64 frame_ms_ = 0.0;
    cy::u32 events_this_frame_ = 0;
    bool started_ = false;
};

}  // namespace sample::tps

#endif  // CY_SAMPLE_THIRD_PERSON_TPS_HOST_H
