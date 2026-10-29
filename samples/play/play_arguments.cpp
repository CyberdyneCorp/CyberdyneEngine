// SPDX-License-Identifier: MIT
// play_arguments.cpp — see play_arguments.h.

#include "play_arguments.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cy::play {
namespace {

/// `<w>x<h>`, or false.
[[nodiscard]] bool parse_extent(const char* text, i32& width, i32& height) noexcept {
    char* end = nullptr;
    const long w = std::strtol(text, &end, 10);
    if (end == nullptr || (*end != 'x' && *end != 'X')) {
        return false;
    }
    const long h = std::strtol(end + 1, nullptr, 10);
    if (w <= 0 || h <= 0) {
        return false;
    }
    width = static_cast<i32>(w);
    height = static_cast<i32>(h);
    return true;
}

/// A flag with no value; true when `flag` was one.
[[nodiscard]] bool parse_switch(const char* flag, PlayArguments& out) noexcept {
    PlayOptions& options = out.options;
    if (std::strcmp(flag, "--play") == 0) {
        out.play = true;
    } else if (std::strcmp(flag, "--fullscreen") == 0) {
        options.fullscreen = true;
    } else if (std::strcmp(flag, "--no-vsync") == 0) {
        options.vsync = false;
    } else if (std::strcmp(flag, "--no-draw") == 0) {
        options.draw = false;
    } else if (std::strcmp(flag, "--fixed-clock") == 0) {
        options.fixed_clock = true;
    } else if (std::strcmp(flag, "--validation") == 0) {
        options.validation = true;
    } else {
        return false;
    }
    return true;
}

/// A flag with a value; true when `flag` was one and its value parsed.
[[nodiscard]] bool parse_valued(const char* flag, const char* value, PlayArguments& out) noexcept {
    PlayOptions& options = out.options;
    if (std::strcmp(flag, "--window") == 0) {
        return parse_extent(value, options.window_width, options.window_height);
    }
    if (std::strcmp(flag, "--frames") == 0) {
        options.frame_limit = std::strtoull(value, nullptr, 10);
    } else if (std::strcmp(flag, "--input-script") == 0) {
        out.input_script = value;
    } else if (std::strcmp(flag, "--bindings") == 0) {
        out.bindings = value;
    } else if (std::strcmp(flag, "--capture") == 0) {
        out.capture_directory = value;
        options.capture_every = options.capture_every == 0U ? 1U : options.capture_every;
    } else if (std::strcmp(flag, "--capture-every") == 0) {
        options.capture_every = static_cast<u32>(std::strtoul(value, nullptr, 10));
    } else {
        return false;
    }
    return true;
}

}  // namespace

bool parse_play_flag(int argc, char** argv, int& index, PlayArguments& out) noexcept {
    const char* flag = argv[index];
    if (parse_switch(flag, out)) {
        return true;
    }
    if (index + 1 >= argc || !parse_valued(flag, argv[index + 1], out)) {
        return false;
    }
    ++index;
    // The strings the options point at live in `out`, which outlives the run.
    out.options.input_script = out.input_script.empty() ? nullptr : out.input_script.c_str();
    out.options.bindings = out.bindings.empty() ? nullptr : out.bindings.c_str();
    return true;
}

bool wants_play(int argc, char** argv) noexcept {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--play") == 0) {
            return true;
        }
    }
    return false;
}

void print_play_report(const PlayReport& report) noexcept {
    std::printf(
        "play loop    frames=%llu ticks=%llu paused_frames=%llu max_ticks=%u dropped=%llu "
        "pauses=%u focus_pauses=%u resumes=%u resizes=%u quit_by_menu=%d closed=%d "
        "relative_mouse=%d forwarded=%llu injected=%llu window=%ux%u\n",
        static_cast<unsigned long long>(report.frames),
        static_cast<unsigned long long>(report.ticks),
        static_cast<unsigned long long>(report.paused_frames), report.max_ticks_in_frame,
        static_cast<unsigned long long>(report.dropped_ticks), report.pauses, report.focus_pauses,
        report.resumes, report.resizes, report.quit_by_menu ? 1 : 0, report.closed ? 1 : 0,
        report.relative_mouse ? 1 : 0, static_cast<unsigned long long>(report.events_forwarded),
        static_cast<unsigned long long>(report.events_injected), report.window_width,
        report.window_height);
    std::printf(
        "play frame   presented=%llu captured=%llu timed=%llu frame_ms=%.3f p50=%.3f p99=%.3f "
        "worst=%.3f work_ms=%.3f work_p99=%.3f work_worst=%.3f validation_errors=%llu\n",
        static_cast<unsigned long long>(report.presented),
        static_cast<unsigned long long>(report.captured),
        static_cast<unsigned long long>(report.frame.frames), report.frame.mean, report.frame.p50,
        report.frame.p99, report.frame.worst, report.work.mean, report.work.p99,
        report.work.worst, static_cast<unsigned long long>(report.validation_errors));
}

}  // namespace cy::play
