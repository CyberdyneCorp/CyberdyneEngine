// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_MENUS_MENUS_HOST_H
#define CY_SAMPLE_MENUS_MENUS_HOST_H
// menus_host.h — the engine side of samples/16-menus. It brings up the servers the menus reach,
// binds their adapters, loads the Swift module and runs frames. It decides nothing a menu does.
//
// ================================================================================================
// WHAT THE MENUS REACH, AND THROUGH WHAT
// ================================================================================================
//
//   the interface      a widget toolkit on a CyberUI store, in the interface font, behind
//                      `UiAdapter` (ABI 1.6 and 1.12), reading the documents through a virtual file
//                      system mounted on the project — so a document the editor saves is reloaded
//   navigation         the `UI.*` actions on a keyboard and a gamepad, through `UiInputBridge`; the
//                      interface keeps the input while a widget has the focus
//   rebinding          `InputAdapter` (ABI 1.3 and 1.17) over the input server, whose `Game`
//   context
//                      holds Jump, Interact, Inventory, Sprint and Pause
//   display settings   `VideoSettingsAdapter` (ABI 1.17): what the renderer (render.h) draws at
//   audio volumes      an audio server's four buses — Master, Music, SFX, Voice — through
//                      `AudioAdapter` (ABI 1.3), and the same buses as the settings document's
//                      `bind=volume.*`, two-way: a slider is the bus
//   global illum.      the RTS sample's level lit by CyberGI (ABI 1.9), whose quality the settings
//   set player files       `UserDataDirectory` (ABI 1.17) over `--user-data <dir>`
//
// ================================================================================================
// ONE FRAME, IN ORDER
// ================================================================================================
//
//   scene tree pump
//   the fixed tick: the input adapter sees the pending events (a listening rebind among them), the
//                   input server resolves, the bridge hands the UI actions that fired to the
//                   toolkit
//   the interface: the toolkit's update — bindings, styles, layout, documents reloaded — then the
//                   pointer routed, and the events delivered to the behaviours (phase U)
//   behaviours' onUpdate (phase U)
//   the GI's update, the audio's
//
// The hand (`press`, `move_pointer`, `button`) injects synthetic events, the door a recorded replay
// uses; the scripted player (script.h) drives it.

#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/ownership.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/audio_backend.h>
#include <cy/game_backend/input_backend.h>
#include <cy/game_backend/menu_backends.h>
#include <cy/game_backend/scene_bridge.h>
#include <cy/game_backend/ui_backend.h>
#include <cy/scene/tree.h>
#include <cy/servers/audio/backend.h>
#include <cy/servers/audio/server.h>
#include <cy/servers/input/server.h>
#include <cy/servers/text/server.h>
#include <cy/ui/input/ui_input.h>
#include <cy/ui/store.h>
#include <cy/ui/text/text_painter.h>
#include <cy/ui/toolkit/document_host.h>
#include <cy/ui/toolkit/toolkit.h>

#if defined(CY_MENUS_INTERFACE_FONT)
#    include <cy/backends/text/complete_backend.h>
#endif

#include <memory>
#include <string>

namespace sample::rts {
class RtsIllumination;
}

namespace sample::menus {

struct HostOptions {
    const char* module_library = "";
    const char* module_manifest = "";
    /// The project the documents are read from, and watched in.
    std::string project;
    /// Where the player's files are kept.
    std::string user_data;
};

/// The interface's reference resolution: a document is designed at it, and scaled to the output.
inline constexpr cy::u32 kReferenceWidth = 1280;
inline constexpr cy::u32 kReferenceHeight = 720;

/// What the game and the engine say after a run: the game's report, and the engine's own state the
/// menus changed — read from the servers, not from the game.
struct Observation {
    struct Game {
        bool found = false;
        cy::f32 screen = 0.0F;
        cy::f32 started = 0.0F;
        cy::f32 applied = 0.0F;
        cy::f32 rebinds = 0.0F;
        cy::f32 conflicts = 0.0F;
        cy::f32 loaded_settings = 0.0F;
        cy::f32 loaded_bindings = 0.0F;
        cy::f32 picks = 0.0F;
        cy::f32 drops = 0.0F;
        cy::f32 drags = 0.0F;
        cy::f32 first_slot_item = -1.0F;
        cy::f32 potion_slot = -1.0F;
        cy::f32 backs = 0.0F;
        cy::f32 jumps = 0.0F;
    };
    Game game;
    /// The widget with the focus, by its id: what a gamepad player is on.
    std::string focused;
    /// The display settings in effect, and the render extent the upscaler draws at.
    CyVideoSettings video{};
    cy::u32 render_width = 0;
    cy::u32 render_height = 0;
    /// The GI's quality, as the system runs it.
    cy::u32 gi_quality = 0;
    /// Each bus's gain, as the audio server holds it: Master, Music, SFX, Voice.
    cy::f32 volumes[4] = {};
    /// The controls in effect for Jump on each scheme, as paths.
    std::string jump_keyboard;
    std::string jump_gamepad;
    std::string interact_keyboard;
    /// The main menu's subtitle as the interface shows it: what a save in the editor changes.
    std::string subtitle;
    cy::u32 widgets = 0;
};

class MenusHost {
public:
    MenusHost(cy::Allocator& allocator, HostOptions options) noexcept;
    ~MenusHost();

