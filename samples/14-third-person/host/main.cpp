// SPDX-License-Identifier: MIT
// samples/14-third-person — a third-person game written in Swift, run by a scripted player, and
// drawn by the engine's frame.
//
//   just run-sample third-person                   the scripted run headless, then the report
//   just run-sample third-person --no-behaviours   the negative control: the same host, no game
//   cy_sample_third-person --render <dir>          the run drawn at 1920x1080 (render.h):
//                                                  stills, `--video <dir>`, `--audio <file.wav>`,
//                                                  `--width`, `--height`, `--upscale <mode>`, and
//                                                  `--no-gi`, `--no-ao`, `--no-shadows`,
//                                                  `--no-upscale`, `--no-validation` as controls
//   --map <hillside.cypak>                         the cooked level; the build's by default
//   --frames <n>                                   stop early
//   --trace <n>                                    every nth frame, where the capsule, the camera
//                                                  and the feet are: a developer's view of the run
//
// The report is one line per claim, and tests/test_third_person_sample.cpp reads it. Every number
// comes from the engine or from the game's `HeroReport`; none is computed here from anything the
// game decided.

#include <cy/core/memory/system_allocator.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "map_load.h"
#include "script.h"
#include "tps_host.h"
#if defined(CY_THIRD_PERSON_RENDER)
#    include "golden.h"
#    include "render.h"
#endif
#if defined(CY_THIRD_PERSON_PLAY)
#    include "hosted_game.h"
#    include "play_arguments.h"
#endif

