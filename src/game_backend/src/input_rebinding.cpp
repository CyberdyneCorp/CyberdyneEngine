// SPDX-License-Identifier: MIT
// ABI 1.13's rebinding and the player's profile, over `cy::input::RebindOperation` and the input
// user's `BindingProfile`. See cy/game_backend/input_backend.h.
//
// THE PROFILE'S TEXT. One override a line, after a `cyinputprofile 1` header:
//
//     cyinputprofile 1
//     2 keyboard 0 keyboard/g Fire
//     2 gamepad 0 gamepad/north Fire
//     5 keyboard 0 none Crouch
//
// The action's STABLE id, the scheme, the slot, the control's path (`none`: unbound by the player),
// and the action's name for a person reading the file. The stable id is what resolves — a renamed
// action keeps the player's choice — and a line naming an action or a control this build does not
// have is skipped, so an old profile loads into a newer game.

#include <cy/abi/errors.h>
#include <cy/game_backend/input_backend.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace cy::game_backend {
namespace {

using cy::input::ActionId;
using cy::input::ActionStableId;
using cy::input::BindingOverride;
using cy::input::ConflictPolicy;
using cy::input::Control;
using cy::input::DeviceKind;
using cy::input::SchemeKind;

constexpr std::string_view kProfileHeader = "cyinputprofile 1";

[[nodiscard]] SchemeKind scheme_kind(u32 scheme) noexcept {
    return scheme == CY_INPUT_SCHEME_GAMEPAD ? SchemeKind::Gamepad : SchemeKind::KeyboardMouse;
}

[[nodiscard]] const char* scheme_word(SchemeKind scheme) noexcept {
    return scheme == SchemeKind::Gamepad ? "gamepad" : "keyboard";
}

/// `keyboard/f`, `gamepad/south`, or `none`, into `out`.
void spell(Control control, char (&out)[64]) noexcept {
    if (!control.is_valid()) {
        (void)std::snprintf(out, sizeof(out), "none");
        return;
    }
    char device[24] = {};
    (void)std::snprintf(device, sizeof(device), "%s", cy::input::device_kind_name(control.kind));
    for (char* at = device; *at != '\0'; ++at) {
        if (*at >= 'A' && *at <= 'Z') {
            *at = static_cast<char>(*at - 'A' + 'a');
        }
    }
    (void)std::snprintf(out, sizeof(out), "%s/%s", device, cy::input::cook::control_path(control));
}

template <usize Size>
void copy_out(char (&out)[Size], std::string_view text) noexcept {
    (void)std::snprintf(out, Size, "%.*s", static_cast<int>(text.size()), text.data());
}

/// The next space-separated word of `line`, consumed from it; empty at the end.
[[nodiscard]] std::string_view next_word(std::string_view& line) noexcept {
    const usize start = line.find_first_not_of(' ');
    if (start == std::string_view::npos) {
        line = {};
        return {};
    }
    line.remove_prefix(start);
    const usize end = std::min(line.find(' '), line.size());
    const std::string_view word = line.substr(0, end);
    line.remove_prefix(end);
    return word;
}

/// A whole decimal word as a number, or false.
[[nodiscard]] bool read_number(std::string_view word, u32& out) noexcept {
    if (word.empty()) {
        return false;
    }
    const char* first = &word.front();
    const char* last = first + word.size();
    const std::from_chars_result read = std::from_chars(first, last, out);
    return read.ec == std::errc{} && read.ptr == last;
}

/// Whether `control` is the cancel control of the scheme being rebound.
[[nodiscard]] bool cancels(Control control) noexcept {
    return control == cy::input::key_control(cy::input::Key::Escape) ||
           control == cy::input::gamepad_control(cy::input::GamepadControl::Back);
}

/// One profile line's four fields, or false. `unbound` is set for the explicit `none`.
[[nodiscard]] bool read_line(std::string_view line, BindingOverride& out, bool& unbound) noexcept {
    u32 stable = 0;
    u32 slot = 0;
    const bool numbered = read_number(next_word(line), stable);
    const std::string_view word = next_word(line);
    const bool slotted = read_number(next_word(line), slot);
    const std::string_view spelled = next_word(line);
    char path[64] = {};
    if (!numbered || !slotted || stable == 0U || slot > 255U || spelled.empty() ||
        spelled.size() >= sizeof(path) || (word != "keyboard" && word != "gamepad")) {
        return false;
    }
    std::memcpy(path, spelled.data(), spelled.size());
    out.action = ActionStableId{stable};
    out.scheme = word == "gamepad" ? SchemeKind::Gamepad : SchemeKind::KeyboardMouse;
    out.slot = static_cast<u8>(slot);
    unbound = std::string_view(path) == "none";
    out.control = unbound ? Control{} : cy::input::cook::parse_control_path(path);
    return true;
}

}  // namespace

