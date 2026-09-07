// samples/07-fidelity — M7's closing artefact. Section 11.
//
// A film-detail interior and exterior, millions of source triangles, dynamic lighting, indirect
// illumination and reflections, holding a frame budget while the arbiter reallocates under a
// scripted load spike. `fidelity.py` is the driver that puts the acts in order and checks what this
// program printed; `just run-fidelity` is the recipe; this program is the half that has to be real.
//
//   --act detail     generate and cook the set, and report what the cook found
//   --act frame      the above, then put it on the device and measure the frames
//   --act light      the above, then converge the illumination system and resolve indirect and
//                    specular over the scene's surfaces
//   --act spike      the arbiter under the scripted load spike, and the starvation case
//   --act all        every act in that order, which is what the driver runs
//
// THE KEYS ARE FLAT AND THEREFORE UNIQUE ACROSS THE ACTS. The driver parses the whole run into one
// table, so `median_frame_ms` (the device's) and `spike_median_ms` (the model's) cannot both be
// called the same thing — they were, briefly, and the driver reported a median above the maximum
// with both halves individually correct.
//
// EVERY LINE IT PRINTS IS `key=value`, one per line, and every figure is either a count that is a
// function of the content or a duration named as a measurement. The driver reads the printed form,
// so a figure that varied with an allocation address would turn the milestone gate into a flake —
// the rule samples/02-headless-sim states and every sample since has kept. The only figures that
// vary run to run are the ones whose key ends in `_ms`, and every one of those is reported as a
// median over samples rather than as a single draw.

#include <cy/core/memory/system_allocator.h>

#include "frame.h"
#include "scene.h"
#include "spike.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

using namespace cy;
using namespace cy::sample::fidelity;

struct Options {
    const char* act = "all";
    /// Scales the cooked resolution of every shell. The cook's cost is superlinear in it, so it is
    /// the one dial the smoke entry turns down.
    f32 detail = 1.0F;
    u32 frames = 48;
    u32 width = 1280;
    u32 height = 720;
    u32 sweep_steps = 9;
    bool help = false;
};

void print_usage() {
    std::fputs(
        "samples/07-fidelity — film detail at a budget an arbiter holds.\n"
        "\n"
        "  --act <detail|frame|light|spike|all>  what to do          (default all)\n"
        "  --detail <f>                          cooked resolution scale  (default 1.0)\n"
        "  --frames <n>                          device frames to time    (default 48)\n"
        "  --width <n> --height <n>              the view                 (default 1280x720)\n"
        "  --sweep <n>                           spike magnitudes swept   (default 9)\n"
        "  --help                                this text\n",
        stderr);
}

// `--frames zz` IS A MISTAKE WORTH NAMING. This parsed with `atoi` and `atof`, which report no
// error: `--detail x` read as 0.0, every shell cooked at the six-quad floor, and the run reported
// a clean set of the wrong content. Both helpers below refuse trailing rubbish and say which flag
// carried it, which is the whole difference between the two families of function.
[[nodiscard]] bool parse_f32(std::string_view flag, const char* text, f32& out) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !(value > 0.0)) {
        std::fprintf(stderr, "07-fidelity: %.*s wants a positive number, not '%s'\n",
                     static_cast<int>(flag.size()), flag.data(), text);
        return false;
    }
    out = static_cast<f32>(value);
    return true;
}

[[nodiscard]] bool parse_u32(std::string_view flag, const char* text, u32& out) {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0) {
        std::fprintf(stderr, "07-fidelity: %.*s wants a positive whole number, not '%s'\n",
                     static_cast<int>(flag.size()), flag.data(), text);
        return false;
    }
    out = static_cast<u32>(value);
    return true;
}

