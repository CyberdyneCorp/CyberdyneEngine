// SPDX-License-Identifier: MIT
// samples/15-first-person — a first-person shooter written in Swift, run by a scripted player.
//
//   just run-sample first-person                     the scripted session headless, then the report
//   just run-sample first-person --no-behaviours     the negative control: the same host, no game
//   cy_sample_first-person --render <dir>            the session drawn every frame on a device at
//                                                    1080p (render.h): the stills, the frame's cost
//       --width <n> --height <n>                     another extent
//       --video <dir> [--video-every <n>]            every n-th frame, for a video
//       --upscale off|native|quality|balanced|performance   the temporal upscaler (quality)
//       --no-gi --no-ao --no-shadows --no-weapon-view --no-decals --no-particles --no-flash
//                                                    each feature's control
//       --no-validation                              for timing: the layer's own checks are not
//                                                    part of what a frame costs
//
// The report is one line per claim, and tests/test_first_person_sample.cpp reads it. Every number
// comes from the engine (the servers, the adapters, the AI) or from the game's `PlayerReport` and
// `Health`; none is computed here from anything the game decided.

#include <cy/core/memory/system_allocator.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>

#include "fps_host.h"
#include "script.h"
#if defined(CY_FIRST_PERSON_RENDER)
#    include "render.h"
#endif

namespace {

using sample::fps::FpsHost;
using sample::fps::Observation;

[[nodiscard]] double wide(float value) noexcept {
    return static_cast<double>(value);
}

[[nodiscard]] unsigned long long bits(CyEntity entity) noexcept {
    return static_cast<unsigned long long>(entity);
}

/// The flags of a drawn run. Parsed whatever the build, so a build without the Vulkan renderer
/// refuses `--render` by name rather than calling its flags unknown.
struct RenderFlags {
    const char* directory = nullptr;
#if defined(CY_FIRST_PERSON_RENDER)
    sample::fps::RenderOptions options;
#endif

