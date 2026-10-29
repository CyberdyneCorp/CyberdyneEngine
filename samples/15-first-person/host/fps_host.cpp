// SPDX-License-Identifier: MIT
// fps_host.cpp — the engine side of samples/15-first-person. See fps_host.h.

#include "fps_host.h"

#include <cy/backends/physics/jolt/server.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/servers/camera/first_person.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sample::fps {
namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;

constexpr f32 kStep = 1.0F / 60.0F;
constexpr cy::Nanoseconds kStepNs = 16'666'666;
constexpr u32 kSampleRate = 48000;
constexpr u32 kBlockFrames = 200;
constexpr u32 kFramesPerTick = kSampleRate / 60;  // 800, a whole number of blocks
constexpr u32 kViewportWidth = 1280;
constexpr u32 kViewportHeight = 720;
/// The pools the game spawns into.
constexpr u32 kLightCapacity = 32;

[[nodiscard]] cy::Status read_file(const char* path, cy::Array<char>& out) noexcept {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return cy::fail(cy::ErrorCode::NotFound, "the module manifest could not be opened");
    }
    bool read = std::fseek(file, 0, SEEK_END) == 0;
    const long length = read ? std::ftell(file) : -1;
    read = length >= 0 && std::fseek(file, 0, SEEK_SET) == 0 &&
           out.resize(static_cast<cy::usize>(length) + 1U).has_value();
    if (read) {
        const auto size = static_cast<cy::usize>(length);
        read = std::fread(out.data(), 1, size, file) == size;
        out[size] = '\0';
    }
    (void)std::fclose(file);
    return read ? cy::ok() : cy::fail(cy::ErrorCode::Io, "the module manifest could not be read");
}

[[nodiscard]] cy::Expected<cy::input::DeviceId, cy::Error> connect(cy::input::InputServer& input,
                                                                   cy::input::DeviceKind kind,
                                                                   const char* name) noexcept {
    cy::input::DeviceDescription description;
    description.kind = kind;
    description.hardware_id = cy::Name::intern(name);
    description.display_name = description.hardware_id;
    auto connected = input.devices().connect(description, 0);
    if (!connected) {
        return connected;
    }
    if (cy::Status assigned = input.assign(*connected, 0, 0); !assigned) {
        return cy::make_unexpected(assigned.error());
    }
    return connected;
}

[[nodiscard]] cy::camera::RigNodeDesc rig_node(const char* id, const char* input,
                                               cy::camera::RigNodeKind kind) noexcept {
    cy::camera::RigNodeDesc desc;
    desc.id = cy::Name::intern(id);
    desc.input = input == nullptr ? cy::Name{} : cy::Name::intern(input);
    desc.kind = kind;
    return desc;
}

/// The first-person rig: a target and an output. Where the eye is, and which way it looks, is the
/// game's (`Camera.overridePose`) and the first-person behaviours'.
[[nodiscard]] cy::camera::RigDefinition fps_rig(cy::Allocator& allocator) noexcept {
    using cy::camera::RigNodeKind;
    cy::camera::RigDefinition definition(allocator);
    definition.name = cy::Name::intern("first_person");
    (void)definition.nodes.push_back(rig_node("target", nullptr, RigNodeKind::Target));
    (void)definition.nodes.push_back(rig_node("output", "target", RigNodeKind::Output));
    return definition;
}

/// A field of a component the game registered, by name, or null.
[[nodiscard]] const cy::abi::FieldRecord* field_of(const cy::abi::World& binding,
                                                   const cy::abi::ComponentRecord& record,
                                                   const char* name) noexcept {
    for (u32 index = 0; index < record.field_count; ++index) {
        const cy::abi::FieldRecord* field = binding.field(record, index);
        if (field != nullptr && std::strcmp(field->name, name) == 0) {
            return field;
        }
    }
    return nullptr;
}

template <class T>
[[nodiscard]] T read_field(const void* bytes, const cy::abi::FieldRecord* field) noexcept {
    T value{};
    if (bytes != nullptr && field != nullptr) {
        std::memcpy(&value, static_cast<const unsigned char*>(bytes) + field->offset, sizeof(T));
    }
    return value;
}

[[nodiscard]] u64 fold(u64 digest, u64 value) noexcept {
    for (u32 byte = 0; byte < 8U; ++byte) {
        digest ^= (value >> (byte * 8U)) & 0xFFULL;
        digest *= 0x100000001B3ULL;
    }
    return digest;
}

/// `now` carried forward by `ahead` of the step that took `before` to it: where a moving thing is
/// at a frame drawn between two ticks. Its turn is carried forward along the same arc.
[[nodiscard]] cy::Transform carried_forward(const cy::Transform& before, const cy::Transform& now,
                                            f32 ahead) noexcept {
    cy::Transform out = now;
    out.translation = now.translation + ((now.translation - before.translation) * ahead);
    out.rotation = cy::nlerp(before.rotation, now.rotation, 1.0F + ahead);
    return out;
}

