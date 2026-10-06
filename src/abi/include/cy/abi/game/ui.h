// SPDX-License-Identifier: MIT
// cy/abi/game/ui.h — the backend behind ABI 1.6's interface entries.
//
// `cy_abi` cannot name CyberUI: `src/ui/` is behind `CY_UI` and the table is in every build. So
// the store is reached through this backend, which `cy::game_backend::UiAdapter` implements over
// `cy::ui::ElementStore`, `cy::ui::TextPainter` and `cy::ui::Interaction`. A build without the
// interface binds none, and every 1.6 entry answers UNAVAILABLE.
//
// The thunks (src/abi/src/game/ui_thunks.cpp) check the engine, the phase, the binding and the
// pointers, and normalise every sized struct; a backend validates DOMAIN facts — a stale element,
// one the module did not create, a write the kind does not take — and reports them with
// `cy::abi::report`, returning the code.

#pragma once

#include <cy/abi/cy_abi.h>
#include <cy/core/base/types.h>

namespace cy::abi::game {

/// The runtime interface, as the 1.6 entries see it. Every method is called on the game thread.
class UiBackend {
public:
    virtual ~UiBackend() = default;

    [[nodiscard]] virtual CyUiElement root() const noexcept = 0;
    /// `desc` is whole (normalised by the thunk), its kind already checked to be a `CyUiKind`.
    [[nodiscard]] virtual CyResult create(CyUiElement parent, const CyUiElementDesc& desc,
                                          CyUiElement& out) noexcept = 0;
    [[nodiscard]] virtual CyResult destroy(CyUiElement element) noexcept = 0;
    /// `layout` is whole, its enums already checked.
    [[nodiscard]] virtual CyResult set_layout(CyUiElement element,
                                              const CyUiLayout& layout) noexcept = 0;
    [[nodiscard]] virtual CyResult set_style(CyUiElement element,
                                             const CyUiStyle& style) noexcept = 0;
    /// `utf8` is non-null; `pixel_scale` is at least 1.
    [[nodiscard]] virtual CyResult set_text(CyUiElement element, const char* utf8, u32 colour,
                                            u32 pixel_scale) noexcept = 0;
    /// `uv` is x y width height, finite.
    [[nodiscard]] virtual CyResult set_image(CyUiElement element, u32 atlas_page,
                                             const f32 (&uv)[4]) noexcept = 0;
    /// `value` is finite and in [0, 1].
    [[nodiscard]] virtual CyResult set_progress(CyUiElement element, f32 value) noexcept = 0;
    /// `visibility` is a `CyUiVisibility`.
    [[nodiscard]] virtual CyResult set_visibility(CyUiElement element,
                                                  CyUiVisibility visibility) noexcept = 0;
    /// `opacity` is finite and in [0, 1].
    [[nodiscard]] virtual CyResult set_opacity(CyUiElement element, f32 opacity) noexcept = 0;
    [[nodiscard]] virtual CyResult rect(CyUiElement element, f32 (&out)[4]) const noexcept = 0;
    /// The module's element under `x, y`, or CY_UI_ELEMENT_NULL. Never fails.
    [[nodiscard]] virtual CyUiElement hit_test(f32 x, f32 y) const noexcept = 0;
    /// The module's element with focus, or CY_UI_ELEMENT_NULL. Never fails.
    [[nodiscard]] virtual CyUiElement focus() const noexcept = 0;
    /// CY_UI_ELEMENT_NULL clears focus.
    [[nodiscard]] virtual CyResult set_focus(CyUiElement element) noexcept = 0;
};

}  // namespace cy::abi::game
