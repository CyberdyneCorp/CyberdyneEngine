#pragma once
// EVERY STAGE OF A MATERIAL'S LOWERING, READABLE. M11.c task 1.3.
//
// `shader-system` — "Visual material editor": "The editor SHALL be able to show, for any material,
// each stage of its lowering: the graph, the material IR before and after optimisation, the
// generated Slang, and the compiled backend output." And `material-compiler` — "IR is inspectable":
// "WHEN a material behaves unexpectedly THEN its IR SHALL be dumpable in a readable form, before
// and after optimisation."
//
// ================================================================================================
// WHAT WAS ACTUALLY MISSING, READ OFF THE TREE RATHER THAN ASSUMED
// ================================================================================================
//
// FOUR of the five stages existed as DATA and NONE of them could be read. `MaterialGraph` holds
// nodes and links; `Module` holds nodes, operands and two side tables; `GeneratedSource` holds
// text; `CompiledProgram` holds a cost report. There was no function anywhere in this tree that
// turned a `Module` into something a person can look at — so "its IR SHALL be dumpable in a
// readable form" was a requirement with no implementation, and the editor's stage list was a panel
// with nothing to put in it.
//
// So this file is two things and the split matters:
//
//   * `dump_graph` and `dump_module`, which are the readable forms that did not exist; and
//   * `inspect_lowering`, which runs the compiler once and collects the stages IN ORDER, so that a
//     front end and a command line show the same five things about the same material rather than
//     each assembling their own list.
//
// ================================================================================================
// WHY THE FIFTH STAGE IS ATTACHED RATHER THAN PRODUCED
// ================================================================================================
//
// The compiled backend output is `shader-system`'s, not this module's — `material-compiler` says so
// in as many words ("The engine SHALL NOT implement a shader optimiser or a backend code
// generator") and this target deliberately does not link the shader toolchain. So
// `inspect_lowering` produces the four stages the material compiler owns and leaves the fifth
// `available = false` WITH A REASON, and `cy::rendering-material-slang`'s `attach_backend_stage`
// fills it in from a real compilation.
//
// AN INSPECTION WITH FOUR STAGES SAYS SO. `complete()` is false until the fifth is attached, and
// that is the whole reason it exists: a panel that showed four stages and called it "every stage"
// would be the same defect as a preview drawn by a second renderer — an observation nobody can tell
// from the real one.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/compiler.h>
#include <cy/rendering/material/graph.h>

#include <string_view>

namespace cy::rendering::material {

/// The stages of a material's lowering, in the order the specification enumerates them.
///
/// The enumerator ORDER is the requirement's order and is load-bearing: `inspect_lowering` fills
/// the array by index, and a stage list presented out of order is a stage list that has stopped
/// describing a pipeline.
enum class LoweringStage : u8 {
    /// The authored node graph: nodes, ports, wires, mutes — including the disconnected ones, which
    /// is what makes "the editor SHALL be able to show which nodes were dropped" answerable.
    Graph = 0,
    /// The IR a front end produced, before any pass has run.
    AuthoredIr = 1,
    /// The IR after the optimisation pipeline reached its fixed point.
    OptimisedIr = 2,
    /// The Slang the emitter wrote. Byte for byte what is compiled — not a rendering of it.
    GeneratedSlang = 3,
    /// What the shader toolchain made of it. Attached by `cy::rendering-material-slang`.
    CompiledProgram = 4,
    Count = 5,
};

inline constexpr u32 kLoweringStageCount = static_cast<u32>(LoweringStage::Count);

[[nodiscard]] const char* lowering_stage_name(LoweringStage stage) noexcept;

/// One stage, as text and as an identity.
struct StageDump {
    explicit StageDump(Allocator& allocator) noexcept : text(allocator) {}

    StageDump(const StageDump&) = delete;
    StageDump& operator=(const StageDump&) = delete;
    StageDump(StageDump&&) noexcept = default;
    StageDump& operator=(StageDump&&) noexcept = default;

    LoweringStage stage = LoweringStage::Graph;
    /// True when this stage was produced. False carries `reason`, and never an empty dump that a
    /// reader would mistake for an empty stage.
    bool available = false;
    /// Why it was not produced. A string literal, so it outlives the inspection.
    const char* reason = "";
    Array<char> text;
    /// A content digest over `text`, so two inspections of one material can be compared without
    /// diffing kilobytes — which is what a front end and a command line comparing notes need.
    u64 digest = 0;

    [[nodiscard]] std::string_view view() const noexcept { return {text.data(), text.size()}; }
};

/// Every stage of one program's lowering.
class LoweringInspection {
public:
    explicit LoweringInspection(Allocator& allocator) noexcept;

    LoweringInspection(const LoweringInspection&) = delete;
    LoweringInspection& operator=(const LoweringInspection&) = delete;
    LoweringInspection(LoweringInspection&&) noexcept = default;
    LoweringInspection& operator=(LoweringInspection&&) noexcept = default;

    [[nodiscard]] Span<const StageDump> stages() const noexcept { return stages_.span(); }
    [[nodiscard]] const StageDump& stage(LoweringStage which) const noexcept {
        return stages_[static_cast<usize>(which)];
    }
    [[nodiscard]] StageDump& stage(LoweringStage which) noexcept {
        return stages_[static_cast<usize>(which)];
    }

    /// True when every stage the specification enumerates was produced. See the header note.
    [[nodiscard]] bool complete() const noexcept;
    /// How many stages were produced. What a report prints beside `kLoweringStageCount`.
    [[nodiscard]] u32 available() const noexcept;

    ProgramKind kind = ProgramKind::Primary;
    QualityTier tier = QualityTier::High;
    u64 cook_key = 0;

private:
    Array<StageDump> stages_;
};

/// The authored graph in a readable form: declarations, then one line per node with its ports.
[[nodiscard]] Status dump_graph(const MaterialGraph& graph, Array<char>& out) noexcept;

/// A module in a readable form: declarations, roots, then one line per value in CANONICAL ORDER
/// with its op, type, operands and the authoring nodes it came from.
///
/// Canonical order and not node order, deliberately: node ids are construction order, and a dump
/// ordered by them would differ between two front-ends that produced the same material — which is
/// exactly the comparison this dump exists to make possible.
[[nodiscard]] Status dump_module(const Module& module, Array<char>& out) noexcept;

/// Compile `graph` and collect every stage of the named program's lowering.
///
/// One compilation, not five: the stages are what the pipeline passed through, so re-running the
/// compiler per stage would be showing five things that were never in sequence.
[[nodiscard]] Expected<LoweringInspection, Error> inspect_lowering(const MaterialGraph& graph,
                                                                   const CompileOptions& options,
                                                                   ProgramKind kind,
                                                                   QualityTier tier,
                                                                   Allocator& allocator) noexcept;

/// The same, for a material already compiled from text. `authored` is the un-optimised module a
/// front end produced; `source` is the text it was parsed from, which stands in for the graph.
[[nodiscard]] Expected<LoweringInspection, Error> inspect_lowering(
    std::string_view source, const Module& authored, const CompileOptions& options,
    ProgramKind kind, QualityTier tier, Allocator& allocator) noexcept;

}  // namespace cy::rendering::material
