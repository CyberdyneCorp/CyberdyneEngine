// samples/08-vertical-slice — the M8.b milestone artefact. Section 12.
//
// The host: it reads a command line, builds the game, runs it, runs the audit, and prints what
// happened as `key = value` lines that `slice.py` checks. It decides nothing about the game.
//
// THE EXIT STATUS IS DERIVED. A run whose audit fails, or whose two independently built slices
// disagree, returns non-zero — hard rule 6, and the same rule `samples/harness/artefact.py`
// enforces one layer up. There is deliberately no switch that says otherwise.

#include <cy/core/memory/system_allocator.h>

#include "slice.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

using cy::sample::slice::InterpretationAudit;
using cy::sample::slice::Options;
using cy::sample::slice::Report;
using cy::sample::slice::Slice;

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsage = 2;

void usage() {
    std::puts(
        "usage: cy_sample_vertical-slice [options]\n"
        "  --agents <n>         characters in the level (default 256)\n"
        "  --ticks <n>          simulation ticks to run (default 240)\n"
        "  --effects <n>        concurrent gameplay effects (default 100)\n"
        "  --seed <n>           the session seed (default 1592313583)\n"
        "  --no-render          skip the assembled frame, leaving the simulation half\n"
        "  --replay             build a SECOND slice with the same options and compare digests\n"
        "  --interpret-control  add a per-entity virtual tick, so the audit has something to find\n"
        "  --shot-data <path>   write the frame's own draw list, projected, for the picture\n");
}

[[nodiscard]] bool number(const char* text, cy::u64& out) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    out = static_cast<cy::u64>(value);
    return true;
}

