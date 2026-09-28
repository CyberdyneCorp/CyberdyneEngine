// `cy_material` — the material compiler's front end. M7 task 6.3.
//
// Three subcommands, and the first split is the same one `cy_build` makes:
//
//   compile <file.cymat>   Compile one material and print its cook report. No graph, no cache, no
//                          artefact store — the shortest path from a definition to "what does this
//                          material cost and what did the compiler have to say about it".
//
//   author <canvas>        M11.c task 6.1a. Read the interchange the editor's material canvas
//                          writes, canonicalise it into a `.cygraph` with the ENGINE's writer,
//                          lower it, compile it, and emit the translation unit the shader pipeline
//                          compiles. See author.cpp for why the editor does not write the canonical
//                          form itself.
//
//   cook <project> <out>   Build the material NODES of a derivation graph over a project directory.
//                          This is the path that ships: every material under the project is a node,
//                          the key covers the toolchain and the compiler version, the artefacts are
//                          content-addressed, and a second run is a cache hit. Nothing here caches
//                          anything itself — see cook.h.

#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/material/author.h>
#include <cy/material/cook.h>
#include <cy/scene/serialization/worldfile.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
                 "                      [--stages]\n"
                 "  cy_material author <canvas.cymatcanvas> --graph <out.cygraph>\n"
                 "                     [--module <out.slang>] [--info <out.cymatinfo>]\n"
                 "  cy_material cook <project-dir> <artefact-dir> [--profile desktop|mobile]\n"
                 "                   [--cache <dir>] [--world <scene.cyworld>]...\n"
                 "                   [--geometry <material>=<sources>]...\n"
                 "                   <material.cymat|material.cygraph>...\n");
    return 2;
}

[[nodiscard]] bool has_flag(int argc, char** argv, std::string_view name) {
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == name) {
            return true;
        }
    }
    return false;
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

    // --stages: EVERY STAGE OF THE LOWERING, which `shader-system`'s "Visual material editor"
    // requires an editor to be able to show and which no front end could obtain before M11.c. The
    // list is the library's; this prints it, and so does whatever panel asks the same function.
    if (has_flag(argc, argv, "--stages")) {
        material::CompileOptions stage_options;
        stage_options.profile = profile.value();
        rendering::material::ParseDiagnostic diagnostic(allocator());
        auto inspected = material::inspect_material(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
            stage_options, rendering::material::ProgramKind::Primary,
            rendering::material::QualityTier::High, allocator(), diagnostic);
        if (!inspected) {
            std::fprintf(stderr, "cy_material: %s\n", inspected.error().message);
            return 1;
        }
        Array<char> stages(allocator());
        if (Status written = material::write_stage_report(inspected.value(), stages); !written) {
            std::fprintf(stderr, "cy_material: %s\n", written.error().message);
            return 1;
        }
        std::fwrite(stages.data(), 1, stages.size(), stdout);
    }

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

struct GeometryAssignment {
    std::string material;
    std::string sources;
    bool used = false;
};

struct CookInputs {
    std::string cache;
    std::string profile = "desktop";
    std::vector<std::string> materials;
    std::vector<GeometryAssignment> geometry;
    std::vector<std::string> worlds;
};

[[nodiscard]] const scene::serialization::WorldTypeDecl* world_type_named(
    const scene::serialization::World& world, std::string_view name) {
    for (const auto& type : world.types()) {
        if (world.text(type.name) == name) {
            return &type;
        }
    }
    return nullptr;
}

[[nodiscard]] u64 world_field_named(const scene::serialization::World& world,
                                    const scene::serialization::WorldTypeDecl& type,
                                    std::string_view name) {
    for (const auto& field : type.fields()) {
        if (world.text(field.name) == name) {
            return field.file_field;
        }
    }
    return 0;
}