/// A float at a fixed quantisation — a millimetre's quarter — so the digest is a statement about
/// the simulation rather than about its last bit.
[[nodiscard]] u64 quantised(f32 value) noexcept {
    const auto scaled = static_cast<cy::f64>(value) * 4096.0;
    return static_cast<u64>(static_cast<cy::i64>(std::llround(scaled)));
}

}  // namespace

FpsHost::FpsHost(cy::Allocator& allocator, const HostOptions& options) noexcept
    : allocator_(allocator),
      options_(options),
      world_(allocator),
      tree_(world_),
      binding_(cy::system_allocator(cy::MemoryDomain::Scripting), world_),
      host_(cy::system_allocator(cy::MemoryDomain::Scripting)),
      runtime_(cy::system_allocator(cy::MemoryDomain::Scripting), host_),
      bridge_(allocator, tree_, host_, runtime_),
      manifest_text_(allocator),
      input_(allocator),
      camera_(allocator),
      audio_(allocator),
      input_adapter_(input_),
      camera_adapter_(camera_, allocator),
      audio_adapter_(audio_, allocator, &tree_),
      first_person_(allocator),
      lights_(allocator),
      mix_scratch_(allocator) {}

FpsHost::~FpsHost() {
    shutdown();
}

cy::Status FpsHost::start(const char** detail) noexcept {
    *detail = "";
    const struct {
        const char* name;
        cy::Status (FpsHost::*step)() noexcept;
    } steps[] = {
        {"scene", &FpsHost::start_scene},     {"input", &FpsHost::start_input},
        {"camera", &FpsHost::start_camera},   {"physics", &FpsHost::start_physics},
        {"audio", &FpsHost::start_audio},     {"presentation", &FpsHost::start_presentation},
        {"targets", &FpsHost::start_targets},
    };
    for (const auto& step : steps) {
        if (cy::Status done = (this->*step.step)(); !done) {
            *detail = step.name;
            return done;
        }
    }
    host_.bind_world(&binding_);
    cy::game_backend::bind(host_, &input_adapter_);
    cy::game_backend::bind(host_, &camera_adapter_);
    cy::game_backend::bind(host_, physics_adapter_.get());
    cy::game_backend::bind(host_, &audio_adapter_);
    cy::game_backend::bind_bodies(host_, body_adapter_.get());
    cy::game_backend::bind_characters(host_, characters_.get());
    cy::game_backend::bind_scene(host_, &bridge_);
    cy::game_backend::bind_first_person(host_, &first_person_);
    cy::game_backend::bind_lights(host_, &lights_);
    cy::game_backend::bind_effects(host_, effects_adapter_.get());
    cy::game_backend::bind_decals(host_, decals_adapter_.get());
    if (options_.bind_services) {
        options_.bind_services(host_);
    }
    started_ = true;

    if (cy::Status loaded = load_module(detail); !loaded) {
        return loaded;
    }
    const auto synced = bridge_.sync_types();
    if (!synced) {
        *detail = "behaviour types";
        return cy::make_unexpected(synced.error());
    }
    if (options_.behaviours) {
        *detail = "Player";
        if (cy::Status attached = bridge_.attach(player_, "Player"); !attached) {
            return attached;
        }
    }
    eye_.translation = kPlayerStart + Vec3{0.0F, 1.65F, 0.0F};
    *detail = "";
    return cy::ok();
}

cy::Status FpsHost::start_scene() noexcept {
    if (cy::Status ready = world_.initialize(); !ready) {
        return ready;
    }
    if (cy::Status ready = tree_.initialize(); !ready) {
        return ready;
    }
    const auto make = [this](const char* name, cy::scene::Node parent,
                             cy::scene::Node& out) noexcept -> cy::Status {
        const auto made = tree_.create_node(cy::Name::intern(name), parent);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        out = *made;
        return cy::ok();
    };
    if (cy::Status made = make(kArenaNode, tree_.root(), arena_); !made) {
        return made;
    }
    cy::scene::Node targets;
    cy::scene::Node props;
    for (const auto& [name, node] :
         {std::pair{kPlayerNode, &player_}, std::pair{kTargetsNode, &targets},
          std::pair{kPropsNode, &props}}) {
        if (cy::Status made = make(name, arena_, *node); !made) {
            return made;
        }
    }
    for (u32 index = 0; index < kTargetCount; ++index) {
        if (cy::Status made = make(target_spots()[index].node, targets, target_nodes_[index]);
            !made) {
            return made;
        }
    }
    for (u32 index = 0; index < kPropCount; ++index) {
        if (cy::Status made = make(prop_boxes()[index].node, props, prop_nodes_[index]); !made) {
            return made;
        }
    }
    return cy::ok();
}

