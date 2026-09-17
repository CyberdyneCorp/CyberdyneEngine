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
    std::printf("cost          build %.1f ms, submit %.2f ms\n", report.build_ms, report.submit_ms);
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
    if (Status staged = stage.stage_shot(shot, report); !staged) {
        std::fprintf(stderr, "cy_sample_beauty: %s\n", staged.error().message);
        return 1;
    }
    if (Status drawn = stage.render(shot, still.empty() ? nullptr : still.c_str(),
                                    no_post.empty() ? nullptr : no_post.c_str(), report);
        !drawn) {
        std::fprintf(stderr, "cy_sample_beauty: %s\n", drawn.error().message);
        return 1;
    }
    print_report(shot, report);
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

    if (!frames.empty()) {
        const u32 count = option_number(argc, argv, "--frames-count", 240);
        const Vec3 pivot = shot.camera_target;
        const Vec3 offset =
            Vec3{shot.camera_position.x - pivot.x, 0.0F, shot.camera_position.z - pivot.z};
        const f32 radius = std::sqrt((offset.x * offset.x) + (offset.z * offset.z));
        const f32 start = std::atan2(offset.z, offset.x);
        for (u32 index = 0; index < count; ++index) {
            const f32 turn = start + ((2.0F * std::numbers::pi_v<f32> * static_cast<f32>(index)) /
                                      static_cast<f32>(count));
            Vec3 eye = shot.camera_position;
            eye.x = pivot.x + (std::cos(turn) * radius);
            eye.z = pivot.z + (std::sin(turn) * radius);
            char path[512] = {};
            (void)std::snprintf(path, sizeof(path), "%s/frame_%04u.png", frames.c_str(), index);
            ShotReport frame_report;
            if (Status drawn = stage.render_from(shot, eye, pivot, path, nullptr, frame_report);
                !drawn) {
                std::fprintf(stderr, "cy_sample_beauty: %s\n", drawn.error().message);
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