    bool parse(int argc, char** argv, int& index) noexcept {
        const char* flag = argv[index];
        const bool valued = index + 1 < argc;
        if (std::strcmp(flag, "--render") == 0 && valued) {
            directory = argv[++index];
            return true;
        }
#if defined(CY_FIRST_PERSON_RENDER)
        if (std::strcmp(flag, "--width") == 0 && valued) {
            options.width = static_cast<cy::u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (std::strcmp(flag, "--height") == 0 && valued) {
            options.height = static_cast<cy::u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (std::strcmp(flag, "--video") == 0 && valued) {
            options.video_directory = argv[++index];
        } else if (std::strcmp(flag, "--video-every") == 0 && valued) {
            options.video_every = static_cast<cy::u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (std::strcmp(flag, "--upscale") == 0 && valued) {
            options.upscale = argv[++index];
        } else {
            return parse_switch(flag);
        }
        return true;
#else
        return false;
#endif
    }

#if defined(CY_FIRST_PERSON_RENDER)
    bool parse_switch(const char* flag) noexcept {
        const struct {
            const char* name;
            bool sample::fps::RenderOptions::* field;
        } kSwitches[] = {
            {"--no-gi", &sample::fps::RenderOptions::gi},
            {"--no-ao", &sample::fps::RenderOptions::ambient_occlusion},
            {"--no-shadows", &sample::fps::RenderOptions::shadows},
            {"--no-weapon-view", &sample::fps::RenderOptions::weapon_view},
            {"--no-decals", &sample::fps::RenderOptions::decals},
            {"--no-particles", &sample::fps::RenderOptions::particles},
            {"--no-flash", &sample::fps::RenderOptions::flashes},
            {"--no-validation", &sample::fps::RenderOptions::validation},
        };
        const auto* found = std::ranges::find_if(
            kSwitches, [flag](const auto& entry) { return std::strcmp(flag, entry.name) == 0; });
        if (found == std::end(kSwitches)) {
            return false;
        }
        options.*(found->field) = false;
        return true;
    }
#endif
};

void print_targets(FpsHost& host, const Observation& now) noexcept {
    for (cy::u32 index = 0; index < sample::fps::kTargetCount; ++index) {
        const sample::fps::TargetState& state = host.target(index);
        std::printf(
            "fps target   %s mode=%s hits=%.0f felt=%u points=%.0f fall=%.2f patrol=%u engage=%u "
            "flinch=%u down=%u sighted=%u at=%.2f,%.2f yaw=%.3f\n",
            sample::fps::target_spots()[index].node, sample::fps::target_mode_name(state.mode),
            wide(now.health[index].hits), state.hits_felt, wide(now.health[index].points),
            wide(state.fall), state.ticks_in[0], state.ticks_in[1], state.ticks_in[2],
            state.ticks_in[3], state.first_sighting, wide(state.feet.x), wide(state.feet.z),
            wide(state.yaw));
    }
}

void print_shots(const FpsHost& host) noexcept {
    cy::u32 index = 0;
    for (const sample::fps::ShotRecord& shot : host.shots()) {
        std::printf(
            "fps shot     %u frame=%llu kind=%u hit=%llu point=%.4f,%.4f,%.4f "
            "normal=%.3f,%.3f,%.3f distance=%.4f confirmed=%d\n",
            index++, static_cast<unsigned long long>(shot.frame), shot.kind, bits(shot.hit),
            wide(shot.point.x), wide(shot.point.y), wide(shot.point.z), wide(shot.normal.x),
            wide(shot.normal.y), wide(shot.normal.z), wide(shot.distance), shot.confirmed ? 1 : 0);
    }
}

void print_report(FpsHost& host, const sample::fps::ScriptFindings& script) noexcept {
    const Observation now = host.observe();
    const Observation::Game& game = now.game;
    std::printf(
        "fps module   behaviours=%u frames=%llu digest=%016llx pulls=%u abandoned=%u "
        "tasks=%u\n",
        host.behaviours(), static_cast<unsigned long long>(host.frames()),
        static_cast<unsigned long long>(host.digest()), script.trigger_pulls, script.abandoned,
        script.tasks);
    std::printf(
        "fps move     walk=%.3f sprint=%.3f crouch=%.3f jumps=%.0f airborne=%.0f peak=%.3f "
        "crouched=%.0f stand_refused=%.0f under_beam=%.0f at=%.3f,%.3f,%.3f grounded=%d "
        "height=%.2f\n",
        wide(game.walk_speed), wide(game.sprint_speed), wide(game.crouch_speed), wide(game.jumps),
        wide(game.airborne), wide(game.peak_height), wide(game.crouched), wide(game.stand_refused),
        wide(game.under_beam), wide(now.player.x), wide(now.player.y), wide(now.player.z),
        now.grounded ? 1 : 0, wide(now.capsule_height));
    std::printf(
        "fps weapon   shots=%.0f target_hits=%.0f prop_hits=%.0f world_hits=%.0f misses=%.0f "
        "kills=%.0f kicks=%.0f impulses=%.0f peak_recoil=%.4f aimed=%.0f peak_aim=%.3f "
        "yaw=%.3f pitch=%.3f refused=%.0f\n",
        wide(game.shots), wide(game.target_hits), wide(game.prop_hits), wide(game.world_hits),
        wide(game.misses), wide(game.kills), wide(game.kicks), wide(game.impulses),
        wide(now.peak_recoil), wide(game.aimed), wide(now.peak_aim), wide(now.yaw), wide(now.pitch),
        wide(game.refused));
    std::printf(
        "fps effects  flashes=%.0f lights_spawned=%llu lights_peak=%u bursts=%.0f sparks=%.0f "
        "played=%llu muzzle=%llu spark_plays=%llu marks=%.0f decals=%llu decals_live=%u "
        "peak_particles=%u particle_frames=%llu sounds=%.0f peak_voices=%u\n",
        wide(game.flashes), static_cast<unsigned long long>(now.lights_spawned),
        now.lights_live_peak, wide(game.bursts), wide(game.sparks),
        static_cast<unsigned long long>(now.effects_played),
        static_cast<unsigned long long>(now.muzzle_plays),
        static_cast<unsigned long long>(now.spark_plays), wide(game.marks),
        static_cast<unsigned long long>(now.decals_spawned), now.decals_live, now.peak_particles,
        static_cast<unsigned long long>(now.particle_frames), wide(game.sounds), now.peak_voices);
    print_targets(host, now);
    const sample::fps::Targets& targets = host.targets();
    std::printf("fps ai       sightings=%u traces=%u program=%016llx think=%016llx\n",
                targets.sightings(), targets.traces(),
                static_cast<unsigned long long>(targets.program_digest()),
                static_cast<unsigned long long>(targets.think_hash()));
    std::printf("fps props    moved=%.3f,%.3f,%.3f,%.3f\n", wide(now.prop_moved[0]),
                wide(now.prop_moved[1]), wide(now.prop_moved[2]), wide(now.prop_moved[3]));
    print_shots(host);
}

}  // namespace

int main(int argc, char** argv) {
    sample::fps::HostOptions options;
    options.module_library = CY_FIRST_PERSON_MODULE_LIBRARY;
    options.module_manifest = CY_FIRST_PERSON_MODULE_MANIFEST;
    RenderFlags render;
    cy::u64 frames = sample::fps::ScriptedPlayer::kFrames;
    for (int index = 1; index < argc; ++index) {
        if (render.parse(argc, argv, index)) {
            continue;
        }
        if (std::strcmp(argv[index], "--no-behaviours") == 0) {
            options.behaviours = false;
        } else if (std::strcmp(argv[index], "--frames") == 0 && index + 1 < argc) {
            frames = std::strtoull(argv[++index], nullptr, 10);
        } else if (std::strcmp(argv[index], "--headless") != 0) {
            std::fprintf(stderr, "first-person: unknown argument '%s'\n", argv[index]);
            return 2;
        }
    }

    // THE GI IS PRESENTATION: a run with no picture has nothing to light, and nothing the game
    // decides reads it, so a headless run leaves it off — the drawn run's game is held to it.
    options.illumination = render.directory != nullptr;
#if defined(CY_FIRST_PERSON_RENDER)
    options.illumination = options.illumination && render.options.gi;
#endif
    FpsHost host(cy::system_allocator(cy::MemoryDomain::World), options);
    const char* detail = "";
    if (const cy::Status started = host.start(&detail); !started) {
        std::fprintf(stderr, "first-person: start failed at '%s': %s\n", detail,
                     started.error().message);
        return 1;
    }
#if defined(CY_FIRST_PERSON_RENDER)
    std::unique_ptr<sample::fps::FpsRenderer> renderer;
    if (render.directory != nullptr) {
        renderer = std::make_unique<sample::fps::FpsRenderer>(
            cy::system_allocator(cy::MemoryDomain::Renderer));
        sample::fps::RenderOptions drawn = render.options;
        drawn.directory = render.directory;
        if (const cy::Status started = renderer->start(host, drawn); !started) {
            std::fprintf(stderr, "first-person: --render: %s\n", started.error().message);
            return started.error().code == cy::ErrorCode::Unavailable ? 0 : 1;
        }
    }
#else
    if (render.directory != nullptr) {
        std::fprintf(
            stderr, "first-person: --render needs the Vulkan renderer, which this build has not\n");
        return 2;
    }
#endif
    if (std::getenv("CY_FPS_DEBUG") != nullptr) {
        cy::physics::RayCastHit hit;
        const CyEntity e =
            host.raycast(cy::Vec3{0.0F, 5.0F, 0.0F}, cy::Vec3{0.0F, -1.0F, 0.0F}, 10.0F, &hit);
        std::printf("debug ray entity=%llu body=%d y=%.3f\n", bits(e), hit.body.is_null() ? 0 : 1,
                    wide(hit.position.y));
    }
    sample::fps::ScriptedPlayer player(host);
    for (cy::u64 frame = 0; frame < frames; ++frame) {
        const cy::Status injected = player.before_frame(frame);
        cy::Status ran = injected ? host.frame() : injected;
#if defined(CY_FIRST_PERSON_RENDER)
        if (ran && renderer != nullptr) {
            ran = renderer->frame(host);
        }
#endif
        if (!ran) {
            std::fprintf(stderr, "first-person: frame %llu failed: %s\n",
                         static_cast<unsigned long long>(frame), ran.error().message);
            return 1;
        }
    }
    print_report(host, player.findings());
#if defined(CY_FIRST_PERSON_RENDER)
    if (renderer != nullptr) {
        renderer->print_report();
    }
#endif
    host.shutdown();
    return 0;
}
