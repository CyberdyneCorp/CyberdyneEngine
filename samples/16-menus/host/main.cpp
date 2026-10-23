// SPDX-License-Identifier: MIT
// samples/16-menus — a game's menus in Swift, over documents made in the UI layout editor.
//
//   cy_sample_menus --script <file>              play a scripted session, headless; report at the
//   end cy_sample_menus --script <file> --render <dir>  the same, drawn on a device; `shot` writes
//   frames cy_sample_menus --user-data <dir>            where the player's settings and bindings
//   are kept cy_sample_menus --project <dir>              the project the documents are read from
//   cy_sample_menus --frames <n>                 how long to run (the script's last frame + 30)
//   cy_sample_menus ... --render <dir> --shot-size 480x270   shots averaged down to that size
//   cy_sample_menus ... --render <dir> --record  every frame written too: the frames of a video
//
// What it prints is one `menus:` line per observation a `report` step asked for, and one at the
// end: the screen the game is on, what it did, the widget with the focus, the display settings and
// the extents they render at, the GI's quality, every bus's gain and the controls in effect — read
// from the servers, which is what the sample's tests hold.

#include <cy/core/memory/system_allocator.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "menus_host.h"
#include "script.h"
#if defined(CY_MENUS_RENDER)
#    include "render.h"
#endif

