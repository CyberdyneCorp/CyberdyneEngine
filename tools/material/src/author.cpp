// `cy_material author` — the editor's canvas becomes a committed material. M11.c task 6.1a.
//
// ================================================================================================
// THE JUNCTION THIS IS
// ================================================================================================
//
// M11.c's spike walked the rung's path — author, compile, encode, bind, assemble, capture — and
// came back with junction 1 REFUSED and junctions 4 and 5 ABSENT. This file is junction 1's second
// half.
//
// The editor authors a material on `GraphCanvas`, the one shared node-graph canvas, and writes an
// INTERCHANGE: a line per node, per property and per wire. It does not write the canonical
// `.cygraph`, and `editor/crates/cy-editor-interface/src/specialised/graph.rs` gives the reason:
//
// > `src/graph/include/cy/graph/text.h` owns it and it is canonical [...] This crate does not
// > re-implement it and must not: a second writer of a canonical format is a second format the day
// > the two disagree about a float.
//
// So this program reads the interchange, builds a `cy::graph::Graph`, and hands it to the ENGINE's
// writer. The `.cygraph` that is committed is `cy::graph::write_graph`'s bytes and nobody else's,
// and the round trip is checked here rather than assumed: the file is parsed back and its semantic
// digest is required to equal the graph's.
//
// THE SAME ARRANGEMENT M8.a CHOSE FOR `.cyprim`. The editor writes a source; the engine owns what
// it becomes. `primitives.rs` states it: "the editor's whole part in a primitive is writing a
// source asset ... There is no geometry in this crate and nowhere here to put any."
//
// ================================================================================================
// WHAT ELSE IT WRITES, AND WHY THE SIDECAR IS NOT A CONVENIENCE
// ================================================================================================
//
// The frame uploads `CyMaterialParams` — the block the generated prelude declares at (set 3,
// binding 0) — from a C++ struct, and that struct's field offsets are a consequence of the MODULE's
// own declaration order. A material whose parameters arrived in a different order would be shaded
// with roughness read out of the metalness slot, and the picture would look plausible.
//
// So `author` writes a `.cymatinfo` naming the parameters and the textures IN THE ORDER THE MODULE
// DECLARES THEM, and `cy_sample_beauty` refuses a signature that is not the one it uploads. The
// check costs four lines and removes a whole class of silent wrongness.

#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/graph/material/canvas.h>
#include <cy/graph/material/lower_material.h>
#include <cy/graph/text.h>
#include <cy/material/author.h>
#include <cy/rendering/material/compiler.h>
#include <cy/rendering/material/emit.h>
#include <cy/rendering/material/slang_program.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace cy::material {
Status write_material_info(const rendering::material::Module& module, u64 cook_key,
                           std::string_view entry_point, Array<char>& out) noexcept {
    const auto append = [&out](std::string_view text) noexcept {
        for (const char character : text) {
            (void)out.push_back(character);
        }
    };
    char line[160] = {};
    (void)std::snprintf(line, sizeof(line),
                        "cymatinfo %u\nmaterial %.*s\ncook_key 0x%016llx\nentry %.*s\n",
                        kInfoVersion, static_cast<int>(module.name().text().size()),
                        module.name().text().data(), static_cast<unsigned long long>(cook_key),
                        static_cast<int>(entry_point.size()), entry_point.data());
    append(line);
    // THE AUTHORED DEFAULT TRAVELS WITH THE NAME. A frame that uploaded a zeroed parameter block
    // would draw a material whose base colour is black however the graph was authored — which is
    // exactly the failure this line removes, and it was found by looking at a picture.
    for (const rendering::material::ParameterDecl& parameter : module.parameters()) {
        (void)std::snprintf(line, sizeof(line), "param %.*s %s %.9g %.9g %.9g %.9g\n",
                            static_cast<int>(parameter.name.text().size()),
                            parameter.name.text().data(),
                            rendering::material::value_type_name(parameter.type),
                            static_cast<double>(parameter.default_value.x),
                            static_cast<double>(parameter.default_value.y),
                            static_cast<double>(parameter.default_value.z),
                            static_cast<double>(parameter.default_value.w));
        append(line);
    }
    for (const rendering::material::TextureDecl& texture : module.textures()) {
        (void)std::snprintf(line, sizeof(line), "texture %.*s\n",
                            static_cast<int>(texture.name.text().size()),
                            texture.name.text().data());
        append(line);
    }
    return out.empty() ? fail(ErrorCode::OutOfMemory, "the sidecar could not be grown") : ok();
}

}  // namespace cy::material

namespace {

using namespace cy;

[[nodiscard]] std::string option_value(int argc, char** argv, std::string_view name,
                                       std::string_view fallback) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] == name) {
            return argv[index + 1];
        }
    }
    return std::string(fallback);
}

[[nodiscard]] Status write_file(const std::string& path, Span<const char> bytes) noexcept {
    return assets::fs::write_atomic(path.c_str(), bytes.data(), bytes.size());
}

}  // namespace