cy::Status FpsHost::start_input() noexcept {
    cy::input::InputServerConfig config;
    config.users = 1;
    config.event_capacity = 256;
    config.allow_synthetic = true;
    if (cy::Status configured = input_.configure(config); !configured) {
        return configured;
    }
    if (cy::Status ready = input_.initialize(); !ready) {
        return ready;
    }
    if (cy::Status declared = declare_input(input_, allocator_); !declared) {
        return declared;
    }
    if (!options_.scripted_devices) {
        return cy::ok();
    }
    const auto keyboard = connect(input_, cy::input::DeviceKind::Keyboard, "fps-keyboard");
    const auto mouse = connect(input_, cy::input::DeviceKind::Mouse, "fps-mouse");
    if (!keyboard || !mouse) {
        return cy::fail(cy::ErrorCode::Unavailable, "the scripted keyboard or mouse was refused");
    }
    keyboard_ = *keyboard;
    mouse_ = *mouse;
    // The pointer starts in the middle of the window, where a captured mouse is.
    return move_pointer(0.0F, 0.0F);
}

cy::Status FpsHost::start_camera() noexcept {
    if (cy::Status configured = camera_.configure(cy::camera::CameraServerConfig{}); !configured) {
        return configured;
    }
    if (cy::Status ready = camera_.initialize(); !ready) {
        return ready;
    }
    const auto definition = camera_.create_definition(fps_rig(allocator_));
    if (!definition) {
        return cy::make_unexpected(definition.error());
    }
    const auto rig = camera_.create_rig(*definition, cy::camera::RigConfig{});
    if (!rig) {
        return cy::make_unexpected(rig.error());
    }
    rig_ = *rig;
    camera_handle_ = rig_.bits();
    cy::camera::TargetBinding origin;
    origin.kind = cy::camera::TargetKind::Position;
    if (cy::Status bound = camera_.set_target(rig_, origin); !bound) {
        return bound;
    }
    cy::camera::RenderViewRequest request;
    request.viewport = cy::render::ViewportRect{0, 0, kViewportWidth, kViewportHeight};
    camera_adapter_.set_primary_view(rig_, request);
    if (cy::Status placed = camera_.override_pose(
            rig_, cy::Transform::from_translation(kPlayerStart + Vec3{0.0F, 1.65F, 0.0F}));
        !placed) {
        return placed;
    }
    cy::camera::EvaluationContext context;
    context.delta_seconds = kStep;
    const auto evaluated = camera_.evaluate(rig_, context);
    return evaluated ? cy::ok() : cy::make_unexpected(evaluated.error());
}

cy::Status FpsHost::start_physics() noexcept {
    // JOLT, not the reference backend: the reference resolves no contacts, and a crate a shot
    // pushes has to land on the floor and knock into the others.
    const auto made = cy::physics::jolt::create_server(allocator_, nullptr);
    if (!made) {
        return cy::make_unexpected(made.error());
    }
    physics_ = *made;
    if (cy::Status ready = physics_->initialize(); !ready) {
        return ready;
    }
    cy::physics::WorldDescription description;
    description.name = cy::Name::intern("arena");
    description.body_capacity = 128;
    const auto world = physics_->create_world(description);
    if (!world) {
        return cy::make_unexpected(world.error());
    }
    physics_world_ = *world;
    bodies_ = std::make_unique<ArenaBodies>(*physics_, physics_world_);
    CyEntity props[kPropCount] = {};
    CyEntity targets[kTargetCount] = {};
    for (u32 index = 0; index < kPropCount; ++index) {
        props[index] = cy::abi::to_abi(prop_nodes_[index].entity());
    }
    for (u32 index = 0; index < kTargetCount; ++index) {
        targets[index] = cy::abi::to_abi(target_nodes_[index].entity());
    }
    if (cy::Status built = bodies_->build(cy::abi::to_abi(arena_.entity()), props, targets);
        !built) {
        return built;
    }
    auto characters = cy::make_unique<cy::game_backend::CharacterAdapter>(
        allocator_, allocator_, *physics_, physics_world_);
    if (!characters) {
        return cy::make_unexpected(characters.error());
    }
    characters_ = std::move(*characters);
    bodies_->set_characters(characters_.get());
    auto queries = cy::make_unique<cy::game_backend::PhysicsQueryAdapter>(
        allocator_, allocator_, *physics_, physics_world_, *bodies_);
    auto writes =
        cy::make_unique<cy::game_backend::PhysicsBodyAdapter>(allocator_, *physics_, *bodies_);
    if (!queries || !writes) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "a physics adapter was refused");
    }
    physics_adapter_ = std::move(*queries);
    body_adapter_ = std::move(*writes);
    return cy::ok();
}

