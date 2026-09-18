#pragma once
// The authored canvas the material-lowering suites share. M11.c task 6.1a.
//
// IT IS A HEADER BECAUSE THERE ARE TWO SUITES AND ONE FIXTURE. `unit.graph_material` asks the cheap
// questions — the palette against the compiler's ops, a refused node type, every input pin against
// the port the compiler reads — and `integration.graph_material` asks the one expensive question,
// which is whether the canvas, the compiler's graph and the text definition cook to one key. The
// split is the test taxonomy's and the instrument's: `tests/harness/src/budget.cpp` measured that
// case at 0.806 ms of CPU against the unit tier's 1.000 ms budget, and its own message says what to
// do about a case that expensive — "the taxonomy in `testing-and-quality` places a test this
// expensive in the next suite up — move it, or make it cheaper". Every other case in the pair costs
// 0.107 ms or less. See src/graph/material/tests/CMakeLists.txt for the declaration.
//
// A COPIED FIXTURE WOULD GO STALE AND A STALE FIXTURE AGREES WITH A BROKEN CHECK, which is
// `tools/editor/selftest.py`'s rule and the reason M7's own `fixtures.h` is included rather than
// transcribed. The same rule applies one level down: the canvas below is written once and included
// twice, not pasted into the second suite.

#include <cy/core/memory/system_allocator.h>
#include <cy/graph/material/lower_material.h>
#include <cy/rendering/material/compiler.h>
#include <cy/rendering/material/graph.h>
#include <cy/test/test.h>

#include "fixtures.h"

namespace cy::graph_material_test {

// The names the canvas below uses unqualified. `Name`, `u8`, `u32` and `usize` already resolve from
// the enclosing `cy` namespace; these four do not.
using cy::graph::Graph;
using cy::graph::Literal;
using cy::graph::NodeKey;
using cy::rendering::material::ValueType;

inline cy::Allocator& allocator() {
    return cy::system_allocator(cy::MemoryDomain::Assets);
}

/// A small builder for an authored graph, so the fixture below reads like a canvas session.
class Canvas {
public:
    explicit Canvas(const char* name) : graph_(allocator(), Name::intern(name)) {}

    NodeKey add(const char* type) {
        const NodeKey key = graph_.allocate_key();
        good_ = good_ && graph_.add_node(key, Name::intern(type)).has_value();
        return key;
    }

    void symbol(NodeKey key, const char* text) {
        Literal literal;
        literal.type = Name::intern("name");
        literal.text = Name::intern(text);
        good_ = good_ && graph_.set_property(key, Name::intern("symbol"), literal).has_value();
    }

    void type_of(NodeKey key, ValueType type) {
        Literal literal;
        literal.type = Name::intern("name");
        literal.text = Name::intern(cy::rendering::material::value_type_name(type));
        good_ = good_ && graph_.set_property(key, Name::intern("type"), literal).has_value();
    }

    void value(NodeKey key, const char* property, cy::f32 x, cy::f32 y, cy::f32 z, cy::f32 w,
               u32 mask) {
        Literal literal;
        literal.type = Name::intern("vec4");
        literal.value = cy::graph::Immediate{x, y, z, w, mask};
        good_ = good_ && graph_.set_property(key, Name::intern(property), literal).has_value();
    }

    void flag(NodeKey key, const char* property) {
        Literal literal;
        literal.type = Name::intern("bool");
        literal.value.mask = 1;
        good_ = good_ && graph_.set_property(key, Name::intern(property), literal).has_value();
    }

    void wire(NodeKey from, NodeKey to, const char* pin) {
        good_ =
            good_ && graph_.connect(from, Name::intern("out"), to, Name::intern(pin)).has_value();
    }

    void mute(NodeKey key) { good_ = good_ && graph_.mute(key, true).has_value(); }

