// samples/02-headless-sim — M2's closing artefact. Tasks 5.1, 5.2 and 5.3.
//
// One program that authors a scene, cooks it into archetype blocks, loads it into a world, ticks
// 10,000 fixed steps, and prints a hierarchical state hash that reproduces exactly on a re-run and
// after a snapshot restore. Every workstream this milestone built is exercised HERE, together,
// because the seams between them are the thing the milestone has that its parts do not:
//
//   ecs           entities live in per-archetype chunks; the systems iterate columns and the
//                 structural changes they make land at the tick's flush point.
//   nodes         a named hierarchy is authored through the façade, and the world transform read
//                 through a `Node` is the entity's component and not a copy of it.
//   serialization the scene is written to its text form, READ BACK, resolved through its prefab's
//                 variants and overrides, flattened, and cooked into blocks laid out for M1's
//                 chunks. The runtime never sees a document.
//   the loop      four fixed simulation ticks then one variable frame, with an interpolation alpha,
//                 and one commit boundary per tick past which the tick's state is authoritative.
//
//   just run-sample headless-sim                     the defaults: 10,000 ticks, 64 instances
//   just run-sample headless-sim --ticks 100         a short run, which is what a smoke test wants
//   just run-sample headless-sim --seed 7            a different session seed, hence a different
//   hash just run-sample headless-sim --show-scene        print the authoring text and exit just
//   run-sample headless-sim --help              the switches
//
// HEADLESS BY DESIGN, not by circumstance. Nothing here draws, and the display server is the null
// one: a simulation whose reproducibility depended on a window would be reporting on the window.
// M3's renderer attaches to `Simulation::frame()`, which this sample already drives and ignores.
//
// EVERY LINE THIS PRINTS IS A FUNCTION OF THE CONTENT, never of the machine or the run. That is not
// a style choice — tests/smoke/test_headless_sim.cpp runs this binary twice and compares the two
// outputs line for line, so a figure that varied with timing or with an allocation address would
// turn the milestone gate into a flake.

#include <cy/core/base/expected.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/platform/headless_display_server.h>
#include <cy/platform/sdl3_platform.h>
#include <cy/runtime/runtime.h>
#include <cy/scene/components.h>
#include <cy/scene/node.h>

#include "content.h"
#include "simulation.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

using cy::u32;
using cy::u64;
using cy::usize;

constexpr const char* kTag = "02-headless-sim";

struct Options {
    u64 ticks = 10000;
    u32 ticks_per_frame = 4;
    u32 instances = 64;
    u32 batteries = 3;
    u32 turrets = 4;
    u64 seed = 1;
    bool show_scene = false;
    bool help = false;
};

void print_usage() {
    std::fputs(
        "samples/02-headless-sim — authors a scene, cooks it, ticks it, and prints a\n"
        "hierarchical state hash that reproduces across runs and across snapshot restore.\n"
        "\n"
        "  --ticks <n>             simulation ticks to run       (default 10000)\n"
        "  --ticks-per-frame <n>   fixed ticks per frame, max 8  (default 4)\n"
        "  --instances <n>         copies of the cooked scene    (default 64)\n"
        "  --batteries <n>         battery nodes in the tree     (default 3)\n"
        "  --turrets <n>           turret nodes per battery      (default 4)\n"
        "  --seed <n>              the session seed              (default 1)\n"
        "  --show-scene            print the authoring text form and exit\n"
        "  --help                  this text\n",
        stderr);
}