CyResult InputAdapter::rebind_begin(u32 user, CyInputAction action, u32 scheme, u32 slot,
                                    u32 conflict) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    if (user >= kMaxPointerUsers) {
        return cy::abi::report(CY_RESULT_OUT_OF_RANGE, "input: rebinding past the tracked users");
    }
    if (action >= server_->actions().count()) {
        return cy::abi::report(CY_RESULT_NOT_FOUND, "input_rebind_begin: no such action");
    }
    Rebind& rebind = rebinds_[user];
    // The flow is the input system's; conflicts are settled here, against the bindings in
    // effect, which the profile alone does not know — so the operation itself allows any.
    rebind.operation =
        cy::input::RebindOperation(server_->actions().at(action).stable_id, scheme_kind(scheme),
                                   static_cast<u8>(slot), ConflictPolicy::Allow);
    rebind.operation.set_cancel_control(cy::input::key_control(cy::input::Key::Escape));
    rebind.action = action;
    rebind.scheme = scheme;
    rebind.slot = slot;
    rebind.policy = conflict;
    rebind.status = CY_INPUT_REBIND_LISTENING;
    rebind.candidate = Control{};
    rebind.conflict = 0;
    rebind.has_conflict = false;
    return CY_RESULT_OK;
}

void InputAdapter::offer(const cy::input::DeviceEvent& event) noexcept {
    const auto& devices = server_->devices();
    bool shared = false;
    const u32 routed = devices.route(event.device, shared);
    for (u32 user = 0; user < kMaxPointerUsers && user < server_->user_count(); ++user) {
        Rebind& rebind = rebinds_[user];
        if (rebind.status != CY_INPUT_REBIND_LISTENING || (!shared && routed != user)) {
            continue;
        }
        if (event.value != 0.0F && cancels(event.control)) {
            rebind.operation.cancel();
            rebind.status = CY_INPUT_REBIND_CANCELLED;
            continue;
        }
        const cy::input::RebindStatus status =
            rebind.operation.offer(event, server_->user(user).profile());
        if (status == cy::input::RebindStatus::Applied) {
            rebind.candidate = rebind.operation.candidate();
            settle(user, rebind);
        }
    }
}

void InputAdapter::settle(u32 user, Rebind& rebind) noexcept {
    cy::input::InputUser& target = server_->user(user);
    (void)server_->refresh_user(user);
    const SchemeKind scheme = scheme_kind(rebind.scheme);
    const ActionStableId mine = server_->actions().at(rebind.action).stable_id;
    ActionId holder = cy::input::kInvalidAction;
    u8 holder_slot = 0;
    const bool held =
        target.holder_of(rebind.candidate, scheme, rebind.action, holder, holder_slot) &&
        (holder != rebind.action || holder_slot != rebind.slot);
    if (held && holder != rebind.action && rebind.policy == CY_INPUT_CONFLICT_REJECT) {
        rebind.status = CY_INPUT_REBIND_CONFLICT;
        rebind.conflict = holder;
        rebind.has_conflict = true;
        return;
    }
    cy::input::BindingProfile& profile = target.profile();
    if (held && rebind.policy != CY_INPUT_CONFLICT_ALLOW) {
        // SWAP: the other binding takes the control this one has now; UNBIND: it is left unbound.
        Control previous;
        if (rebind.policy == CY_INPUT_CONFLICT_SWAP) {
            (void)target.binding_control(rebind.action, scheme, static_cast<u8>(rebind.slot),
                                         previous);
        }
        (void)profile.set(BindingOverride{server_->actions().at(holder).stable_id, scheme,
                                          holder_slot, previous});
        rebind.conflict = holder;
        rebind.has_conflict = holder != rebind.action;
    }
    if (!profile.set(
            BindingOverride{mine, scheme, static_cast<u8>(rebind.slot), rebind.candidate})) {
        rebind.status = CY_INPUT_REBIND_CANCELLED;
        return;
    }
    target.invalidate();
    (void)server_->refresh_user(user);
    rebind.status = CY_INPUT_REBIND_APPLIED;
}

CyResult InputAdapter::rebind_poll(u32 user, CyInputRebind& out) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    if (user >= kMaxPointerUsers) {
        return CY_RESULT_OK;
    }
    const Rebind& rebind = rebinds_[user];
    out.status = rebind.status;
    out.action = rebind.action;
    out.scheme = rebind.scheme;
    out.slot = rebind.slot;
    char path[64] = {};
    if (rebind.candidate.is_valid()) {
        spell(rebind.candidate, path);
        copy_out(out.control, path);
    }
    if (rebind.has_conflict && rebind.conflict < server_->actions().count()) {
        copy_out(out.conflict, server_->actions().at(rebind.conflict).name.text());
    }
    return CY_RESULT_OK;
}

