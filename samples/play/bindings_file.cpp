// SPDX-License-Identifier: MIT
// bindings_file.cpp — see bindings_file.h.

#include "bindings_file.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace cy::play {
namespace {

/// The next whitespace-separated word of `line` from `at`, or empty.
[[nodiscard]] std::string_view word(std::string_view line, usize& at) noexcept {
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
        ++at;
    }
    const usize start = at;
    while (at < line.size() && line[at] != ' ' && line[at] != '\t') {
        ++at;
    }
    return line.substr(start, at - start);
}

[[nodiscard]] std::string refusal(u32 number, const char* why, std::string_view what) {
    return "line " + std::to_string(number) + ": " + why + " '" + std::string(what) + "'";
}

/// One line as an override, or why it is not one. A blank or comment line yields no override and
/// no error.
[[nodiscard]] bool parse_line(const input::InputServer& server, std::string_view line, u32 number,
                              std::vector<input::BindingOverride>& out, std::string& error) {
    if (const usize hash = line.find('#'); hash != std::string_view::npos) {
        line = line.substr(0, hash);
    }
    usize at = 0;
    const std::string_view action_name = word(line, at);
    if (action_name.empty()) {
        return true;
    }
    const std::string_view path = word(line, at);
    const std::string_view slot_text = word(line, at);
    const input::ActionId action =
        server.actions().find(Name::intern(action_name));
    if (action == input::kInvalidAction) {
        error = refusal(number, "no action is declared as", action_name);
        return false;
    }
    const input::Control control = input::cook::parse_control_path(path);
    if (!control.is_valid()) {
        error = refusal(number, "the input layer has no control at", path);
        return false;
    }
    input::BindingOverride entry;
    entry.action = server.actions().at(action).stable_id;
    entry.scheme = input::scheme_of(control.kind);
    entry.slot = slot_text.empty()
                     ? u8{0}
                     : static_cast<u8>(std::strtoul(std::string(slot_text).c_str(), nullptr, 10));
    entry.control = control;
    out.push_back(entry);
    return true;
}

}  // namespace

BindingsResult apply_bindings(input::InputServer& server, u32 user,
                              std::string_view text) noexcept {
    BindingsResult result;
    if (user >= server.user_count()) {
        result.error = "no input user " + std::to_string(user);
        return result;
    }
    std::vector<input::BindingOverride> overrides;
    u32 number = 0;
    while (!text.empty()) {
        ++number;
        const usize end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1U);
        if (!parse_line(server, line, number, overrides, result.error)) {
            return result;
        }
    }
    input::InputUser& target = server.user(user);
    for (const input::BindingOverride& entry : overrides) {
        if (Status stored = target.profile().set(entry); !stored) {
            result.error = stored.error().message;
            return result;
        }
        ++result.applied;
    }
    target.invalidate();
    return result;
}

BindingsResult apply_bindings_file(input::InputServer& server, u32 user,
                                   const char* path) noexcept {
    BindingsResult result;
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        result.error = std::string("cannot open ") + path;
        return result;
    }
    std::string text;
    char buffer[1024];
    for (usize read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) {
        text.append(buffer, read);
    }
    (void)std::fclose(file);
    return apply_bindings(server, user, text);
}

}  // namespace cy::play