/// Returns false when the command line is not one this sample understands, having said so.
bool parse_options(int argument_count, char** arguments, Options& options) {
    for (int i = 1; i < argument_count; ++i) {
        const std::string_view argument{arguments[i]};
        const bool has_value = i + 1 < argument_count;

        if (argument == "--help") {
            print_usage();
            options.help = true;
            return true;
        }
        if (argument == "--show-scene") {
            options.show_scene = true;
        } else if (argument == "--ticks" && has_value) {
            options.ticks = std::strtoull(arguments[++i], nullptr, 10);
        } else if (argument == "--ticks-per-frame" && has_value) {
            options.ticks_per_frame = static_cast<u32>(std::strtoul(arguments[++i], nullptr, 10));
        } else if (argument == "--instances" && has_value) {
            options.instances = static_cast<u32>(std::strtoul(arguments[++i], nullptr, 10));
        } else if (argument == "--batteries" && has_value) {
            options.batteries = static_cast<u32>(std::strtoul(arguments[++i], nullptr, 10));
        } else if (argument == "--turrets" && has_value) {
            options.turrets = static_cast<u32>(std::strtoul(arguments[++i], nullptr, 10));
        } else if (argument == "--seed" && has_value) {
            options.seed = std::strtoull(arguments[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "%s: unrecognised argument '%s'\n\n", kTag, arguments[i]);
            print_usage();
            return false;
        }
    }
    if (options.ticks_per_frame == 0 || options.ticks_per_frame > 8) {
        std::fprintf(stderr, "%s: --ticks-per-frame must be between 1 and 8\n", kTag);
        return false;
    }
    if (options.instances == 0) {
        std::fprintf(stderr, "%s: --instances must be at least 1\n", kTag);
        return false;
    }
    return true;
}

void report(const char* label, const cy::Error& error) {
    std::fprintf(stderr, "%s: %s failed: %s (%s)\n", kTag, label, error.message,
                 cy::error_code_name(error.code));
}

/// The one coherence claim this sample makes about the façade, checked rather than asserted in
/// prose: a node's world transform IS the entity's component. Not a copy kept in step by a sync
/// step — the same bytes, read two ways.
///
/// `scene-graph-and-nodes` states it as an invariant and design.md §3 turns it into the rule that
/// there is no shadow copy at all. Comparing the two readings is the only way a program can say so.
bool node_agrees_with_entity(cy::scene::SceneTree& tree, const char* path) {
    const cy::scene::Node node = tree.find(path);
    if (!node.valid()) {
        return false;
    }
    const cy::Transform through_facade = node.world_transform();
    const auto* through_ecs = tree.world().get<cy::scene::WorldTransform>(
        node.entity(), tree.components().world_transform);
    if (through_ecs == nullptr) {
        return false;
    }
    // `operator==` on a Transform is exact — component by component, no tolerance. A tolerance here
    // would turn "these are the same value" into "these are close", which is a different and much
    // weaker claim than the one the invariant makes.
    return through_facade == through_ecs->value;
}

}  // namespace

