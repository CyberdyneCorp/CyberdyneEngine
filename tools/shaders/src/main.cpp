// `cy_shaderc` — the shader toolchain's front end. M11.c task 1.7, and what `just build-shaders`
// runs.
//
//   cy_shaderc targets
//       Print the targets this build emits. Probed: each one is answered by compiling a shader
//       for it, so a target the machine cannot actually produce is reported as one it cannot
//       produce rather than as an option somebody turned on.
//
//   cy_shaderc build [root...] [--target <t>]... [--out-dir <dir>] [--verbose]
//       Compile every entry point of every `.slang` file under the roots, for every target, and
//       compare what the artefacts of one entry point declare. Exits non-zero when two targets of
//       one graph disagree or when a target refuses a shader. Roots default to `src` and `samples`.
//
// WHY THERE IS NO `--check` AND NO `--no-check`. The comparison is not a mode: compiling for two
// targets and not comparing them produces two files and no evidence, which is precisely the thing
// `m11c:shader-targets-for-the-next-rung` exists to refuse.

#include <cy/core/memory/system_allocator.h>
#include <cy/shaders/targets.h>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

using namespace cy;

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  cy_shaderc targets\n"
                 "  cy_shaderc build [root...] [--target spirv|msl|dxil]... [--out-dir <dir>]\n"
                 "                  [--verbose]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    Allocator& allocator = system_allocator(MemoryDomain::Assets);
    const std::string_view command(argv[1]);

    if (command == "targets") {
        return shadertool::print_targets(allocator, stdout) ? 0 : 1;
    }
    if (command != "build") {
        return usage();
    }

    std::vector<std::string_view> roots;
    std::vector<shader::Target> targets;
    std::string_view out_dir;
    bool verbose = false;

    for (int index = 2; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--verbose") {
            verbose = true;
        } else if (argument == "--target" && index + 1 < argc) {
            shader::Target target = shader::Target::SpirV;
            if (!shader::parse_target(argv[++index], target)) {
                std::fprintf(stderr, "cy_shaderc: '%s' names no target\n", argv[index]);
                return usage();
            }
            targets.push_back(target);
        } else if (argument == "--out-dir" && index + 1 < argc) {
            out_dir = argv[++index];
        } else if (!argument.empty() && argument[0] == '-') {
            std::fprintf(stderr, "cy_shaderc: unknown option '%.*s'\n",
                         static_cast<int>(argument.size()), argument.data());
            return usage();
        } else {
            roots.push_back(argument);
        }
    }
    if (roots.empty()) {
        roots.emplace_back("src");
        roots.emplace_back("samples");
    }

    shadertool::Options options;
    options.roots = Span<const std::string_view>(roots.data(), roots.size());
    options.targets = Span<const shader::Target>(targets.data(), targets.size());
    options.out_dir = out_dir;
    options.verbose = verbose;

    auto report = shadertool::build_shader_set(allocator, options, stdout);
    if (!report) {
        std::fprintf(stderr, "cy_shaderc: %s\n", report.error().message);
        return 1;
    }
    // A run that compared nothing is not a run that agreed about everything.
    if (report->comparisons == 0) {
        std::fprintf(stderr, "cy_shaderc: nothing was compared\n");
        return 1;
    }
    return report->ok() ? 0 : 1;
}
