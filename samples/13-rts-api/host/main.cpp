// SPDX-License-Identifier: MIT
// samples/13-rts-api — an RTS written in Swift against ABI 1.3, run headless by a scripted player.
//
//   just run-sample rts-api                    the scripted session, then the report
//   just run-sample rts-api --no-behaviours    the negative control: the same host, no game
//   just run-sample rts-api --no-systems       the scheduler's control: the module's systems are
//                                              registered with the engine but never installed
//   just run-sample rts-api --no-ui            the interface's control: no interface, so no HUD,
//                                              and the Build click lands on the world
//   just run-sample rts-api --render <dir>     the same session drawn every frame on a device, lit
//   by
//                                              the engine's device GI (render.h); `--gi off` draws
//                                              it without
//   ... --render <dir> --map <frontier.cypak>  the session drawn on the RTS test map
//   (map_render.h):
//                                              `--width`, `--height`, `--view overview`,
//                                              `--video <dir>`, and `--no-fog`, `--no-hud`,
//                                              `--no-ao`, `--no-shadows` and `--gi off` as controls
//   just run-sample rts-api --map <frontier.cypak>
//                                              the session headless on the map: its GI boxes and
//                                              the fog grid over its ground, nothing drawn
//   just run-sample rts-api --battle           ABI 1.16: the game fights a battle on the company's
//                                              field instead of patrolling it, for
//                                              `battle::kTicks` frames; with `--render <dir> --map`
//                                              it is drawn, with the topographic minimap
//   just play-rts                              `--play`: the battle as a game a person plays, in a
//                                              window, in real time (play.h): `--record <file>`,
//                                              `--replay <file>`, `--input <file>`, `--frames <n>`,
//                                              `--no-vsync`, `--validation`, and `--headless` for
//                                              SDL's events with no window
//
// The report is one line per claim, and tests/test_rts_api_sample.cpp reads it. Every number comes
// from the engine (the servers and the adapters) or from the game's `RtsReport`; none is computed
// here from anything the game decided.

#include <cy/core/math/shapes.h>
#include <cy/core/memory/system_allocator.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "battle.h"
#include "company.h"
#include "play.h"
#include "replay.h"
#include "rts_host.h"
#include "script.h"
#if defined(CY_RTS_API_RENDER)
#    include "render.h"
#endif
#if defined(CY_RTS_API_MAP)
#    include "map_load.h"
#    include "map_render.h"
#endif

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

/// The flags of a run drawn on the RTS test map. Parsed whatever the build, so a build without the
/// Vulkan renderer refuses `--map` by name rather than calling it an unknown argument.
struct MapFlags {
    const char* package = nullptr;
#if defined(CY_RTS_API_MAP)
    sample::rts::MapRenderOptions options;
#endif

