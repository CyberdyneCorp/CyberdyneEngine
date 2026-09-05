#include "game.h"

#include <cy/core/determinism/hash.h>
#include <cy/ecs/query.h>
#include <cy/servers/camera/view.h>
#include <cy/servers/physics/reference/server.h>

#include <cy_features.h>

#include <cmath>
#include <cstdio>
#include <cstring>

// Jolt is behind CY_PHYSICS and the reference backend is not. `physics` requires that swapping the
// backend change no gameplay, so the sample runs over either; a build without CY_PHYSICS simply has
// no second one to swap to, and `--jolt` then falls back with a line saying so.
#if defined(CY_PHYSICS)
#    include <cy/backends/physics/jolt/server.h>
#endif

namespace sample {
namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;

/// The fixed step. One place, because three subsystems are stepped with it and a sample that passed
/// the frame time to one of them would have a character whose stairs depend on the frame rate.
constexpr f32 kStep = 1.0F / 60.0F;
constexpr cy::Nanoseconds kStepNs = 16'666'666;

/// The audio device this sample asks for, and the block it is pulled in.
constexpr u32 kSampleRate = 48000;
constexpr u32 kBlockFrames = 200;
constexpr u32 kFramesPerTick = kSampleRate / 60;  // 800, an exact multiple of the block

/// THE LOOK SENSITIVITY IS A PLAYER SETTING, NOT A GAMEPLAY TUNABLE, and the distinction is the
/// reason it is allowed to be here. `camera-system` requires sensitivity, inversion and assistance
/// to be applied "in **one place**", and names that place as the input pipeline plus
/// `CameraSettings` — so this is the input pipeline applying it. The GAME's own multiplier on top
/// of it is `CameraDirector.yawScale`, in Swift, and a game that wanted a slower camera would
/// change that.
///
/// Radians per second at full deflection, before `CameraSettings::look_sensitivity`.
constexpr f32 kLookRadiansPerSecond = 2.4F;

/// Which bit of a command frame's masks each digital action takes. Assigned in declaration order by
/// `InputUser`, so this mirrors the order `start_input()` declares them in and nothing else.
constexpr u32 kJumpBit = 1U << 0U;
constexpr u32 kSprintBit = 1U << 1U;

// --- The scripted player
// --------------------------------------------------------------------------
//
// A timeline of key transitions, submitted into the input server through `submit()` — the same door
// `Sdl3InputSource` uses and the only one there is. It is not a shortcut past the input system: the
// events go through the event buffer, the bindings, the triggers, the per-tick resolution and the
// command frame exactly as a keyboard's would, and `--device-input` swaps the source for a real
// keyboard with nothing else changed.
//
// IT EXISTS BECAUSE A HEADLESS RUN HAS NO PLAYER. `just test-all` runs this sample with no window
// and no hands on it, and a smoke test of a character controller that nobody moves would be a smoke
// test of a character standing still.
//
// THE TAP AT TICK 250 IS THE MILESTONE'S OWN REQUIREMENT, IN THE ARTEFACT. Both edges are stamped
// inside one tick's window, so the key is already up when the tick resolves: design.md §5's "a
// button pressed and released between two ticks must still be observable as both a press and a
// release by the tick that follows". A level-sampling resolver would report no jump at all, the
// character would never leave the ground, and `GameReport::jumps` would be zero.
struct ScriptEvent {
    u64 tick;
    cy::input::Key key;
    bool down;
};

constexpr ScriptEvent kScript[] = {
    {20, cy::input::Key::W, true},            // forward, into the wall
    {170, cy::input::Key::W, false},          //
    {180, cy::input::Key::A, true},           // left, up the four 0.3 m steps
    {250, cy::input::Key::Space, true},       // the tap: both edges inside one tick window
    {250, cy::input::Key::Space, false},      //
    {340, cy::input::Key::A, false},          //
    {360, cy::input::Key::D, true},           // right, down the steps and into the 0.6 m one
    {620, cy::input::Key::D, false},          //
    {640, cy::input::Key::Left, true},        // turn the camera
    {700, cy::input::Key::Left, false},       //
    {700, cy::input::Key::W, true},           // and walk in the new direction
    {780, cy::input::Key::LeftShift, true},   // sprinting, which the game maps to a wider lens
    {860, cy::input::Key::LeftShift, false},  //
    {860, cy::input::Key::W, false},          //
};

// --- Small helpers
// ---------------------------------------------------------------------------------

[[nodiscard]] f32 horizontal_length(Vec3 value) noexcept {
    return std::sqrt((value.x * value.x) + (value.z * value.z));
}

[[nodiscard]] cy::input::Binding simple_binding(cy::input::ActionId action,
                                                cy::input::Control control,
                                                cy::input::TriggerKind trigger) noexcept {
    cy::input::Binding binding;
    binding.action = action;
    binding.kind = cy::input::BindingKind::Simple;
    binding.component_count = 1;
    binding.components[0].control = control;
    binding.components[0].weight = Vec3{1.0F, 0.0F, 0.0F};
    binding.trigger.kind = trigger;
    return binding;
}

/// Four keys as one two-dimensional axis: `input-and-actions`' "Keys become an axis".
[[nodiscard]] cy::input::Binding axis2_binding(cy::input::ActionId action, cy::input::Key left,
                                               cy::input::Key right, cy::input::Key down,
                                               cy::input::Key up) noexcept {
    using cy::input::key_control;
    cy::input::Binding binding;
    binding.action = action;
    binding.kind = cy::input::BindingKind::Axis2D;
    binding.component_count = 4;
    binding.components[0] = {key_control(left), Vec3{-1.0F, 0.0F, 0.0F}};
    binding.components[1] = {key_control(right), Vec3{1.0F, 0.0F, 0.0F}};
    binding.components[2] = {key_control(down), Vec3{0.0F, -1.0F, 0.0F}};
    binding.components[3] = {key_control(up), Vec3{0.0F, 1.0F, 0.0F}};
    binding.trigger.kind = cy::input::TriggerKind::Down;
    return binding;
}

/// A short decaying noise burst, generated rather than loaded.
///
/// M4 has no audio asset pipeline — `audio` at Seed plays float PCM the asset system owns, and
/// there is no asset system for sound yet — so the alternative to generating one is committing a
/// binary nobody can review. The generator is a fixed linear congruential sequence, so the clip is
/// the same on every machine and in every profile, which a recorded file would also have to be.
void make_burst(cy::Array<f32>& out, u32 frames, f32 decay, u32 seed) noexcept {
    if (!out.resize(frames)) {
        return;
    }
    u32 state = seed;
    for (u32 index = 0; index < frames; ++index) {
        state = (state * 1664525U) + 1013904223U;
        const f32 noise = (static_cast<f32>(state >> 9U) / 4194304.0F) - 1.0F;
        const f32 envelope = std::exp(-decay * static_cast<f32>(index) / static_cast<f32>(frames));
        out[index] = noise * envelope * 0.4F;
    }
}

void bridge_system(const cy::ecs::SystemContext& context) noexcept {
    static_cast<Game*>(context.user)->fixed_step();
}

}  // namespace

