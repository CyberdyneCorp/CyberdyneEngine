// SPDX-License-Identifier: MIT
// The engine side of samples/16-menus. See menus_host.h.

#include "menus_host.h"

#include <cy/abi/errors.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/game_backend/illumination_backend.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/upscale/upscale.h>
#include <cy/ui/text/builtin_font.h>
#if defined(CY_MENUS_INTERFACE_FONT)
#    include <cy/ui/text/interface_font.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "illumination.h"

namespace sample::menus {
namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec2;

constexpr f32 kStep = 1.0F / 60.0F;
constexpr cy::Nanoseconds kStepNs = 16'666'667;
constexpr u32 kSampleRate = 48000;
constexpr u32 kBlockFrames = 200;
constexpr u32 kFramesPerTick = kSampleRate / 60;
/// The interface's priority on the user's stack: above the game's context, as a modal is.
constexpr cy::i32 kInterfacePriority = 100;
/// The buses the mixer has and the settings document binds, in that order.
constexpr const char* kBuses[4] = {"Master", "Music", "SFX", "Voice"};
constexpr const char* kBindingNames[4] = {"volume.master", "volume.music", "volume.sfx",
                                          "volume.voice"};
/// The resolutions the display offers.
constexpr CyVideoMode kModes[] = {{1280, 720}, {1920, 1080}, {2560, 1440}};

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

[[nodiscard]] cy::input::Binding simple(cy::input::ActionId action,
                                        cy::input::Control control) noexcept {
    cy::input::Binding binding;
    binding.action = action;
    binding.kind = cy::input::BindingKind::Simple;
    binding.component_count = 1;
    binding.components[0].control = control;
    binding.components[0].weight = cy::Vec3{1.0F, 0.0F, 0.0F};
    binding.trigger.kind = cy::input::TriggerKind::Down;
    return binding;
}

/// The game's actions: their names, stable ids, and the keyboard and gamepad controls they ship
/// with. What the controls tab rebinds, and what a content update may change under the player's
/// overrides.
struct GameAction {
    const char* name;
    u32 stable;
    cy::input::Key key;
    cy::input::GamepadControl pad;
};
constexpr GameAction kGameActions[] = {
    {"Game.Jump", 0x4D450001U, cy::input::Key::Space, cy::input::GamepadControl::South},
    {"Game.Interact", 0x4D450002U, cy::input::Key::E, cy::input::GamepadControl::West},
    {"Game.Inventory", 0x4D450003U, cy::input::Key::I, cy::input::GamepadControl::North},
    {"Game.Sprint", 0x4D450004U, cy::input::Key::LeftShift, cy::input::GamepadControl::LeftStick},
    {"Game.Pause", 0x4D450005U, cy::input::Key::Escape, cy::input::GamepadControl::Start},
};

/// A field of the game's `MenusReport`, by name, read from its bytes.
[[nodiscard]] f32 report_field(const cy::abi::World& binding,
                               const cy::abi::ComponentRecord& record, const void* bytes,
                               const char* name) noexcept {
    for (u32 index = 0; index < record.field_count; ++index) {
        const cy::abi::FieldRecord* field = binding.field(record, index);
        if (field != nullptr && bytes != nullptr && std::strcmp(field->name, name) == 0) {
            f32 value = 0.0F;
            std::memcpy(&value, static_cast<const unsigned char*>(bytes) + field->offset,
                        sizeof(value));
            return value;
        }
    }
    return 0.0F;
}

}  // namespace

// --- The settings document's bindings ------------------------------------------------------------

/// `volume.master` and the rest: each bus's gain, read every frame and written by a slider.
class MenusHost::BusBindings final : public cy::ui::DocumentBindings {
public:
    BusBindings(cy::audio::AudioServer& audio, const cy::audio::BusHandle (&buses)[4]) noexcept {
        for (u32 index = 0; index < 4; ++index) {
            sources_[index].audio = &audio;
            sources_[index].bus = buses[index];
        }
    }