int main(int argument_count, char** arguments) {
    Options options;
    if (!parse_options(argument_count, arguments, options)) {
        return 2;
    }
    if (options.help) {
        return 0;
    }

    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Ecs);

    // --- The simulation, before the runtime
    // -------------------------------------------------------
    //
    // The order below is the one `<cy/runtime/simulation.h>` documents and it is not
    // interchangeable: a game registers its components and its systems BETWEEN constructing the
    // simulation and starting the runtime, because the runtime's Boot stage is what closes
    // registration. A simulation the runtime constructed would leave nowhere to do that.
    cy::runtime::SimulationConfig simulation_config;
    simulation_config.world_name = "emplacement";
    simulation_config.session_seed = options.seed;
    simulation_config.clock.mode = cy::determinism::TickMode::FixedStep;
    simulation_config.clock.fixed_ticks_per_frame = options.ticks_per_frame;
    cy::runtime::Simulation simulation(allocator, simulation_config);
    if (const cy::Status ready = simulation.initialize(); !ready) {
        report("simulation", ready.error());
        return 1;
    }

    const auto ids = sample::register_components(simulation.world());
    if (!ids) {
        report("components", ids.error());
        return 1;
    }

    // --- Author, cook, load
    // -----------------------------------------------------------------------
    cy::Array<char> scene_text(allocator);
    cy::Array<cy::ecs::Entity> spawned(allocator);
    sample::ContentReport content;
    if (const cy::Status built =
            sample::build_content(simulation.world(), options.instances,
                                  options.show_scene ? &scene_text : nullptr, spawned, content);
        !built) {
        report("content", built.error());
        return 1;
    }
    if (options.show_scene) {
        std::fwrite(scene_text.data(), 1, scene_text.size(), stdout);
        return 0;
    }

    sample::NodeReport nodes(allocator);
    if (const cy::Status loaded =
            sample::build_node_scene(*simulation.tree(), options.batteries, options.turrets, nodes);
        !loaded) {
        report("node scene", loaded.error());
        return 1;
    }

    // --- What participates in the hash, and the systems that move it
    // ------------------------------
    if (const cy::Status declared = sample::declare_state_schema(simulation, ids.value());
        !declared) {
        report("state schema", declared.error());
        return 1;
    }
    sample::Systems systems(simulation, ids.value(), nodes);
    if (const cy::Status installed = systems.install(); !installed) {
        report("systems", installed.error());
        return 1;
    }

    // --- The runtime
    // ------------------------------------------------------------------------------
    cy::Sdl3Platform platform;
    if (const cy::Status started = platform.initialise(argument_count, arguments); !started) {
        report("platform", started.error());
        return 1;
    }
    cy::HeadlessDisplayServer display;
    if (const cy::Status started = display.initialise(); !started) {
        report("display", started.error());
        platform.shutdown();
        return 1;
    }

    cy::RuntimeConfig config;
    config.platform = &platform;
    config.display = &display;
    config.create_window = false;
    config.create_surface = false;
    config.install_crash_handler = false;
    config.tick_mode = cy::determinism::TickMode::FixedStep;
    config.fixed_ticks_per_frame = options.ticks_per_frame;
    config.simulation = &simulation;

    cy::Runtime runtime;
    if (const cy::Status started = runtime.startup(config); !started) {
        report("runtime", started.error());
        display.shutdown();
        platform.shutdown();
        return 1;
    }

    std::fprintf(stdout,
                 "%s: authored prefab=%u entities  placements=%u  parameters=%u  text=%u B "
                 "digest=%016llx\n",
                 kTag, content.prefab_entities, content.scene_placements, content.parameters,
                 content.text_bytes, static_cast<unsigned long long>(content.text_digest));
    std::fprintf(stdout, "%s: resolved entities=%u overrides=%u parameters=%u conflicts=%u\n", kTag,
                 content.resolved_entities, content.overrides_applied, content.parameters_applied,
                 content.conflicts);
    std::fprintf(stdout,
                 "%s: cooked   blocks=%u retained=%u flattened=%u references=%u dangling=%u "
                 "payload=%u B\n",
                 kTag, content.blocks, content.relationships_retained,
                 content.relationships_flattened, content.reference_sites,
                 content.dangling_references, content.payload_bytes);

    const cy::ecs::WorldStats world_stats = simulation.world().stats();
    std::fprintf(stdout,
                 "%s: world    instances=%u entities=%llu archetypes=%u chunks=%u fill=%.2f%%\n",
                 kTag, content.instances, static_cast<unsigned long long>(world_stats.entities),
                 world_stats.archetypes, world_stats.chunks, world_stats.fill_ratio * 100.0);
    std::fprintf(stdout, "%s: nodes    scene=%u nodes=%u batteries=%u turrets=%u systems=%u\n",
                 kTag, nodes.scene, nodes.nodes, nodes.batteries, nodes.turrets_per_battery,
                 systems.system_count());

    // --- The loop
    // ---------------------------------------------------------------------------------
    //
    // N fixed simulation ticks, then one variable frame. `Runtime::tick()` owns the split; the host
    // only says how many frames it wants. In fixed-step mode the tick count is exact rather than a
    // function of how fast this machine is, which is what makes a hash comparable across machines.
    const u64 frames = (options.ticks + options.ticks_per_frame - 1) / options.ticks_per_frame;
    for (u64 frame = 0; frame < frames; ++frame) {
        if (const cy::Status ticked = runtime.tick(); !ticked) {
            report("tick", ticked.error());
            runtime.shutdown();
            display.shutdown();
            platform.shutdown();
            return 1;
        }
    }

    const cy::FrameStats frame_stats = runtime.frame();
    std::fprintf(
        stdout, "%s: tick     frames=%llu ticks=%llu epoch=%u tick=%llu version=%llu alpha=%.3f\n",
        kTag, static_cast<unsigned long long>(frame_stats.frame_index),
        static_cast<unsigned long long>(frame_stats.total_ticks), frame_stats.committed.epoch.value,
        static_cast<unsigned long long>(frame_stats.committed.tick),
        static_cast<unsigned long long>(frame_stats.state_version),
        static_cast<double>(frame_stats.interpolation_alpha));

    // --- The hash, and what it does and does not cover
    // ---------------------------------------------
    cy::determinism::StateHashTree tree(allocator);
    cy::runtime::WorldHashReport walk;
    const auto hash = sample::hash_world_now(simulation, tree, walk);
    if (!hash) {
        report("hash", hash.error());
        runtime.shutdown();
        display.shutdown();
        platform.shutdown();
        return 1;
    }
    std::fprintf(stdout,
                 "%s: hash     %016llx  archetypes=%u entities=%u components=%u fields=%u\n", kTag,
                 static_cast<unsigned long long>(hash.value()), walk.archetypes_visited,
                 walk.entities_hashed, walk.components_hashed, walk.fields_hashed);
    // Undeclared subjects are the honest half of the number above: `Parent`, `Children` and eleven
    // of the scene's twelve built-ins have no reflected descriptor, so nothing derives a schema for
    // them and the hash says nothing about them. A hash that quietly covered a tenth of the world
    // would be worse than one that reports the gap.
    std::fprintf(stdout, "%s: schema   subjects declared=%u undeclared=%u nodes=%u\n", kTag,
                 walk.subjects_declared, walk.subjects_undeclared, tree.node_count());
    sample::print_hash_tree(tree, kTag);

    // --- The façade, and the snapshot
    // ---------------------------------------------------------------
    char node_path[128];
    (void)std::snprintf(node_path, sizeof(node_path), "%s/battery-%u/turret-%u",
                        sample::kNodeRootPath, options.batteries / 2, options.turrets / 2);
    const bool coherent = node_agrees_with_entity(*simulation.tree(), node_path);
    std::fprintf(stdout, "%s: facade   %s reads the entity's own transform: %s\n", kTag, node_path,
                 coherent ? "yes" : "NO");

    sample::ReproductionReport reproduction;
    if (const cy::Status checked = sample::check_snapshot_restore(simulation, 128, reproduction);
        !checked) {
        report("snapshot", checked.error());
        runtime.shutdown();
        display.shutdown();
        platform.shutdown();
        return 1;
    }
    std::fprintf(stdout,
                 "%s: restore  settled=%016llx +%u ticks=%016llx restored=%016llx  "
                 "snapshot=%llu B over %llu entities\n",
                 kTag, static_cast<unsigned long long>(reproduction.settled),
                 reproduction.diverging_ticks,
                 static_cast<unsigned long long>(reproduction.diverged),
                 static_cast<unsigned long long>(reproduction.restored),
                 static_cast<unsigned long long>(reproduction.snapshot_bytes),
                 static_cast<unsigned long long>(reproduction.snapshot_entities));
    std::fprintf(stdout, "%s: restore  the world moved: %s   the restore reproduced it: %s%s%s\n",
                 kTag, reproduction.moved ? "yes" : "NO", reproduction.matches ? "yes" : "NO",
                 reproduction.divergence[0] == '\0' ? "" : "   first difference at ",
                 reproduction.divergence);

    const bool reproduced = coherent && reproduction.moved && reproduction.matches;
    const int exit_code = reproduced ? 0 : 1;

    runtime.shutdown();
    display.shutdown();
    platform.shutdown();
    std::fprintf(stdout, "%s: exit %d (%s)\n", kTag, exit_code,
                 exit_code == 0 ? "clean" : "failed");
    return exit_code;
}
