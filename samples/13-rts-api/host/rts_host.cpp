// SPDX-License-Identifier: MIT
#include "rts_host.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/physics/reference/server.h>
#include <cy/ui/layout.h>
#include <cy/ui/text/builtin_font.h>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "level.h"

namespace sample::rts {
namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;

constexpr f32 kStep = 1.0F / 60.0F;
constexpr cy::Nanoseconds kStepNs = 16'666'666;
constexpr u32 kPathLatencyTicks = 3;

constexpr u32 kSampleRate = 48000;
constexpr u32 kBlockFrames = 200;
constexpr u32 kFramesPerTick = kSampleRate / 60;  // 800, a whole number of blocks

/// The window the primary view renders into.
constexpr u32 kViewportWidth = 1280;
constexpr u32 kViewportHeight = 720;

/// The RTS rig's topology and fixed framing: a target, a follow from high up and behind, and a
/// look-at. Where the target is, is the game's; this is the camera's lens and angle.
constexpr Vec3 kCameraOffset{0.0F, 22.0F, 14.0F};

[[nodiscard]] cy::camera::RigNodeDesc rig_node(const char* id, const char* input,
                                               cy::camera::RigNodeKind kind) noexcept {
    cy::camera::RigNodeDesc desc;
    desc.id = cy::Name::intern(id);
    desc.input = input == nullptr ? cy::Name{} : cy::Name::intern(input);
    desc.kind = kind;
    return desc;
}

[[nodiscard]] cy::camera::RigDefinition rts_rig(cy::Allocator& allocator) noexcept {
    using cy::camera::RigNodeKind;
    cy::camera::RigDefinition definition(allocator);
    definition.name = cy::Name::intern("rts");
    (void)definition.nodes.push_back(rig_node("target", nullptr, RigNodeKind::Target));
    cy::camera::RigNodeDesc follow = rig_node("follow", "target", RigNodeKind::Follow);
    follow.follow.space = cy::camera::FollowSpace::World;
    follow.follow.offset = kCameraOffset;
    follow.follow.position_half_life = 0.0F;
    (void)definition.nodes.push_back(follow);
    cy::camera::RigNodeDesc look = rig_node("look", "follow", RigNodeKind::LookAt);
    look.look_at.rotation_half_life = 0.0F;
    (void)definition.nodes.push_back(look);
    (void)definition.nodes.push_back(rig_node("output", "look", RigNodeKind::Output));
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

/// A field of the game's `RtsReport`, by name, or null.
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

/// Every element under `at` whose type name is `name`, depth first.
void collect(const cy::ui::ElementStore& store, cy::ui::ElementId at, std::string_view name,
             cy::Array<cy::ui::ElementId>& out) noexcept {
    if (store.type_of(at).text() == name) {
        (void)out.push_back(at);
    }
    const cy::ui::Hierarchy* node = store.hierarchy(at);
    for (cy::ui::ElementId child = node != nullptr ? node->first_child : cy::ui::kNoElement;
         child.is_valid();) {
        collect(store, child, name, out);
        const cy::ui::Hierarchy* next = store.hierarchy(child);
        child = next != nullptr ? next->next_sibling : cy::ui::kNoElement;
    }
}

[[nodiscard]] bool shown(const cy::ui::ElementStore& store, cy::ui::ElementId element) noexcept {
    return cy::ui::has_flag(store.flags(element), cy::ui::ElementFlags::Visible);
}

/// `text` into `out`, cut to fit.
template <cy::usize N>
void copy_text(std::string_view text, char (&out)[N]) noexcept {
    (void)std::snprintf(out, N, "%.*s", static_cast<int>(text.size()), text.data());
}

template <class T>
[[nodiscard]] T read_field(const void* bytes, const cy::abi::FieldRecord* field) noexcept {
    T value{};
    if (bytes != nullptr && field != nullptr) {
        std::memcpy(&value, static_cast<const unsigned char*>(bytes) + field->offset, sizeof(T));
    }
    return value;
}

}  // namespace

RtsHost::RtsHost(cy::Allocator& allocator, const HostOptions& options) noexcept
    : allocator_(allocator),
      options_(options),
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
      text_(allocator),
      ui_store_(allocator),
      nav_mesh_(allocator, cy::Name::intern("rts.ground"), kLevelSize),
      nav_queue_(allocator, nav_mesh_, kPathLatencyTicks),
      nav_worlds_(allocator),
      input_adapter_(input_),
      camera_adapter_(camera_, allocator),
      audio_adapter_(audio_, allocator, &tree_),
      spawn_adapter_(tree_, allocator),
      motion_(tree_),
      roll_(allocator, world_),
      click_samples_(allocator),
      mix_scratch_(allocator) {}

RtsHost::~RtsHost() {
    shutdown();
}

cy::Status RtsHost::start(const char** detail) noexcept {
    *detail = "";
    const struct {
        const char* name;
        cy::Status (RtsHost::*step)() noexcept;
    } steps[] = {
        {"scene", &RtsHost::start_scene},           {"input", &RtsHost::start_input},
        {"camera", &RtsHost::start_camera},         {"physics", &RtsHost::start_physics},
        {"navigation", &RtsHost::start_navigation}, {"audio", &RtsHost::start_audio},
        {"props", &RtsHost::start_props},
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
    cy::game_backend::bind(host_, nav_adapter_.get());
    cy::game_backend::bind(host_, &audio_adapter_);
    cy::game_backend::bind(host_, &spawn_adapter_);
    cy::game_backend::bind_bodies(host_, body_adapter_.get());
    cy::game_backend::bind_characters(host_, characters_.get());
    cy::game_backend::bind_scene(host_, &bridge_);
    if (options_.ui) {
        *detail = "interface";
        if (cy::Status ui = start_ui(); !ui) {
            return ui;
        }
        cy::game_backend::bind(host_, ui_adapter_.get());
    }
    started_ = true;

    if (cy::Status loaded = load_module(detail); !loaded) {
        return loaded;
    }
    *detail = "systems";
    if (cy::Status scheduled = start_systems(); !scheduled) {
        return scheduled;
    }
    if (cy::Status attached = attach_behaviours(detail); !attached) {
        return attached;
    }
    *detail = "";
    return cy::ok();
}

cy::Status RtsHost::start_ui() noexcept {
    if (cy::Status text = text_server_.start(cy::text::TextServerConfig{}); !text) {
        return text;
    }
    if (cy::Status painter = text_.start(text_server_, cy::ui::builtin_font(), 1); !painter) {
        return painter;
    }
    auto root = ui_store_.create(cy::ui::kNoElement, cy::Name::intern("screen"));
    if (!root) {
        return cy::make_unexpected(root.error());
    }
    ui_root_ = *root;
    ui_store_.layout_input(ui_root_)->model = cy::ui::LayoutModel::Absolute;
    auto adapter = cy::make_unique<cy::game_backend::UiAdapter>(allocator_, allocator_, ui_store_,
                                                                text_, ui_root_);
    if (!adapter) {
        return cy::make_unexpected(adapter.error());
    }
    ui_adapter_ = std::move(*adapter);
    return ui_adapter_->start();
}

cy::Status RtsHost::ui_layout() noexcept {
    cy::ui::ScaleSettings settings;
    settings.mode = cy::ui::ScaleMode::FixedPixel;
    return ui_adapter_->layout(
        settings, cy::Vec2{static_cast<f32>(kViewportWidth), static_cast<f32>(kViewportHeight)});
}

cy::Status RtsHost::ui_frame() noexcept {
    // Laid out first, so the pointer is tested against what the last frame's writes produced; then
    // routed, and the game's pointer told whether it is over the interface before `onUpdate` reads
    // it; then the clicks delivered to their owners, in the frame's phase.
    if (cy::Status laid = ui_layout(); !laid) {
        return laid;
    }
    CyInputPointer pointer{};
    pointer.struct_size = sizeof(pointer);
    if (input_adapter_.pointer(0, pointer) != CY_RESULT_OK) {
        return cy::fail(cy::ErrorCode::Internal, "the input adapter has no pointer for user 0");
    }
    const cy::Vec2 at{pointer.position[0], pointer.position[1]};
    if (cy::Status routed =
            ui_adapter_->route_pointer(at, pointer.buttons_pressed, pointer.buttons_released);
        !routed) {
        return routed;
    }
    input_adapter_.set_pointer_focus(0, true, ui_adapter_->pointer_over());
    for (const CyUiEvent& event : ui_adapter_->events()) {
        (void)runtime_.ui_event(event);
    }
    ui_adapter_->clear_events();
    return cy::ok();
}

cy::Status RtsHost::attach_behaviours(const char** detail) noexcept {
    // Every script type the module registered becomes a scene behaviour of the same name.
    const auto synced = bridge_.sync_types();
    if (!synced) {
        return cy::make_unexpected(synced.error());
    }
    if (!options_.behaviours) {
        return cy::ok();
    }
    // Attached to level nodes, so the tree's pump drives their tree callbacks and their `@Node`
    // paths resolve against `/Level`.
    const struct {
        const char* name = nullptr;
        cy::scene::Node node;
    } attachments[] = {{"Commander", commander_}, {"Scout", scout_}};
    for (const auto& attachment : attachments) {
        *detail = attachment.name;
        if (cy::Status attached = bridge_.attach(attachment.node, attachment.name); !attached) {
            return attached;
        }
    }
    return cy::ok();
}

cy::Status RtsHost::start_systems() noexcept {
    // The native observer reads the column the Swift system writes; registered first, so the
    // scheduler has to order the Swift system against it by their declarations alone.
    if (const cy::abi::ComponentRecord* veterancy = binding_.find("Veterancy");
        veterancy != nullptr) {
        const auto installed = roll_.install(schedule_, veterancy->id);
        if (!installed) {
            return cy::make_unexpected(installed.error());
        }
        roll_id_ = *installed;
    }
    if (options_.systems) {
        const auto installed = script_systems_.install(schedule_);
        if (!installed) {
            return cy::make_unexpected(installed.error());
        }
    }
    return schedule_.build();
}

cy::Status RtsHost::start_scene() noexcept {
    if (cy::Status ready = world_.initialize(); !ready) {
        return ready;
    }
    if (cy::Status ready = tree_.initialize(); !ready) {
        return ready;
    }
    // The level's named nodes: what `@Node("../Barracks")` and `@Node("../Crate")` resolve to,
    // and the two nodes the script behaviours are attached to.
    const auto level = tree_.create_node(cy::Name::intern("Level"), tree_.root());
    if (!level) {
        return cy::make_unexpected(level.error());
    }
    cy::scene::Node* nodes[] = {nullptr, &crate_, &commander_, &scout_};
    const char* names[] = {"Barracks", "Crate", "Commander", "Scout"};
    for (cy::usize index = 0; index < 4U; ++index) {
        const auto made = tree_.create_node(cy::Name::intern(names[index]), *level);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        if (nodes[index] != nullptr) {
            *nodes[index] = *made;
        }
    }
    return spawn_adapter_.add_prefab(kWorkerPrefab, worker_prefab());
}

cy::Status RtsHost::start_props() noexcept {
    // The crate: a 1 m dynamic box of 20 kg on collision layer 2, owned by the Crate node, so a
    // script that resolved `../Crate` can push it. Where it goes when pushed is the server's.
    cy::physics::ShapeDescription box;
    box.type = cy::physics::ShapeType::Box;
    box.half_extents = Vec3{0.5F, 0.5F, 0.5F};
    const auto shape = physics_->create_shape(box);
    if (!shape) {
        return cy::make_unexpected(shape.error());
    }
    crate_shape_ = *shape;
    cy::physics::ColliderDescription collider;
    collider.shape = crate_shape_;
    collider.filter.layer = kPropLayer;
    cy::physics::BodyDescription body;
    body.motion = cy::physics::MotionType::Dynamic;
    body.mass = 20.0F;
    body.transform = cy::Transform::from_translation(kCrateStart);
    body.colliders = &collider;
    body.collider_count = 1;
    body.user_data = cy::abi::to_abi(crate_.entity());
    const auto created = physics_->create_body(physics_world_, body);
    if (!created) {
        return cy::make_unexpected(created.error());
    }
    crate_body_ = *created;
    crate_start_z_ = kCrateStart.z;
    level_bodies_->add_prop(cy::abi::to_abi(crate_.entity()), crate_body_);
    return cy::ok();
}

cy::Status RtsHost::start_input() noexcept {
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
    const auto keyboard = connect(input_, cy::input::DeviceKind::Keyboard, "rts-keyboard");
    const auto mouse = connect(input_, cy::input::DeviceKind::Mouse, "rts-mouse");
    if (!keyboard || !mouse) {
        return cy::fail(cy::ErrorCode::Unavailable, "the scripted keyboard or mouse was refused");
    }
    keyboard_ = *keyboard;
    mouse_ = *mouse;
    return declare_input(input_, allocator_);
}

cy::Status RtsHost::start_camera() noexcept {
    if (cy::Status configured = camera_.configure(cy::camera::CameraServerConfig{}); !configured) {
        return configured;
    }
    if (cy::Status ready = camera_.initialize(); !ready) {
        return ready;
    }
    const auto definition = camera_.create_definition(rts_rig(allocator_));
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
    const auto evaluated = camera_.evaluate(rig_, context);
    return evaluated ? cy::ok() : cy::make_unexpected(evaluated.error());
}

cy::Status RtsHost::start_physics() noexcept {
    const auto made = cy::physics::reference::create_server(allocator_);
    if (!made) {
        return cy::make_unexpected(made.error());
    }
    physics_ = *made;
    if (cy::Status ready = physics_->initialize(); !ready) {
        return ready;
    }
    cy::physics::WorldDescription description;
    description.name = cy::Name::intern("rts");
    description.body_capacity = 256;
    const auto world = physics_->create_world(description);
    if (!world) {
        return cy::make_unexpected(world.error());
    }
    physics_world_ = *world;

    // The ground: the top face at y = 0 over the navigation tile.
    cy::physics::ShapeDescription slab;
    slab.type = cy::physics::ShapeType::Box;
    slab.half_extents = Vec3{kLevelSize * 0.5F, 0.5F, kLevelSize * 0.5F};
    const auto shape = physics_->create_shape(slab);
    if (!shape) {
        return cy::make_unexpected(shape.error());
    }
    ground_shape_ = *shape;
    cy::physics::ColliderDescription collider;
    collider.shape = ground_shape_;
    collider.filter.layer = kGroundLayer;
    cy::physics::BodyDescription body;
    body.motion = cy::physics::MotionType::Static;
    body.transform =
        cy::Transform::from_translation(Vec3{kLevelSize * 0.5F, -0.5F, kLevelSize * 0.5F});
    body.colliders = &collider;
    body.collider_count = 1;
    const auto ground = physics_->create_body(physics_world_, body);
    if (!ground) {
        return cy::make_unexpected(ground.error());
    }
    ground_ = *ground;
    return cy::ok();
}

cy::Status RtsHost::start_navigation() noexcept {
    const auto ids = cy::navigation::NavComponents::register_all(world_);
    if (!ids) {
        return cy::make_unexpected(ids.error());
    }
    nav_components_ = *ids;
    if (const auto added = nav_mesh_.add_tile(make_ground_tile(allocator_)); !added) {
        return cy::make_unexpected(added.error());
    }
    if (cy::Status bound = nav_worlds_.bind(0, nav_mesh_, nav_queue_); !bound) {
        return bound;
    }
    auto bodies = cy::make_unique<UnitBodies>(allocator_, allocator_, world_, nav_components_,
                                              *physics_, physics_world_);
    if (!bodies) {
        return cy::make_unexpected(bodies.error());
    }
    bodies_ = std::move(*bodies);
    auto characters = cy::make_unique<cy::game_backend::CharacterAdapter>(
        allocator_, allocator_, *physics_, physics_world_);
    if (!characters) {
        return cy::make_unexpected(characters.error());
    }
    characters_ = std::move(*characters);
    auto level_bodies = cy::make_unique<LevelBodies>(allocator_, *bodies_, *characters_);
    if (!level_bodies) {
        return cy::make_unexpected(level_bodies.error());
    }
    level_bodies_ = std::move(*level_bodies);
    auto physics = cy::make_unique<cy::game_backend::PhysicsQueryAdapter>(
        allocator_, allocator_, *physics_, physics_world_, *level_bodies_);
    auto body_writes = cy::make_unique<cy::game_backend::PhysicsBodyAdapter>(allocator_, *physics_,
                                                                             *level_bodies_);
    auto navigation = cy::make_unique<cy::game_backend::NavigationAdapter>(
        allocator_, allocator_, world_, nav_components_, nav_worlds_, host_.game.clock);
    if (!physics || !body_writes || !navigation) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "a physics or navigation adapter was refused");
    }
    physics_adapter_ = std::move(*physics);
    body_adapter_ = std::move(*body_writes);
    nav_adapter_ = std::move(*navigation);
    nav_adapter_->set_motion(&motion_);
    return cy::ok();
}

