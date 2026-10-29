// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_FIRST_PERSON_FPS_HOST_H
#define CY_SAMPLE_FIRST_PERSON_FPS_HOST_H
// fps_host.h — the engine side of samples/15-first-person. It brings up the servers, builds the
// arena, binds the ABI's adapters, loads the Swift module and runs frames. It decides nothing about
// the player: how the player moves, crouches, aims and shoots, what a shot does and what it leaves
// behind are the Swift game's (game/Player.swift). What the TARGETS do is the engine's AI
// (targets.h).
//
// ================================================================================================
// ONE FRAME, IN ORDER
// ================================================================================================
//
//   scene tree pump                       (Swift tree callbacks: onEnterTree, onReady — phase N)
//   the frame's one fixed tick:
//     input resolves the tick
//     behaviours' onFixedUpdate           (Swift, phase F: the character moves, a shot is a
//                                          raycast, its impulse and its damage)
//     the targets think and move          (cy::ai: what the game wrote, what each sees, the tree)
//     physics step
//   input begins the frame; the light pool ages; the decal clock
//   behaviours' onUpdate                  (Swift, phase U: the look, the recoil, the flash, the
//                                          bursts, the marks, the sounds, and where the head is)
//   the first-person camera advances      (the look and the recoil since the last frame)
//   the eye: the head the game set, plus the bob, turned by the look
//   the effects' world steps; the GI updates from the eye; the audio mixes
//
// The eye a frame is drawn from is therefore the look the game fed THIS frame — no frame of lag —
// and the aim a shot in the next fixed step takes is the one this frame drew.
//
// ================================================================================================
// THE HAND
// ================================================================================================
//
// `press_key`, `move_pointer` and `press_button` inject synthetic events into the input server, the
// door a recorded replay uses; the scripted player (script.h) uses them, and `eye`, `aim` and
// `raycast` to aim the way a person aims with their eyes. The game never sees them.

#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/audio_backend.h>
#include <cy/game_backend/camera_backend.h>
#include <cy/game_backend/character_backend.h>
#include <cy/game_backend/decals_backend.h>
#include <cy/game_backend/effects_backend.h>
#include <cy/game_backend/first_person_backend.h>
#include <cy/game_backend/input_backend.h>
#include <cy/game_backend/lights_backend.h>
#include <cy/game_backend/physics_backend.h>
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

#include "arena.h"
#include "bodies.h"
#include "content.h"
#include "illumination.h"
#include "shot_effects.h"
#include "targets.h"

