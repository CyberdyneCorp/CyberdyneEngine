// SPDX-License-Identifier: MIT
// samples/13-rts-api — an RTS written in Swift against ABI 1.3, run headless by a scripted player.
//
//   just run-sample rts-api                    the scripted session, then the report
//   just run-sample rts-api --no-behaviours    the negative control: the same host, no game
//   just run-sample rts-api --no-systems       the scheduler's control: the module's systems are
//                                              registered with the engine but never installed
//   just run-sample rts-api --no-ui            the interface's control: no interface, so no HUD,
//                                              and the Build click lands on the world
//
// The report is one line per claim, and tests/test_rts_api_sample.cpp reads it. Every number comes
// from the engine (the servers and the adapters) or from the game's `RtsReport`; none is computed
// here from anything the game decided.

#include <cy/core/memory/system_allocator.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "rts_host.h"
#include "script.h"

namespace {

using sample::rts::Findings;
using sample::rts::Observation;
using sample::rts::RtsHost;

[[nodiscard]] unsigned long long bits(CyEntity entity) noexcept {
    return static_cast<unsigned long long>(entity);
}

[[nodiscard]] float planar_distance(cy::Vec3 a, cy::Vec3 b) noexcept {
    return std::hypot(a.x - b.x, a.z - b.z);
}

[[nodiscard]] double wide(float value) noexcept {
    return static_cast<double>(value);
}

void print_report(RtsHost& host, const Findings& seen) noexcept {
    const Observation now = host.observe();
    const cy::Vec3 reached =
        seen.clicked == CY_ENTITY_NULL ? cy::Vec3{} : host.unit_position(seen.clicked);
    const float miss =
        seen.clicked == CY_ENTITY_NULL ? -1.0F : planar_distance(seen.target, reached);
    const float bystander_moved =
        seen.bystander == CY_ENTITY_NULL
            ? 0.0F
            : planar_distance(seen.bystander_start, host.unit_position(seen.bystander));

    std::printf("rts module   behaviours=%u frames=%llu missed_aims=%u\n", host.behaviours(),
                static_cast<unsigned long long>(host.frames()), seen.missed_aims);
    std::printf("rts camera   start=%.3f keyboard=%.3f edge=%.3f\n", wide(seen.camera_start),
                wide(seen.camera_after_keys - seen.camera_start),
                wide(seen.camera_after_edge - seen.camera_after_keys));
    std::printf("rts select   clicked=%llu selected=%llu\n", bits(seen.clicked),
                bits(now.game.selected));
    std::printf(
        "rts order    orders=%.0f target=%.3f,%.3f reached=%.3f,%.3f miss=%.3f arrived_frame=%llu "
        "bystander_moved=%.3f\n",
        wide(now.game.orders), wide(seen.target.x), wide(seen.target.z), wide(reached.x),
        wide(reached.z), wide(miss), static_cast<unsigned long long>(seen.arrived_frame),
        wide(bystander_moved));
    std::printf("rts audio    arrivals=%.0f cues=%.0f peak_voices=%u\n", wide(now.game.arrivals),
                wide(now.game.cues), seen.peak_voices);
    std::printf("rts spawn    spawns=%.0f units=%u agents=%u workers=%u\n", wide(now.game.spawns),
                now.units, now.agents, now.workers);
    // ABI 1.5: tree callbacks and @Node, a scheduled Swift system, a character and an impulse.
    std::printf("rts tree     entered=%.0f readied=%.0f barracks=%.0f crate_found=%.0f\n",
                wide(now.tree.entered), wide(now.tree.readied), wide(now.tree.barracks_found),
                wide(now.scout.crate_found));
    std::printf("rts system   installed=%u runs=%llu rows=%u most=%.0f ordered=%d\n",
                now.systems.installed, static_cast<unsigned long long>(now.systems.runs),
                now.systems.rows, wide(now.systems.most), now.systems.ordered ? 1 : 0);
    std::printf("rts hero     x=%.3f ground=%.0f airborne=%.0f\n", wide(now.scout.hero_x),
                wide(now.scout.hero_ground), wide(now.scout.airborne));
    std::printf("rts body     kicks=%.0f crate_speed=%.3f crate_moved=%.3f\n",
                wide(now.scout.kicks), wide(now.scout.crate_speed), wide(now.scout.crate_moved));
    // ABI 1.6: the HUD the game built and wrote, read out of the store, and the Build click.
    std::printf(
        "rts hud      mounted=%.0f elements=%u button=%d aimed=%d clicks=%u heard=%.0f builds=%.0f "
        "gold=%s wood=%s food=%s rows=%u health=%s fill=%.3f dots=%u title='%s'\n",
        wide(now.hud.mounted), now.hud.elements, now.hud.button ? 1 : 0, seen.button_found ? 1 : 0,
        now.hud.clicks, wide(now.hud.heard), wide(now.hud.builds), now.hud.gold, now.hud.wood,
        now.hud.food, now.hud.rows, now.hud.health, wide(now.hud.fill), now.hud.dots,
        now.hud.title);
}

}  // namespace

int main(int argc, char** argv) {
    sample::rts::HostOptions options;
    options.module_library = CY_RTS_API_MODULE_LIBRARY;
    options.module_manifest = CY_RTS_API_MODULE_MANIFEST;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--no-behaviours") == 0) {
            options.behaviours = false;
        } else if (std::strcmp(argv[index], "--no-systems") == 0) {
            options.systems = false;
        } else if (std::strcmp(argv[index], "--no-ui") == 0) {
            options.ui = false;
        } else if (std::strcmp(argv[index], "--headless") != 0) {
            std::fprintf(stderr, "rts-api: unknown argument '%s'\n", argv[index]);
            return 2;
        }
    }

    RtsHost host(cy::system_allocator(cy::MemoryDomain::World), options);
    const char* detail = "";
    if (const cy::Status started = host.start(&detail); !started) {
        std::fprintf(stderr, "rts-api: start failed at '%s': %s\n", detail,
                     started.error().message);
        return 1;
    }
    sample::rts::Player player(host);
    for (cy::u64 frame = 0; frame < sample::rts::Player::kFrames; ++frame) {
        const cy::Status injected = player.before_frame(frame);
        const cy::Status ran = injected ? host.frame() : injected;
        if (!ran) {
            std::fprintf(stderr, "rts-api: frame %llu failed: %s\n",
                         static_cast<unsigned long long>(frame), ran.error().message);
            return 1;
        }
        player.after_frame(frame);
    }
    print_report(host, player.findings());
    host.shutdown();
    return 0;
}