    [[nodiscard]] cy::ui::BindingSource* find(std::string_view name) noexcept override {
        for (u32 index = 0; index < 4; ++index) {
            if (name == kBindingNames[index]) {
                return &sources_[index];
            }
        }
        return nullptr;
    }

private:
    class Gain final : public cy::ui::BindingSource {
    public:
        [[nodiscard]] cy::ui::BoundValue read() noexcept override {
            cy::ui::BoundValue value;
            value.kind = cy::ui::BoundValue::Kind::Number;
            value.number = gain();
            seen = value.number;
            return value;
        }
        [[nodiscard]] bool changed() const noexcept override { return gain() != seen; }
        [[nodiscard]] cy::Status write(const cy::ui::BoundValue& value) noexcept override {
            seen = value.number;
            return audio->buses().set_volume(bus, value.number);
        }

        [[nodiscard]] f32 gain() const noexcept {
            const cy::audio::BusDescription* description = audio->buses().description(bus);
            return description != nullptr ? description->volume : 0.0F;
        }

        cy::audio::AudioServer* audio = nullptr;
        cy::audio::BusHandle bus;
        f32 seen = -1.0F;
    };
    Gain sources_[4];
};

// --- The host ------------------------------------------------------------------------------------

MenusHost::MenusHost(cy::Allocator& allocator, HostOptions options) noexcept
    : allocator_(allocator),
      options_(std::move(options)),
      world_(allocator),
      tree_(world_),
      binding_(cy::system_allocator(cy::MemoryDomain::Scripting), world_),
      host_(cy::system_allocator(cy::MemoryDomain::Scripting)),
      runtime_(cy::system_allocator(cy::MemoryDomain::Scripting), host_),
      bridge_(allocator, tree_, host_, runtime_),
      manifest_text_(allocator),
      input_(allocator),
      input_adapter_(input_),
      audio_(allocator),
      audio_adapter_(audio_, allocator, &tree_),
      mix_scratch_(allocator),
      text_(allocator),
      store_(allocator),
      toolkit_(allocator, store_, text_),
      video_(allocator),
      user_data_(allocator) {}

MenusHost::~MenusHost() {
    shutdown();
}

cy::Status MenusHost::start(const char** detail) noexcept {
    *detail = "";
    const struct {
        const char* name;
        cy::Status (MenusHost::*step)() noexcept;
    } steps[] = {
        {"scene", &MenusHost::start_scene},     {"input", &MenusHost::start_input},
        {"audio", &MenusHost::start_audio},     {"interface", &MenusHost::start_interface},
        {"display", &MenusHost::start_display},
    };
    for (const auto& step : steps) {
        if (cy::Status done = (this->*step.step)(); !done) {
            *detail = step.name;
            return done;
        }
    }
    host_.bind_world(&binding_);
    cy::game_backend::bind(host_, &input_adapter_);
    cy::game_backend::bind(host_, &audio_adapter_);
    cy::game_backend::bind(host_, ui_adapter_.get());
    cy::game_backend::bind(host_, &video_, &user_data_);
    cy::game_backend::bind_scene(host_, &bridge_);
    *detail = "illumination";
    gi_ = std::make_unique<sample::rts::RtsIllumination>(allocator_);
    if (cy::Status built = gi_->build(); !built) {
        return built;
    }
    if (cy::Status bound = gi_->bind(host_, CY_ENTITY_NULL); !bound) {
        return bound;
    }
    started_ = true;
    if (cy::Status loaded = load_module(detail); !loaded) {
        return loaded;
    }
    const auto synced = bridge_.sync_types();
    if (!synced) {
        return cy::make_unexpected(synced.error());
    }
    *detail = "Menus";
    if (cy::Status attached = bridge_.attach(menus_, "Menus"); !attached) {
        return attached;
    }
    *detail = "";
    return cy::ok();
}