namespace sample::fps {

struct HostOptions {
    const char* module_library = "";
    const char* module_manifest = "";
    /// THE NEGATIVE CONTROL. False loads the module but attaches no `Player`: every C++ path runs
    /// with no game behind it — no character, no look, no shot, no flash, no mark.
    bool behaviours = true;
    /// Run the arena's GI. A headless run that only checks the game may leave it off.
    bool illumination = true;
    /// The scripted run's own keyboard and mouse, which `press_key` and `move_pointer` inject on.
    /// A played run's devices are the platform's (play.h), so it makes none.
    bool scripted_devices = true;
    /// Services an embedder binds on the ABI host before the game starts — the play host's
    /// interface (ABI 1.6 and 1.12), which the game builds its HUD and its menu through.
    std::function<void(cy::abi::Host&)> bind_services;
};

/// One frame's time, as a real-time loop hands it over. The scripted run's is one step.
struct FrameStep {
    /// The frame's seconds: what the behaviours' `onUpdate` is told.
    cy::f32 seconds = 1.0F / 60.0F;
    /// How far past the last tick the frame is (`CyTime::interpolation`): what is drawn is carried
    /// forward from the last tick by that much of a step.
    cy::f64 interpolation = 0.0;
    /// The simulation is paused (`CY_TIME_PAUSED`): nothing that is presentation advances.
    bool paused = false;
};

/// One shot as the game reported it, captured the frame its count moved.
struct ShotRecord {
    cy::u64 frame = 0;
    cy::Vec3 origin{0.0F, 0.0F, 0.0F};
    cy::Vec3 direction{0.0F, 0.0F, -1.0F};
    CyEntity hit = CY_ENTITY_NULL;
    cy::Vec3 point{0.0F, 0.0F, 0.0F};
    cy::Vec3 normal{0.0F, 1.0F, 0.0F};
    cy::f32 distance = -1.0F;
    /// 0 a miss, 1 the world, 2 a prop, 3 a target.
    cy::u32 kind = 0;
    /// The host's own raycast along the same ray, the same moment, hit the same entity at the same
    /// point (or missed with it).
    bool confirmed = false;
};

/// How far a shot reaches: the weapon's range, which game/Contract.swift states too.
inline constexpr cy::f32 kShotRange = 120.0F;

/// What the game and the engine say happened. Engine facts are read from the servers and the
/// adapters; `game` is the `PlayerReport` component Swift writes, read and never acted on.
struct Observation {
    struct Game {
        bool found = false;
        cy::f32 shots = 0.0F;
        cy::f32 target_hits = 0.0F;
        cy::f32 prop_hits = 0.0F;
        cy::f32 world_hits = 0.0F;
        cy::f32 misses = 0.0F;
        cy::f32 kills = 0.0F;
        cy::f32 walk_speed = 0.0F;
        cy::f32 sprint_speed = 0.0F;
        cy::f32 crouch_speed = 0.0F;
        cy::f32 jumps = 0.0F;
        cy::f32 airborne = 0.0F;
        cy::f32 peak_height = 0.0F;
        cy::f32 crouched = 0.0F;
        cy::f32 stand_refused = 0.0F;
        cy::f32 under_beam = 0.0F;
        cy::f32 flashes = 0.0F;
        cy::f32 bursts = 0.0F;
        cy::f32 sparks = 0.0F;
        cy::f32 marks = 0.0F;
        cy::f32 sounds = 0.0F;
        cy::f32 kicks = 0.0F;
        cy::f32 impulses = 0.0F;
        cy::f32 aimed = 0.0F;
        cy::f32 refused = 0.0F;
    };
    Game game;
    /// The player's character, from the character adapter.
    cy::Vec3 player{0.0F, 0.0F, 0.0F};
    bool grounded = false;
    cy::f32 capsule_height = 0.0F;
    /// The first-person camera: its look, the recoil's peak over the run and the sights.
    cy::f32 yaw = 0.0F;
    cy::f32 pitch = 0.0F;
    cy::f32 peak_recoil = 0.0F;
    cy::f32 peak_aim = 0.0F;
    /// The pools: lights, effects and decals, as their adapters counted them.
    cy::u64 lights_spawned = 0;
    cy::u32 lights_live_peak = 0;
    cy::u64 effects_played = 0;
    cy::u64 muzzle_plays = 0;
    cy::u64 spark_plays = 0;
    cy::u64 decals_spawned = 0;
    cy::u32 decals_live = 0;
    cy::u32 peak_particles = 0;
    cy::u64 particle_frames = 0;
    cy::u32 peak_voices = 0;
    /// Each target's `Health`, as the game wrote it.
    TargetHealth health[kTargetCount];
    /// How far each prop moved from where it started.
    cy::f32 prop_moved[kPropCount] = {};
};

class FpsHost {
public:
    /// A frame's time, as `cy::play::HostedGame` hands it over.
    using Step = FrameStep;

    FpsHost(cy::Allocator& allocator, const HostOptions& options) noexcept;
    ~FpsHost();

    FpsHost(const FpsHost&) = delete;
    FpsHost& operator=(const FpsHost&) = delete;
    FpsHost(FpsHost&&) = delete;
    FpsHost& operator=(FpsHost&&) = delete;

    /// Bring everything up. `detail` names what failed where the failure has a name.
    [[nodiscard]] cy::Status start(const char** detail) noexcept;
    /// One frame of the scripted run — `pump`, one `tick`, `begin_frame`, `update` — and its wall
    /// time. See the header.
    [[nodiscard]] cy::Status frame() noexcept;

    // --- A frame's parts, for a real-time loop (play.h) ----------------------------------------
    /// The scene tree's callbacks.
    [[nodiscard]] cy::Status pump() noexcept;
    /// One fixed tick, its input stamped `now` on the events' clock.
    [[nodiscard]] cy::Status tick(cy::Nanoseconds now) noexcept;
    /// A paused frame's input: resolved again at the last tick, so nothing runs and nothing pressed
    /// meanwhile reaches the next tick as an edge.
    [[nodiscard]] cy::Status drain(cy::Nanoseconds now) noexcept;
    /// The frame's pointer: the edges since the last frame, published.
    void begin_frame() noexcept;
    /// The rest of the frame: the behaviours' `onUpdate`, the eye, the effects, the GI, the mix.
    [[nodiscard]] cy::Status update(const FrameStep& step) noexcept;
    /// The input server the platform feeds, the ABI host the interface binds on, the pointer the
    /// interface routes and a delivered interface event.
    [[nodiscard]] cy::input::InputServer& input() noexcept { return input_; }
    [[nodiscard]] CyInputPointer pointer() noexcept;
    void set_pointer_over_ui(bool over) noexcept;
    void ui_event(const CyUiEvent& event) noexcept;
    /// Tear down in the one safe order. Idempotent; the destructor calls it.
    void shutdown() noexcept;