void print_report(const Report& report, const InterpretationAudit& audit) {
    // One `key = value` per line, so the driver parses without knowing this program's structure.
    std::printf("agents = %u\n", report.agents);
    std::printf("ticks = %u\n", report.ticks);
    std::printf("level_entities = %u\n", report.level_entities);
    std::printf("level_triangles = %u\n", report.level_triangles);
    std::printf("navmesh_polys = %u\n", report.navmesh_polys);
    std::printf("navmesh_tiles = %u\n", report.navmesh_tiles);
    std::printf("navmesh_recast = %d\n", report.navmesh_recast ? 1 : 0);

    std::printf("script_digest = %016llx\n", static_cast<unsigned long long>(report.script_digest));
    std::printf("ability_digest = %016llx\n",
                static_cast<unsigned long long>(report.ability_digest));
    std::printf("pose_digest = %016llx\n", static_cast<unsigned long long>(report.pose_digest));
    std::printf("behaviour_digest = %016llx\n",
                static_cast<unsigned long long>(report.behaviour_digest));
    std::printf("rig_digest = %016llx\n", static_cast<unsigned long long>(report.rig_digest));
    std::printf("rig_ir_digest = %016llx\n", static_cast<unsigned long long>(report.rig_ir_digest));
    std::printf("rig_query_calls = %u\n", report.rig_query_calls);
    std::printf("rig_queries = %u\n", report.rig_queries);

    std::printf("think_instructions = %llu\n",
                static_cast<unsigned long long>(report.think_instructions));
    std::printf("agents_thought = %u\n", report.agents_thought);
    std::printf("agents_starved = %u\n", report.agents_starved);
    std::printf("agents_full = %u\n", report.agents_at_tier[0]);
    std::printf("agents_reduced = %u\n", report.agents_at_tier[1]);
    std::printf("agents_minimal = %u\n", report.agents_at_tier[2]);
    std::printf("agents_statistical = %u\n", report.agents_at_tier[3]);
    std::printf("perception_queries = %u\n", report.perception_queries);
    std::printf("crowd_adjusted = %u\n", report.crowd_adjusted);
    std::printf("paths_found = %u\n", report.paths_found);
    std::printf("clips_sampled = %u\n", report.clips_sampled);
    std::printf("joints_sampled = %u\n", report.joints_sampled);
    std::printf("poses_evaluated = %u\n", report.poses_evaluated);
    std::printf("root_motion_travelled = %.4f\n",
                static_cast<double>(report.root_motion_travelled));

    std::printf("activations_committed = %u\n", report.activations_committed);
    std::printf("activations_refused = %u\n", report.activations_refused);
    std::printf("effects_peak = %u\n", report.effects_peak);
    std::printf("effects_expired = %u\n", report.effects_expired);
    std::printf("cues_emitted = %u\n", report.cues_emitted);
    std::printf("cues_suppressed = %u\n", report.cues_suppressed);

    std::printf("ui_elements = %u\n", report.ui_elements);
    std::printf("ui_primitives = %u\n", report.ui_primitives);
    std::printf("ui_batches = %u\n", report.ui_batches);
    std::printf("ui_accessible = %d\n", report.ui_accessible ? 1 : 0);
    std::printf("ui_findings = %u\n", report.ui_findings);
    std::printf("menu_draws = %u\n", report.menu_draws);
    std::printf("menu_batches = %u\n", report.menu_batches);
    std::printf("audio_sources = %u\n", report.audio_sources);
    std::printf("audio_full_acoustic = %u\n", report.audio_full_acoustic);
    std::printf("audio_virtual = %u\n", report.audio_virtual);
    std::printf("music_transitions = %u\n", report.music_transitions);

    std::printf("frame_assembled = %d\n", report.frame_assembled ? 1 : 0);
    std::printf("frame_draws = %u\n", report.frame_draws);
    std::printf("frame_visible = %u\n", report.frame_visible);
    std::printf("frame_lights = %u\n", report.frame_lights);
    std::printf("frame_passes = %u\n", report.frame_passes);
    std::printf("frame_material_slots = %u\n", report.frame_material_slots);
    std::printf("frame_draws_without_material = %u\n", report.frame_draws_without_material);
    std::printf("frame_distinct_meshes = %u\n", report.frame_distinct_meshes);
    std::printf("frame_clusters = %u\n", report.frame_clusters);
    std::printf("frame_shadow_pages = %u\n", report.frame_shadow_pages);
    std::printf("mesh_assets = %u\n", report.mesh_assets);
    std::printf("meshes_bound = %u\n", report.meshes_bound);
    std::printf("meshes_unresolved = %u\n", report.meshes_unresolved);
    std::printf("materials_bound = %u\n", report.materials_bound);

    std::printf("think_us_median = %.3f\n", report.think_us_median);
    std::printf("sense_us_median = %.3f\n", report.sense_us_median);
    std::printf("navigate_us_median = %.3f\n", report.navigate_us_median);
    std::printf("animate_us_median = %.3f\n", report.animate_us_median);
    std::printf("abilities_us_median = %.3f\n", report.abilities_us_median);
    std::printf("simulation_us_median = %.3f\n", report.simulation_us_median);
    std::printf("simulation_us_worst = %.3f\n", report.simulation_us_worst);
    std::printf("per_agent_us_median = %.5f\n", report.per_agent_us_median);
    std::printf("effects_us_median = %.4f\n", report.effects_us_median);
    std::printf("interface_us_median = %.3f\n", report.interface_us_median);
    std::printf("frame_us_median = %.3f\n", report.frame_us_median);

    std::printf("state_digest = %016llx\n", static_cast<unsigned long long>(report.state_digest));
    std::printf("replay_digest = %016llx\n", static_cast<unsigned long long>(report.replay_digest));

    // The declared budgets, printed BY THE PROGRAM so the driver compares a measurement against
    // this file rather than against a number copied into a script.
    std::printf("budget_simulation_us = %.1f\n", cy::sample::slice::Budgets::kSimulationUs);
    std::printf("budget_effects_us = %.1f\n", cy::sample::slice::Budgets::kEffectsUs);
    std::printf("budget_interface_us = %.1f\n", cy::sample::slice::Budgets::kInterfaceUs);
    std::printf("budget_linearity = %.1f\n", cy::sample::slice::Budgets::kLinearityFactor);
    std::printf("scale_agents = %u\n", cy::sample::slice::Budgets::kScaleAgents);
    std::printf("baseline_agents = %u\n", cy::sample::slice::Budgets::kBaselineAgents);

    std::printf("audit_passed = %d\n", audit.passed() ? 1 : 0);
    std::printf("audit_graphs = %u\n", audit.graphs_audited);
    std::printf("audit_graphs_incomplete = %u\n", audit.graphs_incomplete);
    std::printf("audit_graphs_failed = %u\n", audit.graphs_failed);
    std::printf("audit_state_types = %u\n", audit.state_types_checked);
    std::printf("audit_polymorphic = %u\n", audit.state_types_polymorphic);
    std::printf("audit_compilations_before_loop = %u\n", audit.compilations_before_loop);
    std::printf("audit_compilations_during_loop = %u\n", audit.compilations_during_loop);
    for (const cy::Name offender : audit.offenders.span()) {
        // `%.*s` with the view's own length: `Name::text()` is a string view and its `data()` is
        // not promised to be terminated, so `%s` would read until it found a zero somewhere.
        const std::string_view text = offender.text();
        std::printf("audit_offender = %.*s\n", static_cast<int>(text.size()), text.data());
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    bool replay = false;
    const char* shot_data = nullptr;

    for (int index = 1; index < argc; ++index) {
        const char* argument = argv[index];
        // The increment is OUTSIDE the condition: `(index + 1) < argc && number(argv[++index]…)`
        // reads and modifies `index` in one expression, and C++ does not order the two.
        const auto value = [&](cy::u64& out) {
            if ((index + 1) >= argc) {
                return false;
            }
            index += 1;
            return number(argv[index], out);
        };
        cy::u64 parsed = 0;
        if (std::strcmp(argument, "--agents") == 0 && value(parsed)) {
            options.agents = static_cast<cy::u32>(parsed);
        } else if (std::strcmp(argument, "--ticks") == 0 && value(parsed)) {
            options.ticks = static_cast<cy::u32>(parsed);
        } else if (std::strcmp(argument, "--effects") == 0 && value(parsed)) {
            options.effects = static_cast<cy::u32>(parsed);
        } else if (std::strcmp(argument, "--seed") == 0 && value(parsed)) {
            options.seed = parsed;
        } else if (std::strcmp(argument, "--no-render") == 0) {
            options.render = false;
        } else if (std::strcmp(argument, "--replay") == 0) {
            replay = true;
        } else if (std::strcmp(argument, "--interpret-control") == 0) {
            options.interpret_control = true;
        } else if (std::strcmp(argument, "--shot-data") == 0 && (index + 1) < argc) {
            shot_data = argv[++index];
        } else {
            usage();
            return kUsage;
        }
    }
    if (options.agents == 0U || options.ticks == 0U) {
        usage();
        return kUsage;
    }

    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    Slice slice(allocator);
    if (const cy::Status built = slice.build(options); !built) {
        std::fprintf(stderr, "the slice could not be built: %s\n", built.error().message);
        return kFailed;
    }
    if (const cy::Status ran = slice.run(); !ran) {
        std::fprintf(stderr, "the slice could not be run: %s\n", ran.error().message);
        return kFailed;
    }
    if (shot_data != nullptr) {
        if (const cy::Status written = cy::sample::slice::write_shot_data(slice, shot_data);
            !written) {
            std::fprintf(stderr, "the picture's data could not be written: %s\n",
                         written.error().message);
            return kFailed;
        }
    }

    // THE DETERMINISM CHECK, in this process: a second slice built from the same options, run for
    // the same ticks, folded the same way. `slice.py` runs the binary twice for the cross-process
    // half, which is the one that catches a digest that depends on an address.
    if (replay) {
        Slice again(allocator);
        if (const cy::Status built = again.build(options); !built) {
            std::fprintf(stderr, "the replay could not be built: %s\n", built.error().message);
            return kFailed;
        }
        if (const cy::Status ran = again.run(); !ran) {
            std::fprintf(stderr, "the replay could not be run: %s\n", ran.error().message);
            return kFailed;
        }
        slice.report().replay_digest = again.digest();
    }

    InterpretationAudit audit(allocator);
    if (const cy::Status ran = slice.audit(audit); !ran) {
        std::fprintf(stderr, "the audit could not run: %s\n", ran.error().message);
        return kFailed;
    }
    print_report(slice.report(), audit);

    // Derived, never chosen. A run that found an interpreted consumer, or whose replay disagreed,
    // is a run that failed.
    if (!audit.passed()) {
        std::fprintf(stderr, "the interpretation audit did not pass\n");
        return kFailed;
    }
    if (replay && slice.report().replay_digest != slice.report().state_digest) {
        std::fprintf(stderr, "the replay diverged from the run\n");
        return kFailed;
    }
    return kOk;
}