namespace {

using sample::tps::Observation;
using sample::tps::ThirdPersonHost;

[[nodiscard]] double wide(float value) noexcept {
    return static_cast<double>(value);
}

struct Arguments {
    bool behaviours = true;
    cy::u64 frames = sample::tps::Player::kFrames;
    const char* package = CY_THIRD_PERSON_PACKAGE;
    const char* render = nullptr;
    const char* audio = nullptr;
    cy::u64 trace = 0;
#if defined(CY_THIRD_PERSON_RENDER)
    sample::tps::RenderOptions options;
#endif
    bool valid = true;
};

/// Consume a renderer's flag. Parsed whatever the build, so a build without the Vulkan renderer
/// refuses `--render` by name rather than calling its flags unknown.
[[nodiscard]] bool render_flag(Arguments& out, int argc, char** argv, int& index) noexcept {
#if defined(CY_THIRD_PERSON_RENDER)
    const char* flag = argv[index];
    const bool valued = index + 1 < argc;
    sample::tps::RenderOptions& options = out.options;
    if (std::strcmp(flag, "--width") == 0 && valued) {
        options.width = static_cast<cy::u32>(std::strtoul(argv[++index], nullptr, 10));
    } else if (std::strcmp(flag, "--height") == 0 && valued) {
        options.height = static_cast<cy::u32>(std::strtoul(argv[++index], nullptr, 10));
    } else if (std::strcmp(flag, "--video") == 0 && valued) {
        options.video_directory = argv[++index];
    } else if (std::strcmp(flag, "--video-every") == 0 && valued) {
        options.video_every = static_cast<cy::u32>(std::strtoul(argv[++index], nullptr, 10));
    } else if (std::strcmp(flag, "--upscale") == 0 && valued) {
        options.upscale = true;
        if (!cy::rendering::upscale::parse_upscale_mode(argv[++index], options.upscale_mode)) {
            std::fprintf(stderr, "--upscale is `native`, `quality`, `balanced` or `performance`\n");
            return false;
        }
    } else if (std::strcmp(flag, "--no-upscale") == 0) {
        options.upscale = false;
    } else if (std::strcmp(flag, "--no-gi") == 0) {
        options.gi = false;
    } else if (std::strcmp(flag, "--no-ao") == 0) {
        options.ambient_occlusion = false;
    } else if (std::strcmp(flag, "--no-shadows") == 0) {
        options.shadows = false;
    } else if (std::strcmp(flag, "--no-validation") == 0) {
        options.validation = false;
    } else if (std::strcmp(flag, "--stills") == 0 && valued) {
        options.stills_every = static_cast<cy::u32>(std::strtoul(argv[++index], nullptr, 10));
    } else {
        return false;
    }
    return true;
#else
    (void)out;
    (void)argc;
    (void)argv;
    (void)index;
    return false;
#endif
}

[[nodiscard]] Arguments parse(int argc, char** argv) noexcept {
    Arguments out;
    for (int index = 1; index < argc; ++index) {
        const char* flag = argv[index];
        const bool valued = index + 1 < argc;
        if (std::strcmp(flag, "--no-behaviours") == 0) {
            out.behaviours = false;
        } else if (std::strcmp(flag, "--frames") == 0 && valued) {
            out.frames = std::strtoull(argv[++index], nullptr, 10);
        } else if (std::strcmp(flag, "--map") == 0 && valued) {
            out.package = argv[++index];
        } else if (std::strcmp(flag, "--render") == 0 && valued) {
            out.render = argv[++index];
        } else if (std::strcmp(flag, "--trace") == 0 && valued) {
            out.trace = std::strtoull(argv[++index], nullptr, 10);
        } else if (std::strcmp(flag, "--audio") == 0 && valued) {
            out.audio = argv[++index];
        } else if (!render_flag(out, argc, argv, index)) {
            std::fprintf(stderr, "cy_sample_third-person: unknown argument '%s'\n", flag);
            out.valid = false;
        }
    }
    return out;
}

/// A 16-bit stereo WAV of `mix` at 48 kHz.
[[nodiscard]] bool write_wav(const char* path, const std::vector<float>& mix) noexcept {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return false;
    }
    const auto samples = static_cast<cy::u32>(mix.size());
    const cy::u32 data_bytes = samples * 2U;
    const auto word = [file](cy::u32 value, int bytes) {
        for (int index = 0; index < bytes; ++index) {
            (void)std::fputc(static_cast<int>((value >> (8 * index)) & 0xFFU), file);
        }
    };
    (void)std::fwrite("RIFF", 1, 4, file);
    word(36U + data_bytes, 4);
    (void)std::fwrite("WAVEfmt ", 1, 8, file);
    word(16U, 4);
    word(1U, 2);
    word(2U, 2);
    word(48000U, 4);
    word(48000U * 4U, 4);
    word(4U, 2);
    word(16U, 2);
    (void)std::fwrite("data", 1, 4, file);
    word(data_bytes, 4);
    for (const float sample : mix) {
        const float clamped = std::fmax(-1.0F, std::fmin(1.0F, sample));
        const auto value = static_cast<cy::i16>(std::lround(clamped * 32767.0F));
        word(static_cast<cy::u32>(static_cast<cy::u16>(value)), 2);
    }
    return std::fclose(file) == 0;
}

void print_report(ThirdPersonHost& host) noexcept {
    const Observation seen = host.observe();
    const Observation::Game& game = seen.game;
    std::printf("tps module behaviours=%u report=%d frames=%llu\n", host.behaviours(),
                game.found ? 1 : 0, static_cast<unsigned long long>(host.frames()));
    std::printf(
        "tps hero ticks=%.0f grounded=%.0f airborne=%.0f stepped_up=%.0f jumps=%.0f "
        "landings=%.0f highest=%.3f fastest=%.3f run_ticks=%.0f walk_ticks=%.0f "
        "end_x=%.3f end_y=%.3f end_z=%.3f facing=%.3f states=%.0f\n",
        wide(game.ticks), wide(game.grounded), wide(game.airborne), wide(game.stepped_up),
        wide(game.jumps), wide(game.landings), wide(game.highest), wide(game.fastest),
        wide(game.run_ticks), wide(game.walk_ticks), wide(seen.hero_feet.x), wide(seen.hero_feet.y),
        wide(seen.hero_feet.z), wide(seen.hero_facing), wide(game.states));
    std::printf(
        "tps steps footsteps=%.0f stone=%.0f grass=%.0f dropped=%.0f peak_voices=%u "
        "ambience=%.0f\n",
        wide(game.footsteps), wide(game.stone_steps), wide(game.grass_steps), wide(game.dropped),
        seen.peak_voices, wide(game.ambience));
    std::printf(
        "tps feet solved=%.0f adjusted=%.0f pelvis_drop=%.3f placed_frames=%u "
        "deepest_pelvis=%.3f departure=%.3f\n",
        wide(game.feet_solved), wide(game.feet_adjusted), wide(game.pelvis_drop),
        seen.placed_frames, wide(seen.deepest_pelvis), wide(seen.departure));
    std::printf(
        "tps camera pulled=%.0f nearest=%.3f yaw=%.3f collision_frames=%u "
        "nearest_fraction=%.3f queries=%llu\n",
        wide(game.camera_pulled), wide(game.camera_nearest), wide(game.camera_yaw),
        seen.collision_frames, wide(seen.nearest_fraction),
        static_cast<unsigned long long>(seen.camera_queries));
    std::printf("tps digest value=%016llx\n", static_cast<unsigned long long>(seen.digest));
}

