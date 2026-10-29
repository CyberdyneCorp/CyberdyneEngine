// SPDX-License-Identifier: MIT
// The ABI 1.17 entries: what a game's menus need — rebinding through the input system's own flow,
// the player's profile, the display settings, the player's files and a widget's tooltip.
//
// Every entry is `[N U]`: menus, the display and files are not the simulation's, and a fixed step
// that rebound a key or wrote a file would do it again on every resimulation. The backends are the
// 1.3 input backend, the 1.6 interface backend, and cy/abi/game/menus.h's two; what is checked here
// is what a backend could not tell from a module's mistake — the pointers, the sized structs, the
// enumerators, the flags and a file's name.

#include <cy/abi/errors.h>
#include <cy/abi/game/input.h>
#include <cy/abi/game/menus.h>
#include <cy/abi/game/services.h>
#include <cy/abi/game/ui.h>
#include <cy/abi/host.h>

#include <cstring>

#include "thunks.h"

namespace cy::abi::game {
namespace {

constexpr u32 kMenuPhases = kPhaseNone | kPhaseFrame;
constexpr u32 kLastScheme = CY_INPUT_SCHEME_GAMEPAD;
constexpr u32 kLastConflict = CY_INPUT_CONFLICT_UNBIND_PREVIOUS;
constexpr u32 kLastQuality = CY_VIDEO_QUALITY_ULTRA;
constexpr u32 kLastUpscale = CY_VIDEO_UPSCALE_PERFORMANCE;
constexpr u32 kVideoFlags = CY_VIDEO_FULLSCREEN | CY_VIDEO_VSYNC;
/// The longest player file name: a name, not a path.
constexpr usize kMaxFileName = 64;

/// The engine after the phase check, or null with the refusal in `refused`.
[[nodiscard]] CyEngine enter(CyEngine engine, const char* entry, CyResult& refused) noexcept {
    if (engine == nullptr) {
        refused = report(CY_RESULT_INVALID_ARGUMENT, "the engine is null");
        return nullptr;
    }
    refused = require_phase(engine->game, kMenuPhases, entry);
    return refused == CY_RESULT_OK ? engine : nullptr;
}

template <class Backend>
[[nodiscard]] Backend* bound(Backend* backend, const char* what, CyResult& refused) noexcept {
    if (backend == nullptr) {
        refused = report(CY_RESULT_UNAVAILABLE, what);
    }
    return backend;
}

[[nodiscard]] InputBackend* input(CyEngine engine, const char* entry, CyResult& refused) noexcept {
    CyEngine checked = enter(engine, entry, refused);
    return checked == nullptr
               ? nullptr
               : bound(checked->game.input, "no input server is bound to this engine", refused);
}

[[nodiscard]] VideoBackend* video(CyEngine engine, const char* entry, CyResult& refused) noexcept {
    CyEngine checked = enter(engine, entry, refused);
    return checked == nullptr ? nullptr
                              : bound(checked->game.video,
                                      "no display settings are bound to this engine", refused);
}

[[nodiscard]] UserDataBackend* user_data(CyEngine engine, const char* entry,
                                         CyResult& refused) noexcept {
    CyEngine checked = enter(engine, entry, refused);
    return checked == nullptr ? nullptr
                              : bound(checked->game.user_data,
                                      "no user-data directory is bound to this engine", refused);
}

[[nodiscard]] CyResult answered(CyResult result) noexcept {
    if (result == CY_RESULT_OK) {
        clear_last_error();
    }
    return result;
}

/// Copy `size` bytes of `text` and a NUL into a caller's buffer of `capacity`, cut at a code point
/// when it does not fit; `*out_size` is always the whole size. A null buffer asks the size.
[[nodiscard]] CyResult copy_text(const char* text, u32 size, char* buffer, u32 capacity,
                                 u32* out_size) noexcept {
    *out_size = size;
    if (buffer == nullptr || capacity == 0U) {
        return answered(CY_RESULT_OK);
    }
    u32 fits = size < capacity ? size : capacity - 1U;
    // Back off the continuation bytes of a code point cut in half.
    while (fits > 0U && fits < size && (static_cast<u8>(text[fits]) & 0xC0U) == 0x80U) {
        --fits;
    }
    if (fits > 0U) {
        std::memcpy(buffer, text, fits);
    }
    buffer[fits] = '\0';
    return answered(CY_RESULT_OK);
}

/// One plain file name: letters, digits, '.', '-' and '_', not starting with a dot.
[[nodiscard]] bool plain_name(const char* name) noexcept {
    if (name == nullptr || name[0] == '\0' || name[0] == '.') {
        return false;
    }
    usize length = 0;
    for (const char* at = name; *at != '\0'; ++at, ++length) {
        const char c = *at;
        const bool fine = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!fine || length >= kMaxFileName) {
            return false;
        }
    }
    return true;
}

}  // namespace

