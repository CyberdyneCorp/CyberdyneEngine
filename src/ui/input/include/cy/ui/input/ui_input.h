// SPDX-License-Identifier: MIT
#ifndef CY_UI_INPUT_UI_INPUT_H
#define CY_UI_INPUT_UI_INPUT_H
// The toolkit driven by the engine's input actions, and gameplay suppressed while the interface
// holds the input. Issue #91, stage 4.
//
// `ui-system`: "UI SHALL consume semantic actions from `input-and-actions`, never raw device input",
// and "Interface actions SHALL live in mapping contexts pushed onto the input user's context stack,
// so that a modal consumes navigation while a background context does not observe it, and so that
// gameplay actions bound to the same controls are suppressed while the interface holds focus."
//
// ================================================================================================
// THE ACTIONS, THE CONTEXT, THE BRIDGE
// ================================================================================================
//
//   declare_ui_actions(registry)    the UI.* actions, with stable ids of their own
//   make_ui_context(actions)        the default bindings: keyboard, gamepad — Enter, a gamepad's
//                                   south button and Space all are UI.Accept
//   UiInputBridge::pump(tick)       after the input server resolved the tick: every UI action that
//                                   fired goes to the toolkit, and the user's FOCUS LAYER follows
//                                   the toolkit — `Interface` while a dialog, a drop-down or a text
//                                   field holds the input, `Gameplay` otherwise
//
// THE SUPPRESSION IS THE INPUT SYSTEM'S. The interface context is pushed at
// `FocusLayer::Interface`; the game's own at `Gameplay`. `InputUser` drops every binding whose layer
// is not the user's focus, so while a modal is open a gameplay action bound to Enter is not
// observed — not because game code checked a flag, but because the input user's focus is the
// interface. And the interface's own bindings do nothing while the game has the focus, so pressing
// Enter in play does not click a HUD button nobody is looking at.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/servers/input/server.h>
#include <cy/ui/interaction.h>
#include <cy/ui/toolkit/toolkit.h>

namespace cy::ui {

/// The first stable id the UI actions take: `kUiActionStableBase + UiAction`. A range of its own,
/// so a project's actions never collide with them.
inline constexpr u32 kUiActionStableBase = 0x55490000U;

/// The input server's ids for the UI actions, by `UiAction`.
struct UiInputActions {
    input::ActionId ids[static_cast<u32>(UiAction::Count)] = {};
};

/// Declare every `UiAction` as a digital action named as `ui_action_name` spells it.
[[nodiscard]] Status declare_ui_actions(input::ActionRegistry& registry,
                                        UiInputActions& out) noexcept;

/// The default interface bindings. Keyboard: Enter and Space accept, Escape cancels, the arrows
/// navigate, Tab and Shift+Tab walk the tab order, Page Up and Page Down scroll, Q and E change tabs.
/// Gamepad: south accepts, east cancels, the d-pad and the left stick navigate, the shoulders
/// change tabs, the right stick scrolls, north is the context action.
[[nodiscard]] Expected<input::MappingContext, Error> make_ui_context(
    Allocator& allocator, const UiInputActions& actions) noexcept;

/// What the prompt for an action shows on the active control scheme: "Enter", "A", "Tap". The
/// "glyph display follows the device" half of `ui-system`'s semantic input — a game draws this, or
/// maps it to an icon, and it changes when the scheme does.
[[nodiscard]] const char* ui_action_glyph(UiAction action, input::SchemeKind scheme) noexcept;

/// What one pump did.
struct UiInputReport {
    /// UI actions that fired this tick, and of those the toolkit consumed.
    u32 fired = 0;
    u32 consumed = 0;
    /// The user's focus layer after the pump.
    input::FocusLayer focus = input::FocusLayer::Gameplay;
};

class UiInputBridge {
public:
    /// `server`, `toolkit` and the context the caller registered from `make_ui_context` outlive the
    /// bridge. The context is pushed on the user's stack at `FocusLayer::Interface`.
    UiInputBridge(Toolkit& toolkit, input::InputServer& server, u32 user,
                  const UiInputActions& actions) noexcept;

    [[nodiscard]] Status start(input::ContextHandle context, i32 priority = 100) noexcept;
    /// Pop the interface context.
    void stop() noexcept;

    /// After `InputServer::resolve_tick`: deliver the UI actions that fired, then set the user's
    /// focus layer and text entry from what the toolkit holds now.
    [[nodiscard]] Status pump(UiInputReport* report = nullptr) noexcept;

    /// Keep the interface focused while there is nothing modal: a menu screen the game shows with
    /// the world paused. Off by default.
    void set_menu_mode(bool on) noexcept { menu_mode_ = on; }
    [[nodiscard]] input::SchemeKind scheme() const noexcept;

private:
    Toolkit* toolkit_;
    input::InputServer* server_;
    u32 user_ = 0;
    UiInputActions actions_;
    input::ContextHandle context_;
    bool menu_mode_ = false;
};

}  // namespace cy::ui

#endif  // CY_UI_INPUT_UI_INPUT_H
