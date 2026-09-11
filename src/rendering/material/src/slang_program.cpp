// The step that makes a generated material compile. M8.c task 1b.3.

#include <cy/rendering/material/slang_program.h>

#include <string_view>

namespace cy::rendering::material {
namespace {

/// Append-only text writer over an `Array<char>`. The same shape `emit.cpp`'s has, and separate for
/// the reason this whole file is separate: `emit.cpp` is M7's closed work.
class Writer {
public:
    explicit Writer(Array<char>& out) noexcept : out_(&out) {}

    void text(std::string_view value) noexcept {
        if (!status_) {
            return;
        }
        for (const char character : value) {
            if (Status pushed = out_->push_back(character); !pushed) {
                status_ = pushed;
                return;
            }
        }
    }

    void number(u32 value) noexcept {
        char digits[12];
        u32 length = 0;
        do {
            digits[length++] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
        } while (value != 0);
        while (length > 0) {
            text(std::string_view(&digits[--length], 1));
        }
    }

    [[nodiscard]] Status status() const noexcept { return status_; }

private:
    Array<char>* out_;
    Status status_ = ok();
};

/// The Slang spelling of a value type. `Closure` never reaches here: a parameter, an attribute and
/// a field are all numbers.
[[nodiscard]] const char* slang_type_of(ValueType type) noexcept {
    return type == ValueType::Closure ? "CyClosure" : value_type_name(type);
}

/// A zero of a type, so a generated context can be initialised without an uninitialised read.
void write_zero(Writer& writer, ValueType type) noexcept {
    switch (type) {
        case ValueType::Bool:
            writer.text("false");
            return;
        case ValueType::Int:
            writer.text("0");
            return;
        case ValueType::Float:
            writer.text("0.0");
            return;
        default:
            writer.text(slang_type_of(type));
            writer.text("(0.0)");
            return;
    }
}

/// One declared name, and whether it has already been written. A material may sample the same
/// attribute a dozen times and the struct must declare it once.
struct NameSet {
    explicit NameSet(Allocator& allocator) noexcept : names(allocator), types(allocator) {}

    Array<Name> names;
    Array<ValueType> types;