CyResult InputAdapter::rebind_cancel(u32 user) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    if (user < kMaxPointerUsers && rebinds_[user].status == CY_INPUT_REBIND_LISTENING) {
        rebinds_[user].operation.cancel();
        rebinds_[user].status = CY_INPUT_REBIND_CANCELLED;
    }
    return CY_RESULT_OK;
}

CyResult InputAdapter::binding(u32 user, CyInputAction action, u32 scheme, u32 slot,
                               char (&out)[64]) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    if (action >= server_->actions().count()) {
        return cy::abi::report(CY_RESULT_NOT_FOUND, "input_binding: no such action");
    }
    (void)server_->refresh_user(user);
    Control control;
    if (!server_->user(user).binding_control(action, scheme_kind(scheme), static_cast<u8>(slot),
                                             control)) {
        return cy::abi::report(CY_RESULT_NOT_FOUND,
                               "input_binding: the action has no such binding on that scheme");
    }
    spell(control, out);
    return CY_RESULT_OK;
}

CyResult InputAdapter::profile_save(u32 user, const char*& text, u32& size) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    const cy::input::BindingProfile& profile = server_->user(user).profile();
    usize at = 0;
    const auto put = [this, &at](const char* line) noexcept {
        const usize length = std::strlen(line);
        if (at + length >= sizeof(profile_text_)) {
            return false;
        }
        std::memcpy(profile_text_ + at, line, length);
        at += length;
        return true;
    };
    bool fits = profile.count() == 0U || put("cyinputprofile 1\n");
    for (u32 index = 0; fits && index < profile.count(); ++index) {
        const BindingOverride& entry = profile.at(index);
        const ActionId action = server_->actions().find(entry.action);
        char path[64] = {};
        spell(entry.control, path);
        char line[192] = {};
        (void)std::snprintf(line, sizeof(line), "%u %s %u %s %s\n", entry.action.value,
                            scheme_word(entry.scheme), static_cast<unsigned>(entry.slot), path,
                            action == cy::input::kInvalidAction
                                ? "-"
                                : server_->actions().at(action).name.text().data());
        fits = put(line);
    }
    if (!fits) {
        return cy::abi::report(CY_RESULT_OUT_OF_RANGE, "input_profile_save: too many overrides");
    }
    profile_text_[at] = '\0';
    text = profile_text_;
    size = static_cast<u32>(at);
    return CY_RESULT_OK;
}

CyResult InputAdapter::profile_load(u32 user, const char* text, u64 size) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    const std::string_view whole(text, static_cast<usize>(size));
    // Twice: every line is read before the profile is touched, so a profile that does not read
    // changes nothing; then the overrides it names are put in place of the user's.
    for (const bool applying : {false, true}) {
        if (applying) {
            server_->user(user).profile().clear_all();
        }
        if (const CyResult read = load_lines(user, whole, applying); read != CY_RESULT_OK) {
            return read;
        }
    }
    server_->user(user).invalidate();
    return CY_RESULT_OK;
}

CyResult InputAdapter::load_lines(u32 user, std::string_view text, bool applying) noexcept {
    u32 number = 0;
    while (!text.empty() || number == 0U) {
        const usize end = std::min(text.find('\n'), text.size());
        const std::string_view line = text.substr(0, end);
        text = text.substr(std::min(end + 1, text.size()));
        ++number;
        if (number == 1U) {
            if (line != kProfileHeader && !(line.empty() && text.empty())) {
                return cy::abi::report(CY_RESULT_INVALID_ARGUMENT,
                                       "input_profile_load: line 1 is not `cyinputprofile 1`");
            }
            continue;
        }
        if (line.empty()) {
            continue;
        }
        BindingOverride entry;
        bool unbound = false;
        if (!read_line(line, entry, unbound)) {
            char message[96] = {};
            (void)std::snprintf(message, sizeof(message),
                                "input_profile_load: line %u does not read", number);
            return cy::abi::report(CY_RESULT_INVALID_ARGUMENT, message);
        }
        const bool known = server_->actions().find(entry.action) != cy::input::kInvalidAction;
        if (applying && known && (entry.control.is_valid() || unbound)) {
            if (Status kept = server_->user(user).profile().set(entry); !kept) {
                return cy::abi::report(CY_RESULT_OUT_OF_MEMORY, kept.error().message);
            }
        }
    }
    return CY_RESULT_OK;
}

CyResult InputAdapter::profile_reset(u32 user) noexcept {
    if (const CyResult checked = check_user(user); checked != CY_RESULT_OK) {
        return checked;
    }
    server_->user(user).profile().clear_all();
    server_->user(user).invalidate();
    return CY_RESULT_OK;
}

}  // namespace cy::game_backend