#if defined(CY_THIRD_PERSON_PLAY)
// --- Played ---------------------------------------------------------------------------------------
//
// `--play`: the hillside in a window, a person at the mouse and the keyboard (or a pad), through
// the desktop play host (samples/play). The game is the same Swift module; nothing scripts it.

#    if defined(CY_THIRD_PERSON_RENDER)
using PlayedRenderer = sample::tps::Renderer;
#    else
using PlayedRenderer = cy::play::NoRenderer;
#    endif

struct PlayFlags {
    cy::play::PlayArguments play;
    const char* package = CY_THIRD_PERSON_PACKAGE;
#    if defined(CY_THIRD_PERSON_RENDER)
    sample::tps::RenderOptions render;
#    endif
};

[[nodiscard]] bool parse_play(int argc, char** argv, PlayFlags& out) noexcept {
    out.play.options.title = "CyberEngine — third person (Esc: pause)";
    Arguments render;
#    if defined(CY_THIRD_PERSON_RENDER)
    render.options.validation = false;
#    endif
    for (int index = 1; index < argc; ++index) {
        if (cy::play::parse_play_flag(argc, argv, index, out.play)) {
            continue;
        }
        if (std::strcmp(argv[index], "--map") == 0 && index + 1 < argc) {
            out.package = argv[++index];
            continue;
        }
        if (!render_flag(render, argc, argv, index)) {
            std::fprintf(stderr, "cy_sample_third-person: --play does not take '%s'\n",
                         argv[index]);
            return false;
        }
    }
#    if defined(CY_THIRD_PERSON_RENDER)
    out.render = render.options;
    out.render.validation = out.play.options.validation;
#    endif
    return true;
}

/// Each captured frame as `<dir>/frame-<n>.png`.
[[nodiscard]] cy::Status write_capture(const std::string& directory, cy::u64 frame,
                                       cy::Span<const cy::u32> pixels, cy::u32 width,
                                       cy::u32 height) noexcept {
#    if defined(CY_THIRD_PERSON_RENDER)
    cy::render_test::Image image(cy::system_allocator(cy::MemoryDomain::Renderer));
    if (cy::Status adopted = cy::render_test::adopt(image, pixels, width, height); !adopted) {
        return adopted;
    }
    char name[64];
    (void)std::snprintf(name, sizeof(name), "/frame-%05llu.png",
                        static_cast<unsigned long long>(frame));
    return cy::render_test::write_png((directory + name).c_str(), image);
#    else
    (void)directory;
    (void)frame;
    (void)pixels;
    (void)width;
    (void)height;
    return cy::ok();
#    endif
}