// --- Construction
// ------------------------------------------------------------------------------------

Game::Game(cy::Allocator& allocator, cy::ecs::World& world, const GameOptions& options) noexcept
    : allocator_(allocator),
      world_(world),
      options_(options),
      binding_(allocator, world),
      host_(allocator),
      runtime_(allocator, host_),
      manifest_text_(allocator),
      input_(allocator),
      audio_(allocator),
      camera_(allocator),
      session_(allocator, 0xC4A5AC7EULL),
      control_(allocator),
      commands_(allocator, control_),
      footstep_samples_(allocator),
      landing_samples_(allocator),
      mix_scratch_(allocator) {}

Game::~Game() {
    shutdown();
}

// --- Bring-up
// ------------------------------------------------------------------------------------------

cy::Status Game::start(const char** detail) noexcept {
    *detail = "";
    host_.bind_world(&binding_);

    // THE ORDER IS NOT INTERCHANGEABLE, and each dependency is one-directional:
    //   physics  before the level, which needs a world to put static bodies in;
    //   the module before the contract, because the module is what registers the components;
    //   the behaviours before `build_character`, because `CharacterSpec` is written in `onCreate`;
    //   `finalize_declarations` before any context is pushed, which `start_input` does internally.
    if (const cy::Status ready = start_physics(); !ready) {
        *detail = "physics";
        return ready;
    }
    if (const cy::Status ready = start_input(); !ready) {
        *detail = "input";
        return ready;
    }
    if (const cy::Status ready = start_audio(); !ready) {
        *detail = "audio";
        return ready;
    }
    if (const cy::Status ready = start_gameplay(); !ready) {
        *detail = "gameplay";
        return ready;
    }
    if (const cy::Status ready = load_module(detail); !ready) {
        return ready;
    }
    if (const cy::Status ready = create_behaviours(detail); !ready) {
        return ready;
    }
    if (const cy::Status ready = contract_.resolve(binding_, detail); !ready) {
        return ready;
    }
    if (const cy::Status ready = build_level(); !ready) {
        *detail = "level";
        return ready;
    }
    if (const cy::Status ready = build_character(); !ready) {
        *detail = "character";
        return ready;
    }
    if (const cy::Status ready = build_camera(); !ready) {
        *detail = "camera";
        return ready;
    }

    if (character_) {
        report_.start = character_->state().transform.translation;
        report_.end = report_.start;
        report_.highest_point = report_.start.y;
        report_.lowest_point = report_.start.y;
        position_seen_ = true;
    }
    started_ = true;
    return cy::ok();
}

cy::Status Game::start_physics() noexcept {
#if !defined(CY_PHYSICS)
    if (options_.jolt) {
        std::fprintf(stderr,
                     "04-character: this build has CY_PHYSICS off, so there is no Jolt backend to "
                     "run against; using the reference backend.\n");
        options_.jolt = false;
    }
#endif
    const cy::Expected<cy::physics::PhysicsServer*, cy::Error> made =
#if defined(CY_PHYSICS)
        options_.jolt ? cy::physics::jolt::create_server(allocator_, nullptr)
                      : cy::physics::reference::create_server(allocator_);
#else
        cy::physics::reference::create_server(allocator_);
#endif
    if (!made) {
        return cy::make_unexpected(made.error());
    }
    physics_ = made.value();
    if (const cy::Status ready = physics_->initialize(); !ready) {
        return ready;
    }
    report_.physics_backend = physics_->backend_name();

    cy::physics::WorldDescription description;
    description.name = cy::Name::intern("character");
    description.body_capacity = 64;
    description.body_pair_capacity = 256;
    description.contact_constraint_capacity = 256;
    const cy::Expected<cy::physics::WorldHandle, cy::Error> created =
        physics_->create_world(description);
    if (!created) {
        return cy::make_unexpected(created.error());
    }
    physics_world_ = created.value();
    return cy::ok();
}