    MenusHost(const MenusHost&) = delete;
    MenusHost& operator=(const MenusHost&) = delete;
    MenusHost(MenusHost&&) = delete;
    MenusHost& operator=(MenusHost&&) = delete;

    [[nodiscard]] cy::Status start(const char** detail) noexcept;
    [[nodiscard]] cy::Status frame() noexcept;
    void shutdown() noexcept;

    // --- The hand ------------------------------------------------------------------------------
    /// Press or release a control by its path: `keyboard/escape`, `gamepad/dpadDown`.
    [[nodiscard]] cy::Status press(const char* path, bool down) noexcept;
    /// Move the pointer to a point in the reference resolution.
    [[nodiscard]] cy::Status move_pointer(cy::f32 x, cy::f32 y) noexcept;
    /// The centre of the widget whose id is `id`, in the reference resolution.
    [[nodiscard]] bool widget_centre(const char* id, cy::f32& x, cy::f32& y) const noexcept;

    // --- The eyes ------------------------------------------------------------------------------
    [[nodiscard]] Observation observe() noexcept;
    [[nodiscard]] cy::u64 frames() const noexcept { return frame_; }
    [[nodiscard]] cy::f64 last_frame_ms() const noexcept { return frame_ms_; }
    [[nodiscard]] cy::ui::ElementStore& ui_store() noexcept { return store_; }
    [[nodiscard]] cy::ui::TextPainter& ui_text() noexcept { return text_; }
    [[nodiscard]] const cy::game_backend::VideoSettingsAdapter& video() const noexcept {
        return video_;
    }
    [[nodiscard]] sample::rts::RtsIllumination& illumination() noexcept { return *gi_; }

private:
    [[nodiscard]] cy::Status start_scene() noexcept;
    [[nodiscard]] cy::Status start_input() noexcept;
    [[nodiscard]] cy::Status start_audio() noexcept;
    [[nodiscard]] cy::Status start_interface() noexcept;
    [[nodiscard]] cy::Status start_display() noexcept;
    [[nodiscard]] cy::Status load_module(const char** detail) noexcept;
    [[nodiscard]] cy::Status run_frame() noexcept;
    [[nodiscard]] cy::Status fixed_tick() noexcept;
    [[nodiscard]] cy::Status interface_frame() noexcept;
    [[nodiscard]] static cy::ui::ScaleSettings ui_scale() noexcept;
    [[nodiscard]] cy::Vec2 output() const noexcept;
    [[nodiscard]] std::string binding(const char* action, cy::u32 scheme) noexcept;

    /// The settings document's `bind=volume.*`: each bus's gain, two-way.
    class BusBindings;

    cy::Allocator& allocator_;
    HostOptions options_;

    cy::ecs::World world_;
    cy::scene::SceneTree tree_;
    cy::abi::World binding_;
    cy::abi::Host host_;
    cy::abi::BehaviourRuntime runtime_;
    cy::game_backend::ScriptSceneBridge bridge_;
    cy::Array<char> manifest_text_;
    cy::abi::ModuleManifest manifest_;
    cy::scene::Node menus_;

    cy::input::InputServer input_;
    cy::input::DeviceId keyboard_;
    cy::input::DeviceId mouse_;
    cy::input::DeviceId gamepad_;
    cy::game_backend::InputAdapter input_adapter_;
    cy::ui::UiInputActions ui_actions_;

    cy::audio::NullAudioBackend audio_device_;
    cy::audio::AudioServer audio_;
    cy::game_backend::AudioAdapter audio_adapter_;
    cy::Array<cy::f32> mix_scratch_;
    cy::audio::BusHandle buses_[4];

#if defined(CY_MENUS_INTERFACE_FONT)
    cy::text::CompleteTextBackend text_backend_;
#endif
    cy::text::TextServer text_server_;
    cy::ui::TextPainter text_;
    cy::ui::ElementStore store_;
    cy::ui::ElementId root_;
    cy::UniquePtr<cy::game_backend::UiAdapter> ui_adapter_;
    cy::ui::Toolkit toolkit_;
    std::unique_ptr<cy::ui::UiInputBridge> bridge_input_;
    cy::assets::VirtualFileSystem files_;
    std::unique_ptr<cy::ui::VfsDocumentResolver> resolver_;
    std::unique_ptr<BusBindings> bus_bindings_;

    cy::game_backend::VideoSettingsAdapter video_;
    cy::game_backend::UserDataDirectory user_data_;
    std::unique_ptr<sample::rts::RtsIllumination> gi_;

    cy::u64 frame_ = 0;
    cy::f64 frame_ms_ = 0.0;
    bool started_ = false;
};

}  // namespace sample::menus

#endif  // CY_SAMPLE_MENUS_MENUS_HOST_H