cy::Status FpsHost::start_audio() noexcept {
    cy::audio::AudioBackendConfig device;
    device.requested.sample_rate = kSampleRate;
    device.requested.layout = cy::audio::ChannelLayout::Stereo;
    device.requested.buffer_frames = kBlockFrames;
    if (cy::Status ready = audio_device_.initialize(device); !ready) {
        return ready;
    }
    cy::audio::AudioServerConfig config;
    config.requested = device.requested;
    config.block_frames = kBlockFrames;
    config.voice_capacity = 32;
    if (cy::Status configured = audio_.configure(config); !configured) {
        return configured;
    }
    if (cy::Status ready = audio_.initialize_with(audio_device_); !ready) {
        return ready;
    }
    if (cy::Status sized = mix_scratch_.resize(static_cast<cy::usize>(kFramesPerTick) * 2U);
        !sized) {
        return sized;
    }
    sounds_.clear();
    for (u32 index = 0; index < static_cast<u32>(Sound::Count); ++index) {
        sounds_.emplace_back(allocator_);
    }
    for (u32 index = 0; index < static_cast<u32>(Sound::Count); ++index) {
        const auto sound = static_cast<Sound>(index);
        if (cy::Status made = make_sound(sound, kSampleRate, sounds_[index]); !made) {
            return made;
        }
        cy::audio::ClipDescription clip;
        clip.name = cy::Name::intern(sound_cue(sound));
        clip.samples = sounds_[index].data();
        clip.frame_count = static_cast<u32>(sounds_[index].size());
        clip.sample_rate = kSampleRate;
        const auto created = audio_.create_clip(clip);
        if (!created) {
            return cy::make_unexpected(created.error());
        }
        if (cy::Status added = audio_adapter_.add_cue(cy::Name::intern(sound_cue(sound)), *created);
            !added) {
            return added;
        }
    }
    return cy::ok();
}

cy::Status FpsHost::start_presentation() noexcept {
    if (cy::Status made = first_person_.create(1); !made) {
        return made;
    }
    if (cy::Status made = lights_.create(kLightCapacity); !made) {
        return made;
    }
    effects_ = std::make_unique<ShotEffects>(allocator_);
    if (cy::Status built = effects_->build(); !built) {
        return built;
    }
    effects_adapter_ =
        std::make_unique<cy::game_backend::EffectsAdapter>(allocator_, effects_->world());
    if (cy::Status added = effects_adapter_->add_effect(kMuzzleEffect, effects_->muzzle(),
                                                        ShotEffects::kMuzzleSeconds);
        !added) {
        return added;
    }
    if (cy::Status added = effects_adapter_->add_effect(kSparksEffect, effects_->sparks(),
                                                        ShotEffects::kSparkSeconds);
        !added) {
        return added;
    }
    decals_adapter_ =
        std::make_unique<cy::game_backend::DecalsAdapter>(allocator_, effects_->decals());
    if (cy::Status added = decals_adapter_->add_material(kScorchMaterial, 0); !added) {
        return added;
    }
    gi_ = std::make_unique<ArenaIllumination>(allocator_);
    return options_.illumination ? gi_->build() : cy::ok();
}

cy::Status FpsHost::start_targets() noexcept {
    targets_ = std::make_unique<Targets>(allocator_, *physics_, physics_world_, *bodies_);
    cy::ecs::Entity entities[kTargetCount];
    for (u32 index = 0; index < kTargetCount; ++index) {
        entities[index] = target_nodes_[index].entity();
    }
    return targets_->build(entities);
}

cy::Status FpsHost::load_module(const char** detail) noexcept {
    *detail = options_.module_manifest;
    if (cy::Status read = read_file(options_.module_manifest, manifest_text_); !read) {
        return read;
    }
    const auto parsed = cy::abi::parse_module_manifest(manifest_text_.data());
    if (!parsed) {
        return cy::make_unexpected(parsed.error());
    }
    manifest_ = *parsed;
    *detail = options_.module_library;
    const auto loaded = runtime_.load(manifest_, options_.module_library);
    if (!loaded) {
        return cy::make_unexpected(loaded.error());
    }
    *detail = "";
    return cy::ok();
}

cy::Status FpsHost::frame() noexcept {
    const auto started = std::chrono::steady_clock::now();
    cy::Status ran = pump();
    if (ran) {
        ran = tick(static_cast<cy::Nanoseconds>(tick_ + 1U) * kStepNs);
    }
    if (ran) {
        begin_frame();
        ran = update(FrameStep{});
    }
    frame_ms_ =
        std::chrono::duration<cy::f64, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return ran;
}

cy::Status FpsHost::pump() noexcept {
    return tree_.pump();
}

void FpsHost::begin_frame() noexcept {
    input_adapter_.begin_frame();
}