namespace {

using sample::menus::MenusHost;
using sample::menus::Observation;

struct Options {
    std::string script;
    std::string render;
    std::string project = CY_MENUS_PROJECT;
    std::string user_data;
    cy::u64 frames = 0;
    cy::u32 shot_width = 0;
    cy::u32 shot_height = 0;
    bool record = false;
};

/// `480x270`: a shot's size.
[[nodiscard]] bool parse_size(const char* text, cy::u32& width, cy::u32& height) {
    char* end = nullptr;
    const unsigned long parsed_width = std::strtoul(text, &end, 10);
    if (end == text || *end != 'x') {
        return false;
    }
    const char* rest = end + 1;
    const unsigned long parsed_height = std::strtoul(rest, &end, 10);
    if (end == rest || *end != '\0' || parsed_width == 0 || parsed_height == 0) {
        return false;
    }
    width = static_cast<cy::u32>(parsed_width);
    height = static_cast<cy::u32>(parsed_height);
    return true;
}

[[nodiscard]] bool parse(int argc, char** argv, Options& out) {
    for (int index = 1; index < argc; ++index) {
        const std::string flag = argv[index];
        const bool has_value = index + 1 < argc;
        if (flag == "--script" && has_value) {
            out.script = argv[++index];
        } else if (flag == "--render" && has_value) {
            out.render = argv[++index];
        } else if (flag == "--project" && has_value) {
            out.project = argv[++index];
        } else if (flag == "--user-data" && has_value) {
            out.user_data = argv[++index];
        } else if (flag == "--frames" && has_value) {
            out.frames = std::strtoull(argv[++index], nullptr, 10);
        } else if (flag == "--shot-size" && has_value &&
                   parse_size(argv[index + 1], out.shot_width, out.shot_height)) {
            ++index;
        } else if (flag == "--record") {
            out.record = true;
        } else {
            std::fprintf(stderr, "cy_sample_menus: unknown argument '%s'\n", flag.c_str());
            return false;
        }
    }
    if (out.user_data.empty()) {
        out.user_data = "menus-user-data";
    }
    return true;
}

void print(const Observation& seen, cy::u64 frame) {
    std::printf(
        "menus: frame=%llu found=%d screen=%.0f started=%.0f applied=%.0f rebinds=%.0f "
        "conflicts=%.0f loaded_settings=%.0f loaded_bindings=%.0f picks=%.0f drops=%.0f "
        "drags=%.0f first_slot=%.0f potion_slot=%.0f backs=%.0f jumps=%.0f focused=%s video=%ux%u "
        "quality=%u upscale=%u flags=%u render=%ux%u gi=%u master=%.3f music=%.3f sfx=%.3f "
        "voice=%.3f jump_keyboard=%s jump_gamepad=%s interact_keyboard=%s subtitle=\"%s\" "
        "widgets=%u\n",
        static_cast<unsigned long long>(frame), seen.game.found ? 1 : 0,
        static_cast<double>(seen.game.screen), static_cast<double>(seen.game.started),
        static_cast<double>(seen.game.applied), static_cast<double>(seen.game.rebinds),
        static_cast<double>(seen.game.conflicts), static_cast<double>(seen.game.loaded_settings),
        static_cast<double>(seen.game.loaded_bindings), static_cast<double>(seen.game.picks),
        static_cast<double>(seen.game.drops), static_cast<double>(seen.game.drags),
        static_cast<double>(seen.game.first_slot_item), static_cast<double>(seen.game.potion_slot),
        static_cast<double>(seen.game.backs), static_cast<double>(seen.game.jumps),
        seen.focused.empty() ? "-" : seen.focused.c_str(), seen.video.width, seen.video.height,
        seen.video.quality, seen.video.upscale, seen.video.flags, seen.render_width,
        seen.render_height, seen.gi_quality, static_cast<double>(seen.volumes[0]),
        static_cast<double>(seen.volumes[1]), static_cast<double>(seen.volumes[2]),
        static_cast<double>(seen.volumes[3]), seen.jump_keyboard.c_str(), seen.jump_gamepad.c_str(),
        seen.interact_keyboard.c_str(), seen.subtitle.c_str(), seen.widgets);
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    sample::menus::HostOptions host_options;
    host_options.module_library = CY_MENUS_MODULE_LIBRARY;
    host_options.module_manifest = CY_MENUS_MODULE_MANIFEST;
    host_options.project = options.project;
    host_options.user_data = options.user_data;
    MenusHost host(allocator, host_options);
    const char* detail = "";
    if (cy::Status started = host.start(&detail); !started) {
        std::fprintf(stderr, "cy_sample_menus: %s: %s\n", detail, started.error().message);
        return 1;
    }
    sample::menus::Script script;
    if (!options.script.empty()) {
        if (cy::Status loaded = script.load(options.script.c_str(), options.project); !loaded) {
            std::fprintf(stderr, "cy_sample_menus: %s\n", loaded.error().message);
            return 1;
        }
    }
    const cy::u64 frames = options.frames != 0 ? options.frames : script.last_frame() + 30U;
#if defined(CY_MENUS_RENDER)
    std::unique_ptr<sample::menus::MenusRenderer> renderer;
    if (!options.render.empty()) {
        renderer = std::make_unique<sample::menus::MenusRenderer>(allocator);
        if (cy::Status started =
                renderer->start(host, options.render, options.shot_width, options.shot_height);
            !started) {
            std::fprintf(stderr, "cy_sample_menus: render: %s\n", started.error().message);
            return 3;
        }
    }
#else
    if (!options.render.empty()) {
        std::fprintf(stderr, "cy_sample_menus: render: this build has no Vulkan renderer\n");
        return 3;
    }
#endif
    std::vector<std::string> shots;
    for (cy::u64 index = 0; index < frames; ++index) {
        shots.clear();
        bool report = false;
        if (cy::Status played = script.play(host, shots, report); !played) {
            std::fprintf(stderr, "cy_sample_menus: %s\n", played.error().message);
            return 1;
        }
        if (cy::Status ran = host.frame(); !ran) {
            std::fprintf(stderr, "cy_sample_menus: frame %llu: %s\n",
                         static_cast<unsigned long long>(host.frames()), ran.error().message);
            return 1;
        }
#if defined(CY_MENUS_RENDER)
        if (renderer != nullptr) {
            if (options.record) {
                char name[32] = {};
                (void)std::snprintf(name, sizeof(name), "frame-%05llu",
                                    static_cast<unsigned long long>(index));
                shots.emplace_back(name);
            }
            if (cy::Status drawn = renderer->frame(host, shots); !drawn) {
                std::fprintf(stderr, "cy_sample_menus: render: %s\n", drawn.error().message);
                return 1;
            }
        }
#endif
        if (report) {
            print(host.observe(), host.frames());
        }
    }
    print(host.observe(), host.frames());
#if defined(CY_MENUS_RENDER)
    if (renderer != nullptr) {
        renderer->print_report();
    }
#endif
    host.shutdown();
    return 0;
}