[[nodiscard]] std::string_view world_text_field(
    const scene::serialization::World& world, const scene::serialization::WorldComponent* component,
    u64 file_field) {
    if (component == nullptr || file_field == 0) {
        return {};
    }
    const auto* field = component->find(file_field);
    if (field == nullptr || field->value.kind != scene::serialization::WorldValueKind::Text) {
        return {};
    }
    const Span<const u8> bytes = world.blob(field->value);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void add_world_material(CookInputs& inputs, std::string_view material,
                        std::string_view geometry_source) {
    if (!material.ends_with(".cymat") && !material.ends_with(".cygraph")) {
        return;
    }
    bool listed = false;
    for (const auto& source : inputs.materials) {
        listed |= source == material;
    }
    if (!listed) {
        inputs.materials.emplace_back(material);
    }
    for (auto& assignment : inputs.geometry) {
        if (assignment.material != material) {
            continue;
        }
        const std::string_view names(assignment.sources);
        const std::string source(geometry_source);
        if (names != source && !names.starts_with(source + ",") && !names.ends_with("," + source) &&
            names.find("," + source + ",") == std::string::npos) {
            assignment.sources += "," + source;
        }
        return;
    }
    inputs.geometry.push_back({std::string(material), std::string(geometry_source), false});
}

[[nodiscard]] std::string_view mesh_geometry_source(std::string_view mesh) {
    // CYVG is the Engine's cooked virtual-geometry asset. The world names it through the same
    // MeshRenderer.mesh asset reference as a static mesh; the material compiler needs its distinct
    // path before the visibility renderer gains vertex-material evaluation.
    std::string extension = std::filesystem::path(std::string(mesh)).extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char letter) {
        return static_cast<char>(std::tolower(letter));
    });
    return extension == ".cyvg" ? "VirtualGeometry" : "StaticMesh";
}

void collect_world_materials(const scene::serialization::World& world, CookInputs& inputs) {
    const auto* mesh_type = world_type_named(world, "MeshRenderer");
    const u64 mesh_field = mesh_type == nullptr ? 0 : world_field_named(world, *mesh_type, "mesh");
    const u64 material_field =
        mesh_type == nullptr ? 0 : world_field_named(world, *mesh_type, "material");
    const auto* slots_type = world_type_named(world, "ImportedMaterialSlots");
    for (const auto& node : world.nodes()) {
        if (!node.live || mesh_type == nullptr) {
            continue;
        }
        const auto* mesh = node.find(mesh_type->file_type);
        const std::string_view mesh_asset = world_text_field(world, mesh, mesh_field);
        if (mesh_asset.empty()) {
            continue;
        }
        const std::string_view source = mesh_geometry_source(mesh_asset);
        add_world_material(inputs, world_text_field(world, mesh, material_field), source);
        if (slots_type == nullptr) {
            continue;
        }
        const auto* slots = node.find(slots_type->file_type);
        for (const auto& field : slots_type->fields()) {
            if (world.text(field.name).starts_with("slot_")) {
                add_world_material(inputs, world_text_field(world, slots, field.file_field),
                                   source);
            }
        }
    }

    const auto* terrain_type = world_type_named(world, "TerrainAuthoring");
    const auto* layer_type = world_type_named(world, "TerrainMaterialLayer");
    if (terrain_type == nullptr || layer_type == nullptr) {
        return;
    }
    const u64 layer_material = world_field_named(world, *layer_type, "material");
    for (const auto& node : world.nodes()) {
        if (!node.live || node.parent == scene::serialization::WorldNode::kNoParent) {
            continue;
        }
        const auto& parent = world.nodes()[node.parent];
        if (!parent.live || parent.find(terrain_type->file_type) == nullptr) {
            continue;
        }
        add_world_material(
            inputs, world_text_field(world, node.find(layer_type->file_type), layer_material),
            "Terrain");
    }
}

[[nodiscard]] bool load_world_materials(std::string_view project, CookInputs& inputs) {
    for (const std::string& reference : inputs.worlds) {
        const std::filesystem::path path(reference);
        if (path.is_absolute() || path.extension() != ".cyworld" ||
            reference.find('\\') != std::string::npos || reference.find(':') != std::string::npos) {
            std::fprintf(stderr, "cy_material: expected a project-relative .cyworld path\n");
            return false;
        }
        for (const auto& part : path) {
            if (part == "." || part == "..") {
                std::fprintf(stderr, "cy_material: world path escapes the project\n");
                return false;
            }
        }
        const std::string absolute = (std::filesystem::path(project) / path).string();
        Array<u8> bytes(allocator());
        if (Status read = assets::fs::read_whole(absolute.c_str(), bytes); !read) {
            std::fprintf(stderr, "cy_material: cannot read world %s\n", reference.c_str());
            return false;
        }
        scene::serialization::World world(allocator());
        auto parsed = scene::serialization::read_world(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), reference,
            world);
        if (!parsed) {
            std::fprintf(stderr, "cy_material: cannot parse world %s: %s\n", reference.c_str(),
                         parsed.error().message);
            return false;
        }
        collect_world_materials(world, inputs);
    }
    return true;
}

