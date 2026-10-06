// SPDX-License-Identifier: MIT
// The `ui_*` entries, ABI 1.6: the runtime interface.
//
// `[N U]`, every one: the interface is presentation, and a fixed step that built or read an
// element would make the simulation depend on it. The backend is `cy::game_backend::UiAdapter`;
// see cy/abi/game/ui.h for what is checked here and what is left to it.

#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/game/ui.h>
#include <cy/abi/host.h>

#include <algorithm>
#include <cmath>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kUiPhases = kPhaseNone | kPhaseFrame;

/// The bound backend, after the first three of services.h's steps; null with the refusal in
/// `refused` otherwise.
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

/// The backend's answer, with the last error cleared on success.
CyResult answered(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

[[nodiscard]] bool finite(f32 value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool all_finite(const f32* values, u32 count) noexcept {
    for (u32 index = 0; index < count; ++index) {
        if (!finite(values[index])) {
            return false;
        }
    }
    return true;
}

/// Every float of a layout, so one check refuses a NaN anywhere in it.
[[nodiscard]] bool layout_finite(const CyUiLayout& layout) noexcept {
    const f32 scalars[] = {layout.gap, layout.flex_grow, layout.flex_shrink, layout.aspect_ratio};
    return all_finite(scalars, 4) && all_finite(layout.preferred, 2) &&
           all_finite(layout.minimum, 2) && all_finite(layout.maximum, 2) &&
           all_finite(layout.margin, 4) && all_finite(layout.padding, 4) &&
           all_finite(layout.anchor_min, 2) && all_finite(layout.anchor_max, 2) &&
           all_finite(layout.offset_min, 2) && all_finite(layout.offset_max, 2);
}

[[nodiscard]] bool layout_enums_valid(const CyUiLayout& layout) noexcept {
    return layout.model <= CY_UI_LAYOUT_ABSOLUTE &&
           layout.direction <= CY_UI_DIRECTION_COLUMN_REVERSE &&
           layout.justify <= CY_UI_JUSTIFY_SPACE_EVENLY && layout.align <= CY_UI_ALIGN_END &&
           layout.self_align <= CY_UI_ALIGN_END;
}

}  // namespace

CyResult ui_root(CyEngine engine, CyUiElement* out_root) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_root", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (out_root == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_root needs an output");
    }
    *out_root = ui->root();
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_create(CyEngine engine, CyUiElement parent, const CyUiElementDesc* desc,
                   CyUiElement* out_element) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_create", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (desc == nullptr || out_element == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_create needs a description and an output");
    }
    CyUiElementDesc whole{};
    if (!read_sized(*desc, whole) || whole.kind > CY_UI_BUTTON) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_create: a malformed description or kind");
    }
    CyUiElement made = CY_UI_ELEMENT_NULL;
    if (const CyResult result = ui->create(parent, whole, made); result != CY_RESULT_OK) {
        return result;
    }
    *out_element = made;
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_destroy(CyEngine engine, CyUiElement element) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_destroy", refused);
    return ui == nullptr ? refused : answered(ui->destroy(element));
}

CyResult ui_set_layout(CyEngine engine, CyUiElement element, const CyUiLayout* layout) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_set_layout", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (layout == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_set_layout needs a layout");
    }
    CyUiLayout whole{};
    if (!read_sized(*layout, whole) || !layout_enums_valid(whole) || !layout_finite(whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "ui_set_layout: a malformed layout, an unknown enumerator or a non-finite "
                      "length");
    }
    return answered(ui->set_layout(element, whole));
}

CyResult ui_set_style(CyEngine engine, CyUiElement element, const CyUiStyle* style) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_set_style", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (style == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_set_style needs a style");
    }
    CyUiStyle whole{};
    if (!read_sized(*style, whole) || !finite(whole.border_width) || !finite(whole.corner_radius) ||
        whole.border_width < 0.0F || whole.corner_radius < 0.0F) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "ui_set_style: a malformed style or a negative or non-finite length");
    }
    return answered(ui->set_style(element, whole));
}

CyResult ui_set_text(CyEngine engine, CyUiElement element, const char* utf8, uint32_t colour,
                     uint32_t pixel_scale) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_set_text", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (utf8 == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_set_text needs text; pass \"\" for none");
    }
    return answered(ui->set_text(element, utf8, colour, pixel_scale == 0U ? 1U : pixel_scale));
}

CyResult ui_set_image(CyEngine engine, CyUiElement element, uint32_t atlas_page,
                      const float* uv_xywh) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_set_image", refused);
    if (ui == nullptr) {
        return refused;
    }
    f32 uv[4] = {0.0F, 0.0F, 1.0F, 1.0F};
    if (uv_xywh != nullptr) {
        if (!all_finite(uv_xywh, 4)) {
            return report(CY_RESULT_INVALID_ARGUMENT, "ui_set_image: a non-finite uv rectangle");
        }
        for (u32 index = 0; index < 4U; ++index) {
            uv[index] = uv_xywh[index];
        }
    }
    return answered(ui->set_image(element, atlas_page, uv));
}

CyResult ui_set_progress(CyEngine engine, CyUiElement element, float value) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_set_progress", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (!finite(value)) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_set_progress: the value is not finite");
    }
    const f32 clamped = std::clamp(value, 0.0F, 1.0F);
    return answered(ui->set_progress(element, clamped));
}

CyResult ui_set_visibility(CyEngine engine, CyUiElement element, uint32_t visibility) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_set_visibility", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (visibility > CY_UI_COLLAPSED) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_set_visibility: not a CyUiVisibility");
    }
    return answered(ui->set_visibility(element, static_cast<CyUiVisibility>(visibility)));
}

CyResult ui_set_opacity(CyEngine engine, CyUiElement element, float opacity) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_set_opacity", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (!finite(opacity) || opacity < 0.0F || opacity > 1.0F) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_set_opacity: the opacity is not in [0, 1]");
    }
    return answered(ui->set_opacity(element, opacity));
}

CyResult ui_element_rect(CyEngine engine, CyUiElement element, float* out_rect_xywh) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_element_rect", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (out_rect_xywh == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_element_rect needs an output");
    }
    f32 rect[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    if (const CyResult result = ui->rect(element, rect); result != CY_RESULT_OK) {
        return result;
    }
    for (u32 index = 0; index < 4U; ++index) {
        out_rect_xywh[index] = rect[index];
    }
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_hit_test(CyEngine engine, const float* position_xy, CyUiElement* out_element) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_hit_test", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (position_xy == nullptr || out_element == nullptr || !all_finite(position_xy, 2)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "ui_hit_test needs a finite position and an output");
    }
    *out_element = ui->hit_test(position_xy[0], position_xy[1]);
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_focus(CyEngine engine, CyUiElement* out_element) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_focus", refused);
    if (ui == nullptr) {
        return refused;
    }
    if (out_element == nullptr) {
        return report(CY_RESULT_INVALID_ARGUMENT, "ui_focus needs an output");
    }
    *out_element = ui->focus();
    clear_last_error();
    return CY_RESULT_OK;
}

CyResult ui_set_focus(CyEngine engine, CyUiElement element) {
    CyResult refused = CY_RESULT_OK;
    UiBackend* ui = backend(engine, "ui_set_focus", refused);
    return ui == nullptr ? refused : answered(ui->set_focus(element));
}

}  // namespace cy::abi::game
