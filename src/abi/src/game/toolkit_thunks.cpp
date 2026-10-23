// SPDX-License-Identifier: MIT
// The `ui_*` widget entries, ABI 1.12: the widget toolkit.
//
// `[N U]` every one, as 1.6's are. They reach the same backend as 1.6 — `UiBackend`, implemented by
// `cy::game_backend::UiAdapter` — through the virtuals cy/abi/game/ui.h appends, whose defaults
// answer UNAVAILABLE for a backend with no toolkit attached. What is checked here is what a
// backend could not tell apart from a module's mistake: the pointers, the sized structs, the
// enumerators and every float.

#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/game/ui.h>
#include <cy/abi/host.h>

#include <cmath>
#include <cstring>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kUiPhases = kPhaseNone | kPhaseFrame;

/// The highest of each enumeration, for the range checks below.
constexpr u32 kLastWidgetKind = CY_UI_WIDGET_DIALOG;
constexpr u32 kLastAction = CY_UI_ACTION_FOCUS_PREVIOUS;
constexpr u32 kLastAnimated = CY_UI_ANIMATE_BACKGROUND;
constexpr u32 kLastEasing = CY_UI_EASING_BACK;
constexpr u32 kWidgetFlags = CY_UI_WIDGET_CHECKED | CY_UI_WIDGET_DISABLED;

CyResult no_toolkit() noexcept {
    return report(CY_RESULT_UNAVAILABLE, "no widget toolkit is attached to this interface");
}

UiBackend* backend(CyEngine engine, const char* entry, CyResult& refused) noexcept {
    if (engine == nullptr) {
        refused = report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    refused = require_phase(engine->game, kUiPhases, entry);
    if (refused != CY_RESULT_OK) {
        return nullptr;
    }
    if (engine->game.ui == nullptr) {
        refused = report(CY_RESULT_UNAVAILABLE, "no interface is bound to this engine");
        return nullptr;
    }
    return engine->game.ui;
}

CyResult answered(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

[[nodiscard]] bool finite(f32 value) noexcept {
    return std::isfinite(value);
}

}  // namespace

// --- The defaults: no toolkit --------------------------------------------------------------------

CyResult UiBackend::load_document(CyUiElement, const CyUiDocumentDesc&, CyUiElement&) noexcept {
    return no_toolkit();
}
CyResult UiBackend::load_stylesheet(const char*, u64) noexcept {
    return no_toolkit();
}
CyResult UiBackend::find(CyUiElement, const char*, CyUiElement&) const noexcept {
    return no_toolkit();
}
CyResult UiBackend::create_widget(CyUiElement, const CyUiWidgetDesc&, CyUiElement&) noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_set_text(CyUiElement, const char*) noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_text(CyUiElement, const char*&, u32&) const noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_set_value(CyUiElement, f32) noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_value(CyUiElement, f32&) const noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_set_selected(CyUiElement, u32) noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_selected(CyUiElement, u32&) const noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_set_items(CyUiElement, const char* const*, u32) noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_set_enabled(CyUiElement, bool) noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_set_classes(CyUiElement, const char*) noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_open(CyUiElement, bool) noexcept {
    return no_toolkit();
}
CyResult UiBackend::widget_animate(CyUiElement, const CyUiAnimation&) noexcept {
    return no_toolkit();
}
CyResult UiBackend::navigate(u32, bool&) noexcept {
    return no_toolkit();
}

// --- The entries ---------------------------------------------------------------------------------

CyResult ui_load_document(CyEngine engine, CyUiElement parent, const CyUiDocumentDesc* desc,
                          CyUiElement* out_root) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_load_document", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (desc == nullptr || out_root == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "ui_load_document needs a description and an output");
    }
    CyUiDocumentDesc whole{};
    if (!read_sized(*desc, whole) || whole.reserved != 0U ||
        (whole.text == nullptr) == (whole.path == nullptr)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "ui_load_document: a malformed description; give its text or its path, not "
                      "both, and leave `reserved` zero");
    }
    CyUiElement made = CY_UI_ELEMENT_NULL;
    if (const CyResult result = ui->load_document(parent, whole, made); result != CY_RESULT_OK) {
        return result;
    }
    *out_root = made;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_load_stylesheet(CyEngine engine, const char* utf8, uint64_t size) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_load_stylesheet", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (utf8 == nullptr && size != 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_load_stylesheet: a null sheet of bytes");
    }
    return answered(ui->load_stylesheet(utf8 == nullptr ? "" : utf8, size));
}

CyResult ui_find(CyEngine engine, CyUiElement scope, const char* id, CyUiElement* out_widget) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_find", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (id == nullptr || out_widget == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_find needs an id and an output");
    }
    CyUiElement found = CY_UI_ELEMENT_NULL;
    if (const CyResult result = ui->find(scope, id, found); result != CY_RESULT_OK) {
        return result;
    }
    *out_widget = found;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_create_widget(CyEngine engine, CyUiElement parent, const CyUiWidgetDesc* desc,
                          CyUiElement* out_widget) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_create_widget", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (desc == nullptr || out_widget == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "ui_create_widget needs a description and an output");
    }
    CyUiWidgetDesc whole{};
    const bool numbers = read_sized(*desc, whole) && finite(whole.value) && finite(whole.minimum) &&
                         finite(whole.maximum) && finite(whole.step) && finite(whole.row_height) &&
                         whole.step >= 0.0F && whole.row_height >= 0.0F;
    if (!numbers || whole.kind > kLastWidgetKind || whole.kind == CY_UI_WIDGET_IMAGE ||
        (whole.flags & ~kWidgetFlags) != 0U || (whole.items == nullptr && whole.item_count != 0U)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "ui_create_widget: a malformed description, an unknown kind or flag, a "
                      "non-finite number, or items with no list");
    }
    for (u32 index = 0; index < whole.item_count; ++index) {
        if (whole.items[index] == nullptr) {
            return report(CY_RESULT_INVALID_ARGUMENT, "ui_create_widget: a null item");
        }
    }
    CyUiElement made = CY_UI_ELEMENT_NULL;
    if (const CyResult result = ui->create_widget(parent, whole, made); result != CY_RESULT_OK) {
        return result;
    }
    *out_widget = made;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_widget_set_text(CyEngine engine, CyUiElement widget, const char* utf8) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_set_text", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (utf8 == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_widget_set_text needs text; \"\" is none");
    }
    return answered(ui->widget_set_text(widget, utf8));
}