[[nodiscard]] bool parse_options(int count, char** arguments, Options& options) {
    for (int index = 1; index < count; ++index) {
        const std::string_view argument{arguments[index]};
        const bool has_value = index + 1 < count;
        if (argument == "--help") {
            print_usage();
            options.help = true;
            return true;
        }
        bool parsed = true;
        if (argument == "--act" && has_value) {
            options.act = arguments[++index];
        } else if (argument == "--detail" && has_value) {
            parsed = parse_f32(argument, arguments[++index], options.detail);
        } else if (argument == "--frames" && has_value) {
            parsed = parse_u32(argument, arguments[++index], options.frames);
        } else if (argument == "--width" && has_value) {
            parsed = parse_u32(argument, arguments[++index], options.width);
        } else if (argument == "--height" && has_value) {
            parsed = parse_u32(argument, arguments[++index], options.height);
        } else if (argument == "--sweep" && has_value) {
            parsed = parse_u32(argument, arguments[++index], options.sweep_steps);
        } else {
            std::fprintf(stderr, "07-fidelity: unknown argument '%.*s'\n",
                         static_cast<int>(argument.size()), argument.data());
            print_usage();
            return false;
        }
        if (!parsed) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool wants(const Options& options, const char* act) noexcept {
    return std::strcmp(options.act, "all") == 0 || std::strcmp(options.act, act) == 0;
}

void report_scene(const Scene& scene) {
    std::printf("act=detail\n");
    std::printf("assets=%zu\n", scene.assets.size());
    std::printf("instances=%zu\n", scene.instances.size());
    std::printf("source_triangles=%llu\n", static_cast<unsigned long long>(scene.source_triangles));
    std::printf("distinct_triangles=%u\n", scene.distinct_triangles);
    std::printf("clusters=%u\n", scene.clusters);
    std::printf("pages=%u\n", scene.pages);
    std::printf("cooked_bytes=%llu\n", static_cast<unsigned long long>(scene.cooked_bytes));
    std::printf("resident_bytes=%llu\n", static_cast<unsigned long long>(scene.resident_bytes));
    std::printf("closed_assets=%u\n", scene.closed_assets);
    std::printf("watertight_assets=%u\n", scene.watertight_assets);
    std::printf("cook_ms=%.1f\n", static_cast<double>(scene.cook_ms));
    for (const CookedAsset& asset : scene.assets) {
        std::printf(
            "asset=%s triangles=%u clusters=%u levels=%u pages=%u resident_bytes=%u "
            "closed=%u watertight=%u boundary_mismatches=%u monotonicity=%u open_cuts=%u "
            "thresholds=%u\n",
            shape_name(asset.shape), asset.source_triangles, asset.clusters, asset.levels,
            asset.pages, asset.resident_bytes, asset.watertight.closed_source ? 1U : 0U,
            asset.watertight.watertight() ? 1U : 0U, asset.watertight.boundary_mismatches,
            asset.watertight.monotonicity_violations, asset.watertight.open_cuts,
            asset.watertight.thresholds_tested);
    }
}

void report_frame(const FrameReport& frame) {
    std::printf("act=frame\n");
    std::printf("device=%u\n", frame.device ? 1U : 0U);
    std::printf("backend=%s\n", frame.backend);
    if (!frame.device) {
        std::printf("device_reason=%s\n", frame.reason);
        return;
    }
    std::printf("frames=%u\n", frame.frames);
    std::printf("median_frame_ms=%.3f\n", static_cast<double>(frame.median_ms));
    std::printf("p90_frame_ms=%.3f\n", static_cast<double>(frame.p90_ms));
    std::printf("worst_frame_ms=%.3f\n", static_cast<double>(frame.worst_ms));
    std::printf("covered_pixels=%llu\n", static_cast<unsigned long long>(frame.covered_pixels));
    std::printf("visible_clusters=%llu\n", static_cast<unsigned long long>(frame.visible_clusters));
    std::printf("interior_covered=%u\n", frame.interior_covered);
    std::printf("exterior_covered=%u\n", frame.exterior_covered);
    std::printf("materials_seen=%u\n", frame.materials_seen);
    std::printf("traversal_overflowed=%u\n", frame.overflowed ? 1U : 0U);
    std::printf("levels_exhausted=%u\n", frame.levels_exhausted ? 1U : 0U);
    std::printf("validation_errors=%u\n", frame.validation_errors);
}

void report_light(const LightReport& light) {
    std::printf("act=light\n");
    std::printf("surfels=%u\n", light.surfels);
    std::printf("lights=%u\n", light.lights);
    std::printf("shaded=%u\n", light.shaded);
    std::printf("with_indirect=%u\n", light.with_indirect);
    std::printf("with_reflection=%u\n", light.with_reflection);
    std::printf("mean_indirect=%.6f\n", static_cast<double>(light.mean_indirect));
    std::printf("mean_reflection=%.6f\n", static_cast<double>(light.mean_reflection));
    std::printf("indirect_colour_spread=%.6f\n", static_cast<double>(light.indirect_colour_spread));
    std::printf("world_tier=%s\n", light.tier);
    std::printf("software_rays=%u\n", light.software_rays);
    std::printf("hardware_rays=%u\n", light.hardware_rays);
    std::printf("frames_to_converge=%u\n", light.frames_to_converge);
    std::printf("convergence=%.4f\n", static_cast<double>(light.convergence));
    std::printf("converged=%u\n", light.converged ? 1U : 0U);
    std::printf("gi_ms=%.1f\n", static_cast<double>(light.gi_ms));
}

void report_spike(const SpikeReport& spike, const StarvationReport& starved) {
    std::printf("act=spike\n");
    std::printf("subsystems=%u\n", spike.subsystems);
    std::printf("budget_ms=%.2f\n", static_cast<double>(spike.budget_ms));
    std::printf("allocatable_ms=%.2f\n", static_cast<double>(spike.allocatable_ms));
    std::printf("nominal_ms=%.2f\n", static_cast<double>(spike.nominal_ms));
    std::printf("geometry_measured=%u\n", spike.geometry_measured ? 1U : 0U);
    std::printf("geometry_ms=%.3f\n", static_cast<double>(spike.geometry_ms));
    std::printf("geometry_device_ms=%.3f\n", static_cast<double>(spike.device_ms));
    std::printf("magnitudes=%u\n", spike.magnitudes);
    std::printf("spike_frames=%u\n", spike.frames);
    std::printf("spike_median_ms=%.3f\n", static_cast<double>(spike.median_frame_ms));
    std::printf("magnitudes_oscillating=%u\n", spike.magnitudes_oscillating);
    std::printf("magnitudes_settled_over_budget=%u\n", spike.magnitudes_over_budget);
    std::printf("settled_frames_over_budget=%u\n", spike.settled_frames_over_budget);
    std::printf("magnitudes_restored=%u\n", spike.magnitudes_restored);
    for (const SpikeRun& run : spike.runs) {
        std::printf(
            "magnitude=%.2f frames_over_budget=%u late_over_budget=%u median_ms=%.3f "
            "worst_ms=%.3f settled_filtered_ms=%.3f settled_setpoint_ms=%.3f "
            "settled_changes=%u adjustments=%u resolution_steps=%u "
            "deepest_positions=%u restored=%u\n",
            static_cast<double>(run.magnitude), run.frames_over_budget, run.late_frames_over_budget,
            static_cast<double>(run.median_ms), static_cast<double>(run.worst_ms),
            static_cast<double>(run.settled_filtered_ms),
            static_cast<double>(run.settled_setpoint_ms), run.settled_changes, run.adjustments,
            run.resolution_steps, run.deepest_positions, run.restored ? 1U : 0U);
    }
    std::printf("starved_subsystems=%u\n", starved.subsystems);
    std::printf("starved_at_minimum=%u\n", starved.at_minimum);
    std::printf("starved_below_reserved=%u\n", starved.below_reserved_minimum);
    std::printf("starved_changes_at_the_bottom=%u\n", starved.changes_at_the_bottom);
    std::printf("starved_resolution_scale=%.3f\n", static_cast<double>(starved.resolution_scale));
}

[[nodiscard]] SceneOptions scene_options(const Options& options) noexcept {
    SceneOptions scene;
    for (u32& resolution : scene.resolution) {
        const auto scaled = static_cast<u32>(static_cast<f32>(resolution) * options.detail);
        resolution = scaled > 6U ? scaled : 6U;
    }
    return scene;
}

[[nodiscard]] int fail(const Error& error) {
    std::printf("error=%s\n", error.message);
    return 1;
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

    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    std::printf("artefact=07-fidelity\n");
    std::printf("detail=%.3f\n", static_cast<double>(options.detail));

    Scene scene(allocator);
    if (Status built = build_scene(scene_options(options), scene); !built) {
        return fail(built.error());
    }
    if (wants(options, "detail")) {
        report_scene(scene);
    }

    FrameOptions frame_options;
    frame_options.width = options.width;
    frame_options.height = options.height;
    frame_options.frames = options.frames;

    FrameReport frame(allocator);
    if (wants(options, "frame")) {
        if (Status rendered = render_frames(scene, frame_options, frame); !rendered) {
            return fail(rendered.error());
        }
        report_frame(frame);
    }

    if (wants(options, "light")) {
        LightReport light;
        if (Status lit = light_shot(scene, frame_options, light); !lit) {
            return fail(lit.error());
        }
        report_light(light);
    }

    if (wants(options, "spike")) {
        SpikeOptions spike_options;
        spike_options.measured_geometry_ms = frame.device ? frame.median_ms : 0.0F;
        spike_options.sweep_steps = options.sweep_steps;
        SpikeReport spike(allocator);
        if (Status ran = run_spike(spike_options, spike); !ran) {
            return fail(ran.error());
        }
        StarvationReport starved;
        if (Status ran = run_starvation(spike_options, starved); !ran) {
            return fail(ran.error());
        }
        report_spike(spike, starved);
    }

    std::printf("done=1\n");
    return 0;
}