[[nodiscard]] bool add_geometry_assignment(CookInputs& inputs, std::string_view assignment) {
    const usize separator = assignment.find('=');
    if (separator == 0 || separator == std::string_view::npos ||
        separator + 1 == assignment.size()) {
        std::fprintf(stderr, "cy_material: expected --geometry <material>=<sources>\n");
        return false;
    }
    const std::string material(assignment.substr(0, separator));
    for (const auto& recorded : inputs.geometry) {
        if (recorded.material == material) {
            std::fprintf(stderr, "cy_material: duplicate geometry assignment for %s\n",
                         material.c_str());
            return false;
        }
    }
    inputs.geometry.push_back({material, std::string(assignment.substr(separator + 1)), false});
    return true;
}

[[nodiscard]] bool parse_cook_inputs(int argc, char** argv, CookInputs& inputs) {
    for (int index = 4; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--cache" || argument == "--profile") {
            if (++index >= argc) {
                return false;
            }
            (argument == "--cache" ? inputs.cache : inputs.profile) = argv[index];
            continue;
        }
        if (argument == "--geometry") {
            ++index;
            if (index >= argc || !add_geometry_assignment(inputs, argv[index])) {
                return false;
            }
            continue;
        }
        if (argument == "--world") {
            if (++index >= argc) {
                return false;
            }
            inputs.worlds.emplace_back(argv[index]);
            continue;
        }
        if (argument.starts_with("--")) {
            return false;
        }
        inputs.materials.emplace_back(argument);
    }
    return !inputs.materials.empty() || !inputs.worlds.empty();
}

int cook_project(int argc, char** argv) {
    if (argc < 5) {
        return usage();
    }
    const std::string project = argv[2];
    const std::string artefacts = argv[3];
    CookInputs inputs;
    if (!parse_cook_inputs(argc, argv, inputs)) {
        return usage();
    }
    if (!load_world_materials(project, inputs)) {
        return 1;
    }
    if (inputs.materials.empty()) {
        std::fprintf(stderr, "cy_material: no material sources were assigned by the worlds\n");
        return 1;
    }

    build::ProducerRegistry producers;
    if (Status added = material::add_material_producers(producers); !added) {
        std::fprintf(stderr, "cy_material: %s\n", added.error().message);
        return 1;
    }

    build::BuildGraph graph;
    for (const std::string& argument : inputs.materials) {
        build::NodeDesc node;
        node.kind = build::NodeKind::Shader;
        node.name = "material:" + argument;
        node.producer = "material";
        node.producer_version = material::kMaterialProducerVersion;
        node.sources.emplace_back(argument);
        node.outputs.emplace_back(argument + ".cymatbin");
        node.options.push_back(build::NodeOption{"profile", inputs.profile});
        for (auto& assignment : inputs.geometry) {
            if (assignment.material == argument) {
                node.options.push_back(build::NodeOption{"geometry", assignment.sources});
                assignment.used = true;
                break;
            }
        }
        if (auto added = graph.add(std::move(node)); !added) {
            std::fprintf(stderr, "cy_material: %s\n", added.error().message);
            return 1;
        }
    }
    for (const auto& assignment : inputs.geometry) {
        if (!assignment.used) {
            std::fprintf(stderr, "cy_material: geometry assignment has no material %s\n",
                         assignment.material.c_str());
            return 2;
        }
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
    config.cache.local = inputs.cache.empty() ? "" : inputs.cache.c_str();

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
    if (command == "author") {
        return cy_material_author(argc, argv);
    }
    return usage();
}