cy::Status FpsHost::update(const FrameStep& step) noexcept {
    interpolation_ = step.interpolation;
    // WHAT PRESENTATION ADVANCES BY: the frame's seconds, or nothing while the game is paused —
    // a flash, a burst, the recoil's spring and a sound all hold where they are.
    const f32 presented = step.paused ? 0.0F : step.seconds;
    // THE PRESENTATION POOLS, before the game's frame: last frame's lights age, so a flash this
    // frame spawns is drawn at its full brightness; the marks are stamped with this frame's time.
    lights_.advance(presented);
    decals_adapter_->begin_frame((static_cast<cy::f64>(tick_) * static_cast<cy::f64>(kStep)) +
                                 (step.interpolation * static_cast<cy::f64>(kStep)));
    host_.game.clock.interpolation = step.interpolation;
    host_.game.clock.flags = step.paused ? (host_.game.clock.flags | CY_TIME_PAUSED)
                                         : (host_.game.clock.flags & ~CY_TIME_PAUSED);
    runtime_.frame_update(step.seconds);
    first_person_.advance(presented);
    if (cy::Status placed = place_eye(presented); !placed) {
        return placed;
    }
    if (cy::Status stepped = effects_->step(presented); !stepped) {
        return stepped;
    }
    if (options_.illumination) {
        cy::Transform props[kPropCount];
        for (u32 index = 0; index < kPropCount; ++index) {
            props[index] = drawn_prop(index);
        }
        if (cy::Status lit = gi_->update(eye_.translation, props, frame_ + 1U); !lit) {
            return lit;
        }
    }
    audio_adapter_.update(presented);
    audio_.update(presented);
    (void)audio_device_.advance(mix_scratch_.data(), kFramesPerTick);
    peak_voices_ = cy::math::max(peak_voices_, audio_.statistics().active_voices);
    lights_live_peak_ = cy::math::max(lights_live_peak_, lights_.pool().status().live);
    fold_digest();
    ++frame_;
    events_this_frame_ = 0;
    return cy::ok();
}

cy::Status FpsHost::tick(cy::Nanoseconds now) noexcept {
    return fixed_tick(now);
}

cy::Status FpsHost::drain(cy::Nanoseconds now) noexcept {
    input_adapter_.observe_pending();
    input_.resolve_tick(tick_, now, 0.0F);
    return cy::ok();
}

void FpsHost::remember_tick() noexcept {
    for (u32 index = 0; index < kPropCount; ++index) {
        props_before_[index] = bodies_->prop(index);
    }
    for (u32 index = 0; index < kTargetCount; ++index) {
        targets_before_[index] = targets_->state(index);
    }
}

cy::Status FpsHost::fixed_tick(cy::Nanoseconds now) noexcept {
    remember_tick();
    const u64 tick = ++tick_;
    host_.game.clock.tick = tick;
    input_adapter_.observe_pending();
    input_.resolve_tick(tick, now, kStep);
    runtime_.fixed_update(kStep);
    // A SHOT THE GAME FIRED this step is captured now, before anything moves: the world its ray
    // crossed is still the world the host's own ray crosses.
    capture_shot();
    // THE TARGETS, after the game's step: what it wrote this tick is what they feel.
    TargetHealth health[kTargetCount];
    read_health(health);
    PlayerSighting player;
    player.entity = player_.entity();
    const CyEntity player_bits = cy::abi::to_abi(player_.entity());
    CyCharacterState state{};
    state.struct_size = sizeof(state);
    if (characters_->state(player_bits, state) == CY_RESULT_OK) {
        player.eyes = Vec3{state.position[0], state.position[1] + 0.75F, state.position[2]};
        player.velocity = Vec3{state.velocity[0], state.velocity[1], state.velocity[2]};
    } else {
        player.eyes = kPlayerStart + Vec3{0.0F, 1.65F, 0.0F};
    }
    if (cy::Status thought = targets_->tick(static_cast<u32>(tick), kStep, player, health);
        !thought) {
        return thought;
    }
    cy::physics::StepInput step;
    step.delta_seconds = kStep;
    step.tick = tick;
    return physics_->step(physics_world_, step);
}

cy::Status FpsHost::place_eye(f32 seconds) noexcept {
    // THE HEAD IS THE GAME'S: it overrides the camera's pose with where the player's head is this
    // frame. The look, the bob and the recoil are the first-person behaviours', applied here.
    cy::camera::EvaluationContext context;
    context.delta_seconds = seconds;
    const auto evaluated = camera_.evaluate(rig_, context);
    if (!evaluated) {
        return cy::make_unexpected(evaluated.error());
    }
    const Vec3 head = (*evaluated)->pose.translation;
    const cy::camera::FirstPersonPose* pose = first_person_.pose(camera_handle_);
    if (pose == nullptr) {
        // No behaviours (the negative control): the eye is the head, looking north.
        eye_ = cy::Transform::from_translation(head);
        weapon_ = eye_;
        return cy::ok();
    }
    eye_ = cy::camera::first_person_eye(*pose, head);
    weapon_ = cy::camera::first_person_weapon(*pose, eye_);
    fov_ = pose->lens.gameplay.vertical_fov_radians;
    peak_recoil_ = cy::math::max(peak_recoil_, std::fabs(pose->recoil.x));
    peak_aim_ = cy::math::max(peak_aim_, pose->aim);
    // The rig shows the eye from now on: what `Camera.view` and the camera's projections answer.
    if (cy::Status shown = camera_.override_pose(rig_, eye_); !shown) {
        return shown;
    }
    const auto again = camera_.evaluate(rig_, context);
    return again ? cy::ok() : cy::make_unexpected(again.error());
}

