#pragma once
// The effect every VFX suite uses, and the graph builder that authors it. M8.c section 2.
//
// ONE EFFECT, THREE SUITES. `integration.vfx_compiler` asks what was COMPILED, `integration.vfx`
// asks what was SIMULATED, and `render.vfx` asks what was DRAWN — all of the same asset, for the
// reason tests/render/README.md gives about golden images: a second scene drifts from the first
// inside a milestone, and then two suites disagree about a defect neither of them can see.
//
// The effect is a spark plume, and every part of it is there to make something checkable:
//
//   * `velocity` is written by Initialise and read by Update, so KERNEL FUSION has something to
//     substitute and `dispatches_before_fusion` and `after` are two different numbers.
//   * `scratch` is written and never read by anything, so ATTRIBUTE LIVENESS has something to
//     elide — and the generated Slang has a store that is not emitted.
//   * `color` is declared with a tolerance of 1/255 over [0, 1], so PRECISION SELECTION has a
//     reason to choose `Unorm8`, and reading it back through the layout comes back quantised.
//   * `gravity` is an unexposed parameter, so CONSTANT FOLDING has a parameter to fold.
//   * `intensity` is an exposed one, so the folder has something it must NOT fold.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/graph/cybergraph.h>
#include <cy/vfx/asset.h>
#include <cy/vfx/compile.h>

namespace cy::vfx_test {

using namespace cy::vfx;
using cy::graph::Graph;
using cy::graph::Literal;
using cy::graph::NodeKey;
using cy::graph::NodeRegistry;

/// A terse way to author a stage graph. Every method returns the key it made, so a graph is a
/// straight line of assignments rather than a page of `add_node` and `connect` pairs.
class StageBuilder {
public:
    StageBuilder(Allocator& allocator, const char* name) noexcept;

    [[nodiscard]] NodeKey constant(f32 x, f32 y = 0.0F, f32 z = 0.0F, f32 w = 0.0F,
                                   const char* type = "float") noexcept;
    [[nodiscard]] NodeKey attribute(const char* name) noexcept;
    [[nodiscard]] NodeKey parameter(const char* name) noexcept;
    [[nodiscard]] NodeKey input(const char* name) noexcept;
    [[nodiscard]] NodeKey random() noexcept;
    [[nodiscard]] NodeKey sample(const char* interface_name, const char* field,
                                 NodeKey argument) noexcept;
    [[nodiscard]] NodeKey unary(const char* type, NodeKey x) noexcept;
    [[nodiscard]] NodeKey binary(const char* type, NodeKey a, NodeKey b) noexcept;
    [[nodiscard]] NodeKey ternary(const char* type, NodeKey a, NodeKey b, NodeKey c) noexcept;
    [[nodiscard]] NodeKey make3(NodeKey x, NodeKey y, NodeKey z) noexcept;
    [[nodiscard]] NodeKey make4(NodeKey x, NodeKey y, NodeKey z, NodeKey w) noexcept;

    void write(const char* attribute_name, NodeKey value) noexcept;
    void kill_if(NodeKey predicate) noexcept;
    void emit_event(const char* channel, NodeKey predicate) noexcept;
    void spawn_count(NodeKey value) noexcept;

    /// True when every call above succeeded. A builder that swallowed a failure would author a
    /// graph missing a wire and the suite would blame the compiler.
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] Graph& graph() noexcept { return graph_; }
    [[nodiscard]] Graph&& take() noexcept;

private:
    [[nodiscard]] NodeKey add(const char* type) noexcept;
    void set_text(NodeKey key, const char* property, const char* text) noexcept;
    void wire(NodeKey from, NodeKey to, const char* pin) noexcept;

    Graph graph_;
    bool ok_ = true;
};

/// The spark plume. `emitters` is how many copies of the same emitter the system carries, which is
/// what gives the scheduler something to merge.
[[nodiscard]] Expected<vfx::VfxSystemAsset, Error> build_plume(Allocator& allocator,
                                                               u32 emitters = 1,
                                                               u32 capacity = 512) noexcept;

/// Register the node library and the data interfaces one cook needs.
[[nodiscard]] Status prepare(NodeRegistry& registry,
                             vfx::DataInterfaceRegistry& interfaces) noexcept;

/// Cook the plume with the shipping options. Diagnostics land in `sink`.
[[nodiscard]] Expected<vfx::CompiledSystem, Error> cook_plume(
    Allocator& allocator, graph::DiagnosticSink& sink, vfx::CompileReport& report,
    const vfx::CompileOptions& options, u32 emitters = 1, u32 capacity = 512) noexcept;

}  // namespace cy::vfx_test