// --- The defaults: a backend from before 1.17 ----------------------------------------------------

namespace {
CyResult no_rebinding() noexcept {
    return report(CY_RESULT_UNAVAILABLE, "this input backend has no rebinding");
}
}  // namespace

CyResult InputBackend::rebind_begin(u32, CyInputAction, u32, u32, u32) noexcept {
    return no_rebinding();
}
CyResult InputBackend::rebind_poll(u32, CyInputRebind&) noexcept {
    return no_rebinding();
}
CyResult InputBackend::rebind_cancel(u32) noexcept {
    return no_rebinding();
}
CyResult InputBackend::binding(u32, CyInputAction, u32, u32, char (&)[64]) noexcept {
    return no_rebinding();
}
CyResult InputBackend::profile_save(u32, const char*&, u32&) noexcept {
    return no_rebinding();
}
CyResult InputBackend::profile_load(u32, const char*, u64) noexcept {
    return no_rebinding();
}
CyResult InputBackend::profile_reset(u32) noexcept {
    return no_rebinding();
}
CyResult UiBackend::widget_set_tooltip(CyUiElement, const char*) noexcept {
    return report(CY_RESULT_UNAVAILABLE, "no widget toolkit is attached to this interface");
}

// --- Rebinding and the profile -------------------------------------------------------------------

CyResult input_rebind_begin(CyEngine engine, uint32_t user, CyInputAction action, uint32_t scheme,
                            uint32_t slot, uint32_t conflict) {
    CyResult refused = CY_RESULT_OK;
    InputBackend* backend = input(engine, "input_rebind_begin", refused);
    if (backend == nullptr) {
        return refused;
    }
    if (scheme > kLastScheme || conflict > kLastConflict || slot > 255U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "input_rebind_begin: a scheme, a conflict policy or a slot out of range");
    }
    return answered(backend->rebind_begin(user, action, scheme, slot, conflict));
}

CyResult input_rebind_poll(CyEngine engine, uint32_t user, CyInputRebind* out_rebind) {
    CyResult refused = CY_RESULT_OK;
    InputBackend* backend = input(engine, "input_rebind_poll", refused);
    if (backend == nullptr) {
        return refused;
    }
    if (out_rebind == nullptr || agreed_size<CyInputRebind>(out_rebind->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "input_rebind_poll needs an output with a valid struct_size");
    }
    CyInputRebind whole{};
    whole.struct_size = sizeof(CyInputRebind);
    const CyResult result = backend->rebind_poll(user, whole);
    if (result == CY_RESULT_OK) {
        (void)write_sized(*out_rebind, whole);
    }
    return answered(result);
}

CyResult input_rebind_cancel(CyEngine engine, uint32_t user) {
    CyResult refused = CY_RESULT_OK;
    InputBackend* backend = input(engine, "input_rebind_cancel", refused);
    return backend == nullptr ? refused : answered(backend->rebind_cancel(user));
}

CyResult input_binding(CyEngine engine, uint32_t user, CyInputAction action, uint32_t scheme,
                       uint32_t slot, char* buffer, uint32_t capacity, uint32_t* out_size) {
    CyResult refused = CY_RESULT_OK;
    InputBackend* backend = input(engine, "input_binding", refused);
    if (backend == nullptr) {
        return refused;
    }
    if (out_size == nullptr || (buffer == nullptr && capacity != 0U) || scheme > kLastScheme ||
        slot > 255U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "input_binding needs a size output, a scheme and a slot in range");
    }
    char path[64] = {};
    if (const CyResult result = backend->binding(user, action, scheme, slot, path);
        result != CY_RESULT_OK) {
        return result;
    }
    return copy_text(path, static_cast<u32>(std::strlen(path)), buffer, capacity, out_size);
}

CyResult input_profile_save(CyEngine engine, uint32_t user, char* buffer, uint32_t capacity,
                            uint32_t* out_size) {
    CyResult refused = CY_RESULT_OK;
    InputBackend* backend = input(engine, "input_profile_save", refused);
    if (backend == nullptr) {
        return refused;
    }
    if (out_size == nullptr || (buffer == nullptr && capacity != 0U)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "input_profile_save needs a size output, and a buffer for its capacity");
    }
    const char* text = "";
    u32 size = 0;
    if (const CyResult result = backend->profile_save(user, text, size); result != CY_RESULT_OK) {
        return result;
    }
    return copy_text(text, size, buffer, capacity, out_size);
}

CyResult input_profile_load(CyEngine engine, uint32_t user, const char* text, uint64_t size) {
    CyResult refused = CY_RESULT_OK;
    InputBackend* backend = input(engine, "input_profile_load", refused);
    if (backend == nullptr) {
        return refused;
    }
    if (text == nullptr && size != 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT, "input_profile_load: a null text with a size");
    }
    return answered(backend->profile_load(user, text == nullptr ? "" : text, size));
}

