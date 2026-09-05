#ifndef CY_SAMPLE_CHARACTER_GAME_H
#define CY_SAMPLE_CHARACTER_GAME_H
// game.h — the host. Everything the engine does for a game, and nothing the game decides. Task 5.1.
//
// ================================================================================================
// WHAT THIS OBJECT IS, AND THE ONE CLAIM IT EXISTS TO MAKE
// ================================================================================================
//
// It brings up five servers, loads a Swift module over the C ABI, and translates between them once
// per fixed tick. Read `fixed_step()` and you have the whole program: eleven steps, each of which
// moves a value from one subsystem's vocabulary into another's. Not one of them chooses anything.
//
// Every decision in this sample — how fast the character walks, when it may jump, how far it
// travels between footsteps, where the camera looks, where the walls are — is in
// samples/04-character/game/, in Swift. `tools/check_no_cpp_gameplay.py` checks that statically,
// and `--no-behaviours` checks it by running: the identical host, the identical level, the
// identical command stream, with the two deciding behaviours not instantiated. Nothing moves. That
// is the negative control, and it is worth more than the grep, because a grep can only fail on the
// words somebody thought to forbid.
//
// ================================================================================================
// WHY THE INPUT-TO-COMMAND BRIDGE IS HERE AND NOT IN src/gameplay/
// ================================================================================================
//
// `gameplay-framework`'s invariant is that the simulation has exactly one input path, and
// `src/gameplay/tests/test_bypass.cpp` makes it structural: `cy::gameplay` declares no dependency
// on `cy::servers-input`, so an input header is not on a gameplay translation unit's include path
// at all. Something must nevertheless read a `cy::input::CommandFrame` and record a
// `cy::gameplay::Command`, and that something has to be able to see both — which means it lives
// ABOVE both, in a host or in `src/runtime/`. This is that place.
//
// The rule it keeps is visible in `fixed_step()`: the command frame is read, recorded, committed,
// and only the COMMITTED command is written into `PlayerInput`. The behaviours read `PlayerInput`.
// So what the game consumes is what the log contains, and a replay of the log reproduces the run.
// There is no second path, and adding one would be a one-line change that nothing here would catch
// — which is why `gameplay-framework` asks for the test rather than for the comment.
//
// ================================================================================================
// THE SERVERS, AND WHY THE SAMPLE OWNS THEM RATHER THAN THE RUNTIME
// ================================================================================================
//
// `cy::runtime::ServerRegistry` resolves `ServerKind::Physics` and `ServerKind::Audio` to
// `NullServer`: the four-line adapters that would register the real ones are not written, and
// src/runtime/ belongs to nobody at M4. So this host constructs them directly, which is what a
// sample does anyway — samples/03-first-light owns its RHI device for the same reason. When the
// adapters land, `start()` loses five constructions and gains five lookups.

#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/servers/audio/backend.h>
#include <cy/servers/audio/server.h>
#include <cy/servers/camera/server.h>
#include <cy/servers/input/server.h>
#include <cy/servers/physics/character.h>
#include <cy/servers/physics/server.h>

#include "contract.h"