    // --- The hand ------------------------------------------------------------------------------
    [[nodiscard]] cy::Status press_key(cy::input::Key key, bool down) noexcept;
    /// Move the pointer by (`dx`, `dy`) window pixels: mouse counts.
    [[nodiscard]] cy::Status move_pointer(cy::f32 dx, cy::f32 dy) noexcept;
    [[nodiscard]] cy::Status press_button(cy::input::MouseControl button, bool down) noexcept;
    /// The first person's look as the camera last advanced it: control yaw and pitch, and where
    /// the weapon points (recoil included).
    [[nodiscard]] cy::f32 look_yaw() const noexcept;
    [[nodiscard]] cy::f32 look_pitch() const noexcept;
    [[nodiscard]] cy::Vec3 weapon_aim() const noexcept;
    /// What a ray from the eye along `direction` hits first, the player excluded: the entity, or
    /// null for nothing or the arena's own boxes.
    [[nodiscard]] CyEntity raycast(cy::Vec3 origin, cy::Vec3 direction, cy::f32 distance,
                                   cy::physics::RayCastHit* out = nullptr) const noexcept;

    // --- The eyes ------------------------------------------------------------------------------
    [[nodiscard]] cy::u64 frames() const noexcept { return frame_; }
    /// The eye the frame is drawn from, and its vertical field of view.
    [[nodiscard]] const cy::Transform& eye() const noexcept { return eye_; }
    [[nodiscard]] cy::f32 fov() const noexcept { return fov_; }
    /// The weapon's placement in the world, and the weapon view the frame draws it through.
    [[nodiscard]] const cy::Transform& weapon() const noexcept { return weapon_; }
    [[nodiscard]] cy::rendering::viewmodel::ViewmodelSettings weapon_view() const noexcept;
    /// The player's feet, from its character.
    [[nodiscard]] cy::Vec3 player_feet() const noexcept;
    [[nodiscard]] const TargetState& target(cy::u32 index) const noexcept {
        return targets_->state(index);
    }
    [[nodiscard]] CyEntity target_entity(cy::u32 index) const noexcept;
    [[nodiscard]] CyEntity prop_entity(cy::u32 index) const noexcept;
    [[nodiscard]] cy::Transform prop(cy::u32 index) const noexcept { return bodies_->prop(index); }
    /// What the frame DRAWS: a prop and a target at the frame's own time, carried forward from the
    /// last tick by their motion over it (`FrameStep::interpolation`). The scripted run's frames
    /// fall on their ticks, where these are `prop` and `target` exactly.
    [[nodiscard]] cy::Transform drawn_prop(cy::u32 index) const noexcept;
    [[nodiscard]] TargetState drawn_target(cy::u32 index) const noexcept;
    /// The lights the game spawned that are alive now, appended to `out`.
    [[nodiscard]] cy::Status append_lights(
        cy::Array<cy::render::LightDescription>& out) const noexcept;
    [[nodiscard]] ShotEffects& effects() noexcept { return *effects_; }
    [[nodiscard]] ArenaIllumination& illumination() noexcept { return *gi_; }
    [[nodiscard]] cy::Span<const ShotRecord> shots() const noexcept {
        return {shots_.data(), shots_.size()};
    }
    /// The run so far, folded: the player, the targets and the props, quantised, every frame.
    [[nodiscard]] cy::u64 digest() const noexcept { return digest_; }
    [[nodiscard]] Observation observe() noexcept;
    [[nodiscard]] const Targets& targets() const noexcept { return *targets_; }
    /// The wall time of the last `frame`.
    [[nodiscard]] cy::f64 last_frame_ms() const noexcept { return frame_ms_; }
    [[nodiscard]] cy::u32 behaviours() const noexcept { return runtime_.live_instances(); }

private:
    [[nodiscard]] cy::Status start_scene() noexcept;
    [[nodiscard]] cy::Status start_input() noexcept;
    [[nodiscard]] cy::Status start_camera() noexcept;
    [[nodiscard]] cy::Status start_physics() noexcept;
    [[nodiscard]] cy::Status start_audio() noexcept;
    [[nodiscard]] cy::Status start_presentation() noexcept;
    [[nodiscard]] cy::Status start_targets() noexcept;
    [[nodiscard]] cy::Status load_module(const char** detail) noexcept;
    [[nodiscard]] cy::Status fixed_tick(cy::Nanoseconds now) noexcept;
    /// What a tick moves, as it was before it: what `drawn_*` carry forward from.
    void remember_tick() noexcept;
    [[nodiscard]] cy::Status place_eye(cy::f32 seconds) noexcept;
    void read_health(TargetHealth (&out)[kTargetCount]) const noexcept;
    void capture_shot() noexcept;
    void fold_digest() noexcept;
    void read_report(Observation& seen) const noexcept;
    [[nodiscard]] cy::Nanoseconds event_time() noexcept;