CyResult input_profile_reset(CyEngine engine, uint32_t user) {
    CyResult refused = CY_RESULT_OK;
    InputBackend* backend = input(engine, "input_profile_reset", refused);
    return backend == nullptr ? refused : answered(backend->profile_reset(user));
}

// --- The display ---------------------------------------------------------------------------------

CyResult video_get_settings(CyEngine engine, CyVideoSettings* out_settings) {
    CyResult refused = CY_RESULT_OK;
    VideoBackend* backend = video(engine, "video_get_settings", refused);
    if (backend == nullptr) {
        return refused;
    }
    if (out_settings == nullptr || agreed_size<CyVideoSettings>(out_settings->struct_size) == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "video_get_settings needs an output with a valid struct_size");
    }
    CyVideoSettings whole{};
    whole.struct_size = sizeof(CyVideoSettings);
    const CyResult result = backend->settings(whole);
    if (result == CY_RESULT_OK) {
        (void)write_sized(*out_settings, whole);
    }
    return answered(result);
}

CyResult video_set_settings(CyEngine engine, const CyVideoSettings* settings) {
    CyResult refused = CY_RESULT_OK;
    VideoBackend* backend = video(engine, "video_set_settings", refused);
    if (backend == nullptr) {
        return refused;
    }
    CyVideoSettings whole{};
    if (settings == nullptr || !read_sized(*settings, whole)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "video_set_settings needs settings with a valid struct_size");
    }
    if (whole.quality > kLastQuality || whole.upscale > kLastUpscale ||
        (whole.flags & ~kVideoFlags) != 0U || whole.width == 0U || whole.height == 0U) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "video_set_settings: a quality, an upscaler or a flag out of range, or an "
                      "empty resolution");
    }
    return answered(backend->apply(whole));
}

CyResult video_modes(CyEngine engine, CyVideoMode* out_modes, uint32_t capacity,
                     uint32_t* out_count) {
    CyResult refused = CY_RESULT_OK;
    VideoBackend* backend = video(engine, "video_modes", refused);
    if (backend == nullptr) {
        return refused;
    }
    if (out_count == nullptr || (out_modes == nullptr && capacity != 0U)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "video_modes needs a count output, and a buffer for its capacity");
    }
    u32 count = 0;
    if (const CyResult result = backend->modes(out_modes, capacity, count);
        result != CY_RESULT_OK) {
        return result;
    }
    *out_count = count;
    if (out_modes != nullptr && count > capacity) {
        return report(CY_RESULT_BUFFER_TOO_SMALL, "video_modes: more modes than the buffer holds");
    }
    return answered(CY_RESULT_OK);
}

// --- The player's files --------------------------------------------------------------------------

CyResult user_data_write(CyEngine engine, const char* name, const void* bytes, uint64_t size) {
    CyResult refused = CY_RESULT_OK;
    UserDataBackend* backend = user_data(engine, "user_data_write", refused);
    if (backend == nullptr) {
        return refused;
    }
    if (!plain_name(name) || (bytes == nullptr && size != 0U)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "user_data_write: a file name is letters, digits, '.', '-' and '_', and "
                      "bytes go with a size");
    }
    return answered(backend->write(name, bytes, size));
}

CyResult user_data_read(CyEngine engine, const char* name, void* buffer, uint64_t capacity,
                        uint64_t* out_size) {
    CyResult refused = CY_RESULT_OK;
    UserDataBackend* backend = user_data(engine, "user_data_read", refused);
    if (backend == nullptr) {
        return refused;
    }
    if (!plain_name(name) || out_size == nullptr || (buffer == nullptr && capacity != 0U)) {
        return report(CY_RESULT_INVALID_ARGUMENT,
                      "user_data_read: a plain file name, a size output, and a buffer for its "
                      "capacity");
    }
    u64 size = 0;
    if (const CyResult result = backend->read(name, buffer, capacity, size);
        result != CY_RESULT_OK) {
        return result;
    }
    *out_size = size;
    if (buffer != nullptr && size > capacity) {
        return report(CY_RESULT_BUFFER_TOO_SMALL,
                      "user_data_read: the file is larger than the "
                      "buffer; its first bytes were read");
    }
    return answered(CY_RESULT_OK);
}

// --- Tooltips ------------------------------------------------------------------------------------

CyResult ui_widget_set_tooltip(CyEngine engine, CyUiElement widget, const char* utf8) {
    CyResult refused = CY_RESULT_OK;
    CyEngine checked = enter(engine, "ui_widget_set_tooltip", refused);
    if (checked == nullptr) {
        return refused;
    }
    UiBackend* ui = bound(checked->game.ui, "no interface is bound to this engine", refused);
    if (ui == nullptr) {
        return refused;
    }
    return answered(ui->widget_set_tooltip(widget, utf8 == nullptr ? "" : utf8));
}

}  // namespace cy::abi::game
