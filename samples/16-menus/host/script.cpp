// SPDX-License-Identifier: MIT
// The scripted player. See script.h.

#include "script.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include "menus_host.h"

namespace sample::menus {
namespace {

[[nodiscard]] cy::Status refuse(cy::u32 line, const char* why) noexcept {
    static char message[160];
    (void)std::snprintf(message, sizeof(message), "script line %u: %s", line, why);
    return cy::fail(cy::ErrorCode::InvalidArgument, message);
}

[[nodiscard]] bool read_step(const std::string& text, ScriptStep& out) {
    std::istringstream line(text);
    if (!(line >> out.frame >> out.verb)) {
        return false;
    }
    if (out.verb == "report") {
        return true;
    }
    if (!(line >> out.target)) {
        return false;
    }
    if (out.verb == "point" && out.target[0] != '#') {
        out.x = std::strtof(out.target.c_str(), nullptr);
        out.target.clear();
        return static_cast<bool>(line >> out.y);
    }
    if (out.verb == "hold" && !(line >> out.hold)) {
        return false;
    }
    if (out.verb == "edit") {
        return static_cast<bool>(line >> out.find >> out.replace);
    }
    return out.verb == "tap" || out.verb == "hold" || out.verb == "press" ||
           out.verb == "release" || out.verb == "point" || out.verb == "shot";
}

}  // namespace

cy::Status Script::load(const char* path, std::string project) noexcept {
    project_ = std::move(project);
    std::ifstream input(path);
    if (!input.good()) {
        return cy::fail(cy::ErrorCode::NotFound, "the script could not be opened");
    }
    std::string text;
    cy::u32 number = 0;
    cy::u64 previous = 0;
    while (std::getline(input, text)) {
        ++number;
        const std::size_t first = text.find_first_not_of(" \t");
        if (first == std::string::npos || text[first] == '#') {
            continue;
        }
        ScriptStep step;
        if (!read_step(text, step)) {
            return refuse(number, "a step is `<frame> <verb> <arguments>`");
        }
        if (step.frame < previous) {
            return refuse(number, "frames go forwards");
        }
        previous = step.frame;
        steps_.push_back(step);
    }
    return cy::ok();
}

cy::u64 Script::last_frame() const noexcept {
    return steps_.empty() ? 0U : steps_.back().frame;
}

cy::Status Script::play(MenusHost& host, std::vector<std::string>& shots, bool& report) noexcept {
    const cy::u64 frame = host.frames();
    for (std::size_t index = 0; index < releases_.size();) {
        if (releases_[index].frame <= frame) {
            if (cy::Status released = host.press(releases_[index].target.c_str(), false);
                !released) {
                return released;
            }
            releases_.erase(releases_.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
    while (next_ < steps_.size() && steps_[next_].frame <= frame) {
        if (cy::Status done = step(host, steps_[next_], shots, report); !done) {
            return done;
        }
        ++next_;
    }
    return cy::ok();
}

cy::Status Script::step(MenusHost& host, const ScriptStep& line, std::vector<std::string>& shots,
                        bool& report) noexcept {
    if (line.verb == "tap" || line.verb == "hold") {
        ScriptStep release = line;
        release.frame = host.frames() + line.hold;
        releases_.push_back(release);
        return host.press(line.target.c_str(), true);
    }
    if (line.verb == "press" || line.verb == "release") {
        return host.press(line.target.c_str(), line.verb == "press");
    }
    if (line.verb == "point") {
        cy::f32 x = line.x;
        cy::f32 y = line.y;
        if (!line.target.empty() && !host.widget_centre(line.target.c_str() + 1, x, y)) {
            return cy::fail(cy::ErrorCode::NotFound, "the script points at a widget not shown");
        }
        return host.move_pointer(x, y);
    }
    if (line.verb == "edit") {
        return edit(line);
    }
    if (line.verb == "shot") {
        shots.push_back(line.target);
        return cy::ok();
    }
    report = true;
    return cy::ok();
}

cy::Status Script::edit(const ScriptStep& line) const noexcept {
    const std::string path = project_ + "/" + line.target;
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        return cy::fail(cy::ErrorCode::NotFound, "the script edits a file the project lacks");
    }
    std::ostringstream text;
    text << input.rdbuf();
    std::string content = text.str();
    const std::size_t at = content.find(line.find);
    if (at == std::string::npos) {
        return cy::fail(cy::ErrorCode::NotFound, "the script edits a word the file lacks");
    }
    content.replace(at, line.find.size(), line.replace);
    // Whole, as the editor's save writes it: to a temporary, then renamed over the file.
    const std::string temporary = path + ".saving";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << content;
        if (!output.good()) {
            return cy::fail(cy::ErrorCode::Io, "the script's edit could not be written");
        }
    }
    return std::rename(temporary.c_str(), path.c_str()) == 0
               ? cy::ok()
               : cy::fail(cy::ErrorCode::Io, "the script's edit could not be saved over the file");
}

}  // namespace sample::menus