cy::Status Game::start_input() noexcept {
    cy::input::InputServerConfig config;
    config.users = 1;
    config.event_capacity = 512;
    config.allow_synthetic = true;
    if (const cy::Status configured = input_.configure(config); !configured) {
        return configured;
    }
    if (const cy::Status ready = input_.initialize(); !ready) {
        return ready;
    }

    // DECLARATION ORDER IS THE COMMAND FRAME'S LAYOUT. `InputUser` assigns axis slots and button
    // bits in this order, which is why the two `k*Bit` constants above are next to nothing else.
    struct Declaration {
        const char* name;
        u32 stable;
        cy::input::ActionValueType type;
        f32 buffer_seconds;
        cy::input::ActionId* out;
    };
    const Declaration declarations[] = {
        {"Move", 1, cy::input::ActionValueType::Axis2, 0.0F, &action_move_},
        {"Look", 2, cy::input::ActionValueType::Axis2, 0.0F, &action_look_},
        // A buffer window, because a jump pressed a frame before landing is the ordinary case and
        // `input-and-actions` puts the mechanism here and the semantics in gameplay. The game
        // decides what to do with a buffered press; this is only how long it stays available.
        {"Jump", 3, cy::input::ActionValueType::Digital, 0.15F, &action_jump_},
        {"Sprint", 4, cy::input::ActionValueType::Digital, 0.0F, &action_sprint_},
    };
    for (const Declaration& entry : declarations) {
        cy::input::ActionDeclaration declaration;
        declaration.name = cy::Name::intern(entry.name);
        declaration.stable_id = cy::input::ActionStableId{entry.stable};
        declaration.type = entry.type;
        declaration.buffer_window_seconds = entry.buffer_seconds;
        declaration.in_command_frame = true;
        const cy::Expected<cy::input::ActionId, cy::Error> declared =
            input_.actions().declare(declaration);
        if (!declared) {
            return cy::make_unexpected(declared.error());
        }
        *entry.out = declared.value();
    }

    using cy::input::Key;
    cy::input::MappingContext context(allocator_);
    context.set_name(cy::Name::intern("gameplay"));
    if (const cy::Status added =
            context.add(axis2_binding(action_move_, Key::A, Key::D, Key::S, Key::W));
        !added) {
        return added;
    }
    if (const cy::Status added =
            context.add(axis2_binding(action_look_, Key::Left, Key::Right, Key::Down, Key::Up));
        !added) {
        return added;
    }
    if (const cy::Status added = context.add(
            simple_binding(action_jump_, key_control(Key::Space), cy::input::TriggerKind::Pressed));
        !added) {
        return added;
    }
    if (const cy::Status added = context.add(simple_binding(
            action_sprint_, key_control(Key::LeftShift), cy::input::TriggerKind::Down));
        !added) {
        return added;
    }

    // MUST FOLLOW THE DECLARATIONS AND PRECEDE THE PUSH: it is what sizes every user's per-action
    // records, and it is the one easy step to forget. See src/servers/input/README.md.
    if (const cy::Status sized = input_.finalize_declarations(); !sized) {
        return sized;
    }
    const cy::Expected<cy::input::ContextHandle, cy::Error> registered =
        input_.register_context(std::move(context));
    if (!registered) {
        return cy::make_unexpected(registered.error());
    }
    // The push returns the stack entry's index, which nothing here needs — a context is popped by
    // handle and never by position, which is what `input-and-actions` requires and why the index is
    // discarded rather than kept.
    if (const cy::Expected<cy::u32, cy::Error> pushed =
            input_.user(0).push_context(registered.value(), 0, cy::input::FocusLayer::Gameplay);
        !pushed) {
        return cy::make_unexpected(pushed.error());
    }

    // The scripted keyboard. A real one arrives through `Sdl3InputSource`, which main.cpp attaches
    // and then assigns HERE-equivalent — assignment is always explicit, which is the requirement.
    if (!options_.device_input) {
        cy::input::DeviceDescription description;
        description.kind = cy::input::DeviceKind::Keyboard;
        description.hardware_id = cy::Name::intern("scripted-keyboard");
        description.display_name = description.hardware_id;
        const cy::Expected<cy::input::DeviceId, cy::Error> connected =
            input_.devices().connect(description, 0);
        if (!connected) {
            return cy::make_unexpected(connected.error());
        }
        keyboard_ = connected.value();
        if (const cy::Status assigned = input_.assign(keyboard_, 0, 0); !assigned) {
            return assigned;
        }
    }
    return cy::ok();
}

cy::Status Game::start_audio() noexcept {
    cy::audio::AudioBackendConfig backend_config;
    backend_config.requested.sample_rate = kSampleRate;
    backend_config.requested.layout = cy::audio::ChannelLayout::Stereo;
    backend_config.requested.buffer_frames = kBlockFrames;
    if (const cy::Status ready = audio_backend_.initialize(backend_config); !ready) {
        return ready;
    }

    cy::audio::AudioServerConfig config;
    config.requested = backend_config.requested;
    config.block_frames = kBlockFrames;
    config.voice_capacity = 32;
    if (const cy::Status configured = audio_.configure(config); !configured) {
        return configured;
    }
    if (const cy::Status ready = audio_.initialize_with(audio_backend_); !ready) {
        return ready;
    }

    make_burst(footstep_samples_, kSampleRate / 12, 9.0F, 0x5EED0001U);
    make_burst(landing_samples_, kSampleRate / 6, 5.0F, 0x5EED0002U);
    if (!mix_scratch_.resize(static_cast<cy::usize>(kFramesPerTick) * 2U)) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the mix scratch buffer was refused");
    }

    cy::audio::ClipDescription footstep;
    footstep.name = cy::Name::intern("footstep");
    footstep.samples = footstep_samples_.data();
    footstep.frame_count = static_cast<u32>(footstep_samples_.size());
    footstep.channels = 1;
    footstep.sample_rate = kSampleRate;
    const cy::Expected<cy::audio::ClipHandle, cy::Error> made_footstep =
        audio_.create_clip(footstep);
    if (!made_footstep) {
        return cy::make_unexpected(made_footstep.error());
    }
    footstep_clip_ = made_footstep.value();

    cy::audio::ClipDescription landing = footstep;
    landing.name = cy::Name::intern("landing");
    landing.samples = landing_samples_.data();
    landing.frame_count = static_cast<u32>(landing_samples_.size());
    const cy::Expected<cy::audio::ClipHandle, cy::Error> made_landing = audio_.create_clip(landing);
    if (!made_landing) {
        return cy::make_unexpected(made_landing.error());
    }
    landing_clip_ = made_landing.value();

    const cy::Expected<cy::audio::ListenerHandle, cy::Error> made_listener =
        audio_.create_listener(cy::audio::Listener{});
    if (!made_listener) {
        return cy::make_unexpected(made_listener.error());
    }
    listener_ = made_listener.value();
    return cy::ok();
}

