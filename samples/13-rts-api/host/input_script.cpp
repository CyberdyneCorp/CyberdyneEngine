// SPDX-License-Identifier: MIT
#include "input_script.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace sample::rts {
namespace {

using cy::input::Key;
using cy::input::MouseControl;

[[nodiscard]] Key key_named(const std::string& name) noexcept {
    if (name.size() == 1U && name[0] >= 'a' && name[0] <= 'z') {
        return static_cast<Key>(static_cast<cy::u16>(Key::A) + (name[0] - 'a'));
    }
    if (name.size() == 1U && name[0] >= '0' && name[0] <= '9') {
        return static_cast<Key>(static_cast<cy::u16>(Key::Num0) + (name[0] - '0'));
    }
    static const std::pair<const char*, Key> kNamed[] = {
        {"escape", Key::Escape}, {"space", Key::Space},     {"enter", Key::Enter},
        {"tab", Key::Tab},       {"shift", Key::LeftShift}, {"ctrl", Key::LeftControl},
        {"alt", Key::LeftAlt},   {"left", Key::Left},       {"right", Key::Right},
        {"up", Key::Up},         {"down", Key::Down},
    };
    for (const auto& [text, key] : kNamed) {
        if (name == text) {
            return key;
        }
    }
    return Key::Unknown;
}

[[nodiscard]] MouseControl button_named(const std::string& name) noexcept {
    if (name == "left") {
        return MouseControl::Left;
    }
    if (name == "right") {
        return MouseControl::Right;
    }
    return name == "middle" ? MouseControl::Middle : MouseControl::Unknown;
}

[[nodiscard]] bool parse_key(std::istringstream& words, ScriptEvent& out) {
    std::string name;
    std::string edge;
    if (!(words >> name >> edge) || (edge != "down" && edge != "up")) {
        return false;
    }
    out.kind = ScriptEvent::Kind::Key;
    out.key = key_named(name);
    out.down = edge == "down";
    return out.key != Key::Unknown;
}

[[nodiscard]] bool parse_button(std::istringstream& words, ScriptEvent& out, bool down) {
    std::string name;
    if (!(words >> name)) {
        return false;
    }
    out.kind = down ? ScriptEvent::Kind::Press : ScriptEvent::Kind::Release;
    out.button = button_named(name);
    out.down = down;
    return out.button != MouseControl::Unknown;
}

[[nodiscard]] bool parse_aim(std::istringstream& words, ScriptEvent& out) {
    std::string what;
    if (!(words >> what)) {
        return false;
    }
    if (what == "field") {
        out.kind = ScriptEvent::Kind::AimField;
        return static_cast<bool>(words >> out.x >> out.y);
    }
    if (what == "ui") {
        out.kind = ScriptEvent::Kind::AimUi;
        return static_cast<bool>(words >> out.name);
    }
    if (what != "unit" || !(words >> out.unit)) {
        return false;
    }
    out.kind = ScriptEvent::Kind::AimUnit;
    // The offset is optional: without it the aim is at the chest.
    if (!(words >> out.x)) {
        out.x = 0.0F;
        return true;
    }
    return static_cast<bool>(words >> out.y);
}

[[nodiscard]] bool parse_verb(const std::string& verb, std::istringstream& words,
                              ScriptEvent& out) {
    if (verb == "key") {
        return parse_key(words, out);
    }
    if (verb == "move") {
        out.kind = ScriptEvent::Kind::Move;
        return static_cast<bool>(words >> out.x >> out.y);
    }
    if (verb == "press" || verb == "release") {
        return parse_button(words, out, verb == "press");
    }
    if (verb == "wheel") {
        out.kind = ScriptEvent::Kind::Wheel;
        return static_cast<bool>(words >> out.x);
    }
    if (verb == "aim") {
        return parse_aim(words, out);
    }
    if (verb == "resize") {
        out.kind = ScriptEvent::Kind::Resize;
        return static_cast<bool>(words >> out.x >> out.y) && out.x >= 1.0F && out.y >= 1.0F;
    }
    out.kind = ScriptEvent::Kind::Quit;
    return verb == "quit";
}

}  // namespace

bool parse_script_line(const std::string& line, ScriptEvent& out, bool& blank) noexcept {
    const std::string body = line.substr(0, line.find('#'));
    std::istringstream words(body);
    std::string verb;
    if (!(words >> out.frame)) {
        // A line with nothing on it but a comment or blanks; anything else is malformed.
        blank = body.find_first_not_of(" \t\r\n") == std::string::npos;
        return blank;
    }
    blank = false;
    if (!(words >> verb) || !parse_verb(verb, words, out)) {
        return false;
    }
    std::string extra;
    return !(words >> extra);
}

cy::Status read_input_script(const std::string& path, std::vector<ScriptEvent>& out,
                             std::string& detail) noexcept {
    std::ifstream file(path);
    if (!file) {
        detail = path;
        return cy::fail(cy::ErrorCode::NotFound, "the input script could not be opened");
    }
    out.clear();
    std::string line;
    for (cy::u32 number = 1; std::getline(file, line); ++number) {
        ScriptEvent event;
        bool blank = false;
        if (!parse_script_line(line, event, blank)) {
            detail = path;
            detail.append(":").append(std::to_string(number)).append(": ").append(line);
            return cy::fail(cy::ErrorCode::InvalidArgument, "an input script line is malformed");
        }
        if (!blank) {
            out.push_back(event);
        }
    }
    // Frame order, keeping a frame's lines in the order written: a press after its move.
    std::ranges::stable_sort(
        out, [](const ScriptEvent& a, const ScriptEvent& b) { return a.frame < b.frame; });
    return cy::ok();
}

}  // namespace sample::rts
