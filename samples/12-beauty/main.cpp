// `cy_sample_beauty` — M11.c's closing artefact. Section 7.
//
// ================================================================================================
// WHAT IT DOES
// ================================================================================================
//
//   cy_sample_beauty --shot content/beauty/shot.cyshot
//                    --materials <dir>              where `cy_material author` wrote
//                    .spv/.cymatinfo
//                    --still docs/design/images/m11c-beauty-shot.png
//                    --no-post-still <path>         the same frame with the chain compiled out
//                    --manifest <path>              the provenance a machine can check
//                    --frames <dir> --frames-count <n>   a turntable, for the video
//                    [--orbit-degrees <arc>]        a swing of that arc and back, not a whole turn
//                    [--width 1920] [--height 1080] [--supersample 2]
//                    [--bloom]                      the shot's bloom grade in the post chain
//                    [--fog on|off]                 volumetric fog from the shot's `fog` lines
//                    [--look <file.cygrade>]        a committed look, graded after the tone curve
//                    [--depth-of-field <target>]    focused on a target the shot names
//                    [--decals on|off]              the shot's `decal` lines, applied before
//                    lighting
//                    [--motion-blur <degrees>]      motion blur at that shutter angle
//                    [--orbit-still <path> --orbit-frame <n>]  turntable frame n, drawn after
//                                                   frame n - 1 so it has the orbit's motion
//                    [--gi on|off] [--gi-tier hardware|software] [--gi-divisor <n>]
//                    [--gi-gather probes|per-pixel] [--gi-denoiser probe-space|shared]
//                    [--gi-probe-rays <n>] [--gi-ray-density <f>]  the screen probes' rays
//                                                   a probe, and the share of probes traced a frame
//                    [--gi-timing-frames <n>]       frames after the still the GI is timed over
//                    [--gi-live-frames <n>]         then frames with the caches live, the GI's
//                                                   sun turning and a block moving through it
//                    [--gi-caches device|host]      where the caches' arithmetic runs
//                    [--gi-reference <pixels>]      the path tracer at that many pixels
//                    [--gi-reference-step <n> --gi-reference-still <path> --gi-error-map <path>]
//                                                   the path tracer's picture of the frame
//                    [--upscale native|quality|balanced|performance]  the scene below the output,
//                                                   reconstructed by the temporal upscaler
//                    [--upscale-frames <n>] [--sharpness <0..1>]  frames before the still, and
//                                                   the sharpening of the reconstruction
//                    [--timing-frames <n>]          frames after the still the whole frame's
//                                                   device time is measured over
//
// `just capture-beauty-shot` is the recipe that runs it, and everything it needs that is not
// committed — the compiled material programs — is produced by that recipe from files that are.
//
// ================================================================================================
// WITHOUT A GRAPHICS DEVICE IT STILL LOADS EVERYTHING AND SAYS WHAT IT DID NOT FIND
// ================================================================================================
//
// The same arrangement samples/07-fidelity, 08-vertical-slice, 09b and 10-world use, and the reason
// `just/run.just` gives for all of them: "on a machine without one the tool says so and writes
// nothing, which is why this is a recipe a person runs and not a test." What is gated automatically
// is everything underneath — `unit.import`'s codecs and encoders, `unit.graph_material`'s three
// front ends, `unit.material_compiler`, and the assembly's own suites.

#include <cy/core/memory/system_allocator.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

#include "shot.h"

namespace {

using namespace cy;
using namespace cy::sample::beauty;

[[nodiscard]] std::string option(int argc, char** argv, const char* name, const char* fallback) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], name) == 0) {
            return argv[index + 1];
        }
    }
    return fallback;
}

[[nodiscard]] u32 option_number(int argc, char** argv, const char* name, u32 fallback) {
    const std::string text = option(argc, argv, name, "");
    return text.empty() ? fallback : static_cast<u32>(std::strtoul(text.c_str(), nullptr, 10));
}

/// Read what `cy_material author` reported about one compiled material, and CHECK it.
///
/// The parameter block this program uploads is laid out from the module's own declaration order, so
/// a material whose parameters arrived in a different order would be shaded with roughness read out
/// of the metalness slot — and the picture would look plausible. That is the class of wrongness the
/// sidecar exists to remove, and the check is four lines.
/// The four component defaults of one `param` line, read with `strtod` so a malformed number is a
/// refusal rather than a silent zero. `sscanf`'s `%lf` reports neither, which is why it is not
/// used.
[[nodiscard]] bool read_four(const char* cursor, double (&values)[4]) {
    for (double& value : values) {
        char* end = nullptr;
        value = std::strtod(cursor, &end);
        if (end == cursor) {
            return false;
        }
        cursor = end;
    }
    return true;
}