cy::Status MenusHost::start_scene() noexcept {
    if (cy::Status ready = world_.initialize(); !ready) {
        return ready;
    }
    if (cy::Status ready = tree_.initialize(); !ready) {
        return ready;
    }
    const auto node = tree_.create_node(cy::Name::intern("Menus"), tree_.root());
    if (!node) {
        return cy::make_unexpected(node.error());
    }
    menus_ = *node;
    return cy::ok();
}

cy::Status MenusHost::start_input() noexcept {
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
    const auto keyboard = connect(input_, cy::input::DeviceKind::Keyboard, "menus-keyboard");
    const auto mouse = connect(input_, cy::input::DeviceKind::Mouse, "menus-mouse");
    const auto gamepad = connect(input_, cy::input::DeviceKind::Gamepad, "menus-gamepad");
    if (!keyboard || !mouse || !gamepad) {
        return cy::fail(cy::ErrorCode::Unavailable, "a scripted device was refused");
    }
    keyboard_ = *keyboard;
    mouse_ = *mouse;
    gamepad_ = *gamepad;
    if (cy::Status declared = cy::ui::declare_ui_actions(input_.actions(), ui_actions_);
        !declared) {
        return declared;
    }
    cy::input::MappingContext game(allocator_);
    game.set_name(cy::Name::intern("Game"));
    for (const GameAction& action : kGameActions) {
        cy::input::ActionDeclaration declaration;
        declaration.name = cy::Name::intern(action.name);
        declaration.stable_id = cy::input::ActionStableId{action.stable};
        const auto declared = input_.actions().declare(declaration);
        if (!declared) {
            return cy::make_unexpected(declared.error());
        }
        if (!game.add(simple(*declared, cy::input::key_control(action.key))) ||
            !game.add(simple(*declared, cy::input::gamepad_control(action.pad)))) {
            return cy::fail(cy::ErrorCode::OutOfMemory, "the game context was refused");
        }
    }
    if (cy::Status finalised = input_.finalize_declarations(); !finalised) {
        return finalised;
    }
    auto ui_context = cy::ui::make_ui_context(allocator_, ui_actions_);
    if (!ui_context) {
        return cy::make_unexpected(ui_context.error());
    }
    const auto game_handle = input_.register_context(std::move(game));
    if (!game_handle) {
        return cy::make_unexpected(game_handle.error());
    }
    const auto ui_handle = input_.register_context(std::move(*ui_context));
    if (!ui_handle) {
        return cy::make_unexpected(ui_handle.error());
    }
    if (const auto pushed = input_.user(0).push_context(*game_handle, 0); !pushed) {
        return cy::make_unexpected(pushed.error());
    }
    bridge_input_ = std::make_unique<cy::ui::UiInputBridge>(toolkit_, input_, 0);
    return bridge_input_->start(ui_actions_, *ui_handle, kInterfacePriority);
}

cy::Status MenusHost::start_audio() noexcept {
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
    config.voice_capacity = 8;
    if (cy::Status configured = audio_.configure(config); !configured) {
        return configured;
    }
    if (cy::Status ready = audio_.initialize_with(audio_device_); !ready) {
        return ready;
    }
    buses_[0] = audio_.buses().master();
    for (u32 index = 1; index < 4; ++index) {
        cy::audio::BusDescription description;
        description.name = cy::Name::intern(kBuses[index]);
        const auto made = audio_.buses().create(description);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        buses_[index] = *made;
    }
    if (cy::Status compiled = audio_.buses().compile(); !compiled) {
        return compiled;
    }
    return mix_scratch_.resize(static_cast<cy::usize>(kFramesPerTick) * 2U);
}