/// The renderer on the play host's device, when the run draws.
[[nodiscard]] cy::Status start_renderer(ThirdPersonHost& host, cy::play::PlayHost& play,
                                        const sample::rts::map::MapContent& content,
                                        const PlayFlags& flags,
                                        std::unique_ptr<PlayedRenderer>& out) noexcept {
    if (play.device() == nullptr) {
        return cy::ok();
    }
#    if defined(CY_THIRD_PERSON_RENDER)
    out = std::make_unique<PlayedRenderer>(cy::system_allocator(cy::MemoryDomain::Renderer));
    sample::tps::RenderOptions render = flags.render;
    render.device = play.device();
    return out->start(host, content, render);
#    else
    (void)host;
    (void)content;
    (void)flags;
    (void)out;
    return cy::ok();
#    endif
}

/// The game in the play host until it ends: the report printed, everything released in order.
[[nodiscard]] int play_game(cy::play::PlayHost& play, const sample::rts::map::MapContent& content,
                            const PlayFlags& flags) noexcept {
    sample::tps::HostOptions options;
    options.module_library = CY_THIRD_PERSON_MODULE_LIBRARY;
    options.module_manifest = CY_THIRD_PERSON_MODULE_MANIFEST;
    options.map = &content;
    options.scripted_devices = false;
    options.fenced = true;
#    if defined(CY_THIRD_PERSON_RENDER)
    options.illumination = play.device() != nullptr && flags.render.gi;
#    endif
    cy::play::PlayUi& ui = play.ui();
    options.bind_services = [&ui](cy::abi::Host& abi) { ui.bind(abi); };
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Engine);
    ThirdPersonHost host(allocator, options);
    const char* detail = "";
    std::unique_ptr<PlayedRenderer> renderer;
    cy::Status ready = host.start(&detail);
    ready = ready ? start_renderer(host, play, content, flags, renderer) : ready;
    cy::play::HostedGame<ThirdPersonHost, PlayedRenderer> game(host, ui, renderer.get());
    ready = ready ? play.attach(game) : ready;
    if (!ready) {
        std::fprintf(stderr, "cy_sample_third-person: --play: %s %s\n", detail,
                     ready.error().message);
        return 1;
    }
    const std::string directory = flags.play.capture_directory;
    play.on_capture = [&directory](cy::u64 frame, cy::Span<const cy::u32> pixels, cy::u32 width,
                                   cy::u32 height) {
        return write_capture(directory, frame, pixels, width, height);
    };
    const cy::Expected<cy::play::PlayReport, cy::Error> report = play.run();
    int code = 0;
    if (report) {
        print_report(host);
        cy::play::print_play_report(*report);
    } else {
        std::fprintf(stderr, "cy_sample_third-person: --play: %s\n", report.error().message);
        code = 1;
    }
    renderer.reset();
    host.shutdown();
    return code;
}

[[nodiscard]] int run_play(int argc, char** argv) noexcept {
    PlayFlags flags;
    if (!parse_play(argc, argv, flags)) {
        return 2;
    }
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Engine);
    sample::rts::map::MapContent content;
    sample::rts::map::LoadReport loaded;
    std::string detail;
    if (cy::Status read =
            sample::rts::map::load_map(allocator, flags.package, content, loaded, detail);
        !read) {
        std::fprintf(stderr, "cy_sample_third-person: the level did not load (%s): %s\n",
                     detail.c_str(), read.error().message);
        return 1;
    }
    cy::play::PlayHost play(allocator);
    if (cy::Status opened = play.open(flags.play.options); !opened) {
        std::fprintf(stderr, "cy_sample_third-person: --play: %s\n", opened.error().message);
        return opened.error().code == cy::ErrorCode::Unavailable ? 0 : 1;
    }
    const int code = play_game(play, content, flags);
    play.close();
    return code;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(CY_THIRD_PERSON_PLAY)
    if (cy::play::wants_play(argc, argv)) {
        return run_play(argc, argv);
    }
#endif
    const Arguments arguments = parse(argc, argv);
    if (!arguments.valid) {
        return 2;
    }
#if !defined(CY_THIRD_PERSON_RENDER)
    if (arguments.render != nullptr) {
        std::fprintf(stderr,
                     "cy_sample_third-person: --render needs the Vulkan renderer, which "
                     "this build does not have\n");
        return 2;
    }