/// One line of the sidecar, folded into `material`. Each form is recognised by its keyword and its
/// numbers are converted by a function that reports failure.
void read_material_line(const char* line, ShotMaterial& material) {
    char first[128] = {};
    char second[128] = {};
    int consumed = 0;
    if (std::sscanf(line, "entry %127s", first) == 1) {
        material.entry_point = first;
        return;
    }
    if (std::strncmp(line, "cook_key 0x", 11) == 0) {
        char* end = nullptr;
        const unsigned long long key = std::strtoull(line + 11, &end, 16);
        if (end != line + 11) {
            material.cook_key = static_cast<u64>(key);
        }
        return;
    }
    if (std::sscanf(line, "param %127s %127s %n", first, second, &consumed) == 2 && consumed != 0) {
        double values[4] = {};
        if (read_four(line + consumed, values)) {
            material.parameters.emplace_back(first);
            material.parameter_defaults.push_back(
                Vec4{static_cast<f32>(values[0]), static_cast<f32>(values[1]),
                     static_cast<f32>(values[2]), static_cast<f32>(values[3])});
        }
        return;
    }
    if (std::sscanf(line, "texture %127s", first) == 1) {
        material.textures.emplace_back(first);
    }
}

[[nodiscard]] bool read_material_info(const std::string& path, ShotMaterial& material) {
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        std::fprintf(stderr, "cy_sample_beauty: cannot read %s\n", path.c_str());
        return false;
    }
    char line[512];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        read_material_line(line, material);
    }
    (void)std::fclose(file);

    static const std::vector<std::string> kParameters = {"base_color", "roughness", "metallic"};
    static const std::vector<std::string> kTextures = {"albedo_map", "data_map"};
    if (material.parameters != kParameters || material.textures != kTextures) {
        std::fprintf(stderr,
                     "cy_sample_beauty: %s declares a parameter signature this frame does not "
                     "upload. The frame's `CyMaterialParams` is (base_color, roughness, metallic) "
                     "with textures (albedo_map, data_map), in that order.\n",
                     path.c_str());
        return false;
    }
    return true;
}

[[nodiscard]] bool read_spirv(const std::string& path, std::vector<u32>& out) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "cy_sample_beauty: cannot read %s\n", path.c_str());
        return false;
    }
    (void)std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    (void)std::fseek(file, 0, SEEK_SET);
    if (size <= 0 || (size % 4) != 0) {
        (void)std::fclose(file);
        std::fprintf(stderr, "cy_sample_beauty: %s is not a SPIR-V module\n", path.c_str());
        return false;
    }
    out.resize(static_cast<usize>(size) / 4);
    const usize read = std::fread(out.data(), 4, out.size(), file);
    (void)std::fclose(file);
    return read == out.size();
}

/// THE GI'S COST: the fastest of `frames` more frames of the same view, every stage of the GI on
/// the device, printed as `gi fastest`. After the still and the reference, so neither moves.
///
/// WHY THE HOST'S CACHE IS HELD FOR THEM: the driver picks the device's power state from how busy
/// it is, and this sample's frames are a few milliseconds of GPU work behind 20 to 30 ms of the
/// host's surface-cache update — the sample's stand-in for work a game does elsewhere — so the RTX
/// 5060 sat, run to run, at half its memory clock (P3, 7 GHz) or at its full one (P0, 14 GHz), and
/// every bandwidth-bound stage of the GI took up to twice as long in the first: the sunlit room
/// timed 4.81 or 3.49 ms, and 240 frames with the update did not settle it. With the cache held —
/// the view is still and converged, so an update changes nothing — the frames are issued back to
/// back as a game's are, and every run measured timed at full clocks. The fastest frame is then
/// the one nothing else on the device interrupted.
[[nodiscard]] Status print_gi_timing(Stage& stage, const Shot& shot, u32 frames) noexcept {
    ShotReport fastest;
    stage.set_gi_hold_cache(true);
    for (u32 index = 0; index < frames; ++index) {
        ShotReport frame;
        if (Status drawn = stage.render(shot, nullptr, nullptr, frame); !drawn) {
            return drawn;
        }
        if (frame.gi_timed &&
            (fastest.gi_total_ms <= 0.0 || frame.gi_total_ms < fastest.gi_total_ms)) {
            fastest = frame;
        }
    }
    stage.set_gi_hold_cache(false);
    if (fastest.gi_timed) {
        std::printf(
            "gi fastest    %.3f ms over %u frames of the same view: probe placement %.3f, "
            "probe tracing %.3f, probe filter %.3f, integration %.3f, denoiser %.3f ms\n",
            fastest.gi_total_ms, frames, fastest.gi_probe_place_ms, fastest.gi_probe_trace_ms,
            fastest.gi_probe_filter_ms, fastest.gi_integrate_ms,
            fastest.gi_accumulate_ms + fastest.gi_variance_ms + fastest.gi_filter_ms +
                fastest.gi_publish_ms);
    }
    return ok();
}

/// The `fraction` quantile of `values` (sorted in place): the nearest rank.
[[nodiscard]] f64 quantile(std::vector<f64>& values, f64 fraction) noexcept {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const auto rank =
        static_cast<usize>(std::llround(fraction * static_cast<f64>(values.size() - 1)));
    return values[std::min(rank, values.size() - 1)];
}

