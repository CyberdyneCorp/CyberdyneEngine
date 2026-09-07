// `cy_material` — the material compiler's front end. M7 task 6.3.
//
// Two subcommands, and the split is the same one `cy_build` makes:
//
//   compile <file.cymat>   Compile one material and print its cook report. No graph, no cache, no
//                          artefact store — the shortest path from a definition to "what does this
//                          material cost and what did the compiler have to say about it".
//
//   cook <project> <out>   Build the material NODES of a derivation graph over a project directory.
//                          This is the path that ships: every material under the project is a node,
//                          the key covers the toolchain and the compiler version, the artefacts are
//                          content-addressed, and a second run is a cache hit. Nothing here caches
//                          anything itself — see cook.h.

#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/material/cook.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cy;

[[nodiscard]] Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  cy_material compile <file.cymat> [--profile desktop|mobile] [--slang <dir>]\n"
                 "  cy_material cook <project-dir> <artefact-dir> [--profile desktop|mobile]\n"
                 "                   [--cache <dir>] <material.cymat>...\n");
    return 2;
}

[[nodiscard]] std::string option_value(int argc, char** argv, std::string_view name,
                                       std::string fallback) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) {
            return argv[index + 1];
        }
    }
    return fallback;
}

int compile_one(int argc, char** argv) {
    if (argc < 3) {
        return usage();
    }
    Array<u8> bytes(allocator());
    if (Status read = assets::fs::read_whole(argv[2], bytes); !read) {
        std::fprintf(stderr, "cy_material: cannot read %s\n", argv[2]);
        return 1;
    }
    auto profile = material::profile_from_name(option_value(argc, argv, "--profile", "desktop"));
    if (!profile) {
        std::fprintf(stderr, "cy_material: %s\n", profile.error().message);
        return 1;
    }
    material::CompileOptions options;
    options.profile = profile.value();

    Array<u8> bundle(allocator());
    Array<char> report(allocator());
    const Status cooked = material::cook_material(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options,
        allocator(), bundle, report);
    std::fwrite(report.data(), 1, report.size(), cooked ? stdout : stderr);
    if (!cooked) {
        std::fprintf(stderr, "cy_material: %s\n", cooked.error().message);
        return 1;
    }
    std::printf("bundle: %zu bytes\n", bundle.size());

    const std::string slang_dir = option_value(argc, argv, "--slang", "");
    if (slang_dir.empty()) {
        return 0;
    }
    auto decoded = material::decode_bundle(bundle.span(), allocator());
    if (!decoded) {
        std::fprintf(stderr, "cy_material: %s\n", decoded.error().message);
        return 1;
    }
    if (Status made = assets::fs::create_directories(slang_dir.c_str()); !made) {
        std::fprintf(stderr, "cy_material: cannot create %s\n", slang_dir.c_str());
        return 1;
    }
    for (const material::CookedProgram& program : decoded.value().programs) {
        if (program.absent) {
            continue;
        }
        std::string path = slang_dir;
        path += "/";
        path += rendering::material::program_kind_name(program.kind);
        path += "_";
        path += rendering::material::quality_tier_name(program.tier);
        path += ".slang";
        if (Status written = assets::fs::write_atomic(path.c_str(), program.source.data(),
                                                      program.source.size());
            !written) {
            std::fprintf(stderr, "cy_material: cannot write %s\n", path.c_str());
            return 1;
        }
    }
    return 0;
}

int cook_project(int argc, char** argv) {
    if (argc < 5) {
        return usage();
    }
    const std::string project = argv[2];
    const std::string artefacts = argv[3];
    const std::string cache = option_value(argc, argv, "--cache", "");
    const std::string profile = option_value(argc, argv, "--profile", "desktop");

    build::ProducerRegistry producers;
    if (Status added = material::add_material_producers(producers); !added) {
        std::fprintf(stderr, "cy_material: %s\n", added.error().message);
        return 1;
    }

    build::BuildGraph graph;
    u32 declared = 0;
    for (int index = 4; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument.starts_with("--")) {
            ++index;  // its value
            continue;
        }
        build::NodeDesc node;
        node.kind = build::NodeKind::Shader;
        node.name = "material:" + std::string(argument);
        node.producer = "material";
        node.producer_version = material::kMaterialProducerVersion;
        node.sources.emplace_back(argument);
        node.outputs.emplace_back(std::string(argument) + ".cymatbin");
        node.options.push_back(build::NodeOption{"profile", profile});
        if (auto added = graph.add(std::move(node)); !added) {
            std::fprintf(stderr, "cy_material: %s\n", added.error().message);
            return 1;
        }
        ++declared;
    }
    if (declared == 0) {
        return usage();
    }
    if (Status finalized = graph.finalize(); !finalized) {
        std::fprintf(stderr, "cy_material: %s\n", finalized.error().message);
        return 1;
    }

    build::DirectorySourceProvider sources(project);
    build::BuildConfig config;
    config.graph = &graph;
    config.producers = &producers;
    config.sources = &sources;
    config.artefact_root = artefacts;
    config.cache.local = cache.empty() ? "" : cache.c_str();

    build::BuildService service;
    if (Status configured = service.configure(std::move(config)); !configured) {
        std::fprintf(stderr, "cy_material: %s\n", configured.error().message);
        return 1;
    }
    auto report = service.build();
    if (!report) {
        std::fprintf(stderr, "cy_material: %s\n", report.error().message);
        return 1;
    }
    for (const build::NodeResult& node : report.value().nodes) {
        std::printf("%-12s %s\n", build::node_outcome_name(node.outcome), node.name.c_str());
        for (const build::Diagnostic& diagnostic : node.diagnostics) {
            std::printf("%s", diagnostic.message.c_str());
        }
    }
    std::printf("ran %llu, cached %llu, rebuilt %llu, failed %llu\n",
                static_cast<unsigned long long>(report.value().ran),
                static_cast<unsigned long long>(report.value().cached),
                static_cast<unsigned long long>(report.value().rebuilt),
                static_cast<unsigned long long>(report.value().failed));
    return report.value().failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return usage();
    }
    const std::string_view command = argv[1];
    if (command == "compile") {
        return compile_one(argc, argv);
    }
    if (command == "cook") {
        return cook_project(argc, argv);
    }
    return usage();
}