void FpsHost::read_health(TargetHealth (&out)[kTargetCount]) const noexcept {
    const cy::abi::ComponentRecord* record = binding_.find("Health");
    if (record == nullptr) {
        return;
    }
    const auto field = [&](const char* name) { return field_of(binding_, *record, name); };
    const cy::abi::FieldRecord* points = field("points");
    const cy::abi::FieldRecord* hits = field("hits");
    const cy::abi::FieldRecord* source = field("source");
    for (u32 index = 0; index < kTargetCount; ++index) {
        const void* bytes = world_.get(target_nodes_[index].entity(), record->id);
        out[index].present = bytes != nullptr;
        out[index].points = read_field<f32>(bytes, points);
        out[index].hits = read_field<f32>(bytes, hits);
        const auto from = read_field<cy::Vec3>(bytes, source);
        out[index].source = from;
    }
}

void FpsHost::capture_shot() noexcept {
    const cy::abi::ComponentRecord* record = binding_.find("PlayerReport");
    if (record == nullptr) {
        return;
    }
    const void* bytes = world_.get(player_.entity(), record->id);
    const auto field = [&](const char* name) { return field_of(binding_, *record, name); };
    const f32 shots = read_field<f32>(bytes, field("shots"));
    if (shots <= shots_seen_) {
        return;
    }
    shots_seen_ = shots;
    ShotRecord shot;
    shot.frame = frame_;
    shot.origin = read_field<Vec3>(bytes, field("lastOrigin"));
    shot.direction = read_field<Vec3>(bytes, field("lastDirection"));
    shot.hit = read_field<CyEntity>(bytes, field("lastHit"));
    shot.point = read_field<Vec3>(bytes, field("lastPoint"));
    shot.normal = read_field<Vec3>(bytes, field("lastNormal"));
    shot.distance = read_field<f32>(bytes, field("lastDistance"));
    shot.kind = static_cast<u32>(read_field<f32>(bytes, field("lastKind")));
    // THE HOST'S OWN RAY, along the game's, through the same world: what it hits is what the shot
    // should have hit.
    cy::physics::RayCastHit seen;
    const CyEntity hit = raycast(shot.origin, shot.direction, kShotRange, &seen);
    const bool same_entity = hit == shot.hit;
    const bool both_missed = hit == CY_ENTITY_NULL && seen.body.is_null() && shot.kind == 0U;
    shot.confirmed =
        (same_entity && !seen.body.is_null() && cy::length(seen.position - shot.point) < 1e-3F) ||
        both_missed;
    shots_.push_back(shot);
}

void FpsHost::fold_digest() noexcept {
    const Vec3 feet = player_feet();
    for (const f32 value : {feet.x, feet.y, feet.z, eye_.translation.y}) {
        digest_ = fold(digest_, quantised(value));
    }
    for (u32 index = 0; index < kTargetCount; ++index) {
        const TargetState& state = targets_->state(index);
        for (const f32 value : {state.feet.x, state.feet.z, state.yaw, state.fall}) {
            digest_ = fold(digest_, quantised(value));
        }
        digest_ = fold(digest_, static_cast<u64>(state.mode));
    }
    for (u32 index = 0; index < kPropCount; ++index) {
        const cy::Transform placed = bodies_->prop(index);
        for (const f32 value : {placed.translation.x, placed.translation.y, placed.translation.z}) {
            digest_ = fold(digest_, quantised(value));
        }
    }
}

void FpsHost::shutdown() noexcept {
    if (!started_ && physics_ == nullptr) {
        return;
    }
    started_ = false;
    // Unbind first: a behaviour destroyed after this that calls a game service gets UNAVAILABLE
    // rather than an adapter that no longer exists.
    host_.game = cy::abi::game::GameServices{};
    host_.bind_world(nullptr);
    targets_.reset();
    decals_adapter_.reset();
    effects_adapter_.reset();
    physics_adapter_.reset();
    body_adapter_.reset();
    // Each controller destroys its own body and shape, so the characters go before the world.
    characters_.reset();
    if (bodies_ != nullptr) {
        bodies_->clear();
        bodies_.reset();
    }
    if (physics_ != nullptr) {
        (void)physics_->destroy_world(physics_world_);
        physics_->shutdown();
        cy::physics::jolt::destroy_server(physics_, allocator_);
        physics_ = nullptr;
    }
}