/// THE GI WITH ITS CACHES LIVE, AS A GAME RUNS IT: `frames` frames of the same view in which the
/// GI's sun turns and a block moves through the GI (`BeautyGi::set_live_motion`), every frame's
/// update scheduling, shading and gathering what changed. Printed as `gi live`: the host's update
/// — the processor time a game pays for the GI every frame — at its median, 95th percentile and
/// worst, and the device's resolve, card shading and probe gather at their median, with the
/// fastest frame's whole device cost beside them. After the held timing, so the block that moves
/// here is in no other figure.
[[nodiscard]] Status print_gi_live_timing(Stage& stage, const Shot& shot, u32 frames) noexcept {
    std::vector<f64> update;
    std::vector<f64> resolve;
    std::vector<f64> cards;
    std::vector<f64> gather;
    std::vector<f64> total;
    stage.set_gi_live_motion(true);
    for (u32 index = 0; index < frames; ++index) {
        ShotReport frame;
        if (Status drawn = stage.render(shot, nullptr, nullptr, frame); !drawn) {
            stage.set_gi_live_motion(false);
            return drawn;
        }
        // The first frame registers the block (its field and cards are built then): a level's
        // load, not a frame of it.
        if (index == 0) {
            continue;
        }
        update.push_back(frame.gi_update_ms);
        if (!frame.gi_timed) {
            continue;
        }
        const f64 shaded = frame.gi_cards_timed ? frame.gi_cards_ms : 0.0;
        const f64 gathered = frame.gi_gather_timed ? frame.gi_gather_ms : 0.0;
        resolve.push_back(frame.gi_total_ms);
        cards.push_back(shaded);
        gather.push_back(gathered);
        total.push_back(frame.gi_total_ms + shaded + gathered);
    }
    stage.set_gi_live_motion(false);
    const f64 fastest = total.empty() ? 0.0 : *std::ranges::min_element(total);
    std::printf(
        "gi live       %u frames, caches live (the sun turning, a block moving): host update "
        "median %.3f, p95 %.3f, worst %.3f ms; device median resolve %.3f, cards %.3f, gather "
        "%.3f, whole %.3f ms (p95 %.3f, fastest %.3f)\n",
        frames, quantile(update, 0.5), quantile(update, 0.95), quantile(update, 1.0),
        quantile(resolve, 0.5), quantile(cards, 0.5), quantile(gather, 0.5), quantile(total, 0.5),
        quantile(total, 0.95), fastest);
    return ok();
}

/// Where the turntable's camera is at frame `index` of `count`: an orbit about the shot's target,
/// at the shot's own height and radius, starting from the shot's own camera. Frame `index` of
/// `count` round the shot's target: a whole turn, or — for an `arc_degrees` below 360, an interior
/// a whole turn would leave through its walls — a swing of that arc either side and back, which
/// ends where it began.
[[nodiscard]] Vec3 turntable_eye(const Shot& shot, u32 index, u32 count,
                                 f32 arc_degrees = 360.0F) noexcept {
    const Vec3 pivot = shot.camera_target;
    const Vec3 offset =
        Vec3{shot.camera_position.x - pivot.x, 0.0F, shot.camera_position.z - pivot.z};
    const f32 radius = std::sqrt((offset.x * offset.x) + (offset.z * offset.z));
    const f32 start = std::atan2(offset.z, offset.x);
    const f32 phase =
        (2.0F * std::numbers::pi_v<f32> * static_cast<f32>(index)) / static_cast<f32>(count);
    const f32 turn =
        arc_degrees >= 360.0F
            ? start + phase
            : start + (arc_degrees * std::numbers::pi_v<f32> / 180.0F * 0.5F * std::sin(phase));
    Vec3 eye = shot.camera_position;
    eye.x = pivot.x + (std::cos(turn) * radius);
    eye.z = pivot.z + (std::sin(turn) * radius);
    return eye;
}

[[nodiscard]] std::string stem_of(const std::string& path) {
    const usize slash = path.find_last_of('/');
    const usize dot = path.find_last_of('.');
    const usize begin = slash == std::string::npos ? 0 : slash + 1;
    return path.substr(begin, dot == std::string::npos ? std::string::npos : dot - begin);
}

/// THE WHOLE FRAME'S DEVICE TIME over `frames` frames of the still view, after it: the frames are
/// drawn and written nowhere, each timed by its own two timestamps, and the minimum, median and
/// mean are printed. The median is the number to quote; the minimum shows the floor a quiet machine
/// reaches.
[[nodiscard]] Status print_frame_timing(Stage& stage, const Shot& shot, u32 frames) noexcept {
    std::vector<f64> times;
    times.reserve(frames);
    for (u32 index = 0; index < frames; ++index) {
        ShotReport timed;
        if (Status drawn = stage.render(shot, nullptr, nullptr, timed); !drawn) {
            return drawn;
        }
        if (timed.gpu_timed) {
            times.push_back(timed.gpu_ms);
        }
    }
    if (times.empty()) {
        std::printf("frame timing  not measured: the device answered no timestamps\n");
        return ok();
    }
    std::ranges::sort(times);
    f64 sum = 0.0;
    for (const f64 time : times) {
        sum += time;
    }
    std::printf("frame timing  %llu frames: min %.3f, median %.3f, mean %.3f ms\n",
                static_cast<unsigned long long>(times.size()), times.front(),
                times[times.size() / 2U], sum / static_cast<f64>(times.size()));
    return ok();
}

