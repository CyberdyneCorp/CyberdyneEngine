// SPDX-License-Identifier: MIT
// cy/game_backend/ui_backend.h — the interface adapter behind ABI 1.6.
//
// Implements cy/abi/game/ui.h over CyberUI: elements in the embedder's `cy::ui::ElementStore`,
// text through its `cy::ui::TextPainter`, the pointer through a `cy::ui::Interaction` this adapter
// owns. Behind `CY_UI`, as all of CyberUI is: a build without it has no `cy::game-backend-ui`,
// binds no `UiBackend`, and every `ui_*` entry answers UNAVAILABLE.
//
// ================================================================================================
// WHAT A MODULE MAY TOUCH
// ================================================================================================
//
// The root the embedder hands over — an ABSOLUTE element covering the window — and the elements the
// module created under it, nothing else. Every element made through `create` has a record here
// (its kind, its owner and, for a progress bar, its fill); a handle without one answers NOT_FOUND,
// so the developer console the embedder built in the same store cannot be restyled or destroyed by
// a game. The root itself is a parent, never a target.
//
// The root is made transparent to the pointer (`HitTestMode::Ignore`) and pushed as a `Hud` layer
// that neither captures focus nor blocks input: a point over no element is over the world, which is
// what `ui_hit_test` answering null means to a game.
//
// ================================================================================================
// ONE FRAME
// ================================================================================================
//
//   layout(settings, viewport)             measure and arrange what is dirty
//   route_pointer(position, pressed, ...)  hover, press, release; a press and a release of the
//                                          left button on one button is a CY_UI_EVENT_CLICK, and a
//                                          change of focus is a BLUR and a FOCUS
//   events() → BehaviourRuntime::ui_event  the embedder delivers them to their owners
//   clear_events()
//
// `layout` runs before routing so the pointer is tested against this frame's rects, and the
// behaviours' writes during `frame_update` are laid out by the next frame's `layout` — or by the
// embedder's own call before `flatten`.
//
// WHAT EACH WRITE DIRTIES is what `samples/13-rts-selection/hud.cpp` marks for the same change: a
// text is Measure and Paint, a layout Measure and Arrange, a style or an opacity Paint, a
// visibility whatever `ElementStore::set_flags` decides. That is what makes a HUD built through the
// ABI lay out and draw exactly as the same HUD built in C++, and `render.rts_api_hud` holds the two
// to the byte.

#pragma once

#include <cy/abi/game/ui.h>
#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/ui/interaction.h>
#include <cy/ui/layout.h>
#include <cy/ui/store.h>
#include <cy/ui/text/text_painter.h>

#include <string_view>

namespace cy::game_backend {

/// `CyUiElement` ↔ `cy::ui::ElementId`: the generation high, the index low. Zero is null.
[[nodiscard]] constexpr CyUiElement to_abi(ui::ElementId element) noexcept {
    return (static_cast<CyUiElement>(element.generation) << 32U) | element.index;
}
[[nodiscard]] constexpr ui::ElementId element_of(CyUiElement element) noexcept {
    return ui::ElementId{static_cast<u32>(element & 0xFFFFFFFFULL),
                         static_cast<u32>(element >> 32U)};
}

/// Implements `cy::abi::game::UiBackend`.
class UiAdapter final : public abi::game::UiBackend {
public:
    /// `store`, `text` and `root` are the embedder's and outlive the adapter. `text` must be
    /// started: a label is measured through it.
    UiAdapter(Allocator& allocator, ui::ElementStore& store, ui::TextPainter& text,
              ui::ElementId root) noexcept;

    UiAdapter(const UiAdapter&) = delete;
    UiAdapter& operator=(const UiAdapter&) = delete;
    UiAdapter(UiAdapter&&) = delete;
    UiAdapter& operator=(UiAdapter&&) = delete;
    ~UiAdapter() override = default;

    /// Make the root transparent to the pointer and push the interface layer. Once, before binding.
    [[nodiscard]] Status start() noexcept;

    // --- The embedder's frame -------------------------------------------------------------------