cy::Status Game::start_gameplay() noexcept {
    const cy::Expected<cy::gameplay::ParticipantId, cy::Error> added = session_.add_participant(
        cy::gameplay::ParticipantKind::LocalHuman, cy::Name::intern("player"), 0, 0);
    if (!added) {
        return cy::make_unexpected(added.error());
    }
    participant_ = added.value();

    const cy::Expected<cy::gameplay::ControlSourceId, cy::Error> created = control_.create_source(
        cy::gameplay::ControlSourceKind::Human, participant_, cy::Name::intern("local-input"));
    if (!created) {
        return cy::make_unexpected(created.error());
    }
    source_ = created.value();

    cy::gameplay::CommandDeclaration declaration;
    declaration.name = cy::Name::intern("CharacterIntent");
    declaration.stable_id = 1;
    declaration.schema_version = 1;
    declaration.predictable = true;
    declaration.reliability = cy::gameplay::Reliability::Unreliable;
    declaration.channel = cy::gameplay::channels::movement();
    const cy::Expected<cy::gameplay::CommandTypeId, cy::Error> declared =
        commands_.declare(declaration);
    if (!declared) {
        return cy::make_unexpected(declared.error());
    }
    intent_command_ = declared.value();

    const cy::Expected<u32, cy::Error> opened =
        commands_.open_producer(cy::Name::intern("local-input"));
    if (!opened) {
        return cy::make_unexpected(opened.error());
    }
    producer_ = opened.value();
    return cy::ok();
}

cy::Status Game::load_module(const char** detail) noexcept {
    *detail = options_.module_manifest;
    std::FILE* file = std::fopen(options_.module_manifest, "rb");
    if (file == nullptr) {
        return cy::fail(cy::ErrorCode::NotFound, "the module manifest could not be opened");
    }
    // SIZED FROM THE FILE, NOT FROM A BUFFER. The first version read into a 1 KiB stack array and
    // silently truncated this manifest's comment block, so the parser never reached `name` and
    // reported "module.toml must declare a name" about a file whose first line is exactly that. A
    // fixed buffer here is a bug that only appears once somebody writes a long enough comment.
    // One exit, and the file is closed exactly once on every path. Written as a flag rather than as
    // early returns because each early return would close the handle in a different branch, which
    // is how a leak on the fourth path gets written.
    long length = 0;
    bool measured = std::fseek(file, 0, SEEK_END) == 0;
    if (measured) {
        length = std::ftell(file);
        measured = length >= 0 && std::fseek(file, 0, SEEK_SET) == 0;
    }
    if (measured) {
        measured = manifest_text_.resize(static_cast<cy::usize>(length) + 1).has_value();
    }
    if (measured) {
        const auto size = static_cast<cy::usize>(length);
        const cy::usize read = std::fread(manifest_text_.data(), 1, size, file);
        manifest_text_[read] = '\0';
        measured = read == size;
    }
    (void)std::fclose(file);
    if (!measured) {
        return cy::fail(cy::ErrorCode::Io, "the module manifest could not be read");
    }

    const cy::Expected<cy::abi::ModuleManifest, cy::Error> parsed =
        cy::abi::parse_module_manifest(manifest_text_.data());
    if (!parsed) {
        return cy::make_unexpected(parsed.error());
    }
    manifest_ = parsed.value();

    *detail = options_.module_library;
    const cy::Expected<cy::abi::ReloadReport, cy::Error> loaded =
        runtime_.load(manifest_, options_.module_library);
    if (!loaded) {
        return cy::make_unexpected(loaded.error());
    }
    *detail = "";
    return cy::ok();
}

cy::Status Game::create_behaviours(const char** detail) noexcept {
    const cy::Expected<cy::ecs::Entity, cy::Error> level = world_.create();
    if (!level) {
        return cy::make_unexpected(level.error());
    }
    level_ = level.value();
    const cy::Expected<cy::ecs::Entity, cy::Error> player = world_.create();
    if (!player) {
        return cy::make_unexpected(player.error());
    }
    player_ = player.value();

    // THE CONTROL BINDING, AND IT IS WHY THIS IS NOT IN `start_gameplay`. `CommandStream::validate`
    // asks whether this source controls this entity on this channel, structurally and before any
    // game rule runs — so the binding needs the entity, and the entity does not exist until the
    // module has been loaded and the world has one to give. Without it every command is rejected,
    // every tick, and the game reads an input that never arrives: measured, on the first run of
    // this sample, as `committed=0 rejected=900` and a character that stood still.
    if (const cy::Status bound =
            control_.bind_entity(source_, cy::gameplay::channels::movement(), player_);
        !bound) {
        *detail = "control binding";
        return bound;
    }

    // THE LEVEL IS CREATED EVEN WITH `--no-behaviours`. The negative control is a run with no
    // DECISIONS in it, not a run with no world: leaving the level out would make "the character did
    // not move" true for the wrong reason, which is exactly how a control stops controlling
    // anything.
    struct Creation {
        const char* type;
        cy::ecs::Entity* entity;
        bool decides;
    };
    const Creation creations[] = {
        {"Level", &level_, false},
        // BEFORE `Character`, and the order is load-bearing. Instances are updated in creation
        // order, and the character reads this tick's camera yaw to decide which way "forward" is;
        // created the other way round it would move in last tick's frame, which looks like input
        // lag and is not. game/CameraDirector.swift states the same thing from the other side.
        {"CameraDirector", &player_, true},
        {"Character", &player_, true},
    };
    for (const Creation& creation : creations) {
        if (creation.decides && !options_.behaviours) {
            continue;
        }
        *detail = creation.type;
        const cy::Expected<u32, cy::Error> made =
            runtime_.create(creation.type, cy::abi::to_abi(*creation.entity));
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        ++report_.behaviours;
    }
    *detail = "";
    return cy::ok();
}