CyResult ui_widget_text(CyEngine engine, CyUiElement widget, char* buffer, uint32_t capacity,
                        uint32_t* out_size) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_text", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (out_size == nullptr || (buffer == nullptr && capacity != 0U)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "ui_widget_text needs a size output, and a buffer for its capacity");
    }
    const char* text = nullptr;
    u32 size = 0;
    if (const CyResult result = ui->widget_text(widget, text, size); result != CY_RESULT_OK) {
        return result;
    }
    *out_size = size;
    if (buffer != nullptr && capacity > 0U) {
        // Cut on a code point boundary, so a short buffer never holds half a character.
        u32 fits = size < capacity ? size : capacity - 1U;
        while (fits > 0U && fits < size && (static_cast<u8>(text[fits]) & 0xC0U) == 0x80U) {
            --fits;
        }
        if (fits > 0U) {
            std::memcpy(buffer, text, fits);
        }
        buffer[fits] = '\0';
    }
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_widget_set_value(CyEngine engine, CyUiElement widget, float value) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_set_value", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (!finite(value)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_widget_set_value: the value is not finite");
    }
    return answered(ui->widget_set_value(widget, value));
}

CyResult ui_widget_value(CyEngine engine, CyUiElement widget, float* out_value) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_value", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (out_value == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_widget_value needs an output");
    }
    f32 value = 0.0F;
    if (const CyResult result = ui->widget_value(widget, value); result != CY_RESULT_OK) {
        return result;
    }
    *out_value = value;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_widget_set_selected(CyEngine engine, CyUiElement widget, uint32_t index) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_set_selected", refused);
    return ui == nullptr ? refused : answered(ui->widget_set_selected(widget, index));
}

CyResult ui_widget_selected(CyEngine engine, CyUiElement widget, uint32_t* out_index) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_selected", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (out_index == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_widget_selected needs an output");
    }
    u32 index = 0;
    if (const CyResult result = ui->widget_selected(widget, index); result != CY_RESULT_OK) {
        return result;
    }
    *out_index = index;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_widget_set_items(CyEngine engine, CyUiElement widget, const char* const* items,
                             uint32_t count) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_set_items", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (items == nullptr && count != 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_widget_set_items: items with no list");
    }
    for (u32 index = 0; index < count; ++index) {
        if (items[index] == nullptr) {
            return report(CY_RESULT_INVALID_ARGUMENT, "ui_widget_set_items: a null item");
        }
    }
    return answered(ui->widget_set_items(widget, items, count));
}

CyResult ui_widget_set_enabled(CyEngine engine, CyUiElement widget, uint32_t enabled) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_set_enabled", refused);
    return ui == nullptr ? refused : answered(ui->widget_set_enabled(widget, enabled != 0U));
}

CyResult ui_widget_set_classes(CyEngine engine, CyUiElement widget, const char* classes) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_set_classes", refused);
    if (ui == nullptr) {
        return refused;
    }
    return answered(ui->widget_set_classes(widget, classes == nullptr ? "" : classes));
}

CyResult ui_widget_open(CyEngine engine, CyUiElement widget, uint32_t open) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_open", refused);
    return ui == nullptr ? refused : answered(ui->widget_open(widget, open != 0U));
}

CyResult ui_widget_animate(CyEngine engine, CyUiElement widget, const CyUiAnimation* animation) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_widget_animate", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (animation == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_widget_animate needs an animation");
    }
    CyUiAnimation whole{};
    if (!read_sized(*animation, whole) || whole.property > kLastAnimated ||
        whole.easing > kLastEasing || (whole.flags & ~CY_UI_ANIMATION_STATE_FEEDBACK) != 0U ||
        !finite(whole.from) || !finite(whole.to) || !finite(whole.duration) ||
        !finite(whole.delay) || whole.duration < 0.0F || whole.delay < 0.0F) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "ui_widget_animate: a malformed animation, an unknown property, easing or "
                      "flag, or a negative or non-finite number");
    }
    return answered(ui->widget_animate(widget, whole));
}

CyResult ui_navigate(CyEngine engine, uint32_t action, uint32_t* out_consumed) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_navigate", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (action > kLastAction) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_navigate: not a CyUiAction");
    }
    bool consumed = false;
    if (const CyResult result = ui->navigate(action, consumed); result != CY_RESULT_OK) {
        return result;
    }
    if (out_consumed != nullptr) {
        *out_consumed = consumed ? 1U : 0U;
    }
    clear_last_error();
    return CY_RESULT_OK;
}

}  // namespace cy::abi::game