cy::Status RtsHost::start_audio() noexcept {
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
    config.voice_capacity = 16;
    if (cy::Status configured = audio_.configure(config); !configured) {
        return configured;
    }
    if (cy::Status ready = audio_.initialize_with(audio_device_); !ready) {
        return ready;
    }
    if (cy::Status made = make_click(click_samples_, kSampleRate); !made) {
        return made;
    }
    if (cy::Status sized = mix_scratch_.resize(static_cast<cy::usize>(kFramesPerTick) * 2U);
        !sized) {
        return sized;
    }
    cy::audio::ClipDescription clip;
    clip.name = cy::Name::intern("unit.arrived.pcm");
    clip.samples = click_samples_.data();
    clip.frame_count = static_cast<u32>(click_samples_.size());
    clip.sample_rate = kSampleRate;
    const auto created = audio_.create_clip(clip);
    if (!created) {
        return cy::make_unexpected(created.error());
    }
    return audio_adapter_.add_cue(cy::Name::intern(kArrivedCue), *created);
}

cy::Status RtsHost::load_module(const char** detail) noexcept {
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

cy::Status RtsHost::frame() noexcept {
    // The frame boundary: tree-shape callbacks queued since the last frame are delivered here, in
    // no phase — `onEnterTree` and `onReady` for the behaviours attached at start, on frame 0.
    if (cy::Status pumped = tree_.pump(); !pumped) {
        return pumped;
    }
    if (cy::Status ticked = fixed_tick(); !ticked) {
        return ticked;
    }
    input_adapter_.begin_frame();
    cy::camera::EvaluationContext context;
    context.delta_seconds = kStep;
    if (const auto evaluated = camera_.evaluate(rig_, context); !evaluated) {
        return cy::make_unexpected(evaluated.error());
    }
    host_.game.clock.interpolation = 0.0;
    if (ui_adapter_) {
        if (cy::Status routed = ui_frame(); !routed) {
            return routed;
        }
    }
    runtime_.frame_update(kStep);
    if (cy::Status staged = run_stages(cy::ecs::Stage::Frame, cy::ecs::Stage::UI); !staged) {
        return staged;
    }
    // What a renderer would flatten: this frame's writes, laid out.
    if (ui_adapter_) {
        if (cy::Status laid = ui_layout(); !laid) {
            return laid;
        }
    }
    update_audio();
    ++frame_;
    events_this_frame_ = 0;
    return cy::ok();
}

cy::Status RtsHost::fixed_tick() noexcept {
    const u64 tick = frame_ + 1U;
    host_.game.clock.tick = tick;
    input_adapter_.observe_pending();
    input_.resolve_tick(tick, static_cast<cy::Nanoseconds>(tick) * kStepNs, kStep);
    runtime_.fixed_update(kStep);
    if (cy::Status staged =
            run_stages(cy::ecs::Stage::PreSimulation, cy::ecs::Stage::PostSimulation);
        !staged) {
        return staged;
    }
    if (cy::Status navigated = nav_adapter_->update(kStep); !navigated) {
        return navigated;
    }
    if (cy::Status followed = bodies_->sync(); !followed) {
        return followed;
    }
    cy::physics::StepInput step;
    step.delta_seconds = kStep;
    step.tick = tick;
    return physics_->step(physics_world_, step);
}

cy::Status RtsHost::run_stages(cy::ecs::Stage first, cy::ecs::Stage last) noexcept {
    // Serially, on this thread: the result is the job system's by construction (system.h), and a
    // sample has no job system to hand.
    for (auto stage = static_cast<u32>(first); stage <= static_cast<u32>(last); ++stage) {
        if (cy::Status ran =
                script_systems_.run(schedule_, static_cast<cy::ecs::Stage>(stage), nullptr);
            !ran) {
            return ran;
        }
    }
    return cy::ok();
}

void RtsHost::update_audio() noexcept {
    audio_adapter_.update(kStep);
    audio_.update(kStep);
    (void)audio_device_.advance(mix_scratch_.data(), kFramesPerTick);
}

void RtsHost::shutdown() noexcept {
    if (!started_ && physics_ == nullptr) {
        return;
    }
    started_ = false;
    // Unbind first: a behaviour destroyed after this (the runtime is a member and goes last) that
    // calls a game service gets UNAVAILABLE rather than an adapter that no longer exists.
    host_.game = cy::abi::game::GameServices{};
    host_.bind_world(nullptr);
    ui_adapter_.reset();
    nav_adapter_.reset();
    physics_adapter_.reset();
    body_adapter_.reset();
    level_bodies_.reset();
    // Each controller destroys its own body and shape, so the characters go before the world.
    characters_.reset();
    if (bodies_) {
        bodies_->clear();
        bodies_.reset();
    }
    if (physics_ != nullptr) {
        (void)physics_->destroy_body(crate_body_);
        (void)physics_->destroy_shape(crate_shape_);
        (void)physics_->destroy_body(ground_);
        (void)physics_->destroy_shape(ground_shape_);
        (void)physics_->destroy_world(physics_world_);
        physics_->shutdown();
        cy::physics::reference::destroy_server(physics_, allocator_);
        physics_ = nullptr;
    }
}

// --- The hand ------------------------------------------------------------------------------------

cy::Nanoseconds RtsHost::event_time() noexcept {
    // Inside this frame's tick window, one microsecond apart so the order they were made in is
    // the order they resolve in.
    ++events_this_frame_;
    return (static_cast<cy::Nanoseconds>(frame_) * kStepNs) + 1'000'000 +
           (static_cast<cy::Nanoseconds>(events_this_frame_) * 1'000);
}

cy::Status RtsHost::press_key(cy::input::Key key, bool down) noexcept {
    return input_.inject(keyboard_, cy::input::key_control(key), down ? 1.0F : 0.0F, event_time());
}

cy::Status RtsHost::move_pointer(f32 x, f32 y) noexcept {
    using cy::input::MouseControl;
    const cy::Nanoseconds at = event_time();
    if (cy::Status moved = input_.inject(mouse_, mouse_control(MouseControl::PositionX), x, at);
        !moved) {
        return moved;
    }
    return input_.inject(mouse_, mouse_control(MouseControl::PositionY), y, at);
}

cy::Status RtsHost::press_button(cy::input::MouseControl button, bool down) noexcept {
    return input_.inject(mouse_, cy::input::mouse_control(button), down ? 1.0F : 0.0F,
                         event_time());
}

bool RtsHost::project(Vec3 point, f32& out_x, f32& out_y) noexcept {
    CyCamera camera = 0;
    if (camera_adapter_.active(camera) != CY_RESULT_OK) {
        return false;
    }
    const f32 xyz[3] = {point.x, point.y, point.z};
    CyScreenPoint screen{};
    if (camera_adapter_.world_to_screen(camera, cy::Span<const f32>(xyz, 3),
                                        cy::Span<CyScreenPoint>(&screen, 1)) != CY_RESULT_OK ||
        (screen.flags & CY_SCREEN_POINT_ON_SCREEN) == 0U) {
        return false;
    }
    out_x = screen.position[0];
    out_y = screen.position[1];
    return true;
}

bool RtsHost::ui_centre(const char* name, f32& out_x, f32& out_y) const noexcept {
    if (!ui_adapter_) {
        return false;
    }
    cy::Array<cy::ui::ElementId> found(allocator_);
    collect(ui_store_, ui_root_, name, found);
    if (found.empty()) {
        return false;
    }
    const cy::ui::Rect& rect = ui_store_.layout_output(found[0])->rect;
    out_x = rect.x + (rect.width * 0.5F);
    out_y = rect.y + (rect.height * 0.5F);
    return rect.width > 0.0F && rect.height > 0.0F;
}

// --- The eyes ------------------------------------------------------------------------------------

Vec3 RtsHost::unit_position(CyEntity unit) const noexcept {
    Vec3 position{};
    (void)motion_.position(unit, position);
    return position;
}

CyNavPathStatus RtsHost::unit_status(CyEntity unit) const noexcept {
    CyNavAgentState state{};
    state.struct_size = sizeof(state);
    if (nav_adapter_.get() == nullptr || nav_adapter_->agent_state(unit, state) != CY_RESULT_OK) {
        return CY_NAV_PATH_STATUS_IDLE;
    }
    return static_cast<CyNavPathStatus>(state.status);
}

Vec3 RtsHost::camera_position() const noexcept {
    const cy::camera::EvaluatedCamera* evaluated = camera_.evaluated(rig_);
    return evaluated == nullptr ? Vec3{} : evaluated->pose.translation;
}

Observation RtsHost::observe() noexcept {
    Observation seen;
    seen.units = bodies_.get() == nullptr ? 0U : bodies_->count();
    seen.agents = nav_adapter_.get() == nullptr ? 0U : nav_adapter_->agent_count();
    seen.active_voices = audio_.statistics().active_voices;

    cy::Array<cy::scene::Node> roots(allocator_);
    if (tree_.root().children(roots)) {
        for (const cy::scene::Node& node : roots) {
            // Siblings get unique names ("Worker", then a suffixed one), so match the prefix.
            seen.workers += node.name().text().starts_with("Worker") ? 1U : 0U;
        }
    }

    seen.systems.installed = script_systems_.installed();
    seen.systems.runs = script_systems_.runs("trainUnits");
    seen.systems.rows = roll_.rows();
    seen.systems.most = roll_.most();
    const cy::ecs::SystemId swift = script_systems_.id_of("trainUnits");
    seen.systems.ordered = roll_id_ != cy::ecs::kInvalidSystem &&
                           swift != cy::ecs::kInvalidSystem &&
                           schedule_.ordered_before(cy::ecs::Stage::Simulation, roll_id_, swift);
    observe_scout(seen);
    observe_hud(seen);

    const cy::abi::ComponentRecord* record = binding_.find("RtsReport");
    if (record == nullptr) {
        return seen;
    }
    const void* bytes = world_.get(commander_.entity(), record->id);
    const auto field = [&](const char* name) { return report_field(binding_, *record, name); };
    seen.game.found = bytes != nullptr;
    seen.game.selected = read_field<CyEntity>(bytes, field("selected"));
    seen.game.orders = read_field<f32>(bytes, field("orders"));
    seen.game.arrivals = read_field<f32>(bytes, field("arrivals"));
    seen.game.cues = read_field<f32>(bytes, field("cues"));
    seen.game.spawns = read_field<f32>(bytes, field("spawns"));
    seen.tree.entered = read_field<f32>(bytes, field("entered"));
    seen.tree.readied = read_field<f32>(bytes, field("readied"));
    seen.tree.barracks_found = read_field<f32>(bytes, field("barracksFound"));
    seen.hud.mounted = read_field<f32>(bytes, field("hud"));
    seen.hud.builds = read_field<f32>(bytes, field("hudBuilds"));
    seen.hud.heard = read_field<f32>(bytes, field("hudClicks"));
    return seen;
}

void RtsHost::observe_hud(Observation& seen) noexcept {
    if (!ui_adapter_) {
        return;
    }
    seen.hud.elements = ui_adapter_->elements();
    seen.hud.clicks = ui_adapter_->clicks();
    cy::Array<cy::ui::ElementId> found(allocator_);
    const auto first = [&](const char* name) {
        found.clear();
        collect(ui_store_, ui_root_, name, found);
        return found.empty() ? cy::ui::kNoElement : found[0];
    };
    copy_text(text_.text_of(first("gold")), seen.hud.gold);
    copy_text(text_.text_of(first("wood")), seen.hud.wood);
    copy_text(text_.text_of(first("food")), seen.hud.food);
    copy_text(text_.text_of(first("title")), seen.hud.title);
    copy_text(text_.text_of(first("unit-health")), seen.hud.health);
    seen.hud.button = first("build-button").is_valid();
    if (const cy::ui::ElementId bar = first("health-bar"); bar.is_valid()) {
        const cy::ui::Hierarchy* node = ui_store_.hierarchy(bar);
        seen.hud.fill = ui_store_.layout_output(node->first_child)->rect.width;
    }
    const auto count_shown = [&](const char* name) {
        found.clear();
        collect(ui_store_, ui_root_, name, found);
        cy::u32 visible = 0;
        for (const cy::ui::ElementId element : found) {
            visible += shown(ui_store_, element) ? 1U : 0U;
        }
        return visible;
    };
    seen.hud.rows = count_shown("unit-row");
    seen.hud.dots = count_shown("unit");
}

void RtsHost::observe_scout(Observation& seen) noexcept {
    if (physics_ != nullptr && !crate_body_.is_null()) {
        const auto state = physics_->body_state(crate_body_);
        seen.scout.crate_moved = state ? state->transform.translation.z - crate_start_z_ : 0.0F;
    }
    const cy::abi::ComponentRecord* record = binding_.find("ScoutReport");
    if (record == nullptr) {
        return;
    }
    const void* bytes = world_.get(scout_.entity(), record->id);
    const auto field = [&](const char* name) { return report_field(binding_, *record, name); };
    seen.scout.found = bytes != nullptr;
    seen.scout.crate_found = read_field<f32>(bytes, field("crateFound"));
    seen.scout.hero_x = read_field<f32>(bytes, field("heroX"));
    seen.scout.hero_ground = read_field<f32>(bytes, field("heroGround"));
    seen.scout.airborne = read_field<f32>(bytes, field("airborne"));
    seen.scout.kicks = read_field<f32>(bytes, field("kicks"));
    seen.scout.crate_speed = read_field<f32>(bytes, field("crateSpeed"));
}

}  // namespace sample::rts