namespace sample {

/// One tick of intent, as it crosses the command stream.
///
/// `look` is in RADIANS rather than in raw axis units, because sensitivity is a local player
/// setting and a log that carried raw axes would replay differently on a machine whose owner had
/// moved the slider. What the simulation consumed is what the log has to hold.
///
/// Declared here rather than in game.cpp only because `publish_input` takes one: it is the shape of
/// a command payload and nothing outside this host has a reason to know it.
struct IntentPayload {
    cy::f32 move_x = 0.0F;
    cy::f32 move_z = 0.0F;
    cy::f32 look_yaw = 0.0F;
    cy::f32 look_pitch = 0.0F;
    cy::u32 pressed = 0;
    cy::u32 just_pressed = 0;
};

/// How the host was asked to run. Everything here is about the ENGINE — which module, which input
/// source, how loud — and nothing about the game.
struct GameOptions {
    /// The game module's shared library and its `module.toml`. Both are paths the build supplies.
    const char* module_library = "";
    const char* module_manifest = "";
    /// THE NEGATIVE CONTROL. False loads the module and builds the level from it, but creates
    /// neither `Character` nor `CameraDirector` — so every C++ path in `fixed_step()` runs with no
    /// game behind it. See the class comment.
    bool behaviours = true;
    /// Take input from the SDL3 keyboard rather than from the scripted timeline. Requires a window
    /// system; the headless default is the script, which is what makes a smoke test reproducible.
    bool device_input = false;
    /// Use the Jolt backend where the build has one. The reference backend is the default because
    /// it is always present and because a sample that only ran against Jolt would not be evidence
    /// that `PhysicsServer` is an interface.
    bool jolt = false;
    /// Print one line per tick. Off by default; a ten-second run is six hundred lines.
    bool verbose = false;
};

/// What the run did. Read by the summary and by tests/smoke/test_character_sample.cpp, which is why
/// every field is a number rather than a sentence.
struct GameReport {
    cy::u64 ticks = 0;
    cy::u32 level_bodies = 0;
    cy::u32 behaviours = 0;

    /// The counters the game advanced, as the host observed them.
    cy::u32 footsteps = 0;
    cy::u32 landings = 0;
    cy::u32 jumps = 0;

    cy::u32 grounded_ticks = 0;
    cy::u32 airborne_ticks = 0;
    cy::u32 stepped_up_ticks = 0;
    cy::u32 wall_ticks = 0;

    cy::u32 commands_committed = 0;
    cy::u32 commands_rejected = 0;
    cy::u64 command_log_hash = 0;
    /// The per-tick identity of the input the simulation consumed, folded over the run. What a
    /// desync report's first question is answered with.
    cy::u64 input_hash = 0;

    cy::Vec3 start{0.0F, 0.0F, 0.0F};
    cy::Vec3 end{0.0F, 0.0F, 0.0F};
    cy::f32 distance_travelled = 0.0F;
    cy::f32 highest_point = 0.0F;
    cy::f32 lowest_point = 0.0F;

    cy::Vec3 camera_start{0.0F, 0.0F, 0.0F};
    cy::Vec3 camera_end{0.0F, 0.0F, 0.0F};
    cy::f32 camera_travelled = 0.0F;
    cy::u32 views_produced = 0;

    cy::u32 voices_started = 0;
    cy::u64 audio_frames = 0;

    const char* physics_backend = "";
};

/// The host.
///
/// MEMBER ORDER IS LOAD-BEARING at two places and both are commented at the declaration:
/// `binding_` before `host_` before `runtime_`, because each holds the one above it and reverse
/// destruction is what makes that safe; and `character_` before the physics server is torn down,
/// which `shutdown()` does explicitly because the server is not a member.
class Game {
public:
    Game(cy::Allocator& allocator, cy::ecs::World& world, const GameOptions& options) noexcept;
    ~Game();

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    /// Bring everything up, in the one order that works. `detail` names what failed where the
    /// failure has a name — a component the module did not register, a behaviour it did not export.
    [[nodiscard]] cy::Status start(const char** detail) noexcept;

    /// Register the one system that runs a tick. The whole of `fixed_step()` is one system because
    /// it is one sequence: nothing in it can run in parallel with anything else in it, and
    /// declaring four systems that must run in order would be declaring an order the scheduler
    /// would then have to be told about.
    [[nodiscard]] cy::Status install(cy::ecs::Schedule& schedule) noexcept;

    /// One fixed simulation tick. Called by the schedule; see the class comment.
    void fixed_step() noexcept;

    void shutdown() noexcept;