cy::Status MenusHost::start_interface() noexcept {
#if defined(CY_MENUS_INTERFACE_FONT)
    cy::text::TextServerConfig config;
    config.atlas.initial_extent = 512;
    if (cy::Status backend = text_backend_.start(); !backend) {
        return backend;
    }
    if (cy::Status server = text_server_.start_with(config, text_backend_); !server) {
        return server;
    }
    if (cy::Status painter =
            text_.start(text_server_, cy::text::FontSource{cy::ui::interface_font_bytes(), 0},
                        cy::ui::interface_font_desc(), 1);
        !painter) {
        return painter;
    }
#else
    if (cy::Status server = text_server_.start(cy::text::TextServerConfig{}); !server) {
        return server;
    }
    if (cy::Status painter = text_.start(text_server_, cy::ui::builtin_font(), 1); !painter) {
        return painter;
    }
#endif
    auto root = store_.create(cy::ui::kNoElement, cy::Name::intern("screen"));
    if (!root) {
        return cy::make_unexpected(root.error());
    }
    root_ = *root;
    store_.layout_input(root_)->model = cy::ui::LayoutModel::Absolute;
    auto adapter =
        cy::make_unique<cy::game_backend::UiAdapter>(allocator_, allocator_, store_, text_, root_);
    if (!adapter) {
        return cy::make_unexpected(adapter.error());
    }
    ui_adapter_ = std::move(*adapter);
    if (cy::Status started = ui_adapter_->start(); !started) {
        return started;
    }
    // ONE TREE for the module's 1.6 elements and its documents' widgets: the toolkit starts on the
    // adapter's root, so what the game makes later — an inventory's drag ghost — is drawn after,
    // over, the documents it loaded first. The toolkit's overlay is its own root, over both.
    if (cy::Status started = toolkit_.start(root_); !started) {
        return started;
    }
    auto mount = cy::assets::DirectoryMount::create(options_.project.c_str(),
                                                    cy::assets::MountKind::Project, false);
    if (!mount) {
        return cy::make_unexpected(mount.error());
    }
    if (const auto mounted = files_.mount_owned(std::move(*mount), 0); !mounted) {
        return cy::make_unexpected(mounted.error());
    }
    resolver_ = std::make_unique<cy::ui::VfsDocumentResolver>(files_);
    ui_adapter_->attach_toolkit(&toolkit_, resolver_.get(), &files_);
    bus_bindings_ = std::make_unique<BusBindings>(audio_, buses_);
    ui_adapter_->set_document_bindings(bus_bindings_.get());
    return cy::ok();
}

cy::Status MenusHost::start_display() noexcept {
    CyVideoSettings initial{};
    initial.struct_size = sizeof(initial);
    initial.width = kModes[0].width;
    initial.height = kModes[0].height;
    initial.quality = CY_VIDEO_QUALITY_HIGH;
    initial.upscale = CY_VIDEO_UPSCALE_NATIVE;
    initial.flags = CY_VIDEO_VSYNC;
    if (cy::Status started =
            video_.start(cy::Span<const CyVideoMode>(kModes, std::size(kModes)), initial);
        !started) {
        return started;
    }
    return user_data_.start(options_.user_data);
}