    /// Consume `argv[index]` (and its value) when it is one of the map's flags.
    bool parse(int argc, char** argv, int& index) noexcept {
        const char* flag = argv[index];
        const bool valued = index + 1 < argc;
        if (std::strcmp(flag, "--map") == 0 && valued) {
            package = argv[++index];
            return true;
        }
#if defined(CY_RTS_API_MAP)
        if (std::strcmp(flag, "--width") == 0 && valued) {
            options.width = static_cast<cy::u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (std::strcmp(flag, "--height") == 0 && valued) {
            options.height = static_cast<cy::u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (std::strcmp(flag, "--view") == 0 && valued) {
            options.view = std::strcmp(argv[++index], "overview") == 0
                               ? sample::rts::MapView::Overview
                               : sample::rts::MapView::Game;
        } else if (std::strcmp(flag, "--video") == 0 && valued) {
            options.video_directory = argv[++index];
        } else if (std::strcmp(flag, "--video-every") == 0 && valued) {
            options.video_every = static_cast<cy::u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (std::strcmp(flag, "--no-fog") == 0) {
            options.fog = false;
        } else if (std::strcmp(flag, "--no-hud") == 0) {
            options.hud = false;
        } else if (std::strcmp(flag, "--no-ao") == 0) {
            options.ambient_occlusion = false;
        } else if (std::strcmp(flag, "--no-shadows") == 0) {
            options.shadows = false;
        } else if (std::strcmp(flag, "--no-validation") == 0) {
            options.validation = false;
        } else {
            return false;
        }
        return true;
#else
        return false;
#endif
    }
};

/// `--play`'s flags. Parsed whatever the build: a build without the renderer plays headless.
struct PlayFlags {
    bool play = false;
    bool headless = false;
    bool vsync = true;
    bool validation = false;
    const char* input = nullptr;
    const char* record = nullptr;
    const char* replay = nullptr;
    cy::u64 frames = 0;

    /// Consume `argv[index]` (and its value) when it is one of the play's flags.
    bool parse(int argc, char** argv, int& index) noexcept {
        const char* flag = argv[index];
        const bool valued = index + 1 < argc;
        const struct {
            const char* name;
            const char** out;
        } paths[] = {{"--input", &input}, {"--record", &record}, {"--replay", &replay}};
        for (const auto& path : paths) {
            if (std::strcmp(flag, path.name) == 0 && valued) {
                *path.out = argv[++index];
                return true;
            }
        }
        if (std::strcmp(flag, "--frames") == 0 && valued) {
            frames = std::strtoull(argv[++index], nullptr, 10);
            return true;
        }
        bool* const switches[] = {&play, &headless, &validation};
        const char* const names[] = {"--play", "--headless", "--validation"};
        for (std::size_t which = 0; which < 3U; ++which) {
            if (std::strcmp(flag, names[which]) == 0) {
                *switches[which] = true;
                return true;
            }
        }
        if (std::strcmp(flag, "--no-vsync") == 0) {
            vsync = false;
            return true;
        }
        return false;
    }
};

/// Every battle unit as the session holds it — `unit:health:stance:target kind:target` — what a
/// test asks of an order: which unit is going for which.
void print_units(const RtsHost& host) noexcept {
    const cy::game_backend::LockstepSession* session = host.lockstep();
    if (session == nullptr) {
        return;
    }
    std::printf("rts units   ");
    for (cy::u32 unit = 0; unit < session->units(); ++unit) {
        const auto fighter = session->combatant(unit);
        if (!fighter) {
            continue;
        }
        std::printf(" %u:%u:%u:%u:%u", unit, fighter->health,
                    static_cast<unsigned>(fighter->stance),
                    static_cast<unsigned>(fighter->target.kind), fighter->target.index);
    }
    std::printf("\n");
}

/// `--play`'s report: the session, what the skirmish did, and the command log written or replayed.
void print_skirmish(const Observation& now, const PlayFlags& flags,
                    const sample::rts::ReplayLog* replayed) noexcept {
    const Observation::Skirmish& play = now.skirmish;
    std::printf(
        "rts skirmish moves=%.0f attack_moves=%.0f attacks=%.0f stops=%.0f opponent=%.0f "
        "posture=%.0f selected=%.0f outlined=%u hovered=%u selections=%.0f assigned=%.0f "
        "recalled=%.0f minimap=%.0f menus=%.0f pauses=%.0f paused_frames=%.0f focus=%.3f,%.3f "
        "distance=%.3f heading=%.4f outcome=%.0f outcome_tick=%.0f replay=%d\n",
        wide(play.moves), wide(play.attack_moves), wide(play.attacks), wide(play.stops),
        wide(play.opponent_orders), wide(play.posture), wide(play.selected), play.outlined,
        play.hovered, wide(play.selections), wide(play.assigned), wide(play.recalled),
        wide(play.minimap_jumps), wide(play.menus), wide(play.pauses), wide(play.paused_frames),
        wide(play.focus_x), wide(play.focus_z), wide(play.distance), wide(play.heading),
        wide(play.outcome), wide(play.outcome_tick), play.replay ? 1 : 0);
    if (replayed != nullptr) {
        // The digest the replay ended in against the one the recorded session did.
        const bool match = now.battle.digest == replayed->digest &&
                           now.battle.follower_digest == replayed->follower_digest &&
                           now.battle.ticks == replayed->ticks;
        std::printf(
            "rts replay   source=%s orders=%zu ticks=%llu recorded_ticks=%llu digest=%016llx "
            "recorded=%016llx follower=%016llx recorded_follower=%016llx match=%d\n",
            flags.replay, replayed->orders.size(),
            static_cast<unsigned long long>(now.battle.ticks),
            static_cast<unsigned long long>(replayed->ticks),
            static_cast<unsigned long long>(now.battle.digest),
            static_cast<unsigned long long>(replayed->digest),
            static_cast<unsigned long long>(now.battle.follower_digest),
            static_cast<unsigned long long>(replayed->follower_digest), match ? 1 : 0);
    }
}

/// ABI 1.16: the battle, as the session holds it, and what the game made of it.
void print_battle(const Observation& now) noexcept {
    const sample::rts::BattleObservation& battle = now.battle;
    if (!battle.on) {
        return;
    }
    std::printf(
        "rts battle   units=%u structures=%u ticks=%llu commands=%llu events=%llu damage=%llu "
        "blue=%u red=%u standing=%u peak_projectiles=%u disagreements=%u digest=%016llx "
        "follower=%016llx committed=%016llx\n",
        battle.units, battle.structures, static_cast<unsigned long long>(battle.ticks),
        static_cast<unsigned long long>(battle.commands),
        static_cast<unsigned long long>(battle.events),
        static_cast<unsigned long long>(battle.damage), battle.blue_alive, battle.red_alive,
        battle.standing, battle.peak_projectiles, battle.disagreements,
        static_cast<unsigned long long>(battle.digest),
        static_cast<unsigned long long>(battle.follower_digest),
        static_cast<unsigned long long>(sample::rts::battle::kBattleDigest));
    const sample::rts::BattleObservation::Game& game = battle.game;
    std::printf(
        "rts fight    soldiers=%.0f orders=%.0f events=%.0f damage=%.0f deaths=%.0f fallen=%.0f "
        "shown=%.0f hidden=%.0f effects=%.0f played=%llu lights=%.0f spawned=%llu "
        "lights_peak=%u sounds=%.0f trails=%.0f particles_peak=%u\n",
        wide(game.soldiers), wide(game.orders), wide(game.events), wide(game.damage),
        wide(game.deaths), wide(game.fallen), wide(game.shown), wide(game.hidden),
        wide(game.effects), static_cast<unsigned long long>(battle.effects), wide(game.lights),
        static_cast<unsigned long long>(battle.lights), battle.lights_peak, wide(game.sounds),
        wide(game.trails), battle.particles_peak);
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
    // ABI 1.7: the workers' animators — played by the game, run by the engine.
    std::printf(
        "rts anim     animated=%u walked=%u cheered=%u idle_after=%u only_idle=%u "
        "departure=%.4f footsteps=%.0f cheer_events=%.0f\n",
        now.animation.animated, now.animation.walked, now.animation.cheered,
        now.animation.idle_after, now.animation.only_idle, wide(now.animation.departure),
        wide(now.animation.footsteps), wide(now.animation.cheer_events));
    // ABI 1.8: the lockstep company — ordered by the game in fixed point, run by two peers — and
    // the digest every CI leg computes from the same orders in C++ (company.h).
    std::printf(
        "rts lockstep units=%u ticks=%llu commands=%llu executed=%u paths=%u/%u orders=%.0f "
        "arrivals=%.0f disagreements=%u lead=%lld reported_lead=%lld digest=%016llx "
        "follower=%016llx committed=%016llx\n",
        now.lockstep.units, static_cast<unsigned long long>(now.lockstep.ticks),
        static_cast<unsigned long long>(now.lockstep.commands), now.lockstep.orders_executed,
        now.lockstep.paths_found, now.lockstep.paths_planned, wide(now.lockstep.orders),
        wide(now.lockstep.arrivals), now.lockstep.disagreements,
        static_cast<long long>(now.lockstep.lead),
        static_cast<long long>(now.lockstep.reported_lead),
        static_cast<unsigned long long>(now.lockstep.digest),
        static_cast<unsigned long long>(now.lockstep.follower_digest),
        static_cast<unsigned long long>(sample::rts::company::kCompanyDigest));
    // ABI 1.9: the level's GI as the system holds it, and what the game read and did to it.
    std::printf(
        "rts gi       volumes=%u placed=%.0f emitters=%u disabled=%u promoted=%u lit=%.4f "
        "dark=%.4f relit=%.4f refused=%.0f frame=%llu\n",
        now.illumination.volumes, wide(now.illumination.placed), now.illumination.emitters,
        now.illumination.disabled, now.illumination.promoted, wide(now.illumination.lit),
        wide(now.illumination.dark), wide(now.illumination.relit), wide(now.illumination.refused),
        static_cast<unsigned long long>(now.illumination.frame));
    // ABI 1.10: the level's fog of war as the grid holds it, and what the game read and decided.
    std::printf(
        "rts fog      width=%u visions=%u units=%u cells=%u ghosts=%u tick=%llu "
        "digest=%016llx raiders_visible=%u tower_sight=%u radius=%.3f game_cells=%.0f "
        "tower_seen=%.0f tower_ghost=%.0f raiders_seen=%.0f far_hidden=%.0f enemy_dots=%.0f "
        "refused=%.0f\n",
        now.fog.width, now.fog.visions, now.fog.units, now.fog.cells, now.fog.ghosts,
        static_cast<unsigned long long>(now.fog.tick),
        static_cast<unsigned long long>(now.fog.digest), now.fog.raiders_visible,
        now.fog.tower_sight, wide(now.fog.radius), wide(now.fog.game_cells),
        wide(now.fog.tower_seen), wide(now.fog.tower_ghost), wide(now.fog.raiders_seen),
        wide(now.fog.far_hidden), wide(now.fog.enemy_dots), wide(now.fog.refused));
    print_battle(now);
}

/// `--play`'s host options, its recorded log read when it replays one, and the map it draws on by
/// default. False, having said why, when the log cannot be read.
bool configure_play(const PlayFlags& play, sample::rts::HostOptions& options, MapFlags& map,
                    sample::rts::ReplayLog& replay_log) noexcept {
    options.battle = true;
    options.skirmish = true;
    options.platform_input = true;
#if defined(CY_RTS_API_MAP)
    if (!play.headless && map.package == nullptr) {
        map.package = CY_RTS_API_MAP_PACKAGE;
    }
#else
    (void)map;
#endif
    if (play.replay == nullptr) {
        return true;
    }
    if (const cy::Status read = sample::rts::read_replay(play.replay, replay_log); !read) {
        std::fprintf(stderr, "rts-api: --replay %s: %s\n", play.replay, read.error().message);
        return false;
    }
    options.replay = &replay_log;
    return true;
}

/// `--play`: the host started, the session run until it ends, and the reports.
int play_session(RtsHost& host, const PlayFlags& flags, const MapFlags& map,
                 const void* map_content, const sample::rts::ReplayLog* replayed) noexcept {
    sample::rts::PlayOptions options;
    options.window = !flags.headless;
    options.vsync = flags.vsync;
    options.input = flags.input != nullptr ? flags.input : "";
    options.record = flags.record != nullptr ? flags.record : "";
    options.replay = replayed;
    options.frames = flags.frames;
#if defined(CY_RTS_API_MAP)
    options.render = map.options;
    // The game picks and lays out in a 16:9 view, so the frame is 16:9 too: its height follows
    // its width, and the window letterboxes it.
    options.render.height = (options.render.width * 9U) / 16U;
    options.render.outlines = true;
    options.render.stills = false;
    // A person's session measures what the frame costs: the validation layer is asked for.
    options.render.validation = flags.validation;
    options.map = static_cast<const sample::rts::map::MapContent*>(map_content);
#else
    (void)map;
    (void)map_content;
#endif
    sample::rts::PlayReport report;
    if (const cy::Status ran = sample::rts::run_play(host, options, report); !ran) {
        std::fprintf(stderr, "rts-api: --play: %s\n", ran.error().message);
        return ran.error().code == cy::ErrorCode::Unavailable ? 0 : 1;
    }
    print_report(host, Findings{});
    sample::rts::print_play_report(report);
    print_skirmish(host.observe(), flags, replayed);
    print_units(host);
    host.shutdown();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    sample::rts::HostOptions options;
    options.module_library = CY_RTS_API_MODULE_LIBRARY;
    options.module_manifest = CY_RTS_API_MODULE_MANIFEST;
    const char* render_directory = nullptr;
    bool render_gi = true;
    bool render_fog = false;
    MapFlags map;
    PlayFlags play;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--render") == 0 && index + 1 < argc) {
            render_directory = argv[++index];
        } else if (std::strcmp(argv[index], "--gi") == 0 && index + 1 < argc) {
            render_gi = std::strcmp(argv[++index], "off") != 0;
        } else if (std::strcmp(argv[index], "--fog") == 0) {
            render_fog = true;
        } else if (map.parse(argc, argv, index) || play.parse(argc, argv, index)) {
            continue;
        } else if (std::strcmp(argv[index], "--no-behaviours") == 0) {
            options.behaviours = false;
        } else if (std::strcmp(argv[index], "--no-systems") == 0) {
            options.systems = false;
        } else if (std::strcmp(argv[index], "--no-ui") == 0) {
            options.ui = false;
        } else if (std::strcmp(argv[index], "--battle") == 0) {
            options.battle = true;
        } else {
            std::fprintf(stderr, "rts-api: unknown argument '%s'\n", argv[index]);
            return 2;
        }
    }
    // `--play`: the battle, a person's hand on blue, the platform's devices, and — with
    // `--replay` — the recorded orders in place of anyone's.
    sample::rts::ReplayLog replay_log;
    if (play.play && !configure_play(play, options, map, replay_log)) {
        return 1;
    }

#if defined(CY_RTS_API_MAP)
    // THE MAP LOADS BEFORE THE HOST STARTS: its buildings are boxes the level's GI holds, and the
    // fog of war's grid covers the ground it spans. Without `--render` the session runs headless on
    // the map: the gameplay a drawn run is held to.
    sample::rts::map::MapContent map_content;
    std::vector<sample::rts::FieldBox> map_boxes;
    cy::Aabb map_ground;
    if (map.package != nullptr) {
        sample::rts::map::LoadReport loaded;
        std::string detail;
        if (const cy::Status read =
                sample::rts::map::load_map(cy::system_allocator(cy::MemoryDomain::Assets),
                                           map.package, map_content, loaded, detail);
            !read) {
            std::fprintf(stderr, "rts-api: --map: %s (%s)\n", read.error().message, detail.c_str());
            return 1;
        }
        std::printf("rts map load assets=%u bytes=%llu load_ms=%.1f digest=%016llx\n",
                    loaded.assets, static_cast<unsigned long long>(loaded.bytes), loaded.load_ms,
                    static_cast<unsigned long long>(loaded.digest));
        map_boxes = sample::rts::map_gi_boxes(map_content);
        options.extra_gi_boxes = map_boxes.data();
        options.extra_gi_box_count = static_cast<cy::u32>(map_boxes.size());
        map_ground = sample::rts::map::map_bounds(map_content);
        options.fog_bounds = &map_ground;
    }
#endif

    RtsHost host(cy::system_allocator(cy::MemoryDomain::World), options);
    const char* detail = "";
    if (const cy::Status started = host.start(&detail); !started) {
        std::fprintf(stderr, "rts-api: start failed at '%s': %s\n", detail,
                     started.error().message);
        return 1;
    }
    if (play.play) {
#if defined(CY_RTS_API_MAP)
        const void* content = map.package != nullptr ? &map_content : nullptr;
#else
        const void* content = nullptr;
#endif
        return play_session(host, play, map, content,
                            options.replay != nullptr ? &replay_log : nullptr);
    }
#if defined(CY_RTS_API_RENDER)
    std::unique_ptr<sample::rts::RtsRenderer> renderer;
#    if defined(CY_RTS_API_MAP)
    std::unique_ptr<sample::rts::MapRenderer> map_renderer;
    if (render_directory != nullptr && map.package != nullptr) {
        map_renderer = std::make_unique<sample::rts::MapRenderer>(
            cy::system_allocator(cy::MemoryDomain::Renderer));
        sample::rts::MapRenderOptions render = map.options;
        render.directory = render_directory;
        render.gi = render_gi;
        if (const cy::Status started = map_renderer->start(host, map_content, render); !started) {
            std::fprintf(stderr, "rts-api: --render --map: %s\n", started.error().message);
            return started.error().code == cy::ErrorCode::Unavailable ? 0 : 1;
        }
    } else
#    else
    if (map.package != nullptr) {
        std::fprintf(stderr, "rts-api: --map needs the map's cook, which this build has not\n");
        return 2;
    }
#    endif
        if (render_directory != nullptr) {
        renderer = std::make_unique<sample::rts::RtsRenderer>(
            cy::system_allocator(cy::MemoryDomain::Renderer));
        sample::rts::RenderOptions render;
        render.directory = render_directory;
        render.gi = render_gi;
        render.fog = render_fog;
        if (const cy::Status started = renderer->start(host, render); !started) {
            std::fprintf(stderr, "rts-api: --render: %s\n", started.error().message);
            return started.error().code == cy::ErrorCode::Unavailable ? 0 : 1;
        }
    }
#else
    if (render_directory != nullptr || map.package != nullptr) {
        std::fprintf(stderr,
                     "rts-api: --render needs the Vulkan renderer, which this build has not\n");
        return 2;
    }
    (void)render_gi;
    (void)render_fog;
#endif
    sample::rts::Player player(host);
    // A battle runs its own length; the scripted player's hand is done by frame 420 either way.
    const cy::u64 frames =
        options.battle ? sample::rts::battle::kTicks : sample::rts::Player::kFrames;
    for (cy::u64 frame = 0; frame < frames; ++frame) {
        const cy::Status injected = player.before_frame(frame);
        cy::Status ran = injected ? host.frame() : injected;
#if defined(CY_RTS_API_RENDER)
        if (ran && renderer != nullptr) {
            ran = renderer->frame(host);
        }
#    if defined(CY_RTS_API_MAP)
        if (ran && map_renderer != nullptr) {
            ran = map_renderer->frame(host);
        }
#    endif
#endif
        if (!ran) {
            std::fprintf(stderr, "rts-api: frame %llu failed: %s\n",
                         static_cast<unsigned long long>(frame), ran.error().message);
            return 1;
        }
        player.after_frame(frame);
    }
    print_report(host, player.findings());
#if defined(CY_RTS_API_RENDER)
    if (renderer != nullptr) {
        renderer->print_report();
    }
#    if defined(CY_RTS_API_MAP)
    if (map_renderer != nullptr) {
        map_renderer->print_report();
    }
#    endif
#endif
    host.shutdown();
    return 0;
}