    [[nodiscard]] const GameReport& report() const noexcept { return report_; }
    [[nodiscard]] cy::input::InputServer& input() noexcept { return input_; }

private:
    [[nodiscard]] cy::Status start_physics() noexcept;
    [[nodiscard]] cy::Status start_input() noexcept;
    [[nodiscard]] cy::Status start_audio() noexcept;
    [[nodiscard]] cy::Status start_gameplay() noexcept;
    [[nodiscard]] cy::Status load_module(const char** detail) noexcept;
    [[nodiscard]] cy::Status create_behaviours(const char** detail) noexcept;
    [[nodiscard]] cy::Status build_level() noexcept;
    [[nodiscard]] cy::Status build_character() noexcept;
    [[nodiscard]] cy::Status build_camera() noexcept;

    void inject_script(cy::Nanoseconds now) noexcept;
    void publish_input(const IntentPayload& intent) noexcept;
    void drive_character(cy::f32 step) noexcept;
    void publish_state() noexcept;
    void play_cues() noexcept;
    void drive_camera(cy::f32 step) noexcept;

    cy::Allocator& allocator_;
    cy::ecs::World& world_;
    GameOptions options_;

    // The ABI, innermost first. `BehaviourRuntime` destroys every live instance through the records
    // `Host` owns, and `Host` reaches the world through `World` — so the declaration order here is
    // the reverse of the destruction order that is safe. cy/abi/module.h states the rule.
    cy::abi::World binding_;
    cy::abi::Host host_;
    cy::abi::BehaviourRuntime runtime_;
    cy::Array<char> manifest_text_;
    cy::abi::ModuleManifest manifest_;

    // The servers.
    cy::input::InputServer input_;
    cy::audio::NullAudioBackend audio_backend_;
    cy::audio::AudioServer audio_;
    cy::camera::CameraServer camera_;
    cy::physics::PhysicsServer* physics_ = nullptr;
    cy::physics::WorldHandle physics_world_;
    cy::UniquePtr<cy::physics::CharacterController> character_;

    // The one door into the simulation.
    cy::gameplay::GameSession session_;
    cy::gameplay::ControlRegistry control_;
    cy::gameplay::CommandStream commands_;
    cy::gameplay::ParticipantId participant_;
    cy::gameplay::ControlSourceId source_;
    cy::gameplay::CommandTypeId intent_command_ = cy::gameplay::kInvalidCommandType;
    cy::u32 producer_ = 0;

    Contract contract_;
    cy::ecs::Entity player_;
    cy::ecs::Entity level_;

    // Input.
    cy::input::DeviceId keyboard_;
    cy::input::ActionId action_move_ = cy::input::kInvalidAction;
    cy::input::ActionId action_look_ = cy::input::kInvalidAction;
    cy::input::ActionId action_jump_ = cy::input::kInvalidAction;
    cy::input::ActionId action_sprint_ = cy::input::kInvalidAction;
    cy::u32 script_cursor_ = 0;

    // Audio: two procedurally generated clips, and the buffer the null backend is pulled into.
    cy::Array<cy::f32> footstep_samples_;
    cy::Array<cy::f32> landing_samples_;
    cy::Array<cy::f32> mix_scratch_;
    cy::audio::ClipHandle footstep_clip_;
    cy::audio::ClipHandle landing_clip_;
    cy::audio::ListenerHandle listener_;

    // Camera.
    cy::camera::RigHandle rig_;
    cy::f32 previous_yaw_ = 0.0F;
    cy::f32 previous_pitch_ = 0.0F;
    bool camera_seen_ = false;

    // The cue counters as they were last tick. A counter that advanced is a sound.
    cy::f32 previous_footsteps_ = 0.0F;
    cy::f32 previous_landings_ = 0.0F;
    cy::f32 previous_jumps_ = 0.0F;

    cy::u64 tick_ = 0;
    bool started_ = false;
    bool position_seen_ = false;
    GameReport report_;
};

}  // namespace sample

#endif  // CY_SAMPLE_CHARACTER_GAME_H