cy::Status Game::build_level() noexcept {
    cy::ecs::QueryDesc desc(allocator_);
    if (const cy::Status declared = desc.read(contract_.level.type); !declared) {
        return declared;
    }
    cy::ecs::Query query(world_, std::move(desc));

    const cy::physics::CollisionFilter filter;
    cy::Status outcome = cy::ok();
    u32 built = 0;
    cy::Status walked = query.for_each_chunk([&](cy::ecs::QueryChunk& chunk) noexcept {
        for (const cy::ecs::Entity entity : chunk.entities()) {
            if (!outcome) {
                return;
            }
            const Vec3 center = read_vec3(world_, entity, contract_.level, contract_.level.center);
            const Vec3 half =
                read_vec3(world_, entity, contract_.level, contract_.level.half_extents);

            cy::physics::ShapeDescription shape;
            shape.type = cy::physics::ShapeType::Box;
            shape.half_extents = half;
            const cy::Expected<cy::physics::ShapeHandle, cy::Error> made =
                physics_->create_shape(shape);
            if (!made) {
                outcome = cy::make_unexpected(made.error());
                return;
            }

            cy::physics::ColliderDescription collider;
            collider.shape = made.value();
            collider.filter = filter;
            cy::physics::BodyDescription body;
            body.motion = cy::physics::MotionType::Static;
            body.transform = cy::Transform::from_translation(center);
            body.colliders = &collider;
            body.collider_count = 1;
            body.user_data = static_cast<cy::physics::UserData>(entity.bits());
            const cy::Expected<cy::physics::BodyHandle, cy::Error> created =
                physics_->create_body(physics_world_, body);
            if (!created) {
                outcome = cy::make_unexpected(created.error());
                return;
            }
            ++built;
        }
    });
    if (!walked) {
        return walked;
    }
    if (!outcome) {
        return outcome;
    }
    if (built == 0) {
        return cy::fail(cy::ErrorCode::NotFound, "the game module declared no level geometry");
    }
    report_.level_bodies = built;
    return cy::ok();
}

cy::Status Game::build_character() noexcept {
    // Every number here comes out of `CharacterSpec`, which game/Character.swift wrote in its
    // `onCreate`. With `--no-behaviours` the component is absent, the reads answer zero, and
    // `validate()` refuses the description — so the control's character stands at the origin with
    // no capsule rather than silently getting the engine's defaults, which would be the host
    // quietly supplying the game's numbers. NO SPEC, NO CHARACTER. With `--no-behaviours` nothing
    // described a capsule, and the host does not have one to fall back on: a default capsule at the
    // origin would be the host inventing the character's size and spawn, which is the whole thing
    // this sample claims it never does. So the control run has no body at all, and that is the
    // honest answer rather than a degraded one.
    if (world_.get(player_, contract_.spec.type) == nullptr) {
        return cy::ok();
    }

    cy::physics::CharacterDescription description;
    description.start = cy::Transform::from_translation(
        read_vec3(world_, player_, contract_.spec, contract_.spec.spawn));
    description.radius = read_f32(world_, player_, contract_.spec, contract_.spec.radius);
    description.height = read_f32(world_, player_, contract_.spec, contract_.spec.height);
    description.step_offset = read_f32(world_, player_, contract_.spec, contract_.spec.step_offset);
    description.max_slope_radians =
        read_f32(world_, player_, contract_.spec, contract_.spec.max_slope_radians);
    if (const cy::Status valid = cy::physics::validate(description); !valid) {
        return valid;
    }

    cy::Expected<cy::UniquePtr<cy::physics::CharacterController>, cy::Error> made =
        cy::make_unique<cy::physics::CharacterController>(allocator_, *physics_, physics_world_);
    if (!made) {
        return cy::make_unexpected(made.error());
    }
    character_ = std::move(made.value());
    return character_->create(description);
}

