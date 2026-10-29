// SPDX-License-Identifier: MIT
// input_script.cpp — see input_script.h.

#include "input_script.h"

#include "read_text.h"

#include <cy/core/platform/display_server.h>
#include <cy/platform/sdl3_event_injector.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace cy::play {
namespace {

/// A line split into words.
[[nodiscard]] std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    usize at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) {
            ++at;
        }
        const usize start = at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t' && line[at] != '\r') {
            ++at;
        }
        if (at > start) {
            out.push_back(line.substr(start, at - start));
        }
    }
    return out;
}

[[nodiscard]] f32 number(std::string_view text) {
    return std::strtof(std::string(text).c_str(), nullptr);
}

[[nodiscard]] input::Control control(const char* device, std::string_view name) {
    return input::cook::parse_control_path(std::string(device) + "/" + std::string(name));
}

[[nodiscard]] bool is_down(std::string_view text) noexcept {
    return text == "down";
}

/// The verb and its arguments, after the frame; false for a line this does not understand.
[[nodiscard]] bool parse_mouse(const std::vector<std::string_view>& w, ScriptEvent& event) {
    if (w.size() == 5 && (w[2] == "move" || w[2] == "to")) {
        event.verb = w[2] == "move" ? ScriptVerb::MouseMove : ScriptVerb::MouseTo;
        event.x = number(w[3]);
        event.y = number(w[4]);
        return true;
    }
    if (w.size() == 4) {
        event.verb = ScriptVerb::MouseButton;
        event.control = control("mouse", w[2]);
        event.down = is_down(w[3]);
        return event.control.is_valid();
    }
    return false;
}

[[nodiscard]] bool parse_pad(const std::vector<std::string_view>& w, ScriptEvent& event) {
    if (w.size() == 3 && w[2] == "attach") {
        event.verb = ScriptVerb::PadAttach;
        return true;
    }
    if (w.size() != 5) {
        return false;
    }
    event.control = control("gamepad", w[3]);
    if (w[2] == "axis") {
        event.verb = ScriptVerb::PadAxis;
        event.x = number(w[4]);
    } else if (w[2] == "button") {
        event.verb = ScriptVerb::PadButton;
        event.down = is_down(w[4]);
    } else {
        return false;
    }
    return event.control.is_valid();
}

[[nodiscard]] bool parse_event(const std::vector<std::string_view>& w, ScriptEvent& event) {
    event.frame = std::strtoull(std::string(w[0]).c_str(), nullptr, 10);
    const std::string_view verb = w.size() > 1 ? w[1] : std::string_view{};
    if (verb == "key" && w.size() == 4) {
        event.verb = ScriptVerb::Key;
        event.control = control("keyboard", w[2]);
        event.down = is_down(w[3]);
        return event.control.is_valid();
    }
    if (verb == "mouse") {
        return parse_mouse(w, event);
    }
    if (verb == "focus" && w.size() == 3) {
        event.verb = ScriptVerb::Focus;
        event.down = w[2] == "gained";
        return true;
    }
    if (verb == "pad") {
        return parse_pad(w, event);
    }
    if (verb == "window" && w.size() == 4) {
        event.verb = ScriptVerb::Window;
        event.x = number(w[2]);
        event.y = number(w[3]);
        return event.x >= 1.0F && event.y >= 1.0F;
    }
    if (verb == "quit" && w.size() == 2) {
        event.verb = ScriptVerb::Quit;
        return true;
    }
    return false;
}

}  // namespace

Status InputScript::parse(std::string_view text) noexcept {
    events_.clear();
    next_ = 0;
    u32 number = 0;
    while (!text.empty()) {
        ++number;
        const usize end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1U);
        if (const usize hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        const std::vector<std::string_view> w = words(line);
        if (w.empty()) {
            continue;
        }
        ScriptEvent event;
        if (!parse_event(w, event)) {
            error_ = "input script line " + std::to_string(number) + ": '" + std::string(line) +
                     "' is not an event this script knows";
            return fail(ErrorCode::InvalidArgument, error_.c_str());
        }
        events_.push_back(event);
    }
    // Stable: lines of one frame keep their written order.
    std::ranges::stable_sort(
        events_, [](const ScriptEvent& a, const ScriptEvent& b) { return a.frame < b.frame; });
    return ok();
}

Status InputScript::load(const char* path) noexcept {
    std::string text;
    if (!read_text(path, text)) {
        error_ = std::string("cannot read the input script ") + path;
        return fail(ErrorCode::NotFound, error_.c_str());
    }
    return parse(text);
}

Status InputScript::push(const ScriptEvent& event, const ScriptTarget& target) noexcept {
    Sdl3EventInjector& inject = *target.inject;
    switch (event.verb) {
        case ScriptVerb::Key:
            return inject.key(static_cast<input::Key>(event.control.code), event.down);
        case ScriptVerb::MouseMove:
            cursor_x_ += event.x;
            cursor_y_ += event.y;
            return inject.mouse_motion(event.x, event.y, cursor_x_, cursor_y_);
        case ScriptVerb::MouseTo: {
            const f32 dx = event.x - cursor_x_;
            const f32 dy = event.y - cursor_y_;
            cursor_x_ = event.x;
            cursor_y_ = event.y;
            return inject.mouse_motion(dx, dy, cursor_x_, cursor_y_);
        }
        case ScriptVerb::MouseButton:
            return inject.mouse_button(static_cast<input::MouseControl>(event.control.code),
                                       event.down, cursor_x_, cursor_y_);
        case ScriptVerb::Focus:
            return inject.window_focus(target.native_window, event.down);
        case ScriptVerb::PadAttach:
            return inject.attach_gamepad();
        case ScriptVerb::PadAxis:
            return inject.gamepad_axis(static_cast<input::GamepadControl>(event.control.code),
                                       event.x);
        case ScriptVerb::PadButton:
            return inject.gamepad_button(static_cast<input::GamepadControl>(event.control.code),
                                         event.down);
        case ScriptVerb::Window:
            if (target.display == nullptr) {
                return fail(ErrorCode::Unavailable, "a window line needs the display server");
            }
            return target.display->set_window_size(
                static_cast<WindowId>(target.window),
                Extent{static_cast<i32>(event.x), static_cast<i32>(event.y)});
        case ScriptVerb::Quit:
            break;
    }
    return ok();
}

Expected<bool, Error> InputScript::run(u64 frame, const ScriptTarget& target) noexcept {
    bool quit = false;
    while (next_ < events_.size() && events_[next_].frame <= frame) {
        const ScriptEvent& event = events_[next_++];
        if (event.verb == ScriptVerb::Quit) {
            quit = true;
            continue;
        }
        if (target.inject == nullptr) {
            return fail(ErrorCode::Unavailable, "an input script needs the SDL3 platform");
        }
        if (Status pushed = push(event, target); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    return quit;
}

}  // namespace cy::play
