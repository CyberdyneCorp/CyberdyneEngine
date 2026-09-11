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
        "  --shot-data <path>   write the frame's own draw list, projected, for the picture\n"
        "  --no-spectacle       run without the particles and the cut. THE CONTROL FOR TASK 5.3\n"
        "  --cut-tick <n>       the tick the cinematic starts on (default 30)\n"
        "  --capture <prefix>   RECORD the frame on a graphics device and write <prefix>.png,\n"
        "                       <prefix>-no-callbacks.png and <prefix>-cut.png\n"
        "  --capture-tick <n>   the tick the capture is taken on (default: the last)\n");
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

/// M8.c's two systems and the camera, printed the same way everything else is.
void print_spectacle(const cy::sample::slice::SpectacleReport& spectacle) {
    std::printf("effects_played = %u\n", spectacle.effects_played);
    std::printf("effects_refused = %u\n", spectacle.effects_refused);
    std::printf("vfx_live_particles = %u\n", spectacle.vfx_live_particles);
    std::printf("vfx_peak_particles = %u\n", spectacle.vfx_peak_particles);
    std::printf("vfx_critical = %u\n", spectacle.vfx_by_importance[0]);
    std::printf("vfx_important = %u\n", spectacle.vfx_by_importance[1]);
    std::printf("vfx_ambient = %u\n", spectacle.vfx_by_importance[2]);
    std::printf("vfx_decorative = %u\n", spectacle.vfx_by_importance[3]);
    std::printf("vfx_spawned = %u\n", spectacle.vfx_spawned);
    std::printf("vfx_killed = %u\n", spectacle.vfx_killed);
    std::printf("vfx_substeps = %u\n", spectacle.vfx_substeps);
    std::printf("vfx_dispatches_unmerged = %u\n", spectacle.vfx_dispatches_unmerged);
    std::printf("vfx_dispatches_merged = %u\n", spectacle.vfx_dispatches_merged);
    std::printf("vfx_gpu_emitters = %u\n", spectacle.vfx_gpu_emitters);
    std::printf("vfx_cpu_emitters = %u\n", spectacle.vfx_cpu_emitters);
    std::printf("vfx_cpu_fallbacks = %u\n", spectacle.vfx_cpu_fallbacks);
    std::printf("vfx_published = %u\n", spectacle.vfx_published);
    std::printf("vfx_dropped = %u\n", spectacle.vfx_dropped);
    std::printf("vfx_pool_used_bytes = %llu\n",
                static_cast<unsigned long long>(spectacle.vfx_pool_used_bytes));
    std::printf("vfx_pool_total_bytes = %llu\n",
                static_cast<unsigned long long>(spectacle.vfx_pool_total_bytes));

    std::printf("cut_ran = %d\n", spectacle.cut_ran ? 1 : 0);
    std::printf("cut_frames = %u\n", spectacle.cut_frames);
    std::printf("cut_blend_frames = %u\n", spectacle.cut_blend_frames);
    std::printf("cut_segments = %u\n", spectacle.cut_segments);
    std::printf("cut_channels = %u\n", spectacle.cut_channels);
    std::printf("cut_pushed = %u\n", spectacle.cut_pushed);
    std::printf("cut_released = %u\n", spectacle.cut_released);
    std::printf("cut_cuts = %u\n", spectacle.cut_cuts);
    std::printf("cut_anticipated_cuts = %u\n", spectacle.cut_anticipated_cuts);
    std::printf("cut_unresolved_rigs = %u\n", spectacle.cut_unresolved_rigs);
    // THE TWO ZEROES TASK 5.2 IS ABOUT. A sequence that wrote a camera transform would have to
    // write it as a property or force a pose, and neither of these would still be zero.
    std::printf("cut_camera_property_writes = %u\n", spectacle.cut_camera_property_writes);
    std::printf("cut_pose_overrides = %u\n", spectacle.cut_pose_overrides);
    std::printf("cut_blend_wide = %.4f\n", static_cast<double>(spectacle.cut_blend_wide));
    std::printf("cut_blend_tight = %.4f\n", static_cast<double>(spectacle.cut_blend_tight));
    std::printf("cut_blend_fov = %.4f\n", static_cast<double>(spectacle.cut_blend_fov));
    std::printf("cut_wide_fov = %.4f\n", static_cast<double>(spectacle.cut_wide_fov));
    std::printf("cut_tight_fov = %.4f\n", static_cast<double>(spectacle.cut_tight_fov));
}

void print_capture(const char* prefix, const cy::sample::slice::CaptureReport& capture) {
    std::printf("%s_device = %d\n", prefix, capture.device ? 1 : 0);
    std::printf("%s_captured = %d\n", prefix, capture.captured ? 1 : 0);
    std::printf("%s_passes = %u\n", prefix, capture.passes);
    std::printf("%s_prepass_draws = %u\n", prefix, capture.prepass_draws);
    std::printf("%s_opaque_draws = %u\n", prefix, capture.opaque_draws);
    std::printf("%s_transparent_draws = %u\n", prefix, capture.transparent_draws);
    std::printf("%s_skipped_draws = %u\n", prefix, capture.skipped_draws);
    std::printf("%s_extensions_run = %u\n", prefix, capture.extensions_run);
    std::printf("%s_particles_drawn = %u\n", prefix, capture.particles_drawn);
    std::printf("%s_particles_dropped = %u\n", prefix, capture.particles_dropped);
    std::printf("%s_uploaded_bytes = %llu\n", prefix,
                static_cast<unsigned long long>(capture.uploaded_bytes));
    std::printf("%s_validation_errors = %u\n", prefix, capture.validation_errors);
    std::printf("%s_lit_texels = %u\n", prefix, capture.lit_texels);
    std::printf("%s_differing_texels = %u\n", prefix, capture.differing_texels);
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
    std::printf("particles_us_median = %.4f\n", report.particles_us_median);
    std::printf("cinematic_us_median = %.4f\n", report.cinematic_us_median);
    std::printf("spectacle_us_median = %.4f\n", report.spectacle_us_median);
    std::printf("spectacle_us_worst = %.4f\n", report.spectacle_us_worst);

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
    std::printf("budget_spectacle_us = %.1f\n", cy::sample::slice::Budgets::kSpectacleUs);

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
        } else if (std::strcmp(argument, "--no-spectacle") == 0) {
            options.spectacle = false;
        } else if (std::strcmp(argument, "--cut-tick") == 0 && value(parsed)) {
            options.cut_start_tick = static_cast<cy::u32>(parsed);
        } else if (std::strcmp(argument, "--capture") == 0 && (index + 1) < argc) {
            options.capture_prefix = argv[++index];
        } else if (std::strcmp(argument, "--capture-tick") == 0 && value(parsed)) {
            options.capture_tick = static_cast<cy::u32>(parsed);
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
    std::printf("spectacle = %d\n", options.spectacle ? 1 : 0);
    print_spectacle(slice.spectacle_report());
    print_capture("capture", slice.capture_report());
    print_capture("control", slice.control_capture_report());
    if (options.capture_prefix != nullptr && !slice.capture_report().device) {
        // A REPORTED GAP, and the program says so and fails. A capture asked for and not taken must
        // not read as a capture taken.
        std::fprintf(stderr, "no frame was captured: %s\n", slice.capture_unavailable_reason());
        return kFailed;
    }

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