int cy_material_author(int argc, char** argv) {
    using namespace cy;
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage:\n"
                     "  cy_material author <canvas.cymatcanvas> --graph <out.cygraph>\n"
                     "                     [--module <out.slang>] [--info <out.cymatinfo>]\n");
        return 2;
    }
    Allocator& memory = system_allocator(MemoryDomain::Assets);

    Array<u8> bytes(memory);
    if (Status read = assets::fs::read_whole(argv[2], bytes); !read) {
        std::fprintf(stderr, "cy_material: cannot read %s\n", argv[2]);
        return 1;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    graph::Graph authored(memory, Name::intern("material"));
    auto canvas = graph::material::read_canvas(text, authored);
    if (!canvas) {
        std::fprintf(stderr, "cy_material: %s (line %lld)\n", canvas.error().message,
                     static_cast<long long>(canvas.error().system_code));
        return 1;
    }

    // --- The canonical form, written by the engine and checked by round trip --------------------
    Array<char> canonical(memory);
    if (Status written = graph::write_graph(authored, canonical); !written) {
        std::fprintf(stderr, "cy_material: %s\n", written.error().message);
        return 1;
    }
    // THE ROUND TRIP, CHECKED RATHER THAN ASSUMED. The registry is the engine's own material
    // vocabulary, so a node type the canonical file names and this build cannot lower is reported
    // here — by the reader — rather than surviving as an opaque node into a material that compiles
    // without it.
    graph::NodeRegistry registry(memory);
    if (Status registered = graph::material::register_material_nodes(registry); !registered) {
        std::fprintf(stderr, "cy_material: %s\n", registered.error().message);
        return 1;
    }
    graph::DiagnosticSink sink(memory);
    auto reparsed = graph::parse_graph(std::string_view(canonical.data(), canonical.size()),
                                       &registry, memory, sink);
    if (!reparsed) {
        std::fprintf(stderr, "cy_material: the canonical form does not parse back: %s\n",
                     reparsed.error().message);
        return 1;
    }
    for (const graph::Diagnostic& diagnostic : sink.entries()) {
        std::fprintf(stderr, "cy_material: %s\n", diagnostic.message);
    }
    if (sink.errors() != 0) {
        return 1;
    }
    if (reparsed.value().semantic_digest() != authored.semantic_digest()) {
        std::fprintf(stderr,
                     "cy_material: the canonical form does not round-trip: the graph written and "
                     "the graph read back have different semantic digests\n");
        return 1;
    }

    const std::string graph_path = option_value(argc, argv, "--graph", "");
    if (!graph_path.empty()) {
        if (Status saved = write_file(graph_path, canonical.span()); !saved) {
            std::fprintf(stderr, "cy_material: cannot write %s\n", graph_path.c_str());
            return 1;
        }
    }

    // --- The engine's own lowering, and the compile ---------------------------------------------
    rendering::material::MaterialGraph lowered(memory, canvas.value().name);
    if (Status made = graph::material::lower_material(reparsed.value(), lowered); !made) {
        std::fprintf(stderr, "cy_material: %s\n", made.error().message);
        return 1;
    }
    auto module = rendering::material::lower_graph(lowered, memory);
    if (!module) {
        std::fprintf(stderr, "cy_material: %s\n", module.error().message);
        return 1;
    }
    rendering::material::CompileOptions options;
    auto compiled = rendering::material::compile_material(module.value(), options, memory);
    if (!compiled) {
        std::fprintf(stderr, "cy_material: %s\n", compiled.error().message);
        return 1;
    }

    const rendering::material::CompiledProgram* primary = nullptr;
    for (const rendering::material::CompiledProgram& program : compiled.value().programs()) {
        if (program.kind == rendering::material::ProgramKind::Primary &&
            program.tier == rendering::material::QualityTier::High) {
            primary = &program;
        }
    }
    if (primary == nullptr || primary->absent) {
        std::fprintf(stderr, "cy_material: the material has no primary/high program\n");
        return 1;
    }

    Array<char> entry(memory);
    if (Status named = rendering::material::entry_point_name(compiled.value().primary().name(),
                                                             primary->kind, primary->tier, entry);
        !named) {
        std::fprintf(stderr, "cy_material: %s\n", named.error().message);
        return 1;
    }

    const std::string module_path = option_value(argc, argv, "--module", "");
    if (!module_path.empty()) {
        Array<char> unit(memory);
        auto report = rendering::material::assemble_translation_unit(
            primary->module, primary->source, primary->kind, primary->tier,
            rendering::material::PreludeOptions{}, unit);
        if (!report) {
            std::fprintf(stderr, "cy_material: %s\n", report.error().message);
            return 1;
        }
        if (Status saved = write_file(module_path, unit.span()); !saved) {
            std::fprintf(stderr, "cy_material: cannot write %s\n", module_path.c_str());
            return 1;
        }
    }

    const std::string info_path = option_value(argc, argv, "--info", "");
    if (!info_path.empty()) {
        Array<char> info(memory);
        if (Status made = material::write_material_info(
                compiled.value().primary(), compiled.value().cook_key(),
                std::string_view(entry.data(), entry.size()), info);
            !made) {
            std::fprintf(stderr, "cy_material: %s\n", made.error().message);
            return 1;
        }
        if (Status saved = write_file(info_path, info.span()); !saved) {
            std::fprintf(stderr, "cy_material: cannot write %s\n", info_path.c_str());
            return 1;
        }
    }

    std::printf("material %.*s  %u nodes, %u wires  cook key 0x%016llx  entry %.*s\n",
                static_cast<int>(canvas.value().name.text().size()),
                canvas.value().name.text().data(), canvas.value().nodes, canvas.value().links,
                static_cast<unsigned long long>(compiled.value().cook_key()),
                static_cast<int>(entry.size()), entry.data());
    std::printf("  %llu parameters, %llu textures, %llu programs\n",
                static_cast<unsigned long long>(compiled.value().primary().parameters().size()),
                static_cast<unsigned long long>(compiled.value().primary().textures().size()),
                static_cast<unsigned long long>(compiled.value().programs().size()));
    return 0;
}