cy::Status MenusHost::load_module(const char** detail) noexcept {
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

cy::Status MenusHost::frame() noexcept {
    const auto started = std::chrono::steady_clock::now();
    const cy::Status ran = run_frame();
    frame_ms_ =
        std::chrono::duration<cy::f64, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return ran;
}

cy::Status MenusHost::run_frame() noexcept {
    if (cy::Status pumped = tree_.pump(); !pumped) {
        return pumped;
    }
    if (cy::Status ticked = fixed_tick(); !ticked) {
        return ticked;
    }
    input_adapter_.begin_frame();
    if (cy::Status routed = interface_frame(); !routed) {
        return routed;
    }
    gi_->begin_frame(frame_ + 1U);
    runtime_.frame_update(kStep);
    if (cy::Status lit = gi_->update(cy::Vec3{16.0F, 0.0F, 16.0F}, cy::Vec3{}, frame_ + 1U); !lit) {
        return lit;
    }
    audio_adapter_.update(kStep);
    audio_.update(kStep);
    (void)audio_device_.advance(mix_scratch_.data(), kFramesPerTick);
    ++frame_;
    return cy::ok();
}

cy::Status MenusHost::fixed_tick() noexcept {
    const u64 tick = frame_ + 1U;
    host_.game.clock.tick = tick;
    input_adapter_.observe_pending();
    input_.resolve_tick(tick, static_cast<cy::Nanoseconds>(tick) * kStepNs, kStep);
    // THE INTERFACE KEEPS THE INPUT WHILE A WIDGET HAS THE FOCUS: a menu on screen, with the game
    // behind it, as the bridge's menu mode says. With nothing focused — playing — the game has it.
    bridge_input_->set_menu_mode(toolkit_.focused().is_valid());
    if (cy::Status pumped = bridge_input_->pump(); !pumped) {
        return pumped;
    }
    runtime_.fixed_update(kStep);
    return cy::ok();
}

cy::ui::ScaleSettings MenusHost::ui_scale() noexcept {
    cy::ui::ScaleSettings settings;
    settings.mode = cy::ui::ScaleMode::ScaleWithSmaller;
    settings.reference =
        Vec2{static_cast<f32>(kReferenceWidth), static_cast<f32>(kReferenceHeight)};
    return settings;
}

Vec2 MenusHost::output() const noexcept {
    return Vec2{static_cast<f32>(video_.current().width),
                static_cast<f32>(video_.current().height)};
}

cy::Status MenusHost::interface_frame() noexcept {
    const cy::i64 now_ns = static_cast<cy::i64>(frame_) * kStepNs;
    if (cy::Status laid = ui_adapter_->frame(kStep, ui_scale(), output(), now_ns); !laid) {
        return laid;
    }
    CyInputPointer pointer{};
    pointer.struct_size = sizeof(pointer);
    if (input_adapter_.pointer(0, pointer) != CY_RESULT_OK) {
        return cy::fail(cy::ErrorCode::Internal, "the input adapter has no pointer for user 0");
    }
    // The pointer is in the output's pixels; the interface in its reference units.
    const f32 scale = cy::ui::resolve_scale(ui_scale(), output());
    const Vec2 at{pointer.position[0] / scale, pointer.position[1] / scale};
    if (cy::Status routed =
            ui_adapter_->route_pointer(at, pointer.buttons_pressed, pointer.buttons_released);
        !routed) {
        return routed;
    }
    input_adapter_.set_pointer_focus(0, true, ui_adapter_->pointer_over());
    ui_adapter_->drain_events(
        [this](const CyUiEvent& event) noexcept { (void)runtime_.ui_event(event); });
    return cy::ok();
}

// --- The hand ------------------------------------------------------------------------------------

cy::Status MenusHost::press(const char* path, bool down) noexcept {
    const cy::input::Control control = cy::input::cook::parse_control_path(path);
    if (!control.is_valid()) {
        return cy::fail(cy::ErrorCode::InvalidArgument, "the script names no such control");
    }
    cy::input::DeviceId device = keyboard_;
    if (control.kind == cy::input::DeviceKind::Gamepad) {
        device = gamepad_;
    } else if (control.kind == cy::input::DeviceKind::Mouse) {
        device = mouse_;
    }
    const auto at = (static_cast<cy::Nanoseconds>(frame_) * kStepNs) + 1;
    return input_.inject(device, control, down ? 1.0F : 0.0F, at);
}

cy::Status MenusHost::move_pointer(f32 x, f32 y) noexcept {
    const f32 scale = cy::ui::resolve_scale(ui_scale(), output());
    const auto at = (static_cast<cy::Nanoseconds>(frame_) * kStepNs) + 1;
    const auto mouse = [](cy::input::MouseControl control) noexcept {
        return cy::input::Control{cy::input::DeviceKind::Mouse, static_cast<cy::u16>(control)};
    };
    if (cy::Status moved =
            input_.inject(mouse_, mouse(cy::input::MouseControl::PositionX), x * scale, at);
        !moved) {
        return moved;
    }
    return input_.inject(mouse_, mouse(cy::input::MouseControl::PositionY), y * scale, at);
}

bool MenusHost::widget_centre(const char* id, f32& x, f32& y) const noexcept {
    const cy::ui::WidgetId widget = toolkit_.find(cy::ui::kNoElement, id);
    const cy::ui::LayoutOutput* placed = widget.is_valid() ? store_.layout_output(widget) : nullptr;
    if (placed == nullptr) {
        return false;
    }
    x = placed->rect.x + (placed->rect.width * 0.5F);
    y = placed->rect.y + (placed->rect.height * 0.5F);
    return true;
}

// --- The eyes ------------------------------------------------------------------------------------

std::string MenusHost::binding(const char* action, u32 scheme) noexcept {
    CyInputAction found = 0;
    if (input_adapter_.find_action(action, found) != CY_RESULT_OK) {
        return "?";
    }
    char path[64] = {};
    return input_adapter_.binding(0, found, scheme, 0, path) == CY_RESULT_OK ? std::string(path)
                                                                             : "?";
}

Observation MenusHost::observe() noexcept {
    Observation seen;
    if (const cy::abi::ComponentRecord* record = binding_.find("MenusReport"); record != nullptr) {
        const void* bytes = world_.get(menus_.entity(), record->id);
        const auto field = [&](const char* name) {
            return report_field(binding_, *record, bytes, name);
        };
        seen.game.found = bytes != nullptr;
        seen.game.screen = field("screen");
        seen.game.started = field("started");
        seen.game.applied = field("applied");
        seen.game.rebinds = field("rebinds");
        seen.game.conflicts = field("conflicts");
        seen.game.loaded_settings = field("loadedSettings");
        seen.game.loaded_bindings = field("loadedBindings");
        seen.game.picks = field("picks");
        seen.game.drops = field("drops");
        seen.game.drags = field("drags");
        seen.game.first_slot_item = field("firstSlotItem");
        seen.game.potion_slot = field("potionSlot");
        seen.game.backs = field("backs");
        seen.game.jumps = field("jumps");
    }
    const cy::ui::WidgetId focused = toolkit_.focused();
    const cy::ui::WidgetId owner = toolkit_.widget_of(focused);
    seen.focused = std::string(toolkit_.id_of(owner.is_valid() ? owner : focused).text());
    seen.video = video_.current();
    const cy::rendering::upscale::Extent2D extent = cy::rendering::upscale::render_extent_for(
        {seen.video.width, seen.video.height},
        cy::rendering::upscale::upscale_mode_scale(
            static_cast<cy::rendering::upscale::UpscaleMode>(seen.video.upscale)));
    seen.render_width = extent.width;
    seen.render_height = extent.height;
    seen.gi_quality = static_cast<u32>(gi_->system().runtime_settings().quality);
    for (u32 index = 0; index < 4; ++index) {
        const cy::audio::BusDescription* description = audio_.buses().description(buses_[index]);
        seen.volumes[index] = description != nullptr ? description->volume : -1.0F;
    }
    seen.jump_keyboard = binding("Game.Jump", CY_INPUT_SCHEME_KEYBOARD_MOUSE);
    seen.jump_gamepad = binding("Game.Jump", CY_INPUT_SCHEME_GAMEPAD);
    seen.interact_keyboard = binding("Game.Interact", CY_INPUT_SCHEME_KEYBOARD_MOUSE);
    seen.widgets = toolkit_.widget_count();
    seen.subtitle = std::string(toolkit_.text(toolkit_.find(cy::ui::kNoElement, "subtitle")));
    return seen;
}

void MenusHost::shutdown() noexcept {
    if (!started_) {
        return;
    }
    started_ = false;
    host_.game = cy::abi::game::GameServices{};
    host_.bind_world(nullptr);
    if (gi_ != nullptr) {
        gi_->unbind(host_);
    }
    if (bridge_input_ != nullptr) {
        bridge_input_->stop();
    }
}

}  // namespace sample::menus
