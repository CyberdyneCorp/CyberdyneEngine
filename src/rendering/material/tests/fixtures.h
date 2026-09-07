#pragma once
// The reference material, authored twice. M7 task 6.1.
//
// One material, written as a NODE GRAPH and as a TEXT DEFINITION, deliberately as unalike as two
// real authoring paths are (design.md §1.1). The exit criterion is that the compiler makes them one
// material; every wart below exists because an editor produces it and a person does not:
//
//   * a weight port on every closure node, untouched by the author;
//   * closure sums built pairwise, because a node has two inputs;
//   * `base_color_map` sampled from TWO separate nodes, because two parts of the graph each
//     dragged the texture in;
//   * `base_color * sample` wired in the opposite order from the text's `sample * base_color`;
//   * a tint node dragged in and left at one, so `albedo * 1.0` reaches the IR;
//   * a MUTED emission closure the author did not delete;
//   * a DISCONNECTED multiply left over from an experiment.
//
// The text has none of them, and writes `1 - metallic` where the graph drags in a "one minus" node.

#include <cy/rendering/material/graph.h>
#include <cy/rendering/material/ir.h>
#include <cy/rendering/material/text.h>

#include <string_view>

namespace cy::rendering::material::testing {

/// The authoring node ids the graph fixture uses, so a case can name one.
struct GraphIds {
    u32 uv = 0;
    u32 albedo_sample = 0;
    u32 albedo_swizzle = 0;
    u32 base_color = 0;
    u32 albedo = 0;
    u32 metallic = 0;
    u32 one_minus = 0;
    u32 grime_sample = 0;
    u32 grime = 0;
    u32 worn = 0;
    u32 worn_scaled = 0;
    u32 tint_constant = 0;
    u32 tinted = 0;
    u32 diffuse = 0;
    u32 second_sample = 0;
    u32 second_swizzle = 0;
    u32 second_albedo = 0;
    u32 roughness = 0;
    u32 specular = 0;
    u32 sum = 0;
    u32 emissive = 0;
    u32 emission = 0;
    u32 outer_sum = 0;
    u32 opacity = 0;
    u32 orphan = 0;
    u32 orphan_constant = 0;
};

[[nodiscard]] inline Immediate vec3(f32 x, f32 y, f32 z) noexcept {
    return Immediate{x, y, z, 0.0F, 0};
}

/// Declare the parameters and textures both front-ends share, in one order.
[[nodiscard]] inline bool declare_common(MaterialGraph& graph) noexcept {
    const ParameterDecl parameters[] = {
        {Name::intern("base_color"), ValueType::Vec3, vec3(0.82F, 0.78F, 0.74F), false},
        {Name::intern("metallic"), ValueType::Float, Immediate::scalar(1.0F), false},
        {Name::intern("roughness"), ValueType::Float, Immediate::scalar(0.35F), false},
        {Name::intern("emissive"), ValueType::Vec3, vec3(1.0F, 0.4F, 0.1F), false},
    };
    for (const ParameterDecl& decl : parameters) {
        if (!graph.declare_parameter(decl)) {
            return false;
        }
    }
    const TextureDecl textures[] = {
        {Name::intern("base_color_map"), Immediate{0.5F, 0.5F, 0.5F, 1.0F, 0}, false},
        {Name::intern("grime_map"), Immediate{0.35F, 0.35F, 0.35F, 1.0F, 0}, false},
    };
    for (const TextureDecl& decl : textures) {
        if (!graph.declare_texture(decl)) {
            return false;
        }
    }
    return true;
}

/// Build the graph. `false` on any failure, so a case can `CY_REQUIRE` it in one line.
[[nodiscard]] inline bool build_reference_graph(MaterialGraph& graph, GraphIds& ids) noexcept {
    if (!declare_common(graph)) {
        return false;
    }
    const u8 components[] = {0, 1, 2};
    Immediate swizzle_xyz;
    swizzle_xyz.mask = Builder::swizzle_mask(Span<const u8>(components, 3));
    const u8 first_component[] = {0};
    Immediate swizzle_x;
    swizzle_x.mask = Builder::swizzle_mask(Span<const u8>(first_component, 1));

    bool good = true;
    const auto add = [&graph, &good](GraphOp op, Name symbol = Name{},
                                     ValueType type = ValueType::Float,
                                     const Immediate& value = Immediate{}) noexcept {
        auto added = graph.add(op, symbol, type, value);
        good = good && added.has_value();
        return added ? added.value() : 0U;
    };
    const auto wire = [&graph, &good](u32 from, u32 to, u8 port) noexcept {
        good = good && graph.connect(from, to, port).has_value();
    };

    ids.uv = add(GraphOp::Attribute, Name::intern("uv0"), ValueType::Vec2);
    ids.albedo_sample =
        add(GraphOp::TextureSample, Name::intern("base_color_map"), ValueType::Vec4);
    wire(ids.uv, ids.albedo_sample, 0);
    ids.albedo_swizzle = add(GraphOp::Swizzle, Name{}, ValueType::Count, swizzle_xyz);
    wire(ids.albedo_sample, ids.albedo_swizzle, 0);
    ids.base_color = add(GraphOp::Parameter, Name::intern("base_color"), ValueType::Vec3);
    // The wire order an editor produces: the parameter landed on port 0 and the sample on port 1,
    // where the text writes `sample * base_color`. Canonical commutative ordering is what makes
    // these one value.
    ids.albedo = add(GraphOp::Multiply);
    wire(ids.base_color, ids.albedo, 0);
    wire(ids.albedo_swizzle, ids.albedo, 1);

    ids.metallic = add(GraphOp::Parameter, Name::intern("metallic"), ValueType::Float);
    ids.one_minus = add(GraphOp::OneMinus);
    wire(ids.metallic, ids.one_minus, 0);

    ids.grime_sample = add(GraphOp::TextureSample, Name::intern("grime_map"), ValueType::Vec4);
    wire(ids.uv, ids.grime_sample, 0);
    good = good && graph.annotate(ids.grime_sample, NodeFlags::Microdetail).has_value();
    ids.grime = add(GraphOp::Swizzle, Name{}, ValueType::Count, swizzle_x);
    wire(ids.grime_sample, ids.grime, 0);

    ids.worn = add(GraphOp::Multiply);
    wire(ids.albedo, ids.worn, 0);
    wire(ids.one_minus, ids.worn, 1);
    ids.worn_scaled = add(GraphOp::Multiply);
    wire(ids.worn, ids.worn_scaled, 0);
    wire(ids.grime, ids.worn_scaled, 1);

    // A tint node the author dragged in and left at one. An editor produces these constantly; a
    // person writing the material does not write `* 1.0`. Constant folding is what removes it.
    ids.tint_constant = add(GraphOp::Constant, Name{}, ValueType::Float, Immediate::scalar(1.0F));
    ids.tinted = add(GraphOp::Multiply);
    wire(ids.worn_scaled, ids.tinted, 0);
    wire(ids.tint_constant, ids.tinted, 1);

    ids.diffuse = add(GraphOp::Diffuse);
    wire(ids.tinted, ids.diffuse, 0);

    // The same texture, dragged in a second time by the part of the graph that feeds specular.
    ids.second_sample =
        add(GraphOp::TextureSample, Name::intern("base_color_map"), ValueType::Vec4);
    wire(ids.uv, ids.second_sample, 0);
    ids.second_swizzle = add(GraphOp::Swizzle, Name{}, ValueType::Count, swizzle_xyz);
    wire(ids.second_sample, ids.second_swizzle, 0);
    ids.second_albedo = add(GraphOp::Multiply);
    wire(ids.base_color, ids.second_albedo, 0);
    wire(ids.second_swizzle, ids.second_albedo, 1);

    ids.roughness = add(GraphOp::Parameter, Name::intern("roughness"), ValueType::Float);
    ids.specular = add(GraphOp::Specular);
    wire(ids.second_albedo, ids.specular, 0);
    wire(ids.roughness, ids.specular, 1);

    ids.sum = add(GraphOp::AddClosures);
    wire(ids.diffuse, ids.sum, 0);
    wire(ids.specular, ids.sum, 1);

    ids.emissive = add(GraphOp::Parameter, Name::intern("emissive"), ValueType::Vec3);
    ids.emission = add(GraphOp::Emission);
    wire(ids.emissive, ids.emission, 0);
    good = good && graph.mute(ids.emission, true).has_value();

    ids.outer_sum = add(GraphOp::AddClosures);
    wire(ids.sum, ids.outer_sum, 0);
    wire(ids.emission, ids.outer_sum, 1);

    ids.opacity = add(GraphOp::Constant, Name{}, ValueType::Float, Immediate::scalar(1.0F));

    // Left over from an experiment, wired to nothing.
    ids.orphan_constant = add(GraphOp::Constant, Name{}, ValueType::Float, Immediate::scalar(2.0F));
    ids.orphan = add(GraphOp::Multiply);
    wire(ids.roughness, ids.orphan, 0);
    wire(ids.orphan_constant, ids.orphan, 1);

    good = good && graph.set_surface_output(ids.outer_sum).has_value();
    good = good && graph.set_opacity_output(ids.opacity).has_value();
    return good;
}

/// The same material, written the way a person writes it.
[[nodiscard]] inline std::string_view reference_text() noexcept {
    return R"(
material worn_metal {
    param base_color : float3 = (0.82, 0.78, 0.74);
    param metallic   : float  = 1.0;
    param roughness  : float  = 0.35;
    param emissive   : float3 = (1.0, 0.4, 0.1);

    texture base_color_map average (0.5, 0.5, 0.5, 1.0);
    texture grime_map      average (0.35, 0.35, 0.35, 1.0);

    attribute uv0 : float2;

    let albedo = sample(base_color_map, uv0).xyz * base_color;
    @microdetail let grime = sample(grime_map, uv0);

    surface = diffuse(albedo * (1 - metallic) * grime.x) + specular(albedo, roughness);
    opacity = 1.0;
}
)";
}

}  // namespace cy::rendering::material::testing