    cy::Allocator& allocator_;
    HostOptions options_;

    cy::ecs::World world_;
    cy::scene::SceneTree tree_;

    // The ABI, innermost first: reverse destruction order is the safe one.
    cy::abi::World binding_;
    cy::abi::Host host_;
    cy::abi::BehaviourRuntime runtime_;
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

    // The adapters.
    cy::game_backend::InputAdapter input_adapter_;
    cy::game_backend::CameraAdapter camera_adapter_;
    cy::game_backend::AudioAdapter audio_adapter_;
    cy::game_backend::FirstPersonAdapter first_person_;
    cy::game_backend::LightsAdapter lights_;
    std::unique_ptr<ArenaBodies> bodies_;
    cy::UniquePtr<cy::game_backend::CharacterAdapter> characters_;
    cy::UniquePtr<cy::game_backend::PhysicsQueryAdapter> physics_adapter_;
    cy::UniquePtr<cy::game_backend::PhysicsBodyAdapter> body_adapter_;
    std::unique_ptr<ShotEffects> effects_;
    std::unique_ptr<cy::game_backend::EffectsAdapter> effects_adapter_;
    std::unique_ptr<cy::game_backend::DecalsAdapter> decals_adapter_;
    std::unique_ptr<Targets> targets_;
    std::unique_ptr<ArenaIllumination> gi_;

    // Content.
    /// The generated sounds, one a `Sound`, alive as long as the clips that play them.
    std::vector<cy::Array<cy::f32>> sounds_;
    cy::Array<cy::f32> mix_scratch_;
    cy::camera::RigHandle rig_;
    /// The rig as the ABI names it: what the game's first-person behaviours are keyed by.
    CyCamera camera_handle_ = 0;
    cy::input::DeviceId keyboard_;
    cy::input::DeviceId mouse_;
    cy::f32 pointer_x_ = 640.0F;
    cy::f32 pointer_y_ = 360.0F;

    // The arena's nodes.
    cy::scene::Node arena_;
    cy::scene::Node player_;
    cy::scene::Node target_nodes_[kTargetCount];
    cy::scene::Node prop_nodes_[kPropCount];

    // What the frame is drawn from.
    cy::Transform eye_ = cy::Transform::identity();
    cy::Transform weapon_ = cy::Transform::identity();
    cy::f32 fov_ = 1.308997F;

    // The last tick's props and targets, and how far past it the frame is.
    cy::Transform props_before_[kPropCount];
    TargetState targets_before_[kTargetCount];
    cy::f64 interpolation_ = 0.0;

    // The run.
    std::vector<ShotRecord> shots_;
    cy::f32 shots_seen_ = 0.0F;
    cy::f32 peak_recoil_ = 0.0F;
    cy::f32 peak_aim_ = 0.0F;
    cy::u32 lights_live_peak_ = 0;
    cy::u32 peak_voices_ = 0;
    cy::u64 digest_ = 0xCBF29CE484222325ULL;
    cy::u64 frame_ = 0;
    /// Ticks run: one a frame in the scripted run, where it is `frame_ + 1` inside a frame.
    cy::u64 tick_ = 0;
    cy::f64 frame_ms_ = 0.0;
    cy::u32 events_this_frame_ = 0;
    bool started_ = false;
};

}  // namespace sample::fps

#endif  // CY_SAMPLE_FIRST_PERSON_FPS_HOST_H
