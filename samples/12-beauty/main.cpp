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
//                    [--gi-reference <pixels>]      the path tracer at that many pixels
//                    [--gi-reference-step <n> --gi-reference-still <path> --gi-error-map <path>]
//                                                   the path tracer's picture of the frame
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

/// The stem of a material's authored graph, which is what the recipe named its outputs after.
/// Where the turntable's camera is at frame `index` of `count`: an orbit about the shot's target,
/// at the shot's own height and radius, starting from the shot's own camera.
[[nodiscard]] Vec3 turntable_eye(const Shot& shot, u32 index, u32 count) noexcept {
    const Vec3 pivot = shot.camera_target;
    const Vec3 offset =
        Vec3{shot.camera_position.x - pivot.x, 0.0F, shot.camera_position.z - pivot.z};
    const f32 radius = std::sqrt((offset.x * offset.x) + (offset.z * offset.z));
    const f32 start = std::atan2(offset.z, offset.x);
    const f32 turn = start + ((2.0F * std::numbers::pi_v<f32> * static_cast<f32>(index)) /
                              static_cast<f32>(count));
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
    const u32 gi_reference = option_number(argc, argv, "--gi-reference", 0);
    const u32 gi_reference_samples = option_number(argc, argv, "--gi-reference-samples", 256);
    // THE PATH TRACER'S PICTURE, beside the GI's: `--gi-reference-step <n>` path traces every
    // n-th pixel of the last frame each way and upsamples it; `--gi-reference-still` draws the
    // frame once more through it in place of the GI's indirect diffuse, and `--gi-error-map`
    // writes where the two differ. Needs `--supersample 1`: the picture is the GI's own grid.
    const u32 gi_reference_step = option_number(argc, argv, "--gi-reference-step", 0);
    const u32 gi_reference_threads = option_number(argc, argv, "--gi-reference-threads", 8);
    const std::string gi_reference_still = option(argc, argv, "--gi-reference-still", "");
    const std::string gi_error_map = option(argc, argv, "--gi-error-map", "");
    const u32 orbit_frame = option_number(argc, argv, "--orbit-frame", 1);

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
    gi_settings.per_pixel = gather == "per-pixel";
    gi_settings.shared_denoiser = denoiser == "shared";
    stage.set_global_illumination(gi_settings);
    std::printf(
        "gi            %s, %s tier asked for, resolve grid divisor %u, %s gather, %s "
        "denoiser\n",
        gi.c_str(), gi_tier.c_str(), gi_settings.divisor, gather.c_str(), denoiser.c_str());
    if (Status staged = stage.stage_shot(shot, report); !staged) {
        std::fprintf(stderr, "cy_sample_beauty: %s\n", staged.error().message);
        return 1;
    }
    ShotReport gi_fastest;
    if (gi_settings.enabled) {
        // THE GI CONVERGES OVER FRAMES, as a game's does: the same view drawn and written nowhere,
        // so the still is the frame a player standing here would see after `gi_frames` frames.
        for (u32 index = 1; index < gi_frames; ++index) {
            ShotReport warm;
            if (Status drawn = stage.render(shot, nullptr, nullptr, warm); !drawn) {
                std::fprintf(stderr, "cy_sample_beauty: %s\n", drawn.error().message);
                return 1;
            }
            // THE FASTEST FRAME, as the GI's suites time it: a device another process shares
            // time-slices its work into this one's, and the fastest frame is the one nothing
            // interrupted. The first frames carry the pipelines' first use.
            if (warm.gi_timed && index >= 4 &&
                (gi_fastest.gi_total_ms <= 0.0 || warm.gi_total_ms < gi_fastest.gi_total_ms)) {
                gi_fastest = warm;
            }
        }
    }
    if (Status drawn = stage.render(shot, still.empty() ? nullptr : still.c_str(),
                                    no_post.empty() ? nullptr : no_post.c_str(), report);
        !drawn) {
        std::fprintf(stderr, "cy_sample_beauty: %s\n", drawn.error().message);
        return 1;
    }
    print_report(shot, report);
    if (gi_fastest.gi_timed) {
        std::printf(
            "gi fastest    %.3f ms over the %u frames before the still: probe placement %.3f, "
            "probe tracing %.3f, probe filter %.3f, integration %.3f, denoiser %.3f ms\n",
            gi_fastest.gi_total_ms, gi_frames - 1U, gi_fastest.gi_probe_place_ms,
            gi_fastest.gi_probe_trace_ms, gi_fastest.gi_probe_filter_ms, gi_fastest.gi_integrate_ms,
            gi_fastest.gi_accumulate_ms + gi_fastest.gi_variance_ms + gi_fastest.gi_filter_ms +
                gi_fastest.gi_publish_ms);
    }
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
        if (Status traced = stage.gi_reference_image(gi_reference_step, gi_reference_samples, 3,
                                                     gi_reference_threads, picture);
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
        const Vec3 pivot = shot.camera_target;
        for (u32 index = 0; index < count; ++index) {
            const Vec3 eye = turntable_eye(shot, index, count);
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