cy::Status Game::build_camera() noexcept {
    cy::camera::CameraServerConfig config;
    if (const cy::Status configured = camera_.configure(config); !configured) {
        return configured;
    }
    if (const cy::Status ready = camera_.initialize(); !ready) {
        return ready;
    }

    // THE HOST CHOOSES THE RIG'S SHAPE AND THE GAME CHOOSES ITS NUMBERS. A third-person camera is
    // five nodes in this order in every game that has one — resolve the target, orbit it, offset to
    // the shoulder, look back at it, set the lens — and which five is an engine's answer. How far
    // back, how high, how fast it catches up and how wide the lens is are not, so every one of them
    // is read out of `CameraSpec`.
    const bool specified = world_.get(player_, contract_.camera_spec.type) != nullptr;
    const auto spec = [&](FieldSlot field) noexcept {
        return specified ? read_f32(world_, player_, contract_.camera_spec, field) : 0.0F;
    };

    cy::camera::RigDefinition definition(allocator_);
    definition.name = cy::Name::intern("third-person");

    cy::camera::RigNodeDesc target;
    target.id = cy::Name::intern("target");
    target.kind = cy::camera::RigNodeKind::Target;
    target.target.anchor_half_life = spec(contract_.camera_spec.position_half_life);
    if (!definition.nodes.push_back(target)) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the rig definition was refused");
    }

    cy::camera::RigNodeDesc orbit;
    orbit.id = cy::Name::intern("orbit");
    orbit.input = target.id;
    orbit.kind = cy::camera::RigNodeKind::Orbit;
    orbit.orbit.near_distance = spec(contract_.camera_spec.near_distance);
    orbit.orbit.far_distance = spec(contract_.camera_spec.far_distance);
    orbit.orbit.distance_half_life = spec(contract_.camera_spec.position_half_life);
    if (!definition.nodes.push_back(orbit)) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the rig definition was refused");
    }

    cy::camera::RigNodeDesc offset;
    offset.id = cy::Name::intern("shoulder");
    offset.input = orbit.id;
    offset.kind = cy::camera::RigNodeKind::Offset;
    offset.offset.offset =
        specified ? read_vec3(world_, player_, contract_.camera_spec, contract_.camera_spec.offset)
                  : Vec3{};
    if (!definition.nodes.push_back(offset)) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the rig definition was refused");
    }

    cy::camera::RigNodeDesc look;
    look.id = cy::Name::intern("look");
    look.input = offset.id;
    look.kind = cy::camera::RigNodeKind::LookAt;
    look.look_at.rotation_half_life = spec(contract_.camera_spec.rotation_half_life);
    if (!definition.nodes.push_back(look)) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the rig definition was refused");
    }

    cy::camera::RigNodeDesc lens;
    lens.id = cy::Name::intern("lens");
    lens.input = look.id;
    lens.kind = cy::camera::RigNodeKind::Lens;
    lens.lens.near_value = spec(contract_.camera_spec.near_field_of_view);
    lens.lens.far_value = spec(contract_.camera_spec.far_field_of_view);
    if (!definition.nodes.push_back(lens)) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the rig definition was refused");
    }

    cy::camera::RigNodeDesc output;
    output.id = cy::Name::intern("output");
    output.input = lens.id;
    output.kind = cy::camera::RigNodeKind::Output;
    if (!definition.nodes.push_back(output)) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the rig definition was refused");
    }

    const cy::Expected<cy::camera::DefinitionHandle, cy::Error> compiled =
        camera_.create_definition(definition);
    if (!compiled) {
        return cy::make_unexpected(compiled.error());
    }

    cy::camera::RigConfig rig_config;
    rig_config.name = cy::Name::intern("third-person");
    rig_config.mode = cy::camera::EvaluationMode::Simulation;
    const cy::Expected<cy::camera::RigHandle, cy::Error> rig =
        camera_.create_rig(compiled.value(), rig_config);
    if (!rig) {
        return cy::make_unexpected(rig.error());
    }
    rig_ = rig.value();

    // The target is bound by stable id and the host resolves it every tick from
    // `CameraIntent::focus` — which is the game's decision about what to frame. `camera-system`
    // requires the binding to be independent of control, and here it literally is: nothing connects
    // this id to the player entity except the line in `drive_camera` that fills the sample.
    cy::camera::TargetBinding binding;
    binding.kind = cy::camera::TargetKind::Entity;
    binding.stable_id = 1;
    return camera_.set_target(rig_, binding);
}

cy::Status Game::install(cy::ecs::Schedule& schedule) noexcept {
    // The access this system takes, declared rather than derived, because the components are the
    // module's and there is no query to read it off. Every one of them is written by `fixed_step`.
    cy::jobs::AccessSet access;
    const Component* written[] = {&contract_.input, &contract_.state, &contract_.drive,
                                  &contract_.cue, &contract_.camera_intent};
    for (const Component* component : written) {
        if (const cy::Status declared = access.write(component->type); !declared) {
            return declared;
        }
    }

    cy::ecs::SystemDesc desc;
    desc.name = "character.bridge";
    desc.body = &bridge_system;
    desc.user = this;
    desc.access = access;
    const cy::Expected<cy::ecs::SystemId, cy::Error> added =
        schedule.add(cy::ecs::Stage::Simulation, desc);
    if (!added) {
        return cy::make_unexpected(added.error());
    }
    return cy::ok();
}

// --- One tick
// ------------------------------------------------------------------------------------------