    [[nodiscard]] Status add(Name name, ValueType type) noexcept {
        for (const Name existing : names.span()) {
            if (existing == name) {
                return ok();
            }
        }
        if (Status pushed = names.push_back(name); !pushed) {
            return pushed;
        }
        return types.push_back(type);
    }
};

/// Collect every attribute and every field the module reads, in the module's own node order.
///
/// NODE ORDER, not the order the emitter happens to walk in: the prelude is a declaration and its
/// field order must be a function of the module rather than of a traversal, or two runs of one
/// material would produce two different structs.
[[nodiscard]] Status collect_leaves(const Module& module, NameSet& attributes,
                                    NameSet& fields) noexcept {
    for (u32 id = 0; id < module.size(); ++id) {
        const Node& node = module.node(id);
        if (node.op == Op::Attribute) {
            if (Status added = attributes.add(node.symbol, node.type); !added) {
                return added;
            }
        } else if (node.op == Op::Field) {
            if (Status added = fields.add(node.symbol, node.type); !added) {
                return added;
            }
        }
    }
    return ok();
}

void write_struct(Writer& writer, const char* name, const NameSet& members,
                  const char* trailer) noexcept {
    writer.text("struct ");
    writer.text(name);
    writer.text("\n{\n");
    for (usize index = 0; index < members.names.size(); ++index) {
        writer.text("    ");
        writer.text(slang_type_of(members.types[index]));
        writer.text(" ");
        writer.text(members.names[index].text());
        writer.text(";\n");
    }
    if (trailer != nullptr) {
        writer.text(trailer);
    }
    writer.text("};\n\n");
}

void write_params(Writer& writer, const Module& module, u32 texture_count,
                  u32 field_count) noexcept {
    writer.text(
        "/// The material's own parameters, and the bindless slot each texture resolves to.\n");
    writer.text("struct CyMaterialParams\n{\n");
    for (const ParameterDecl& parameter : module.parameters()) {
        writer.text("    ");
        writer.text(slang_type_of(parameter.type));
        writer.text(" ");
        writer.text(parameter.name.text());
        writer.text(";\n");
    }
    if (texture_count > 0) {
        writer.text("    uint textures[");
        writer.number(texture_count);
        writer.text("];\n");
    }
    if (field_count > 0) {
        writer.text("    float fields[");
        writer.number(field_count);
        writer.text("];\n");
    }
    // ALWAYS PRESENT. A material with no parameters, no textures and no fields would otherwise
    // declare an empty struct, and an empty constant buffer is not a thing every back end accepts.
    writer.text("    uint cyMaterialSlotCount;\n");
    writer.text("};\n\n");
}

void write_slots(Writer& writer, const char* prefix, const NameSet& names) noexcept {
    for (usize index = 0; index < names.names.size(); ++index) {
        writer.text("static const uint ");
        writer.text(prefix);
        writer.text(names.names[index].text());
        writer.text(" = ");
        writer.number(static_cast<u32>(index));
        writer.text(";\n");
    }
    if (!names.names.empty()) {
        writer.text("\n");
    }
}

void write_texture_slots(Writer& writer, const Module& module) noexcept {
    u32 index = 0;
    for (const TextureDecl& texture : module.textures()) {
        writer.text("static const uint CY_TEXTURE_");
        writer.text(texture.name.text());
        writer.text(" = ");
        writer.number(index++);
        writer.text(";\n");
    }
    if (index != 0) {
        writer.text("\n");
    }
}

void write_accessors(Writer& writer, u32 texture_count, u32 field_count) noexcept {
    if (texture_count > 0) {
        writer.text(
            "/// The emitter writes `cy_material_sample(ctx, CY_TEXTURE_x, uv)`; the slot "
            "resolves\n"
            "/// to a bindless index in the material's own parameters, and the table itself is "
            "the\n"
            "/// standard library's global one.\n"
            "///\n"
            "/// THE EXPLICIT LEVEL, and it is a limitation rather than a choice: implicit\n"
            "/// derivatives exist only in a pixel stage, a material program is compiled and\n"
            "/// reflected before it is placed in one, and the probe entry point below is a "
            "compute\n"
            "/// shader. `cyMaterialSampleTexture` is the implicit form for a lowering that knows\n"
            "/// its stage. See slang_program.h.\n"
            "float4 cy_material_sample(CyMaterialContext ctx, uint slot, float2 uv)\n"
            "{\n"
            "    return cyMaterialSampleTextureLevel(ctx.params.textures[slot], uv, 0.0);\n"
            "}\n\n");
    }
    if (field_count > 0) {
        writer.text(
            "/// A field sample. Scalar: see slang_program.h for why the type is not carried.\n"
            "float cy_field_sample(CyMaterialContext ctx, uint field)\n"
            "{\n"
            "    return ctx.params.fields[field];\n"
            "}\n\n");
    }
}

void write_context(Writer& writer, const PreludeOptions& options) noexcept {
    writer.text("struct CyMaterialContext\n{\n");
    writer.text("    CyMaterialParams params;\n");
    writer.text("    CyMaterialAttributes attributes;\n");
    writer.text("};\n\n");
    writer.text("[[vk::binding(0, ");
    writer.number(options.material_set);
    writer.text(")]]\nConstantBuffer<CyMaterialParams> cyMaterialParameters;\n\n");
}

void write_zero_attributes(Writer& writer, const NameSet& attributes) noexcept {
    writer.text("CyMaterialAttributes cyZeroAttributes()\n{\n    CyMaterialAttributes zero;\n");
    for (usize index = 0; index < attributes.names.size(); ++index) {
        writer.text("    zero.");
        writer.text(attributes.names[index].text());
        writer.text(" = ");
        write_zero(writer, attributes.types[index]);
        writer.text(";\n");
    }
    writer.text("    return zero;\n}\n\n");
}

}  // namespace

Expected<PreludeReport, Error> emit_prelude(const Module& module, const PreludeOptions& options,
                                            Array<char>& out) noexcept {
    NameSet attributes(module.allocator());
    NameSet fields(module.allocator());
    if (Status collected = collect_leaves(module, attributes, fields); !collected) {
        return make_unexpected(collected.error());
    }

    PreludeReport report;
    report.parameters = static_cast<u32>(module.parameters().size());
    report.attributes = static_cast<u32>(attributes.names.size());
    report.textures = static_cast<u32>(module.textures().size());
    report.fields = static_cast<u32>(fields.names.size());

    Writer writer(out);
    writer.text(
        "// CyberMaterial generated PRELUDE. Do not edit: the material is the source.\n"
        "// The declarations the emitted program refers to and does not declare — see\n"
        "// src/rendering/material/slang_program.h for why they cannot live in the standard\n"
        "// library and why they are generated around the emitter's output rather than into it.\n"
        "// material: ");
    writer.text(module.name().text());
    writer.text("\nimport cy.material;\n\n");

    write_params(writer, module, report.textures, report.fields);
    write_struct(writer, "CyMaterialAttributes", attributes, "    uint cyAttributeCount;\n");
    write_context(writer, options);
    write_texture_slots(writer, module);
    write_slots(writer, "CY_FIELD_", fields);
    write_accessors(writer, report.textures, report.fields);
    write_zero_attributes(writer, attributes);

    if (!writer.status()) {
        return make_unexpected(writer.status().error());
    }
    return report;
}

Expected<PreludeReport, Error> assemble_translation_unit(const Module& module,
                                                         const GeneratedSource& generated,
                                                         ProgramKind kind, QualityTier tier,
                                                         const PreludeOptions& options,
                                                         Array<char>& out) noexcept {
    out.clear();
    Expected<PreludeReport, Error> report = emit_prelude(module, options, out);
    if (!report.has_value()) {
        return report;
    }

    Writer writer(out);
    // THE EMITTER'S TEXT, BYTE FOR BYTE. Its `import cy.material;` is repeated here and that is
    // deliberate: rewriting the generated text would put this step inside M7's cook key, and a
    // duplicate import is a no-op in Slang.
    writer.text(std::string_view(generated.text.data(), generated.text.size()));
    writer.text("\n");

    Array<char> entry(module.allocator());
    if (Status named = entry_point_name(module.name(), kind, tier, entry); !named) {
        return make_unexpected(named.error());
    }

    // A COMPUTE PROBE, not a fragment shader: a fragment entry point would need a vertex semantic
    // per attribute and the attribute set is the material's, so the probe would be as generated as
    // the program. What it proves is that the program compiles, reflects and links against the
    // standard library — see slang_program.h for what it deliberately does not prove.
    writer.text("[[vk::binding(1, ");
    writer.number(options.material_set);
    writer.text(")]]\nRWStructuredBuffer<float4> cyMaterialProbeOutput;\n\n");
    writer.text("[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid ");
    writer.text(kMaterialProbeEntryPoint);
    writer.text("(uint3 id: SV_DispatchThreadID)\n{\n");
    writer.text("    CyMaterialContext ctx;\n");
    writer.text("    ctx.params = cyMaterialParameters;\n");
    writer.text("    ctx.attributes = cyZeroAttributes();\n");
    writer.text("    CySurface compiled = cyDefaultSurface();\n    ");
    writer.text(std::string_view(entry.data(), entry.size()));
    writer.text("(ctx, compiled);\n");
    writer.text("    let surface = cyResolveSurface(compiled);\n");
    writer.text("    cyMaterialProbeOutput[id.x] = float4(surface.albedo, surface.opacity);\n}\n");

    if (!writer.status()) {
        return make_unexpected(writer.status().error());
    }
    return report;
}

}  // namespace cy::rendering::material