    /// Lay the store out — the whole store, the embedder's elements as well — measuring text.
    [[nodiscard]] Status layout(const ui::ScaleSettings& settings, Vec2 viewport) noexcept;
    /// Route the pointer. `pressed` and `released` are CY_INPUT_BUTTON_* edges since the last
    /// call; only the left button presses a button.
    [[nodiscard]] Status route_pointer(Vec2 position, u32 pressed, u32 released) noexcept;
    /// True when the last routed position was over any element of the store but the root — the
    /// embedder's own included — which is CY_INPUT_POINTER_OVER_UI.
    [[nodiscard]] bool pointer_over() const noexcept { return pointer_over_; }
    /// The events routed since `clear_events`, in the order they happened.
    [[nodiscard]] Span<const CyUiEvent> events() const noexcept { return events_.span(); }
    void clear_events() noexcept { events_.clear(); }

    /// The first module element whose type name is `name`, or CY_UI_ELEMENT_NULL. For a driver and
    /// a test; a game holds its handles.
    [[nodiscard]] CyUiElement find(std::string_view name) const noexcept;
    /// Elements the module has created and not destroyed.
    [[nodiscard]] u32 elements() const noexcept { return live_; }
    /// Clicks routed since `start`, delivered or not.
    [[nodiscard]] u32 clicks() const noexcept { return clicks_; }
    [[nodiscard]] const ui::ElementStore& store() const noexcept { return store_; }

    // --- UiBackend ------------------------------------------------------------------------------
    [[nodiscard]] CyUiElement root() const noexcept override { return to_abi(root_); }
    [[nodiscard]] CyResult create(CyUiElement parent, const CyUiElementDesc& desc,
                                  CyUiElement& out) noexcept override;
    [[nodiscard]] CyResult destroy(CyUiElement element) noexcept override;
    [[nodiscard]] CyResult set_layout(CyUiElement element,
                                      const CyUiLayout& layout) noexcept override;
    [[nodiscard]] CyResult set_style(CyUiElement element, const CyUiStyle& style) noexcept override;
    [[nodiscard]] CyResult set_text(CyUiElement element, const char* utf8, u32 colour,
                                    u32 pixel_scale) noexcept override;
    [[nodiscard]] CyResult set_image(CyUiElement element, u32 atlas_page,
                                     const f32 (&uv)[4]) noexcept override;
    [[nodiscard]] CyResult set_progress(CyUiElement element, f32 value) noexcept override;
    [[nodiscard]] CyResult set_visibility(CyUiElement element,
                                          CyUiVisibility visibility) noexcept override;
    [[nodiscard]] CyResult set_opacity(CyUiElement element, f32 opacity) noexcept override;
    [[nodiscard]] CyResult rect(CyUiElement element, f32 (&out)[4]) const noexcept override;
    [[nodiscard]] CyUiElement hit_test(f32 x, f32 y) const noexcept override;
    [[nodiscard]] CyUiElement focus() const noexcept override;
    [[nodiscard]] CyResult set_focus(CyUiElement element) noexcept override;

private:
    /// What the adapter knows about one module element, at its store index.
    struct Record {
        ui::ElementId element;
        /// A progress bar's fill: a child the module cannot address.
        ui::ElementId fill;
        CyUiKind kind = CY_UI_PANEL;
        CyEntity owner = CY_ENTITY_NULL;
    };

    /// The record for a live module element, or null having reported NOT_FOUND.
    [[nodiscard]] Record* record(CyUiElement element) noexcept;
    [[nodiscard]] const Record* lookup(ui::ElementId element) const noexcept;
    /// The nearest module element at or above `element`, or none.
    [[nodiscard]] const Record* owned_ancestor(ui::ElementId element) const noexcept;
    [[nodiscard]] Status remember(const Record& made) noexcept;
    /// Records whose element a destroy took with it are dropped, with their text.
    void forget_dead() noexcept;
    /// Queue a BLUR and a FOCUS when focus moved since the last call.
    void note_focus() noexcept;
    void push_event(CyUiEventKind kind, const Record& target, Vec2 position, u32 button) noexcept;

    ui::ElementStore& store_;
    ui::TextPainter& text_;
    ui::ElementId root_;
    ui::Interaction interaction_;
    Array<Record> records_;
    Array<CyUiEvent> events_;
    ui::ElementId pressed_;
    ui::ElementId focus_;
    u32 live_ = 0;
    u32 clicks_ = 0;
    bool pointer_over_ = false;
};

/// Bind `adapter` as the host's interface; null unbinds it, and every `ui_*` entry then answers
/// UNAVAILABLE.
inline void bind(cy::abi::Host& host, UiAdapter* adapter) noexcept {
    host.game.ui = adapter;
}

}  // namespace cy::game_backend
