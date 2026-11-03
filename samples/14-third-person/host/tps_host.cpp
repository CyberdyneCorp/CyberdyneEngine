// SPDX-License-Identifier: MIT
// The engine side of samples/14-third-person. See tps_host.h.

#include "tps_host.h"

#if defined(CY_THIRD_PERSON_STREAM_CODECS)
#    include <cy/backends/audio/miniaudio_codecs.h>
#endif

#include <cy/backends/physics/jolt/server.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <utility>

#include "level.h"
#include "play_motion.h"
#include "sounds.h"

namespace sample::tps {
namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
namespace map = sample::rts::map;

constexpr f32 kStep = 1.0F / 60.0F;
constexpr cy::Nanoseconds kStepNs = 16'666'666;
constexpr u32 kSampleRate = 48000;
constexpr u32 kBlockFrames = 200;
constexpr u32 kFramesPerTick = kSampleRate / 60;  // 800, a whole number of blocks

/// The window the primary view is evaluated for.
constexpr u32 kViewportWidth = 1920;
constexpr u32 kViewportHeight = 1080;

/// The rig's distances at zoom 0 and 1: the game asks for a distance between them, which the host
/// turns into the orbit's one normalised zoom.
constexpr f32 kNearestFraming = 1.0F;
constexpr f32 kFarthestFraming = 10.0F;
/// Over the right shoulder, and a touch above it.
constexpr Vec3 kShoulder{0.38F, 0.06F, 0.0F};
/// The camera's probe: it stops this far in front of whatever it meets.
constexpr f32 kProbeRadius = 0.22F;
/// The hero's node eases over a stair's riser this quickly, and never trails its capsule by more.
constexpr f32 kStepHalfLife = 0.045F;
constexpr f32 kStepSlack = 0.25F;

[[nodiscard]] cy::camera::RigNodeDesc rig_node(const char* id, const char* input,
                                               cy::camera::RigNodeKind kind) noexcept {
    cy::camera::RigNodeDesc desc;
    desc.id = cy::Name::intern(id);
    desc.input = input == nullptr ? cy::Name{} : cy::Name::intern(input);
    desc.kind = kind;
    return desc;
}

/// THE THIRD-PERSON COMPOSITION `camera-system` names: a target, an orbit, a shoulder offset, a
/// collision node and a lens. Where it frames, from how far and at what angle is the game's (its
/// `Camera.setTarget`); this is the rig's topology and its smoothing.
[[nodiscard]] cy::camera::RigDefinition third_person_rig(cy::Allocator& allocator) noexcept {
    using cy::camera::RigNodeKind;
    cy::camera::RigDefinition definition(allocator);
    definition.name = cy::Name::intern("third-person");
    cy::camera::RigNodeDesc target = rig_node("target", nullptr, RigNodeKind::Target);
    target.target.anchor_half_life = 0.035F;
    (void)definition.nodes.push_back(target);
    cy::camera::RigNodeDesc orbit = rig_node("orbit", "target", RigNodeKind::Orbit);
    orbit.orbit.near_distance = kNearestFraming;
    orbit.orbit.far_distance = kFarthestFraming;
    orbit.orbit.min_pitch_radians = -1.4F;
    orbit.orbit.max_pitch_radians = 1.4F;
    orbit.orbit.distance_half_life = 0.0F;
    (void)definition.nodes.push_back(orbit);
    cy::camera::RigNodeDesc shoulder = rig_node("shoulder", "orbit", RigNodeKind::Offset);
    shoulder.offset.offset = kShoulder;
    (void)definition.nodes.push_back(shoulder);
    cy::camera::RigNodeDesc collide = rig_node("collide", "shoulder", RigNodeKind::Collision);
    collide.collision.probe_radius = kProbeRadius;
    collide.collision.collision_response = cy::camera::CollisionResponse::PullIn;
    // A wall between the camera it wants and the hero pulls it in already: the occlusion ray is
    // asked for what it reports, and fades rather than moves.
    collide.collision.occlusion_response = cy::camera::CollisionResponse::FadeObstacle;
    collide.collision.recovery_half_life = 0.3F;
    (void)definition.nodes.push_back(collide);
    cy::camera::RigNodeDesc lens = rig_node("lens", "collide", RigNodeKind::Lens);
    lens.lens.near_value = 1.0821F;  // 62 degrees
    lens.lens.far_value = 1.0821F;
    lens.lens.near_plane = 0.1F;
    (void)definition.nodes.push_back(lens);
    (void)definition.nodes.push_back(rig_node("output", "lens", RigNodeKind::Output));
    return definition;
}

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

[[nodiscard]] const cy::abi::FieldRecord* report_field(const cy::abi::World& binding,
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

[[nodiscard]] f32 wrap_angle(f32 angle) noexcept {
    constexpr f32 kTurn = 2.0F * std::numbers::pi_v<f32>;
    angle = std::fmod(angle + std::numbers::pi_v<f32>, kTurn);
    if (angle < 0.0F) {
        angle += kTurn;
    }
    return angle - std::numbers::pi_v<f32>;
}

/// FNV-1a over `bytes`, into `hash`.
void fold(u64& hash, const void* bytes, cy::usize size) noexcept {
    const auto* data = static_cast<const unsigned char*>(bytes);
    for (cy::usize index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ULL;
    }
}

/// A box round a placed mesh, turned with it: what a building, a crate or a rock collides as.
struct PlacedBox {
    Vec3 centre;
    Vec3 half;
    cy::Quat rotation;
};

[[nodiscard]] PlacedBox placed_box(const map::Placement& placement,
                                   const map::MeshRange& mesh) noexcept {
    const Vec3 low = cy::cwise_mul(mesh.min, placement.scale);
    const Vec3 high = cy::cwise_mul(mesh.max, placement.scale);
    const Vec3 local_centre = (low + high) * 0.5F;
    const cy::Quat turn = cy::Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, placement.yaw);
    return PlacedBox{placement.position + (turn * local_centre), (high - low) * 0.5F, turn};
}

}  // namespace