    [[nodiscard]] bool good() const { return good_; }
    [[nodiscard]] const Graph& graph() const { return graph_; }

private:
    Graph graph_;
    bool good_ = true;
};

/// The reference material, authored on a canvas.
///
/// Node for node and wart for wart the same material as
/// `cy::rendering::material::testing::build_reference_graph` — the untouched weight ports, the
/// pairwise closure sums, the texture dragged in twice, the reversed operand order, the tint left
/// at one, the muted emission and the orphan. The declarations come out of the nodes themselves,
/// which is what an editor has: a texture is declared because somebody dropped a sample node on the
/// canvas, not because a separate list was edited.
[[nodiscard]] inline bool author_reference(Canvas& canvas) {
    const u8 components[] = {0, 1, 2};
    const u32 swizzle_xyz =
        cy::rendering::material::Builder::swizzle_mask(cy::Span<const u8>(components, 3));
    const u8 first[] = {0};
    const u32 swizzle_x =
        cy::rendering::material::Builder::swizzle_mask(cy::Span<const u8>(first, 1));

    const NodeKey uv = canvas.add("material.attribute");
    canvas.symbol(uv, "uv0");
    canvas.type_of(uv, ValueType::Vec2);

    const NodeKey albedo_sample = canvas.add("material.texture_sample");
    canvas.symbol(albedo_sample, "base_color_map");
    canvas.type_of(albedo_sample, ValueType::Vec4);
    canvas.value(albedo_sample, "average", 0.5F, 0.5F, 0.5F, 1.0F, 0);
    canvas.wire(uv, albedo_sample, "uv");

    const NodeKey albedo_swizzle = canvas.add("material.swizzle");
    canvas.type_of(albedo_swizzle, ValueType::Count);
    canvas.value(albedo_swizzle, "value", 0.0F, 0.0F, 0.0F, 0.0F, swizzle_xyz);
    canvas.wire(albedo_sample, albedo_swizzle, "value");

    const NodeKey base_color = canvas.add("material.parameter");
    canvas.symbol(base_color, "base_color");
    canvas.type_of(base_color, ValueType::Vec3);
    canvas.value(base_color, "default", 0.82F, 0.78F, 0.74F, 0.0F, 0);

    const NodeKey albedo = canvas.add("material.multiply");
    canvas.wire(base_color, albedo, "a");
    canvas.wire(albedo_swizzle, albedo, "b");

    const NodeKey metallic = canvas.add("material.parameter");
    canvas.symbol(metallic, "metallic");
    canvas.type_of(metallic, ValueType::Float);
    canvas.value(metallic, "default", 1.0F, 0.0F, 0.0F, 0.0F, 0);

    const NodeKey one_minus = canvas.add("material.one_minus");
    canvas.wire(metallic, one_minus, "value");

    const NodeKey grime_sample = canvas.add("material.texture_sample");
    canvas.symbol(grime_sample, "grime_map");
    canvas.type_of(grime_sample, ValueType::Vec4);
    canvas.value(grime_sample, "average", 0.35F, 0.35F, 0.35F, 1.0F, 0);
    canvas.wire(uv, grime_sample, "uv");
    canvas.flag(grime_sample, "microdetail");

    const NodeKey grime = canvas.add("material.swizzle");
    canvas.type_of(grime, ValueType::Count);
    canvas.value(grime, "value", 0.0F, 0.0F, 0.0F, 0.0F, swizzle_x);
    canvas.wire(grime_sample, grime, "value");

    const NodeKey worn = canvas.add("material.multiply");
    canvas.wire(albedo, worn, "a");
    canvas.wire(one_minus, worn, "b");

    const NodeKey worn_scaled = canvas.add("material.multiply");
    canvas.wire(worn, worn_scaled, "a");
    canvas.wire(grime, worn_scaled, "b");

    const NodeKey tint = canvas.add("material.constant");
    canvas.type_of(tint, ValueType::Float);
    canvas.value(tint, "value", 1.0F, 0.0F, 0.0F, 0.0F, 0);

    const NodeKey tinted = canvas.add("material.multiply");
    canvas.wire(worn_scaled, tinted, "a");
    canvas.wire(tint, tinted, "b");

    const NodeKey diffuse = canvas.add("material.diffuse");
    canvas.wire(tinted, diffuse, "colour");

    const NodeKey second_sample = canvas.add("material.texture_sample");
    canvas.symbol(second_sample, "base_color_map");
    canvas.type_of(second_sample, ValueType::Vec4);
    canvas.value(second_sample, "average", 0.5F, 0.5F, 0.5F, 1.0F, 0);
    canvas.wire(uv, second_sample, "uv");

    const NodeKey second_swizzle = canvas.add("material.swizzle");
    canvas.type_of(second_swizzle, ValueType::Count);
    canvas.value(second_swizzle, "value", 0.0F, 0.0F, 0.0F, 0.0F, swizzle_xyz);
    canvas.wire(second_sample, second_swizzle, "value");

    const NodeKey second_albedo = canvas.add("material.multiply");
    canvas.wire(base_color, second_albedo, "a");
    canvas.wire(second_swizzle, second_albedo, "b");

    const NodeKey roughness = canvas.add("material.parameter");
    canvas.symbol(roughness, "roughness");
    canvas.type_of(roughness, ValueType::Float);
    canvas.value(roughness, "default", 0.35F, 0.0F, 0.0F, 0.0F, 0);

    const NodeKey specular = canvas.add("material.specular");
    canvas.wire(second_albedo, specular, "colour");
    canvas.wire(roughness, specular, "roughness");

    const NodeKey sum = canvas.add("material.add_closures");
    canvas.wire(diffuse, sum, "a");
    canvas.wire(specular, sum, "b");

    const NodeKey emissive = canvas.add("material.parameter");
    canvas.symbol(emissive, "emissive");
    canvas.type_of(emissive, ValueType::Vec3);
    canvas.value(emissive, "default", 1.0F, 0.4F, 0.1F, 0.0F, 0);

    const NodeKey emission = canvas.add("material.emission");
    canvas.wire(emissive, emission, "colour");
    canvas.mute(emission);

    const NodeKey outer_sum = canvas.add("material.add_closures");
    canvas.wire(sum, outer_sum, "a");
    canvas.wire(emission, outer_sum, "b");

    const NodeKey opacity = canvas.add("material.constant");
    canvas.type_of(opacity, ValueType::Float);
    canvas.value(opacity, "value", 1.0F, 0.0F, 0.0F, 0.0F, 0);

    const NodeKey orphan_constant = canvas.add("material.constant");
    canvas.type_of(orphan_constant, ValueType::Float);
    canvas.value(orphan_constant, "value", 2.0F, 0.0F, 0.0F, 0.0F, 0);
    const NodeKey orphan = canvas.add("material.multiply");
    canvas.wire(roughness, orphan, "a");
    canvas.wire(orphan_constant, orphan, "b");

    const NodeKey output = canvas.add("material.output");
    canvas.wire(outer_sum, output, "surface");
    canvas.wire(opacity, output, "opacity");
    return canvas.good();
}

}  // namespace cy::graph_material_test