void Game::fixed_step() noexcept {
    if (!started_) {
        return;
    }
    ++tick_;
    const cy::Nanoseconds now = static_cast<cy::Nanoseconds>(tick_) * kStepNs;

    //  1. the player, real or scripted, and the same door either way
    inject_script(now);
    //  2. resolve the accumulated events into this tick's actions and command frame
    input_.resolve_tick(tick_, now, kStep);
    const cy::input::CommandFrame& frame = input_.user(0).command_frame();
    report_.input_hash = cy::determinism::fold_hash(report_.input_hash, frame.hash());

    //  3. record it as a command, and 4. commit it — the simulation's only input
    IntentPayload payload;
    payload.move_x = frame.axes[0].x;
    payload.move_z = frame.axes[0].y;
    const f32 look_scale = kLookRadiansPerSecond * camera_.settings().look_sensitivity * kStep;
    payload.look_yaw = frame.axes[1].x * look_scale;
    payload.look_pitch = frame.axes[1].y * look_scale;
    payload.pressed = frame.pressed;
    payload.just_pressed = frame.just_pressed;

    cy::gameplay::Command command;
    command.type = intent_command_;
    command.participant = participant_;
    command.source = source_;
    command.target = player_;
    (void)command.set_payload(payload);
    (void)commands_.producer(producer_).record(command);

    cy::gameplay::GameplayContext context;
    context.world = &world_;
    context.session = &session_;
    context.services = &session_.services();
    context.commands = &commands_;
    context.at.tick = tick_;
    commands_.commit(context, tick_);
    report_.commands_committed += commands_.committed_count();
    report_.commands_rejected += commands_.rejection_count();

    //  5. apply what was committed — and nothing else — into what the game reads
    for (u32 index = 0; index < commands_.committed_count(); ++index) {
        IntentPayload applied;
        if (commands_.committed(index).read_payload(applied)) {
            publish_input(applied);
        }
    }

    //  6. the game: every decision this program makes happens inside this call
    runtime_.fixed_update(kStep);

    //  7. and 8. the world, then the character against it
    cy::physics::StepInput step;
    step.delta_seconds = kStep;
    step.tick = tick_;
    (void)physics_->step(physics_world_, step);
    drive_character(kStep);

    //  9. what the character did, back into what the game reads next tick
    publish_state();
    // 10. what it should sound like
    play_cues();
    // 11. where the camera should be, and the view a renderer would draw
    drive_camera(kStep);

    audio_.update(kStep);
    report_.audio_frames += audio_backend_.advance(mix_scratch_.data(), kFramesPerTick);
    report_.ticks = tick_;

    if (options_.verbose && character_) {
        const Vec3 position = character_->state().transform.translation;
        std::fprintf(stdout, "  tick %6llu  pos %7.2f %6.2f %7.2f  %s  steps %u\n",
                     static_cast<unsigned long long>(tick_), static_cast<double>(position.x),
                     static_cast<double>(position.y), static_cast<double>(position.z),
                     cy::physics::ground_state_name(character_->state().ground), report_.footsteps);
    }
}

void Game::inject_script(cy::Nanoseconds now) noexcept {
    if (options_.device_input || keyboard_.is_null()) {
        return;
    }
    // Stamped inside the window that is about to be resolved, not at its edge: the tap at tick 250
    // is two transitions of one key and both must land in the same window for the claim to be the
    // one design.md §5 makes.
    const cy::Nanoseconds inside = now - (kStepNs / 2);
    while (script_cursor_ < (sizeof(kScript) / sizeof(kScript[0])) &&
           kScript[script_cursor_].tick <= tick_) {
        const ScriptEvent& event = kScript[script_cursor_];
        cy::input::DeviceEvent device_event;
        device_event.timestamp = inside;
        device_event.device = keyboard_;
        device_event.control = key_control(event.key);
        device_event.value = event.down ? 1.0F : 0.0F;
        device_event.source = cy::input::EventSource::Synthetic;
        input_.submit(device_event);
        ++script_cursor_;
    }
}

void Game::publish_input(const IntentPayload& intent) noexcept {
    // THE ARGUMENT IS THE COMMITTED COMMAND'S PAYLOAD, NOT THE COMMAND FRAME, and the difference is
    // the whole of design.md §3. Reading the frame here would work, produce the same numbers today,
    // and quietly make the log incomplete: a command the validation rejected would still reach the
    // game, and a replay of the log would then diverge from the run it was recorded from. That is
    // the second input path the requirement forbids, and it is one identifier wide.
    write_vec3(world_, player_, contract_.input, contract_.input.move,
               Vec3{intent.move_x, 0.0F, intent.move_z});
    write_vec3(world_, player_, contract_.input, contract_.input.look,
               Vec3{intent.look_yaw, intent.look_pitch, 0.0F});
    write_f32(world_, player_, contract_.input, contract_.input.jump,
              (intent.just_pressed & kJumpBit) != 0 ? 1.0F : 0.0F);
    write_f32(world_, player_, contract_.input, contract_.input.sprint,
              (intent.pressed & kSprintBit) != 0 ? 1.0F : 0.0F);
}

void Game::drive_character(f32 step) noexcept {
    if (!character_) {
        return;
    }
    cy::physics::CharacterInput input;
    input.desired_velocity = read_vec3(world_, player_, contract_.drive, contract_.drive.velocity);
    input.jump = read_f32(world_, player_, contract_.drive, contract_.drive.jump) > 0.5F;
    input.jump_speed = read_f32(world_, player_, contract_.drive, contract_.drive.jump_speed);
    (void)character_->move(step, input);
}

void Game::publish_state() noexcept {
    if (!character_) {
        return;
    }
    const cy::physics::CharacterState& state = character_->state();
    const Vec3 position = state.transform.translation;
    write_vec3(world_, player_, contract_.state, contract_.state.position, position);
    write_vec3(world_, player_, contract_.state, contract_.state.velocity, state.velocity);
    write_f32(world_, player_, contract_.state, contract_.state.grounded,
              state.ground == cy::physics::GroundState::Grounded ? 1.0F : 0.0F);
    write_f32(world_, player_, contract_.state, contract_.state.speed,
              horizontal_length(state.velocity));

    if (position_seen_) {
        report_.distance_travelled += cy::distance(position, report_.end);
    }
    report_.end = position;
    report_.highest_point = position.y > report_.highest_point ? position.y : report_.highest_point;
    report_.lowest_point = position.y < report_.lowest_point ? position.y : report_.lowest_point;
    if (state.ground == cy::physics::GroundState::Grounded) {
        ++report_.grounded_ticks;
    } else {
        ++report_.airborne_ticks;
    }
    report_.stepped_up_ticks += state.stepped_up ? 1U : 0U;
    report_.wall_ticks += state.touching_wall ? 1U : 0U;
}