// --- The hand ------------------------------------------------------------------------------------

cy::Nanoseconds FpsHost::event_time() noexcept {
    ++events_this_frame_;
    return (static_cast<cy::Nanoseconds>(frame_) * kStepNs) + 1'000'000 +
           (static_cast<cy::Nanoseconds>(events_this_frame_) * 1'000);
}

cy::Status FpsHost::press_key(cy::input::Key key, bool down) noexcept {
    return input_.inject(keyboard_, cy::input::key_control(key), down ? 1.0F : 0.0F, event_time());
}

cy::Status FpsHost::move_pointer(f32 dx, f32 dy) noexcept {
    using cy::input::MouseControl;
    // A captured mouse: its relative motion is what the game turns by, and its position moves
    // with it as a desktop mouse's would. `dy` is in window pixels, +y down; the input layer's
    // `MoveY` is +y up, as `Sdl3InputSource` delivers a real mouse's, so it is injected flipped.
    pointer_x_ += dx;
    pointer_y_ += dy;
    const cy::Nanoseconds at = event_time();
    const struct {
        MouseControl control;
        f32 value;
    } events[] = {{MouseControl::MoveX, dx},
                  {MouseControl::MoveY, -dy},
                  {MouseControl::PositionX, pointer_x_},
                  {MouseControl::PositionY, pointer_y_}};
    for (const auto& event : events) {
        if (event.value == 0.0F &&
            (event.control == MouseControl::MoveX || event.control == MouseControl::MoveY)) {
            continue;
        }
        if (cy::Status moved = input_.inject(mouse_, mouse_control(event.control), event.value, at);
            !moved) {
            return moved;
        }
    }
    return cy::ok();
}

cy::Status FpsHost::press_button(cy::input::MouseControl button, bool down) noexcept {
    return input_.inject(mouse_, cy::input::mouse_control(button), down ? 1.0F : 0.0F,
                         event_time());
}

f32 FpsHost::look_yaw() const noexcept {
    const cy::camera::FirstPersonPose* pose = first_person_.pose(camera_handle_);
    return pose == nullptr ? 0.0F : pose->yaw;
}

f32 FpsHost::look_pitch() const noexcept {
    const cy::camera::FirstPersonPose* pose = first_person_.pose(camera_handle_);
    return pose == nullptr ? 0.0F : pose->pitch;
}

Vec3 FpsHost::weapon_aim() const noexcept {
    const cy::camera::FirstPersonPose* pose = first_person_.pose(camera_handle_);
    return pose == nullptr ? Vec3{0.0F, 0.0F, -1.0F} : pose->aims.weapon_aim;
}

CyEntity FpsHost::raycast(Vec3 origin, Vec3 direction, f32 distance,
                          cy::physics::RayCastHit* out) const noexcept {
    cy::physics::RayCastInput ray;
    ray.origin = origin;
    ray.direction = direction;
    ray.max_distance = distance;
    const cy::physics::BodyHandle own = bodies_->body_of(cy::abi::to_abi(player_.entity()));
    cy::physics::QueryFilter filter;
    filter.filter.mask = (1U << kWorldLayer) | (1U << kPropLayer) | (1U << kTargetLayer);
    filter.ignore = own.is_null() ? nullptr : &own;
    filter.ignore_count = own.is_null() ? 0U : 1U;
    const auto hit = physics_->raycast(physics_world_, ray, filter);
    if (!hit || hit->body.is_null()) {
        return CY_ENTITY_NULL;
    }
    if (out != nullptr) {
        *out = *hit;
    }
    return static_cast<CyEntity>(hit->user_data);
}

// --- A played frame's interface -----------------------------------------------------------------

CyInputPointer FpsHost::pointer() noexcept {
    CyInputPointer out{};
    out.struct_size = sizeof(out);
    (void)input_adapter_.pointer(0, out);
    return out;
}

void FpsHost::set_pointer_over_ui(bool over) noexcept {
    input_adapter_.set_pointer_focus(0, true, over);
}

void FpsHost::ui_event(const CyUiEvent& event) noexcept {
    (void)runtime_.ui_event(event);
}

// --- The eyes ------------------------------------------------------------------------------------

cy::Transform FpsHost::drawn_prop(u32 index) const noexcept {
    const cy::Transform now = bodies_->prop(index);
    if (interpolation_ <= 0.0) {
        return now;
    }
    return carried_forward(props_before_[index], now, static_cast<f32>(interpolation_));
}

TargetState FpsHost::drawn_target(u32 index) const noexcept {
    TargetState drawn = targets_->state(index);
    if (interpolation_ <= 0.0) {
        return drawn;
    }
    const TargetState& before = targets_before_[index];
    const auto ahead = static_cast<f32>(interpolation_);
    drawn.feet = drawn.feet + ((drawn.feet - before.feet) * ahead);
    drawn.yaw = drawn.yaw + (cy::math::wrap_angle(drawn.yaw - before.yaw) * ahead);
    drawn.fall = cy::math::clamp(drawn.fall + ((drawn.fall - before.fall) * ahead), 0.0F, 1.0F);
    return drawn;
}

cy::rendering::viewmodel::ViewmodelSettings FpsHost::weapon_view() const noexcept {
    return first_person_.viewmodel(camera_handle_);
}

Vec3 FpsHost::player_feet() const noexcept {
    CyCharacterState state{};
    state.struct_size = sizeof(state);
    if (characters_.get() == nullptr ||
        characters_->state(cy::abi::to_abi(player_.entity()), state) != CY_RESULT_OK) {
        return kPlayerStart;
    }
    const cy::physics::CharacterController* controller =
        characters_->controller(cy::abi::to_abi(player_.entity()));
    const f32 half = controller == nullptr ? 0.9F : controller->description().height * 0.5F;
    return Vec3{state.position[0], state.position[1] - half, state.position[2]};
}

CyEntity FpsHost::target_entity(u32 index) const noexcept {
    return cy::abi::to_abi(target_nodes_[index].entity());
}

CyEntity FpsHost::prop_entity(u32 index) const noexcept {
    return cy::abi::to_abi(prop_nodes_[index].entity());
}

cy::Status FpsHost::append_lights(cy::Array<cy::render::LightDescription>& out) const noexcept {
    const auto appended = lights_.append_to(out);
    return appended ? cy::ok() : cy::make_unexpected(appended.error());
}

void FpsHost::read_report(Observation& seen) const noexcept {
    const cy::abi::ComponentRecord* record = binding_.find("PlayerReport");
    if (record == nullptr) {
        return;
    }
    const void* bytes = world_.get(player_.entity(), record->id);
    seen.game.found = bytes != nullptr;
    const auto value = [&](const char* name) {
        return read_field<f32>(bytes, field_of(binding_, *record, name));
    };
    Observation::Game& game = seen.game;
    game.shots = value("shots");
    game.target_hits = value("targetHits");
    game.prop_hits = value("propHits");
    game.world_hits = value("worldHits");
    game.misses = value("misses");
    game.kills = value("kills");
    game.walk_speed = value("walkSpeed");
    game.sprint_speed = value("sprintSpeed");
    game.crouch_speed = value("crouchSpeed");
    game.jumps = value("jumps");
    game.airborne = value("airborne");
    game.peak_height = value("peakHeight");
    game.crouched = value("crouched");
    game.stand_refused = value("standRefused");
    game.under_beam = value("underBeam");
    game.flashes = value("flashes");
    game.bursts = value("bursts");
    game.sparks = value("sparks");
    game.marks = value("marks");
    game.sounds = value("sounds");
    game.kicks = value("kicks");
    game.impulses = value("impulses");
    game.aimed = value("aimed");
    game.refused = value("refused");
    game.ammo = value("ammo");
    game.reloads = value("reloads");
    game.dry_fires = value("dryFires");
    game.respawns = value("respawns");
}

Observation FpsHost::observe() noexcept {
    Observation seen;
    read_report(seen);
    CyCharacterState state{};
    state.struct_size = sizeof(state);
    const CyEntity player = cy::abi::to_abi(player_.entity());
    if (characters_.get() != nullptr && characters_->state(player, state) == CY_RESULT_OK) {
        seen.player = Vec3{state.position[0], state.position[1], state.position[2]};
        seen.grounded = state.ground == CY_GROUND_GROUNDED;
        const cy::physics::CharacterController* controller = characters_->controller(player);
        seen.capsule_height = controller == nullptr ? 0.0F : controller->description().height;
    }
    seen.yaw = look_yaw();
    seen.pitch = look_pitch();
    seen.peak_recoil = peak_recoil_;
    seen.peak_aim = peak_aim_;
    seen.lights_spawned = lights_.pool().status().spawned;
    seen.lights_live_peak = lights_live_peak_;
    seen.effects_played = effects_adapter_ == nullptr ? 0U : effects_adapter_->played();
    seen.muzzle_plays = effects_adapter_ == nullptr ? 0U : effects_adapter_->played(1);
    seen.spark_plays = effects_adapter_ == nullptr ? 0U : effects_adapter_->played(2);
    seen.decals_spawned = decals_adapter_ == nullptr ? 0U : decals_adapter_->spawned();
    seen.decals_live = static_cast<u32>(effects_->decals().size());
    seen.peak_particles = effects_->peak_particles();
    seen.particle_frames = effects_->particle_frames();
    seen.peak_voices = peak_voices_;
    read_health(seen.health);
    for (u32 index = 0; index < kPropCount; ++index) {
        seen.prop_moved[index] =
            cy::length(bodies_->prop(index).translation - prop_boxes()[index].centre);
    }
    return seen;
}

}  // namespace sample::fps