void print_report(const Shot& shot, const ShotReport& report) {
    std::printf("shot          %s\n", shot.name.c_str());
    std::printf("content       %u instances, %u triangles, %u materials, %u textures\n",
                report.instances, report.triangles, report.materials, report.textures);
    std::printf("textures      %llu bytes of PNG cooked to %llu bytes of blocks\n",
                static_cast<unsigned long long>(report.texture_source_bytes),
                static_cast<unsigned long long>(report.texture_cooked_bytes));
    std::printf("sky           sun %.0f %.0f %.0f lux, irradiance %.0f %.0f %.0f, %.0f ms\n",
                static_cast<double>(report.sun_illuminance.x),
                static_cast<double>(report.sun_illuminance.y),
                static_cast<double>(report.sun_illuminance.z),
                static_cast<double>(report.sky_irradiance.x),
                static_cast<double>(report.sky_irradiance.y),
                static_cast<double>(report.sky_irradiance.z), report.sky_ms);
    std::printf("frame         %u passes declared, %u post stages, %ux supersampled\n",
                report.frame_passes, report.post_stages, report.supersample);
    std::printf("air           %u mote(s) in %u draw(s), %u dropped\n", report.particles,
                report.particle_draws, report.particles_dropped);
    std::printf("trails        %u vertices in %u strip(s), %u segment(s), %u draw(s), %u dropped\n",
                report.trail_vertices, report.trail_strips, report.trail_segments,
                report.trail_draws, report.trail_dropped);
    std::printf("cost          build %.1f ms, submit %.2f ms\n", report.build_ms, report.submit_ms);
    std::printf("render        %ux%u%s\n", report.render_width, report.render_height,
                report.gpu_timed ? "" : ", device time not measured");
    if (report.gpu_timed) {
        std::printf("gpu frame     %.3f ms\n", report.gpu_ms);
    }
    if (report.gi_cards != 0U) {
        std::printf("gi            %u cards, %u placements, traced through the %s\n",
                    report.gi_cards, report.gi_placements, report.gi_tier);
        std::printf(
            "gi cost       host cache update %.2f ms; converged mode took %u frames, "
            "%.0f ms, before the first\n",
            report.gi_update_ms, report.gi_converged_frames, report.gi_converge_ms);
        if (report.gi_timed) {
            std::printf(
                "gi gpu        unpack %.3f, reproject %.3f, resolve %.3f, accumulate %.3f, "
                "variance %.3f, filter %.3f, publish %.3f = %.3f ms\n",
                report.gi_unpack_ms, report.gi_reproject_ms, report.gi_resolve_ms,
                report.gi_accumulate_ms, report.gi_variance_ms, report.gi_filter_ms,
                report.gi_publish_ms, report.gi_total_ms);
            std::printf(
                "gi resolve    probe placement %.3f, probe tracing %.3f, probe filter %.3f, "
                "integration %.3f ms\n",
                report.gi_probe_place_ms, report.gi_probe_trace_ms, report.gi_probe_filter_ms,
                report.gi_integrate_ms);
        }
    }
    std::printf("validation    %u error(s)\n", report.validation_errors);
}

}  // namespace

