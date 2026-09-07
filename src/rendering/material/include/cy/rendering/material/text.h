#pragma once
// The text material front-end. M7 task 6.1.
//
// The second of the two authoring forms `material-compiler` requires, and the other half of M7's
// exit criterion: "a graph and a hand-written material produce identical programs".
//
// ================================================================================================
// IT IS WRITTEN THE WAY A PERSON WRITES, WHICH IS THE POINT
// ================================================================================================
//
// design.md §1.1: the two front-ends "were deliberately written to be as unalike as two real
// authoring paths are. The graph emits what an editor emits ... The text emits what a person
// writes: a `let`, a literal, no redundant weight. They are equal only *after* the compiler runs."
//
// So this front-end has no weight ports, no muted nodes and no disconnected ones. It has `let`
// bindings, infix arithmetic, and `1 - metallic` written as a subtraction — which the builder
// canonicalises into the one spelling the graph's "one minus" node also produces (decision 7).
//
// A material reads:
//
//     material worn_metal {
//         param base_color : float3 = (0.82, 0.78, 0.74);
//         param metallic   : float  = 1.0;
//         param roughness  : float  = 0.35;
//         texture base_color_map average (0.5, 0.5, 0.5, 1.0);
//         attribute uv0 : float2;
//
//         let albedo = sample(base_color_map, uv0).xyz * base_color;
//         surface = diffuse(albedo * (1 - metallic)) + specular(albedo, roughness);
//         opacity = 1.0;
//     }
//
// `+` between two closures is a closure sum and `*` between a closure and a float is a closure
// weight, so the notation an author reaches for is the notation the closure model wants. Everything
// else is ordinary arithmetic.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/ir.h>
#include <cy/rendering/material/passes.h>

#include <string_view>

namespace cy::rendering::material {

/// Where a parse stopped and why.
///
/// A SEPARATE STRUCTURE RATHER THAN THE `Error`'s MESSAGE, because `cy::Error::message` is a
/// pointer to something that outlives it and a parse error is about text that does not. It is also
/// the shape `material-compiler`'s "a diagnostic naming the responsible node or parameter" wants.
struct ParseDiagnostic {
    explicit ParseDiagnostic(Allocator& allocator) noexcept : message(allocator) {}

    ParseDiagnostic(const ParseDiagnostic&) = delete;
    ParseDiagnostic& operator=(const ParseDiagnostic&) = delete;

    u32 line = 0;
    u32 column = 0;
    Array<char> message;

    [[nodiscard]] std::string_view text() const noexcept {
        return {message.data(), message.size()};
    }
    [[nodiscard]] bool empty() const noexcept { return message.empty(); }
};

/// Parse a text material definition into the IR.
///
/// The module comes back un-optimised, exactly as `lower_graph` returns one: running the pipeline
/// is the caller's, so that both front-ends are compared before AND after it.
[[nodiscard]] Expected<Module, Error> parse_material(std::string_view source, Allocator& allocator,
                                                     ParseDiagnostic& diagnostic,
                                                     const PassSwitches& switches = {}) noexcept;

}  // namespace cy::rendering::material