ThirdPersonHost::ThirdPersonHost(cy::Allocator& allocator, HostOptions options) noexcept
    : allocator_(allocator),
      options_(std::move(options)),
      world_(allocator),
      tree_(world_),
      schedule_(world_),
      binding_(cy::system_allocator(cy::MemoryDomain::Scripting), world_),
      host_(cy::system_allocator(cy::MemoryDomain::Scripting)),
      runtime_(cy::system_allocator(cy::MemoryDomain::Scripting), host_),
      script_systems_(cy::system_allocator(cy::MemoryDomain::Scripting), host_),
      bridge_(allocator, tree_, host_, runtime_),
      manifest_text_(allocator),
      input_(allocator),
      camera_(allocator),
      audio_(allocator),
      input_adapter_(input_),
      camera_adapter_(camera_, allocator),
      audio_adapter_(audio_, allocator, &tree_),
      hero_rig_(allocator) {}

ThirdPersonHost::~ThirdPersonHost() {
    shutdown();
}

cy::Status ThirdPersonHost::start(const char** detail) noexcept {
    *detail = "";
    if (options_.map == nullptr) {
        *detail = "level";
        return cy::fail(cy::ErrorCode::InvalidArgument, "the host needs the cooked level");
    }
    const struct {
        const char* name;
        cy::Status (ThirdPersonHost::*step)() noexcept;
    } steps[] = {
        {"scene", &ThirdPersonHost::start_scene},
        {"input", &ThirdPersonHost::start_input},
        {"camera", &ThirdPersonHost::start_camera},
        {"physics", &ThirdPersonHost::start_physics},
        {"level", &ThirdPersonHost::start_level},
        {"audio", &ThirdPersonHost::start_audio},
        {"animation", &ThirdPersonHost::start_animation},
        {"streams", &ThirdPersonHost::start_streams},
        {"illumination", &ThirdPersonHost::start_illumination},
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
    cy::game_backend::bind(host_, &audio_adapter_);
    cy::game_backend::bind_characters(host_, characters_.get());
    cy::game_backend::bind_scene(host_, &bridge_);
    cy::game_backend::bind_animation(host_, animation_adapter_.get());
    if (options_.bind_services) {
        options_.bind_services(host_);
    }
    started_ = true;
    if (cy::Status loaded = load_module(detail); !loaded) {
        return loaded;
    }
    *detail = "systems";
    if (const auto installed = animation_->install(schedule_, animation_clock_); !installed) {
        return cy::make_unexpected(installed.error());
    }
    if (cy::Status built = schedule_.build(); !built) {
        return built;
    }
    if (cy::Status attached = attach_behaviours(detail); !attached) {
        return attached;
    }
    hero_before_ = hero_placement();
    *detail = "";
    return cy::ok();
}

cy::Status ThirdPersonHost::start_scene() noexcept {
    if (cy::Status ready = world_.initialize(); !ready) {
        return ready;
    }
    if (cy::Status ready = tree_.initialize(); !ready) {
        return ready;
    }
    const auto level = tree_.create_node(cy::Name::intern("Level"), tree_.root());
    if (!level) {
        return cy::make_unexpected(level.error());
    }
    cy::scene::Node* nodes[] = {&terrain_, &courtyard_, &hero_, &camera_node_, &ambience_};
    const char* names[] = {"Terrain", "Courtyard", "Hero", "Camera", "Ambience"};
    for (cy::usize index = 0; index < 5U; ++index) {
        const auto made = tree_.create_node(cy::Name::intern(names[index]), *level);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        *nodes[index] = *made;
    }
    return cy::ok();
}

cy::Status ThirdPersonHost::start_input() noexcept {
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
    if (!options_.scripted_devices) {
        return declare_input(input_, allocator_);
    }
    cy::input::DeviceDescription description;
    description.kind = cy::input::DeviceKind::Keyboard;
    description.hardware_id = cy::Name::intern("third-person-keyboard");
    description.display_name = description.hardware_id;
    const auto keyboard = input_.devices().connect(description, 0);
    if (!keyboard) {
        return cy::make_unexpected(keyboard.error());
    }
    if (cy::Status assigned = input_.assign(*keyboard, 0, 0); !assigned) {
        return assigned;
    }
    keyboard_ = *keyboard;
    return declare_input(input_, allocator_);
}

cy::Status ThirdPersonHost::start_camera() noexcept {
    if (cy::Status configured = camera_.configure(cy::camera::CameraServerConfig{}); !configured) {
        return configured;
    }
    if (cy::Status ready = camera_.initialize(); !ready) {
        return ready;
    }
    const auto definition = camera_.create_definition(third_person_rig(allocator_));
    if (!definition) {
        return cy::make_unexpected(definition.error());
    }
    const auto rig = camera_.create_rig(*definition, cy::camera::RigConfig{});
    if (!rig) {
        return cy::make_unexpected(rig.error());
    }
    rig_ = *rig;
    cy::camera::TargetBinding origin;
    origin.kind = cy::camera::TargetKind::Position;
    if (cy::Status bound = camera_.set_target(rig_, origin); !bound) {
        return bound;
    }
    cy::camera::RenderViewRequest request;
    request.viewport = cy::render::ViewportRect{0, 0, kViewportWidth, kViewportHeight};
    camera_adapter_.set_primary_view(rig_, request);
    cy::camera::EvaluationContext context;
    context.delta_seconds = kStep;
    context.aspect = static_cast<f32>(kViewportWidth) / static_cast<f32>(kViewportHeight);
    const auto evaluated = camera_.evaluate(rig_, context);
    return evaluated ? cy::ok() : cy::make_unexpected(evaluated.error());
}

cy::Status ThirdPersonHost::start_physics() noexcept {
    // JOLT, because the level is not boxes standing on a plane: the terrain is a triangle mesh and
    // the ramp a turned box, and the reference backend collides every shape as its bounds. A single
    // thread — no job system — so two runs step the same.
    const auto made = cy::physics::jolt::create_server(allocator_, nullptr);
    if (!made) {
        return cy::make_unexpected(made.error());
    }
    physics_ = *made;
    if (cy::Status ready = physics_->initialize(); !ready) {
        return ready;
    }
    cy::physics::WorldDescription description;
    description.name = cy::Name::intern("third-person");
    description.body_capacity = 512;
    const auto world = physics_->create_world(description);
    if (!world) {
        return cy::make_unexpected(world.error());
    }
    physics_world_ = *world;
    auto characters = cy::make_unique<cy::game_backend::CharacterAdapter>(
        allocator_, allocator_, *physics_, physics_world_);
    if (!characters) {
        return cy::make_unexpected(characters.error());
    }
    characters_ = std::move(*characters);
    camera_collision_ = std::make_unique<cy::game_backend::CameraCollisionResolver>(
        allocator_, *physics_, physics_world_);
    ground_ = std::make_unique<cy::animation::PhysicsGroundProbe>(*physics_, physics_world_);
    return cy::ok();
}

cy::Status ThirdPersonHost::static_box(Vec3 centre, Vec3 half, cy::Quat rotation,
                                       CyEntity owner) noexcept {
    cy::physics::ShapeDescription box;
    box.type = cy::physics::ShapeType::Box;
    box.half_extents = half;
    const auto shape = physics_->create_shape(box);
    if (!shape) {
        return cy::make_unexpected(shape.error());
    }
    shapes_.push_back(*shape);
    cy::physics::ColliderDescription collider;
    collider.shape = *shape;
    cy::physics::BodyDescription body;
    body.motion = cy::physics::MotionType::Static;
    body.transform = cy::Transform::from_translation(centre);
    body.transform.rotation = rotation;
    body.colliders = &collider;
    body.collider_count = 1;
    body.user_data = owner;
    const auto made = physics_->create_body(physics_world_, body);
    if (!made) {
        return cy::make_unexpected(made.error());
    }
    bodies_.push_back(*made);
    return cy::ok();
}

cy::Status ThirdPersonHost::start_level() noexcept {
    const map::MapContent& content = *options_.map;
    // THE TERRAIN, as the triangles the renderer draws: one static mesh over the whole map, owned
    // by /Level/Terrain so the game hears grass on it.
    std::vector<Vec3> vertices;
    std::vector<u32> indices;
    const map::MeshLibrary& ground = content.ground;
    for (const map::MeshRange& range : ground.meshes) {
        const auto base = static_cast<u32>(vertices.size());
        vertices.insert(vertices.end(), ground.positions.begin() + range.first_vertex,
                        ground.positions.begin() + range.first_vertex + range.vertex_count);
        for (u32 part = range.first_part; part < range.first_part + range.part_count; ++part) {
            const map::MeshPart& run = ground.parts[part];
            for (u32 index = 0; index < run.index_count; ++index) {
                indices.push_back(base + ground.indices[run.first_index + index]);
            }
        }
    }
    cy::physics::ShapeDescription mesh;
    mesh.type = cy::physics::ShapeType::TriangleMesh;
    mesh.vertices = vertices.data();
    mesh.vertex_count = static_cast<u32>(vertices.size());
    mesh.indices = indices.data();
    mesh.index_count = static_cast<u32>(indices.size());
    const auto shape = physics_->create_shape(mesh);
    if (!shape) {
        return cy::make_unexpected(shape.error());
    }
    shapes_.push_back(*shape);
    cy::physics::ColliderDescription collider;
    collider.shape = *shape;
    cy::physics::BodyDescription body;
    body.motion = cy::physics::MotionType::Static;
    body.colliders = &collider;
    body.collider_count = 1;
    body.user_data = cy::abi::to_abi(terrain_.entity());
    const auto made = physics_->create_body(physics_world_, body);
    if (!made) {
        return cy::make_unexpected(made.error());
    }
    bodies_.push_back(*made);

    // THE COURTYARD'S PIECES, owned by /Level/Courtyard: the game hears stone on them.
    const CyEntity courtyard = cy::abi::to_abi(courtyard_.entity());
    for (const LevelPiece& piece : level_pieces()) {
        const cy::Quat turn = cy::Quat::from_axis_angle(Vec3{0.0F, 0.0F, 1.0F}, piece.roll);
        if (cy::Status built = static_box(piece.centre, piece.half, turn, courtyard); !built) {
            return built;
        }
    }
    // THE MAP'S BUILDINGS, PROPS AND ROCKS, each a box round its mesh. The water is not solid.
    for (const map::Placement& placement : content.layout.placements) {
        if (placement.kind == map::PlacementKind::Water) {
            continue;
        }
        const PlacedBox box = placed_box(placement, content.props.meshes[placement.mesh]);
        if (cy::Status built = static_box(box.centre, box.half, box.rotation, courtyard); !built) {
            return built;
        }
    }
    return options_.fenced ? fence() : cy::ok();
}

cy::Status ThirdPersonHost::fence() noexcept {
    // FOUR WALLS A METRE IN FROM THE TERRAIN'S EDGE, tall and not drawn: past the edge there is
    // nothing to stand on, and a hero that walked off it would fall for ever.
    const map::MapLayout& layout = options_.map->layout;
    const f32 half = layout.size * 0.5F;
    const Vec3 middle{layout.origin_x + half, 0.0F, layout.origin_z + half};
    constexpr f32 kInset = 1.0F;
    constexpr f32 kThick = 0.5F;
    constexpr f32 kTall = 60.0F;
    const f32 reach = half - kInset + kThick;
    const struct {
        Vec3 offset;
        Vec3 half;
    } walls[] = {
        {Vec3{reach, 0.0F, 0.0F}, Vec3{kThick, kTall, half}},
        {Vec3{-reach, 0.0F, 0.0F}, Vec3{kThick, kTall, half}},
        {Vec3{0.0F, 0.0F, reach}, Vec3{half, kTall, kThick}},
        {Vec3{0.0F, 0.0F, -reach}, Vec3{half, kTall, kThick}},
    };
    for (const auto& wall : walls) {
        if (cy::Status built =
                static_box(middle + wall.offset, wall.half, cy::Quat::identity(), CY_ENTITY_NULL);
            !built) {
            return built;
        }
    }
    return cy::ok();
}

cy::Status ThirdPersonHost::start_audio() noexcept {
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
    config.voice_capacity = 24;
    if (cy::Status configured = audio_.configure(config); !configured) {
        return configured;
    }
    if (cy::Status ready = audio_.initialize_with(audio_device_); !ready) {
        return ready;
    }
    mix_scratch_.assign(static_cast<cy::usize>(kFramesPerTick) * 2U, 0.0F);
    // Kept alive here: a clip's description points at its samples.
    for (Sound& sound : synthesise_sounds(kSampleRate)) {
        sound_samples_.push_back(std::move(sound.samples));
        cy::audio::ClipDescription clip;
        clip.name = cy::Name::intern(sound.cue);
        clip.samples = sound_samples_.back().data();
        clip.frame_count = static_cast<u32>(sound_samples_.back().size());
        clip.sample_rate = kSampleRate;
        const auto created = audio_.create_clip(clip);
        if (!created) {
            return cy::make_unexpected(created.error());
        }
        if (cy::Status added = audio_adapter_.add_cue(cy::Name::intern(sound.cue), *created);
            !added) {
            return added;
        }
    }
    cy::audio::Listener listener;
    const auto made = audio_.create_listener(listener);
    if (!made) {
        return cy::make_unexpected(made.error());
    }
    listener_ = *made;
    return cy::ok();
}

cy::Status ThirdPersonHost::start_streams() noexcept {
    // ISSUE #87: the hillside's wind as a STREAMED track — a 24-second Ogg Vorbis bed cooked by
    // cy_audio_cook, read from disk a chunk at a time on the async service, decoded on the rig's
    // job system and drained by the mixer from a half-second ring. Optional: without the file (a
    // build with CY_AUDIO off cooks none) the game plays its in-memory wind loop instead.
    if (options_.ambience == nullptr || options_.ambience[0] == '\0') {
        return cy::ok();
    }
    if (cy::Status opened = ambience_file_.open(options_.ambience); !opened) {
        std::fprintf(stderr, "tps: no streamed ambience (%s): %s\n", options_.ambience,
                     opened.error().message);
        return cy::ok();
    }
    const auto track = cy::audio::track_from_cooked(ambience_file_);
    if (!track) {
        return cy::make_unexpected(track.error());
    }
    stream_executor_ =
        std::make_unique<cy::audio::JobRefillExecutor>(hero_rig_.jobs(), hero_rig_.async());
    streams_ = std::make_unique<cy::audio::StreamPlayer>(audio_, *stream_executor_, allocator_);
    cy::audio::StreamPlayerConfig config;
    config.capacity = 2;
#if defined(CY_THIRD_PERSON_STREAM_CODECS)
    config.codecs = &cy::audio::miniaudio_audio_codecs();
#endif
    if (cy::Status configured = streams_->configure(config); !configured) {
        return configured;
    }
    audio_adapter_.bind_streams(streams_.get());
    return audio_adapter_.add_track(cy::Name::intern("ambience.hillside"), *track);
}

cy::Status ThirdPersonHost::start_animation() noexcept {
    // THE RIG ARRIVES THROUGH THE ASSET SYSTEM: cooked into a memory mount and loaded back by id.
    if (cy::Status loaded = hero_rig_.load(); !loaded) {
        return loaded;
    }
    if (cy::Status configured = animation_clock_.configure(cy::determinism::ClockConfig{});
        !configured) {
        return configured;
    }
    const auto registered = cy::animation::register_animator(world_);
    if (!registered) {
        return cy::make_unexpected(registered.error());
    }
    animator_ = *registered;
    animation_ =
        std::make_unique<cy::animation::AnimationSystem>(allocator_, world_, animator_, &tree_);
    // FOOT PLACEMENT'S GROUND is the physics world the hero walks on: a ray down under each ankle,
    // skipping the hero's own capsule.
    animation_->set_ground_probe(ground_.get());
    const auto rig = animation_->add_rig(hero_rig_.rig());
    if (!rig) {
        return cy::make_unexpected(rig.error());
    }
    animation_adapter_ = std::make_unique<cy::game_backend::AnimationAdapter>(
        allocator_, world_, *animation_, animator_, &tree_);
    animation_adapter_->bind_characters(characters_.get());
    return animation_adapter_->add_rig(kHeroRig, *rig);
}

cy::Status ThirdPersonHost::start_illumination() noexcept {
    gi_ = std::make_unique<TpsIllumination>(allocator_);
    if (!options_.illumination) {
        return cy::ok();
    }
    const map::MapContent& content = *options_.map;
    std::vector<GiBox> boxes;
    // The courtyard's ground: its top alone matters, a metre of cards apart.
    boxes.push_back(GiBox{Vec3{0.0F, -0.25F, -1.0F}, Vec3{15.0F, 0.25F, 16.0F},
                          Vec3{0.50F, 0.47F, 0.42F}, 1.0F});
    for (const LevelPiece& piece : level_pieces()) {
        if (piece.material[0] == '\0') {
            continue;
        }
        // A turned piece as the box round it: the GI's field is axis-aligned boxes.
        const f32 c = std::fabs(std::cos(piece.roll));
        const f32 s = std::fabs(std::sin(piece.roll));
        const Vec3 half{(piece.half.x * c) + (piece.half.y * s),
                        (piece.half.x * s) + (piece.half.y * c), piece.half.z};
        boxes.push_back(GiBox{piece.centre, half, piece.albedo, 0.5F});
    }
    for (const map::Placement& placement : content.layout.placements) {
        if (placement.kind == map::PlacementKind::Water) {
            continue;
        }
        const map::MeshRange& mesh = content.props.meshes[placement.mesh];
        const PlacedBox box = placed_box(placement, mesh);
        const f32 c = std::fabs(std::cos(placement.yaw));
        const f32 s = std::fabs(std::sin(placement.yaw));
        const Vec3 half{(box.half.x * c) + (box.half.z * s), box.half.y,
                        (box.half.x * s) + (box.half.z * c)};
        const u32 material =
            mesh.part_count > 0 ? content.props.parts[mesh.first_part].material : map::kNoMaterial;
        const Vec3 albedo = material < content.layout.materials.size()
                                ? content.layout.materials[material].base_colour * 0.8F
                                : Vec3{0.5F, 0.5F, 0.5F};
        boxes.push_back(GiBox{box.centre, half, albedo, 0.5F});
    }
    return gi_->build(boxes, content.layout.sun_travel, content.layout.sun_lux);
}

cy::Status ThirdPersonHost::load_module(const char** detail) noexcept {
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

cy::Status ThirdPersonHost::attach_behaviours(const char** detail) noexcept {
    const auto synced = bridge_.sync_types();
    if (!synced) {
        return cy::make_unexpected(synced.error());
    }
    if (!options_.behaviours) {
        return cy::ok();
    }
    // The hero first: the camera director and the ambience resolve `../Hero` and read it.
    const struct {
        const char* name = nullptr;
        cy::scene::Node node;
    } attachments[] = {{"Hero", hero_}, {"CameraDirector", camera_node_}, {"Ambience", ambience_}};
    for (const auto& attachment : attachments) {
        *detail = attachment.name;
        if (cy::Status attached = bridge_.attach(attachment.node, attachment.name); !attached) {
            return attached;
        }
    }
    // The camera's probe passes through the capsule it frames.
    if (const cy::physics::CharacterController* hero = hero_controller(); hero != nullptr) {
        const cy::physics::BodyHandle own[] = {hero->body()};
        if (cy::Status ignored = camera_collision_->set_ignored({own, 1}); !ignored) {
            return ignored;
        }
    }
    place_hero();
    return cy::ok();
}

// --- The frame -----------------------------------------------------------------------------------

cy::Status ThirdPersonHost::frame() noexcept {
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

cy::Status ThirdPersonHost::pump() noexcept {
    return tree_.pump();
}

cy::Status ThirdPersonHost::tick(cy::Nanoseconds now) noexcept {
    return fixed_tick(now);
}

cy::Status ThirdPersonHost::drain(cy::Nanoseconds now) noexcept {
    input_adapter_.observe_pending();
    input_.resolve_tick(tick_, now, 0.0F);
    return cy::ok();
}

void ThirdPersonHost::begin_frame() noexcept {
    input_adapter_.begin_frame();
}

cy::Status ThirdPersonHost::update(const FrameStep& step) noexcept {
    interpolation_ = step.interpolation;
    // WHAT PRESENTATION ADVANCES BY: the frame's seconds, or nothing while the game is paused.
    const f32 presented = step.paused ? 0.0F : step.seconds;
    if (cy::Status snapshot = animation_adapter_->begin_frame(); !snapshot) {
        return snapshot;
    }
    if (cy::Status driven = drive_camera(presented); !driven) {
        return driven;
    }
    host_.game.clock.interpolation = step.interpolation;
    host_.game.clock.flags = step.paused ? (host_.game.clock.flags | CY_TIME_PAUSED)
                                         : (host_.game.clock.flags & ~CY_TIME_PAUSED);
    runtime_.frame_update(step.seconds);
    if (cy::Status staged = run_stages(cy::ecs::Stage::Frame, cy::ecs::Stage::UI); !staged) {
        return staged;
    }
    if (gi_->built()) {
        if (cy::Status lit = gi_->update(drawn_hero().translation, frame_ + 1U); !lit) {
            return lit;
        }
    }
    update_audio(presented);
    ++frame_;
    events_this_frame_ = 0;
    track();
    return cy::ok();
}

cy::Status ThirdPersonHost::fixed_tick(cy::Nanoseconds now) noexcept {
    hero_before_ = hero_placement();
    const u64 tick = ++tick_;
    host_.game.clock.tick = tick;
    input_adapter_.observe_pending();
    input_.resolve_tick(tick, now, kStep);
    runtime_.fixed_update(kStep);
    if (cy::Status staged =
            run_stages(cy::ecs::Stage::PreSimulation, cy::ecs::Stage::PostSimulation);
        !staged) {
        return staged;
    }
    if (cy::Status moved = animation_adapter_->update(kStep); !moved) {
        return moved;
    }
    cy::physics::StepInput step;
    step.delta_seconds = kStep;
    step.tick = tick;
    if (cy::Status stepped = physics_->step(physics_world_, step); !stepped) {
        return stepped;
    }
    place_hero();
    return cy::ok();
}

cy::Status ThirdPersonHost::drive_camera(f32 seconds) noexcept {
    // THE GAME'S LAST REQUEST, as the orbit's intent: the yaw and pitch it moved by since the one
    // last applied, and its distance as the rig's one zoom parameter.
    const cy::game_backend::CameraAdapter::Framing* framing = camera_adapter_.framing(rig_);
    if (framing != nullptr && framing->revision != framing_revision_) {
        framing_revision_ = framing->revision;
        cy::camera::CameraIntent intent;
        const f32 yaw = framing_applied_ ? applied_yaw_ : 0.0F;
        const f32 pitch = framing_applied_ ? applied_pitch_ : 0.0F;
        intent.look = cy::Vec2{wrap_angle(framing->yaw - yaw), framing->pitch - pitch};
        intent.zoom = cy::math::clamp(
            (framing->distance - kNearestFraming) / (kFarthestFraming - kNearestFraming), 0.0F,
            1.0F);
        intent.zoom_set = true;
        if (cy::Status applied = camera_.apply_intent(rig_, intent); !applied) {
            return applied;
        }
        applied_yaw_ = framing->yaw;
        applied_pitch_ = framing->pitch;
        framing_applied_ = true;
    }
    camera_.begin_frame();
    cy::camera::EvaluationContext context;
    context.delta_seconds = seconds;
    context.aspect = static_cast<f32>(kViewportWidth) / static_cast<f32>(kViewportHeight);
    const auto evaluated = camera_.evaluate(rig_, context);
    if (!evaluated) {
        return cy::make_unexpected(evaluated.error());
    }
    // The listener hears from where the camera is.
    cy::audio::Listener listener;
    listener.transform = (*evaluated)->pose;
    if (cy::Status heard = audio_.set_listener(listener_, listener); !heard) {
        return heard;
    }
    // THE RIG'S QUERIES, answered by the world the hero walks in, for the next evaluation — once
    // the game has asked for a framing. Before it has, the rig frames the origin it was created
    // on, inside the platform, and an answer about that would pull the first framing of the hero
    // in to nothing.
    if (framing == nullptr) {
        return cy::ok();
    }
    if (cy::Status resolved = camera_collision_->resolve(camera_); !resolved) {
        return resolved;
    }
    const cy::game_backend::CameraCollisionResolver::Stats& stats = camera_collision_->stats();
    camera_queries_ += stats.collision_queries + stats.occlusion_queries;
    collision_frames_ += stats.collisions > 0U ? 1U : 0U;
    nearest_fraction_ = cy::math::min(nearest_fraction_, stats.nearest_fraction);
    return cy::ok();
}

cy::Status ThirdPersonHost::run_stages(cy::ecs::Stage first, cy::ecs::Stage last) noexcept {
    for (auto stage = static_cast<u32>(first); stage <= static_cast<u32>(last); ++stage) {
        if (cy::Status ran =
                script_systems_.run(schedule_, static_cast<cy::ecs::Stage>(stage), nullptr);
            !ran) {
            return ran;
        }
    }
    return cy::ok();
}

const cy::physics::CharacterController* ThirdPersonHost::hero_controller() const noexcept {
    return characters_.get() == nullptr ? nullptr
                                        : characters_->controller(cy::abi::to_abi(hero_.entity()));
}

void ThirdPersonHost::place_hero() noexcept {
    const cy::physics::CharacterController* hero = hero_controller();
    if (hero == nullptr) {
        return;
    }
    // THE FEET, under the capsule; eased over a riser rather than lifted in one tick, and never
    // more than a hand's breadth behind.
    const Vec3 centre = hero->state().transform.translation;
    const f32 feet = centre.y - (hero->description().height * 0.5F);
    if (!presented_) {
        presented_feet_ = feet;
        presented_ = true;
    } else {
        presented_feet_ += (feet - presented_feet_) * (1.0F - std::exp2(-kStep / kStepHalfLife));
        presented_feet_ = cy::math::clamp(presented_feet_, feet - kStepSlack, feet + kStepSlack);
    }
    // THE FACING is the game's: HeroPose, which the hero writes every tick.
    f32 facing = 0.0F;
    if (const cy::abi::ComponentRecord* record = binding_.find("HeroPose"); record != nullptr) {
        const void* bytes = world_.get(hero_.entity(), record->id);
        facing = read_field<f32>(bytes, report_field(binding_, *record, "facing"));
    }
    cy::Transform placement =
        cy::Transform::from_translation(Vec3{centre.x, presented_feet_, centre.z});
    placement.rotation = cy::Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, facing);
    (void)hero_.set_local_transform(placement);
    // Its WORLD transform, which the animation system places the feet's probes with.
    (void)tree_.propagate();
}

void ThirdPersonHost::update_audio(f32 seconds) noexcept {
    audio_adapter_.update(seconds);
    // THE NULL DEVICE RUNS FASTER THAN REAL TIME. A device would take this frame's buffer 16 ms
    // from now, by when the refills the pump just scheduled are long done; the null device takes
    // it at once. So the host waits for them here, which is what keeps the recorded mix free of
    // underruns that only a faster-than-real-time clock could cause.
    while (streams_ != nullptr && streams_->refills_in_flight() != 0) {
        hero_rig_.jobs().wait_for_idle();
    }
    audio_.update(seconds);
    const u32 mixed = audio_device_.advance(mix_scratch_.data(), kFramesPerTick);
    if (options_.mix != nullptr) {
        options_.mix->insert(options_.mix->end(), mix_scratch_.begin(),
                             mix_scratch_.begin() + (static_cast<std::ptrdiff_t>(mixed) * 2));
    }
    peak_voices_ = std::max(peak_voices_, audio_.statistics().active_voices);
}

void ThirdPersonHost::track() noexcept {
    // EVERYTHING THAT MOVES, folded into the digest: the hero's node, the camera's pose and the
    // hero's skinning matrices. Two runs of the same script must agree on every bit of it.
    const cy::Transform hero = hero_placement();
    fold(digest_, &hero.translation, sizeof(hero.translation));
    fold(digest_, &hero.rotation, sizeof(hero.rotation));
    const Vec3 eye = camera_position();
    fold(digest_, &eye, sizeof(eye));
    const cy::Span<const cy::Mat4> pose = hero_pose();
    if (!pose.empty()) {
        fold(digest_, pose.data(), pose.size() * sizeof(cy::Mat4));
        if (!first_pose_taken_ && pose.size() >= kHeroJoints) {
            std::copy(pose.begin(), pose.begin() + kHeroJoints, first_pose_);
            first_pose_taken_ = true;
        }
        for (cy::usize joint = 0; joint < pose.size() && joint < kHeroJoints; ++joint) {
            for (u32 column = 0; column < 4U; ++column) {
                const cy::Vec4 delta =
                    pose[joint].columns[column] - first_pose_[joint].columns[column];
                departure_ = std::max(
                    {departure_, std::fabs(delta.x), std::fabs(delta.y), std::fabs(delta.z)});
            }
        }
    }
    if (animation_ != nullptr) {
        const auto feet = animation_->foot_placement(hero_.entity());
        if (feet && feet->solved) {
            ++placed_frames_;
            deepest_pelvis_ = std::max(deepest_pelvis_, -feet->pelvis);
        }
    }
}

void ThirdPersonHost::shutdown() noexcept {
    if (!started_ && physics_ == nullptr) {
        return;
    }
    started_ = false;
    host_.game = cy::abi::game::GameServices{};
    host_.bind_world(nullptr);
    // The streams before the rig: a refill runs on the rig's job system, and the player's
    // destructor needs none in flight.
    while (streams_ != nullptr && streams_->refills_in_flight() != 0) {
        hero_rig_.jobs().wait_for_idle();
    }
    audio_adapter_.bind_streams(nullptr);
    streams_.reset();
    stream_executor_.reset();
    animation_adapter_.reset();
    animation_.reset();
    hero_rig_.shutdown();
    camera_collision_.reset();
    ground_.reset();
    characters_.reset();
    if (physics_ != nullptr) {
        for (const cy::physics::BodyHandle body : bodies_) {
            (void)physics_->destroy_body(body);
        }
        for (const cy::physics::ShapeHandle shape : shapes_) {
            (void)physics_->destroy_shape(shape);
        }
        bodies_.clear();
        shapes_.clear();
        (void)physics_->destroy_world(physics_world_);
        physics_->shutdown();
        cy::physics::jolt::destroy_server(physics_, allocator_);
        physics_ = nullptr;
    }
}

// --- The hand and the eyes -----------------------------------------------------------------------

cy::Nanoseconds ThirdPersonHost::event_time() noexcept {
    ++events_this_frame_;
    return (static_cast<cy::Nanoseconds>(frame_) * kStepNs) + 1'000'000 +
           (static_cast<cy::Nanoseconds>(events_this_frame_) * 1'000);
}

cy::Status ThirdPersonHost::press_key(cy::input::Key key, bool down) noexcept {
    return input_.inject(keyboard_, cy::input::key_control(key), down ? 1.0F : 0.0F, event_time());
}

Vec3 ThirdPersonHost::camera_position() const noexcept {
    const cy::camera::EvaluatedCamera* evaluated = camera_.evaluated(rig_);
    return evaluated == nullptr ? Vec3{} : evaluated->pose.translation;
}

Vec3 ThirdPersonHost::camera_forward() const noexcept {
    const cy::camera::EvaluatedCamera* evaluated = camera_.evaluated(rig_);
    return evaluated == nullptr ? Vec3{0.0F, 0.0F, -1.0F} : evaluated->pose.forward();
}

f32 ThirdPersonHost::camera_fov() const noexcept {
    const cy::camera::EvaluatedCamera* evaluated = camera_.evaluated(rig_);
    return evaluated == nullptr ? 1.08F : evaluated->lens.vertical_fov_radians();
}

cy::Transform ThirdPersonHost::hero_placement() const noexcept {
    return hero_.local_transform();
}

cy::Transform ThirdPersonHost::drawn_hero() const noexcept {
    return cy::play::carried_forward(hero_before_, hero_placement(),
                                     static_cast<f32>(interpolation_));
}

CyInputPointer ThirdPersonHost::pointer() noexcept {
    CyInputPointer out{};
    out.struct_size = sizeof(out);
    (void)input_adapter_.pointer(0, out);
    return out;
}

void ThirdPersonHost::set_pointer_over_ui(bool over) noexcept {
    input_adapter_.set_pointer_focus(0, true, over);
}

void ThirdPersonHost::ui_event(const CyUiEvent& event) noexcept {
    (void)runtime_.ui_event(event);
}

cy::Span<const cy::Mat4> ThirdPersonHost::hero_pose() const noexcept {
    if (animation_ == nullptr || !animation_->status(hero_.entity())) {
        return {};
    }
    return animation_->poses().current(animation_->pose_of(hero_.entity()));
}

void ThirdPersonHost::print_trace() const noexcept {
    const cy::physics::CharacterController* hero = hero_controller();
    if (hero == nullptr) {
        std::printf("trace frame=%llu no hero\n", static_cast<unsigned long long>(frame_));
        return;
    }
    const cy::physics::CharacterState& state = hero->state();
    const Vec3 at = state.transform.translation;
    const Vec3 eye = camera_position();
    CyCharacterState abi{};
    abi.struct_size = sizeof(abi);
    (void)characters_->state(cy::abi::to_abi(hero_.entity()), abi);
    std::printf(
        "trace frame=%llu centre=(%.2f %.2f %.2f) vel=(%.2f %.2f %.2f) ground=%d "
        "ground_entity=%llu terrain=%llu courtyard=%llu eye=(%.2f %.2f %.2f)",
        static_cast<unsigned long long>(frame_), static_cast<double>(at.x),
        static_cast<double>(at.y), static_cast<double>(at.z), static_cast<double>(state.velocity.x),
        static_cast<double>(state.velocity.y), static_cast<double>(state.velocity.z),
        static_cast<int>(state.ground), static_cast<unsigned long long>(abi.ground_entity),
        static_cast<unsigned long long>(cy::abi::to_abi(terrain_.entity())),
        static_cast<unsigned long long>(cy::abi::to_abi(courtyard_.entity())),
        static_cast<double>(eye.x), static_cast<double>(eye.y), static_cast<double>(eye.z));
    if (animation_ != nullptr) {
        const auto feet = animation_->foot_placement(hero_.entity());
        if (feet) {
            std::printf(
                " feet solved=%d pelvis=%.3f off=(%.3f %.3f) hit=(%d %d) ground=(%.2f %.2f)",
                feet->solved ? 1 : 0, static_cast<double>(feet->pelvis),
                static_cast<double>(feet->offset[0]), static_cast<double>(feet->offset[1]),
                feet->hit[0] ? 1 : 0, feet->hit[1] ? 1 : 0, static_cast<double>(feet->ground[0].y),
                static_cast<double>(feet->ground[1].y));
        }
    }
    std::printf("\n");
}

Observation ThirdPersonHost::observe() noexcept {
    Observation seen;
    if (const cy::physics::CharacterController* hero = hero_controller(); hero != nullptr) {
        seen.hero = true;
        const Vec3 centre = hero->state().transform.translation;
        seen.hero_feet = Vec3{centre.x, centre.y - (hero->description().height * 0.5F), centre.z};
    }
    seen.collision_frames = collision_frames_;
    seen.nearest_fraction = nearest_fraction_;
    seen.camera_queries = camera_queries_;
    seen.departure = departure_;
    seen.deepest_pelvis = deepest_pelvis_;
    seen.placed_frames = placed_frames_;
    seen.peak_voices = peak_voices_;
    seen.digest = digest_;
    if (const cy::abi::ComponentRecord* record = binding_.find("HeroPose"); record != nullptr) {
        seen.hero_facing = read_field<f32>(world_.get(hero_.entity(), record->id),
                                           report_field(binding_, *record, "facing"));
    }
    const cy::abi::ComponentRecord* record = binding_.find("HeroReport");
    if (record == nullptr) {
        return seen;
    }
    const void* bytes = world_.get(hero_.entity(), record->id);
    const auto field = [&](const char* name) {
        return read_field<f32>(bytes, report_field(binding_, *record, name));
    };
    Observation::Game& game = seen.game;
    game.found = bytes != nullptr;
    game.ticks = field("ticks");
    game.grounded = field("groundedTicks");
    game.airborne = field("airborneTicks");
    game.stepped_up = field("steppedUp");
    game.jumps = field("jumps");
    game.landings = field("landings");
    game.footsteps = field("footsteps");
    game.stone_steps = field("stoneSteps");
    game.grass_steps = field("grassSteps");
    game.highest = field("highest");
    game.fastest = field("fastest");
    game.run_ticks = field("runTicks");
    game.walk_ticks = field("walkTicks");
    game.feet_solved = field("feetSolved");
    game.feet_adjusted = field("feetAdjusted");
    game.pelvis_drop = field("pelvisDrop");
    game.states = field("states");
    game.camera_pulled = field("cameraPulled");
    game.camera_nearest = bytes == nullptr ? 1.0F : field("cameraNearest");
    game.camera_yaw = field("cameraYaw");
    game.ambience = field("ambience");
    game.dropped = field("dropped");
    return seen;
}

}  // namespace sample::tps