int main(int argc, char** argv) {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);

    const std::string shot_path = option(argc, argv, "--shot", "content/beauty/shot.cyshot");
    const std::string materials = option(argc, argv, "--materials", "");
    const std::string still = option(argc, argv, "--still", "");
    const std::string no_post = option(argc, argv, "--no-post-still", "");
    const std::string manifest = option(argc, argv, "--manifest", "");
    const std::string frames = option(argc, argv, "--frames", "");
    const u32 width = option_number(argc, argv, "--width", 1920);
    const u32 height = option_number(argc, argv, "--height", 1080);
    const u32 supersample = option_number(argc, argv, "--supersample", 2);
    // THE AMBIENT OCCLUSION SETTING. Off by default, which is the published M11.c frame; `on` is
    // what `just capture-ambient-occlusion` photographs beside it.
    const std::string occlusion = option(argc, argv, "--ambient-occlusion", "off");
    if (occlusion != "on" && occlusion != "off") {
        std::fprintf(stderr, "cy_sample_beauty: --ambient-occlusion is `on` or `off`\n");
        return 1;
    }
    // SOFT AND CONTACT SHADOWS. Off by default, which is the published M11.c frame; `on` is what
    // `just capture-soft-shadows` photographs beside it.
    const std::string soft_shadows = option(argc, argv, "--soft-shadows", "off");
    if (soft_shadows != "on" && soft_shadows != "off") {
        std::fprintf(stderr, "cy_sample_beauty: --soft-shadows is `on` or `off`\n");
        return 1;
    }
    // DECALS. Off by default, which is the published M11.c frame; `on` is what `just
    // capture-beauty-decals` photographs beside it.
    const std::string decals = option(argc, argv, "--decals", "off");
    if (decals != "on" && decals != "off") {
        std::fprintf(stderr, "cy_sample_beauty: --decals is `on` or `off`\n");
        return 1;
    }
    // VOLUMETRIC FOG. Off by default, which is the published M11.c frame; `on` is what
    // `just capture-volumetric-fog` photographs beside it, with the shot's own `fog` lines.
    const std::string fog = option(argc, argv, "--fog", "off");
    if (fog != "on" && fog != "off") {
        std::fprintf(stderr, "cy_sample_beauty: --fog is `on` or `off`\n");
        return 1;
    }
    // A CONTROL, not a quality setting: `--albedo-levels 1` photographs the shot with level 0 of
    // every albedo map's cooked chain and nothing beneath it. See `Stage::limit_albedo_levels`.
    const u32 albedo_levels = option_number(argc, argv, "--albedo-levels", 0);
    // Bloom is off unless asked for: the published M11.c still is the frame without it, and
    // `just capture-beauty-bloom` photographs the same frame both ways.
    bool bloom = false;
    for (int index = 1; index < argc; ++index) {
        bloom = bloom || std::strcmp(argv[index], "--bloom") == 0;
    }
    // No look unless one is named: the published still is ungraded, and `just
    // capture-beauty-grading` photographs it beside the warm and the cool look.
    const std::string look = option(argc, argv, "--look", "");
    // No depth of field unless a focus target is named: the published still has none, and `just
    // capture-beauty-depth-of-field` photographs it focused on the sphere and on a far column.
    const std::string focus = option(argc, argv, "--depth-of-field", "");
    // MOTION BLUR, off unless a shutter angle is named: the published still is the frame without
    // it, and `just capture-beauty-motion-blur` photographs one turntable frame both ways.
    const std::string motion_blur = option(argc, argv, "--motion-blur", "");
    const std::string orbit_still = option(argc, argv, "--orbit-still", "");
    // GLOBAL ILLUMINATION. Off by default, which is the published M11.c frame; `on` is what `just
    // capture-beauty-gi` photographs beside it. The still is drawn after `--gi-frames` frames of
    // the same view, because the GI's caches and its denoiser's history converge over frames as a
    // game's do. `--gi-reference <pixels>` path traces that many pixels of the last frame and
    // reports how far the GI and the flat ambient it replaces are from them.
    const std::string gi = option(argc, argv, "--gi", "off");
    if (gi != "on" && gi != "off") {
        std::fprintf(stderr, "cy_sample_beauty: --gi is `on` or `off`\n");
        return 1;
    }
    const std::string gi_tier = option(argc, argv, "--gi-tier", "hardware");
    if (gi_tier != "hardware" && gi_tier != "software") {
        std::fprintf(stderr, "cy_sample_beauty: --gi-tier is `hardware` or `software`\n");
        return 1;
    }
    const u32 gi_frames = option_number(argc, argv, "--gi-frames", 48);
    // The frames `print_gi_timing` times the GI over: see there for why this many.
    const u32 gi_timing_frames = option_number(argc, argv, "--gi-timing-frames", 240);
    const u32 gi_live_frames = option_number(argc, argv, "--gi-live-frames", 240);
    const std::string gi_caches = option(argc, argv, "--gi-caches", "device");
    if (gi_caches != "device" && gi_caches != "host") {
        std::fprintf(stderr, "cy_sample_beauty: --gi-caches is `device` or `host`\n");
        return 1;
    }
    const u32 gi_reference = option_number(argc, argv, "--gi-reference", 0);
    const u32 gi_reference_samples = option_number(argc, argv, "--gi-reference-samples", 256);
    // THE PATH TRACER'S PICTURE, beside the GI's: `--gi-reference-step <n>` path traces every
    // n-th pixel of the last frame each way and upsamples it; `--gi-reference-still` draws the
    // frame once more through it in place of the GI's indirect diffuse, and `--gi-error-map`
    // writes where the two differ. Needs `--supersample 1`: the picture is the GI's own grid.
    const u32 gi_reference_step = option_number(argc, argv, "--gi-reference-step", 0);
    const u32 gi_reference_threads = option_number(argc, argv, "--gi-reference-threads", 8);
    // Three bounces is the bake's path length; a closed room keeps more of its light past it.
    const u32 gi_reference_bounces = option_number(argc, argv, "--gi-reference-bounces", 3);
    const std::string gi_reference_still = option(argc, argv, "--gi-reference-still", "");
    const std::string gi_error_map = option(argc, argv, "--gi-error-map", "");
    const u32 orbit_frame = option_number(argc, argv, "--orbit-frame", 1);
    // TEMPORAL UPSCALING. Off by default, which is the published frame. The still is drawn after
    // `--upscale-frames` frames of the same view: a temporal reconstruction converges over frames
    // as a game's does, and the still is the frame a player standing here would see.
    const std::string upscale = option(argc, argv, "--upscale", "off");
    rendering::upscale::UpscaleMode upscale_mode = rendering::upscale::UpscaleMode::Quality;
    if (upscale != "off" &&
        !rendering::upscale::parse_upscale_mode(upscale.c_str(), upscale_mode)) {
        std::fprintf(stderr,
                     "cy_sample_beauty: --upscale is `off`, `native`, `quality`, `balanced` or "
                     "`performance`\n");
        return 1;
    }
    const u32 upscale_frames = option_number(argc, argv, "--upscale-frames", 48);
    const f32 sharpness = std::strtof(option(argc, argv, "--sharpness", "0.3").c_str(), nullptr);
    const u32 timing_frames = option_number(argc, argv, "--timing-frames", 0);

    std::string problem;
    auto parsed = Shot::read(shot_path.c_str(), problem);
    if (!parsed) {
        std::fprintf(stderr, "cy_sample_beauty: %s\n", problem.c_str());
        return 1;
    }
    Shot shot = std::move(parsed.value());
    std::printf("shot          %s: %llu materials, %llu meshes, %llu instances\n",
                shot_path.c_str(), static_cast<unsigned long long>(shot.materials.size()),
                static_cast<unsigned long long>(shot.meshes.size()),
                static_cast<unsigned long long>(shot.instances.size()));

    if (materials.empty()) {
        std::fprintf(stderr,
                     "cy_sample_beauty: --materials names the directory `cy_material author` wrote "
                     "the compiled programs into. `just capture-beauty-shot` supplies it.\n");
        return 1;
    }
    for (ShotMaterial& material : shot.materials) {
        const std::string stem = stem_of(material.graph_path);
        std::string base = materials;
        base += '/';
        base += stem;
        std::string info = base;
        info += ".cymatinfo";
        if (!read_material_info(info, material)) {
            return 1;
        }
        if (material.has_base_color) {
            // The shot's own colour for this material in place of the graph's default: the block
            // the frame uploads and the GI's card albedo both read it from here.
            material.parameter_defaults[0] =
                Vec4{material.base_color.x, material.base_color.y, material.base_color.z,
                     material.parameter_defaults[0].w};
        }
        std::string spirv = base;
        spirv += ".spv";
        if (!read_spirv(spirv, material.spirv)) {
            return 1;
        }
        std::printf("material      %-18s cook key 0x%016llx  %llu SPIR-V words\n", stem.c_str(),
                    static_cast<unsigned long long>(material.cook_key),
                    static_cast<unsigned long long>(material.spirv.size()));
    }

    Stage stage(allocator);
    if (Status opened = stage.open(width, height, supersample); !opened) {
        std::fprintf(stderr, "cy_sample_beauty: %s\n", opened.error().message);
        return 1;
    }
    if (!stage.available()) {
        std::printf("device        NOT FOUND: %s\n", stage.absence());
        std::printf(
            "\nEverything above loaded. The picture needs a graphics device and this "
            "machine has none, so nothing was written.\n");
        return 0;
    }

    ShotReport report;
    stage.set_ambient_occlusion(occlusion == "on", shot);
    std::printf("occlusion     %s, radius %.2f m, power %.2f\n", occlusion.c_str(),
                static_cast<double>(shot.occlusion_radius),
                static_cast<double>(shot.occlusion_power));
    stage.set_soft_shadows(soft_shadows == "on", shot);
    std::printf("soft shadows  %s, sun angular radius %.5f rad, contact reach %.2f m\n",
                soft_shadows.c_str(), static_cast<double>(shot.sun_angular_radius),
                static_cast<double>(shot.contact_length));
    stage.set_decals(decals == "on");
    std::printf("decals        %s, %llu in the shot over %llu decal materials\n", decals.c_str(),
                static_cast<unsigned long long>(shot.decals.size()),
                static_cast<unsigned long long>(shot.decal_materials.size()));
    stage.set_fog(fog == "on", shot);
    std::printf(
        "fog           %s, visibility %.0f m at %.2f m, scale height %.1f m, g %.2f\n", fog.c_str(),
        static_cast<double>(shot.fog_visibility_metres), static_cast<double>(shot.fog_base_height),
        static_cast<double>(shot.fog_scale_height), static_cast<double>(shot.fog_anisotropy));
    stage.limit_albedo_levels(albedo_levels);
    if (bloom) {
        stage.enable_bloom(shot);
    }
    if (!look.empty()) {
        stage.enable_grading(look.c_str());
    }
    if (!focus.empty()) {
        const Vec3* target = shot.focus_target(focus);
        if (target == nullptr) {
            std::fprintf(stderr, "cy_sample_beauty: the shot names no focus target `%s`\n",
                         focus.c_str());
            return 1;
        }
        stage.enable_depth_of_field(shot, *target);
        std::printf("depth of field focused on %s at (%.2f, %.2f, %.2f), f/%.1f\n", focus.c_str(),
                    static_cast<double>(target->x), static_cast<double>(target->y),
                    static_cast<double>(target->z), static_cast<double>(shot.dof_f_number));
    }
    if (!motion_blur.empty()) {
        const f32 degrees = std::strtof(motion_blur.c_str(), nullptr);
        stage.set_motion_blur(true, degrees);
        std::printf("motion blur   on, shutter %.0f degrees\n", static_cast<double>(degrees));
    }
    BeautyGiSettings gi_settings;
    gi_settings.enabled = gi == "on";
    gi_settings.hardware = gi_tier == "hardware";
    gi_settings.divisor = std::max(1U, option_number(argc, argv, "--gi-divisor", 1));
    gi_settings.readback = gi_reference != 0U || gi_reference_step != 0U;
    gi_settings.converged = option(argc, argv, "--gi-converged", "on") == "on";
    const std::string gather = option(argc, argv, "--gi-gather", "probes");
    const std::string denoiser = option(argc, argv, "--gi-denoiser", "probe-space");
    if ((gather != "probes" && gather != "per-pixel") ||
        (denoiser != "probe-space" && denoiser != "shared")) {
        std::fprintf(stderr,
                     "cy_sample_beauty: --gi-gather is `probes` or `per-pixel`, --gi-denoiser "
                     "`probe-space` or `shared`\n");
        return 1;
    }
    gi_settings.probe_rays = std::max(1U, option_number(argc, argv, "--gi-probe-rays", 32));
    gi_settings.ray_density =
        std::strtof(option(argc, argv, "--gi-ray-density", "1").c_str(), nullptr);
    gi_settings.per_pixel = gather == "per-pixel";
    gi_settings.shared_denoiser = denoiser == "shared";
    gi_settings.device_caches = gi_caches == "device";
    stage.set_global_illumination(gi_settings);
    stage.set_upscale(upscale != "off", upscale_mode, sharpness);
    std::printf("upscale       %s%s\n", upscale.c_str(),
                upscale == "off" ? "" : ", the scene below the output and reconstructed");
    std::printf(
        "gi            %s, %s tier asked for, resolve grid divisor %u, %s gather, %s "
        "denoiser\n",
        gi.c_str(), gi_tier.c_str(), gi_settings.divisor, gather.c_str(), denoiser.c_str());
    if (Status staged = stage.stage_shot(shot, report); !staged) {
        std::fprintf(stderr, "cy_sample_beauty: %s\n", staged.error().message);
        return 1;
    }
    if (gi_settings.enabled) {
        // ONLY THE STILL IS READ BACK: the frames before it copy nothing.
        stage.set_gi_readback_frames(false);
        // THE GI CONVERGES OVER FRAMES, as a game's does: the same view drawn and written nowhere,
        // so the still is the frame a player standing here would see after `gi_frames` frames.
        for (u32 index = 1; index < gi_frames; ++index) {
            ShotReport warm;
            if (Status drawn = stage.render(shot, nullptr, nullptr, warm); !drawn) {
                std::fprintf(stderr, "cy_sample_beauty: %s\n", drawn.error().message);
                return 1;
            }
        }
    }
    if (upscale != "off") {
        // THE RECONSTRUCTION CONVERGES OVER FRAMES: the same view drawn and written nowhere.
        for (u32 index = 1; index < upscale_frames; ++index) {
            ShotReport warm;
            if (Status drawn = stage.render(shot, nullptr, nullptr, warm); !drawn) {
                std::fprintf(stderr, "cy_sample_beauty: %s\n", drawn.error().message);
                return 1;
            }
        }
    }
    stage.set_gi_readback_frames(true);
    if (Status drawn = stage.render(shot, still.empty() ? nullptr : still.c_str(),
                                    no_post.empty() ? nullptr : no_post.c_str(), report);
        !drawn) {
        std::fprintf(stderr, "cy_sample_beauty: %s\n", drawn.error().message);
        return 1;
    }
    print_report(shot, report);
    if (gi_reference != 0U) {
        auto measured = stage.measure_gi_reference(gi_reference, gi_reference_samples, 3);
        if (!measured) {
            std::fprintf(stderr, "cy_sample_beauty: %s\n", measured.error().message);
            return 1;
        }
        std::printf(
            "gi reference  %u pixels path traced (%u paths, 3 bounces): the GI's indirect "
            "diffuse %.3f relative error, the flat ambient's %.3f\n",
            measured.value().pixels, gi_reference_samples,
            static_cast<double>(measured.value().gi_relative_error),
            static_cast<double>(measured.value().ambient_relative_error));
    }
    if (gi_reference_step != 0U) {
        BeautyGiReferenceImage picture;
        if (Status traced =
                stage.gi_reference_image(gi_reference_step, gi_reference_samples,
                                         gi_reference_bounces, gi_reference_threads, picture);
            !traced) {
            std::fprintf(stderr, "cy_sample_beauty: %s\n", traced.error().message);
            return 1;
        }
        std::printf(
            "gi picture    %u pixels path traced every %u px (%u paths, %u bounces) in %.1f s: "
            "the GI's indirect diffuse %.3f relative error, the reference's own noise %.3f\n",
            picture.traced, picture.step, picture.samples, picture.bounces,
            picture.trace_ms / 1000.0, static_cast<double>(picture.relative_error),
            static_cast<double>(picture.reference_noise));
        std::printf(
            "gi flat       the frame without the GI over the same pixels: its flat sky term %.3f "
            "relative error\n",
            static_cast<double>(picture.flat_relative_error));
        if (!gi_error_map.empty()) {
            if (Status written = stage.write_gi_error_map(picture, gi_error_map.c_str());
                !written) {
                std::fprintf(stderr, "cy_sample_beauty: %s\n", written.error().message);
                return 1;
            }
            std::printf("error map     %s\n", gi_error_map.c_str());
        }
        if (!gi_reference_still.empty()) {
            ShotReport reference_report;
            if (Status drawn = stage.render_with_gi_reference(
                    shot, picture, gi_reference_still.c_str(), reference_report);
                !drawn) {
                std::fprintf(stderr, "cy_sample_beauty: %s\n", drawn.error().message);
                return 1;
            }
            std::printf("reference     %s\n", gi_reference_still.c_str());
        }
    }
    if (gi_settings.enabled && gi_timing_frames != 0U) {
        // AFTER the still and the reference, so neither moves: the timing burst's frames advance
        // the GI's history past the still's.
        stage.set_gi_readback_frames(false);
        if (Status timed = print_gi_timing(stage, shot, gi_timing_frames); !timed) {
            std::fprintf(stderr, "cy_sample_beauty: %s\n", timed.error().message);
            return 1;
        }
    }
    if (gi_settings.enabled && gi_live_frames != 0U) {
        stage.set_gi_readback_frames(false);
        if (Status timed = print_gi_live_timing(stage, shot, gi_live_frames); !timed) {
            std::fprintf(stderr, "cy_sample_beauty: %s\n", timed.error().message);
            return 1;
        }
    }
    if (timing_frames != 0U) {
        // AFTER the still, so it does not move: a burst of the same view, each frame timed.
        stage.set_gi_readback_frames(false);
        if (Status timed = print_frame_timing(stage, shot, timing_frames); !timed) {
            std::fprintf(stderr, "cy_sample_beauty: %s\n", timed.error().message);
            return 1;
        }
    }
    if (!still.empty()) {
        std::printf("still         %s\n", still.c_str());
    }
    if (!manifest.empty()) {
        if (Status written = stage.write_manifest(shot, report, manifest.c_str()); !written) {
            std::fprintf(stderr, "cy_sample_beauty: %s\n", written.error().message);
            return 1;
        }
        std::printf("manifest      %s\n", manifest.c_str());
    }

    if (!orbit_still.empty()) {
        // ONE TURNTABLE FRAME WITH ITS MOTION: the frame before it drawn first and written nowhere,
        // so the frame's camera — and, with motion blur, its motion — is the orbit's step. The air
        // steps between the two, as the turntable's does.
        const u32 count = option_number(argc, argv, "--frames-count", 240);
        const u32 frame_index = orbit_frame == 0 ? 1U : orbit_frame;
        for (u32 index = frame_index - 1U; index <= frame_index; ++index) {
            ShotReport orbit_report;
            const char* path = index == frame_index ? orbit_still.c_str() : nullptr;
            if (Status drawn = stage.render_from(shot, turntable_eye(shot, index, count),
                                                 shot.camera_target, path, nullptr, orbit_report);
                !drawn) {
                std::fprintf(stderr, "cy_sample_beauty: %s\n", drawn.error().message);
                return 1;
            }
            if (index < frame_index) {
                if (Status stepped = stage.advance_air(); !stepped) {
                    std::fprintf(stderr, "cy_sample_beauty: %s\n", stepped.error().message);
                    return 1;
                }
            }
        }
        std::printf("orbit still   frame %u of %u, %s\n", frame_index, count, orbit_still.c_str());
    }

    if (!frames.empty()) {
        const u32 count = option_number(argc, argv, "--frames-count", 240);
        const f32 arc = std::strtof(option(argc, argv, "--orbit-degrees", "360").c_str(), nullptr);
        const Vec3 pivot = shot.camera_target;
        for (u32 index = 0; index < count; ++index) {
            const Vec3 eye = turntable_eye(shot, index, count, arc);
            char path[512] = {};
            (void)std::snprintf(path, sizeof(path), "%s/frame_%04u.png", frames.c_str(), index);
            ShotReport frame_report;
            if (Status drawn = stage.render_from(shot, eye, pivot, path, nullptr, frame_report);
                !drawn) {
                std::fprintf(stderr, "cy_sample_beauty: %s\n", drawn.error().message);
                return 1;
            }
            // THE AIR MOVES BETWEEN FRAMES AND NOT WITHIN THE STILL. `render_from` draws the field
            // as it stands; stepping it here is what makes the turntable a film of drifting embers
            // rather than a film of a camera orbiting a frozen one, and it leaves the still above —
            // which never reaches this loop — exactly reproducible.
            if (Status stepped = stage.advance_air(); !stepped) {
                std::fprintf(stderr, "cy_sample_beauty: %s\n", stepped.error().message);
                return 1;
            }
        }
        std::printf("frames        %u written to %s\n", count, frames.c_str());
    }

    if (!no_post.empty()) {
        // THE BEFORE/AFTER PAIR CAME OUT OF THE SAME FRAME. `Stage::render` reads back both the
        // tonemapped output the resolve wrote and the linear scene colour it read, so the two
        // pictures differ in the post chain and in nothing else — not in a second set of draws, not
        // in a second sun, not in a second jitter.
        std::printf(
            "no-post still %s  (the same frame's linear scene colour, display transfer "
            "only)\n",
            no_post.c_str());
    }
    return report.validation_errors == 0 ? 0 : 1;
}
