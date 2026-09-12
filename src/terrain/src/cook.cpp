// The cook side: a terrain tile becoming virtual geometry through the engine's own builder, and the
// terrain surface material authored as an ordinary material graph. M10 task 2.1.

#include <cy/terrain/cook.h>

namespace cy::terrain {
namespace {

using rendering::material::GraphOp;
using rendering::material::Immediate;
using rendering::material::MaterialGraph;

using rendering::material::ValueType;

/// The parameter name of one layer slot's albedo. A function rather than four literals at the call
/// site, because the declaration and the use must agree and a typo between them would be a
/// `NotFound` from the builder rather than a compile error.
[[nodiscard]] Name albedo_name(u32 slot) noexcept {
    switch (slot) {
        case 0:
            return Name::intern("terrain.layer0.albedo");
        case 1:
            return Name::intern("terrain.layer1.albedo");
        case 2:
            return Name::intern("terrain.layer2.albedo");
        default:
            return Name::intern("terrain.layer3.albedo");
    }
}

/// The vertex attribute carrying one layer slot's blend weight, as the texel stores it.
[[nodiscard]] Name weight_name(u32 slot) noexcept {
    switch (slot) {
        case 0:
            return Name::intern("terrain.weight0");
        case 1:
            return Name::intern("terrain.weight1");
        case 2:
            return Name::intern("terrain.weight2");
        default:
            return Name::intern("terrain.weight3");
    }
}

/// Wire `from` into port `port` of `to`, reporting the first failure.
[[nodiscard]] Status wire(MaterialGraph& graph, u32 from, u32 to, u8 port) noexcept {
    return graph.connect(from, to, port);
}

/// Declare one albedo parameter per bounded layer slot.
///
/// `Builder::parameter()` refuses an undeclared name, "so that a misspelt name is a diagnostic
/// rather than a silent zero", and a terrain material is authored under that rule like every other.
[[nodiscard]] Status declare_layer_parameters(MaterialGraph& graph, u32 slots) noexcept {
    for (u32 slot = 0; slot < slots; ++slot) {
        rendering::material::ParameterDecl decl;
        decl.name = albedo_name(slot);
        decl.type = ValueType::Vec3;
        decl.default_value = Immediate{0.5F, 0.5F, 0.5F, 1.0F, 0};
        if (Status declared = graph.declare_parameter(decl); !declared) {
            return declared;
        }
    }
    return ok();
}

/// The environment fields, as the compiler's own `Field` nodes, composed into ONE attenuation
/// factor. `kInvalidNode` when the material reads no field.
///
/// `CompileOptions::check_fields` then refuses a material sampling a field the project has not
/// declared, "rather than silently substituting a default" — the same firewall
/// `environment::FieldRegistry::validate()` applies on the CPU side, applied here by the compiler
/// that already owns it.
[[nodiscard]] Expected<u32, Error> build_field_factor(MaterialGraph& graph,
                                                      Span<const Name> fields) noexcept {
    u32 factor = rendering::material::kInvalidNode;
    for (const Name field : fields) {
        Expected<u32, Error> sampled = graph.add(GraphOp::Field, field, ValueType::Float);
        if (!sampled) {
            return sampled;
        }
        Expected<u32, Error> attenuate = graph.add(GraphOp::OneMinus, {}, ValueType::Float);
        if (!attenuate) {
            return attenuate;
        }
        if (Status wired = wire(graph, sampled.value(), attenuate.value(), 0); !wired) {
            return make_unexpected(wired.error());
        }
        if (factor == rendering::material::kInvalidNode) {
            factor = attenuate.value();
            continue;
        }
        Expected<u32, Error> product = graph.add(GraphOp::Multiply, {}, ValueType::Float);
        if (!product) {
            return product;
        }
        if (Status wired = wire(graph, factor, product.value(), 0); !wired) {
            return make_unexpected(wired.error());
        }
        if (Status wired = wire(graph, attenuate.value(), product.value(), 1); !wired) {
            return make_unexpected(wired.error());
        }
        factor = product.value();
    }
    return factor;
}

/// One layer slot's closure: its albedo, its blend weight, and the environment attenuation on that
/// weight.
///
/// Every node is added BEFORE the node that consumes it, because `MaterialGraph::connect()`
/// requires a wire to run from an earlier node to a later one — the acyclic dependency order an
/// editor's own topological sort produces.
[[nodiscard]] Expected<u32, Error> build_layer_closure(MaterialGraph& graph, u32 slot,
                                                       u32 factor) noexcept {
    Expected<u32, Error> albedo = graph.add(GraphOp::Parameter, albedo_name(slot), ValueType::Vec3);
    if (!albedo) {
        return albedo;
    }
    Expected<u32, Error> weight =
        graph.add(GraphOp::Attribute, weight_name(slot), ValueType::Float);
    if (!weight) {
        return weight;
    }

    u32 contribution = weight.value();
    if (factor != rendering::material::kInvalidNode) {
        // Wetness and snow attenuate each layer's contribution: the environment reaches the surface
        // through the material graph, not through a terrain-specific shader path. The product is
        // taken on the WEIGHT, which is a float like the field, rather than on the albedo, which is
        // not.
        Expected<u32, Error> modulated = graph.add(GraphOp::Multiply, {}, ValueType::Float);
        if (!modulated) {
            return modulated;
        }
        if (Status wired = wire(graph, contribution, modulated.value(), 0); !wired) {
            return make_unexpected(wired.error());
        }
        if (Status wired = wire(graph, factor, modulated.value(), 1); !wired) {
            return make_unexpected(wired.error());
        }
        contribution = modulated.value();
    }

    Expected<u32, Error> closure = graph.add(GraphOp::Diffuse, {}, ValueType::Closure);
    if (!closure) {
        return closure;
    }
    if (Status wired = wire(graph, albedo.value(), closure.value(), 0); !wired) {
        return make_unexpected(wired.error());
    }
    // A Diffuse closure takes one operand and a weight, so THE WEIGHT IS PORT 1. graph.h's "the
    // last input is always the weight" is an arity rule, not a fixed port number.
    if (Status wired = wire(graph, contribution, closure.value(), 1); !wired) {
        return make_unexpected(wired.error());
    }
    return closure.value();
}

}  // namespace

rendering::vg::SourceMesh source_mesh_of(const TerrainMesh& mesh) noexcept {
    rendering::vg::SourceMesh source;
    source.positions = mesh.positions.span();
    source.normals = mesh.normals.span();
    source.uvs = mesh.uvs.span();
    source.indices = mesh.indices.span();
    source.triangle_materials = mesh.triangle_materials.span();
    return source;
}

Expected<rendering::vg::GeometryBuild, Error> build_tile_geometry(
    Allocator& allocator, const TerrainMesh& mesh,
    const rendering::vg::BuildOptions& options) noexcept {
    if (mesh.indices.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: a tile that is entirely holes has no geometry to build");
    }
    // The engine's builder, with the engine's options, on a mesh like any other. There is nothing
    // else in this function on purpose — see cook.h.
    return rendering::vg::build_geometry(source_mesh_of(mesh), options, allocator);
}

Status build_terrain_material(MaterialGraph& graph,
                              const TerrainMaterialDescription& description) noexcept {
    if (description.layers_per_texel == 0 || description.layers_per_texel > kMaxTexelLayers) {
        return fail(ErrorCode::InvalidArgument,
                    "terrain: a surface material blends between one and kMaxTexelLayers layers per "
                    "texel");
    }
    if (Status declared = declare_layer_parameters(graph, description.layers_per_texel);
        !declared) {
        return declared;
    }

    Expected<u32, Error> factor = build_field_factor(graph, description.fields);
    if (!factor) {
        return Status{make_unexpected(factor.error())};
    }

    // ONE closure per BOUNDED layer slot, never one per layer in the world. A terrain with forty
    // layers authors this same graph: the texel names which four it blends, and the shader blends
    // four. That is "shading cost SHALL not scale with the layer count of the region", as a
    // property of the graph rather than as an assertion about it.
    u32 blended = rendering::material::kInvalidNode;
    for (u32 slot = 0; slot < description.layers_per_texel; ++slot) {
        Expected<u32, Error> closure = build_layer_closure(graph, slot, factor.value());
        if (!closure) {
            return Status{make_unexpected(closure.error())};
        }
        if (blended == rendering::material::kInvalidNode) {
            blended = closure.value();
            continue;
        }
        Expected<u32, Error> sum = graph.add(GraphOp::AddClosures, {}, ValueType::Closure);
        if (!sum) {
            return Status{make_unexpected(sum.error())};
        }
        if (Status wired = wire(graph, blended, sum.value(), 0); !wired) {
            return wired;
        }
        if (Status wired = wire(graph, closure.value(), sum.value(), 1); !wired) {
            return wired;
        }
        blended = sum.value();
    }

    if (Status output = graph.set_surface_output(blended); !output) {
        return output;
    }
    return ok();
}

}  // namespace cy::terrain