#endif
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Engine);

    // THE LEVEL, as a game loads it: the cooked package mounted and read back by asset id.
    sample::rts::map::MapContent content;
    sample::rts::map::LoadReport loaded;
    std::string detail;
    if (cy::Status read =
            sample::rts::map::load_map(allocator, arguments.package, content, loaded, detail);
        !read) {
        std::fprintf(stderr, "cy_sample_third-person: the level did not load (%s): %s\n",
                     detail.c_str(), read.error().message);
        return 1;
    }
    std::printf("tps level assets=%u bytes=%llu load_ms=%.1f placements=%zu plants=%zu\n",
                loaded.assets, static_cast<unsigned long long>(loaded.bytes), loaded.load_ms,
                content.layout.placements.size(), content.foliage.plants.size());

    std::vector<float> mix;
    sample::tps::HostOptions options;
    options.module_library = CY_THIRD_PERSON_MODULE_LIBRARY;
    options.module_manifest = CY_THIRD_PERSON_MODULE_MANIFEST;
    options.behaviours = arguments.behaviours;
    options.map = &content;
    options.illumination = arguments.render != nullptr;
    options.mix = arguments.audio != nullptr ? &mix : nullptr;
#if defined(CY_THIRD_PERSON_RENDER)
    options.illumination = arguments.render != nullptr && arguments.options.gi;
#endif
    auto host = std::make_unique<ThirdPersonHost>(allocator, options);
    const char* failed = "";
    if (cy::Status started = host->start(&failed); !started) {
        std::fprintf(stderr, "cy_sample_third-person: start failed at '%s': %s\n", failed,
                     started.error().message);
        return 1;
    }

#if defined(CY_THIRD_PERSON_RENDER)
    std::unique_ptr<sample::tps::Renderer> renderer;
    if (arguments.render != nullptr) {
        sample::tps::RenderOptions render = arguments.options;
        render.directory = arguments.render;
        renderer = std::make_unique<sample::tps::Renderer>(allocator);
        if (cy::Status ready = renderer->start(*host, content, render); !ready) {
            std::fprintf(stderr, "cy_sample_third-person: the renderer did not start: %s\n",
                         ready.error().message);
            return 1;
        }
    }
#endif

    sample::tps::Player player(*host);
    for (cy::u64 frame = 0; frame < arguments.frames; ++frame) {
        if (cy::Status injected = player.before_frame(frame); !injected) {
            std::fprintf(stderr, "cy_sample_third-person: frame %llu input: %s\n",
                         static_cast<unsigned long long>(frame), injected.error().message);
            return 1;
        }
        if (cy::Status ran = host->frame(); !ran) {
            std::fprintf(stderr, "cy_sample_third-person: frame %llu failed: %s\n",
                         static_cast<unsigned long long>(frame), ran.error().message);
            return 1;
        }
        if (arguments.trace != 0 && frame % arguments.trace == 0) {
            host->print_trace();
        }
#if defined(CY_THIRD_PERSON_RENDER)
        if (renderer != nullptr) {
            if (cy::Status drawn = renderer->frame(*host); !drawn) {
                std::fprintf(stderr, "cy_sample_third-person: frame %llu draw: %s\n",
                             static_cast<unsigned long long>(frame), drawn.error().message);
                return 1;
            }
        }
#endif
    }
    print_report(*host);
#if defined(CY_THIRD_PERSON_RENDER)
    if (renderer != nullptr) {
        renderer->print_report();
    }
#endif
    if (arguments.audio != nullptr) {
        if (!write_wav(arguments.audio, mix)) {
            std::fprintf(stderr, "cy_sample_third-person: could not write %s\n", arguments.audio);
            return 1;
        }
        std::printf("tps audio wrote=%s seconds=%.2f\n", arguments.audio,
                    static_cast<double>(mix.size()) / (2.0 * 48000.0));
    }
    return 0;
}