void Game::play_cues() noexcept {
    const Vec3 position =
        character_ ? character_->state().transform.translation : Vec3{0.0F, 0.0F, 0.0F};
    struct Cue {
        FieldSlot field;
        f32* previous = nullptr;
        cy::audio::ClipHandle clip;
        u32* count = nullptr;
        f32 volume = 1.0F;
    };
    const Cue cues[] = {
        {contract_.cue.footsteps, &previous_footsteps_, footstep_clip_, &report_.footsteps, 0.6F},
        {contract_.cue.landings, &previous_landings_, landing_clip_, &report_.landings, 1.0F},
        {contract_.cue.jumps, &previous_jumps_, footstep_clip_, &report_.jumps, 0.5F},
    };
    for (const Cue& cue : cues) {
        const f32 now = read_f32(world_, player_, contract_.cue, cue.field);
        // A COUNTER, NOT A FLAG, so a frame holding several ticks cannot lose one. The loop plays
        // as many as the counter advanced by, which for a footstep is always one and for a
        // teleporting character need not be.
        while (*cue.previous + 0.5F < now) {
            cy::audio::VoiceDescription voice;
            voice.clip = cue.clip;
            voice.volume = cue.volume;
            voice.spatialised = true;
            voice.position = position;
            voice.one_shot = true;
            voice.pitch_variation = 0.08F;
            if (audio_.play_one_shot(voice)) {
                ++report_.voices_started;
            }
            *cue.previous += 1.0F;
            ++(*cue.count);
        }
        *cue.previous = now;
    }
}

void Game::drive_camera(f32 step) noexcept {
    const Vec3 focus =
        read_vec3(world_, player_, contract_.camera_intent, contract_.camera_intent.focus);
    const f32 yaw = read_f32(world_, player_, contract_.camera_intent, contract_.camera_intent.yaw);
    const f32 pitch =
        read_f32(world_, player_, contract_.camera_intent, contract_.camera_intent.pitch);
    const f32 zoom =
        read_f32(world_, player_, contract_.camera_intent, contract_.camera_intent.zoom);

    // THE INTENT IS A DELTA AND THE GAME'S YAW IS ABSOLUTE, so what crosses is the difference. The
    // orbit node integrates look intent; the game integrates the same look intent into the number
    // the character steers by. Feeding the node the change keeps the two integrators equal by
    // construction rather than by two pieces of code agreeing about clamping and wrapping.
    cy::camera::CameraIntent intent;
    intent.look = cy::Vec2{yaw - previous_yaw_, pitch - previous_pitch_};
    intent.zoom = zoom;
    intent.zoom_set = true;
    previous_yaw_ = yaw;
    previous_pitch_ = pitch;
    (void)camera_.apply_intent(rig_, intent);

    cy::camera::TargetSample sample;
    sample.stable_id = 1;
    sample.valid = true;
    sample.transform.translation = focus;

    cy::camera::EvaluationContext context;
    context.delta_seconds = step;
    context.tick = tick_;
    context.simulation = true;
    context.aspect = 16.0F / 9.0F;
    context.targets = cy::Span<const cy::camera::TargetSample>(&sample, 1);
    if (character_) {
        context.controlled = character_->state().transform;
        context.controlled_velocity = character_->state().velocity;
    }

    camera_.begin_frame();
    const cy::Expected<const cy::camera::EvaluatedCamera*, cy::Error> evaluated =
        camera_.evaluate(rig_, context);
    if (!evaluated) {
        return;
    }
    const cy::camera::EvaluatedCamera& camera = *evaluated.value();

    // The frame a renderer would draw. `camera-system`'s "Render view production" is the seam M3's
    // renderer consumes, and producing the description is what proves the seam is joined even in a
    // run that draws nothing — see README.md on why this sample has no window.
    cy::camera::RenderViewRequest request;
    request.name = cy::Name::intern("primary");
    request.viewport.width = 1920;
    request.viewport.height = 1080;
    cy::render::ViewDescription view;
    if (cy::camera::produce_view(camera, request, view)) {
        ++report_.views_produced;
    }

    // The listener follows the camera, which is `ListenerPolicy::AtCamera` — the rig's default and
    // the right one for a third-person game, where a sound is heard from where it is seen.
    cy::audio::Listener listener;
    listener.transform = camera.pose;
    listener.velocity = camera.velocity;
    (void)audio_.set_listener(listener_, listener);

    if (camera_seen_) {
        report_.camera_travelled += cy::distance(camera.pose.translation, report_.camera_end);
    } else {
        report_.camera_start = camera.pose.translation;
        camera_seen_ = true;
    }
    report_.camera_end = camera.pose.translation;
}

// --- Teardown
// ------------------------------------------------------------------------------------------

void Game::shutdown() noexcept {
    if (physics_ == nullptr) {
        return;
    }
    report_.command_log_hash = commands_.log().hash();

    // The controller owns a shape and a body in the world, so it goes before the server does. It is
    // a `UniquePtr` rather than a member for exactly this: a member would be destroyed after the
    // pointer it holds had been freed.
    character_.reset();
    physics_->shutdown();
#if defined(CY_PHYSICS)
    if (options_.jolt) {
        cy::physics::jolt::destroy_server(physics_, allocator_);
    } else {
        cy::physics::reference::destroy_server(physics_, allocator_);
    }
#else
    cy::physics::reference::destroy_server(physics_, allocator_);
#endif
    physics_ = nullptr;

    audio_.shutdown();
    audio_backend_.shutdown();
    camera_.shutdown();
    input_.shutdown();
    started_ = false;
}

}  // namespace sample
