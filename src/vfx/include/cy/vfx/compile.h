#pragma once
// THE COMPILER: graph -> typed VFX IR -> optimisation -> Slang. M8.c tasks 2.1 and 2.2.
//
// ================================================================================================
// THE PIPELINE THE SPECIFICATION NAMES, STAGE FOR STAGE
// ================================================================================================
//
// `vfx-system`: "Compilation SHALL proceed: graph -> typed VFX IR -> optimisation -> Slang source
// -> the engine's existing shader compilation pipeline (see `shader-system`)."
//
//   graph              `cy::graph::Graph`, CyberGraph unchanged. `register_vfx_nodes` is the
//                      library it is authored from.
//   typed VFX IR       `ir.h`: an ordered write list whose values are on the shared expression
//                      core. Typed by `Builder::make`, SSA by construction.
//   optimisation       `cy::graph::optimise` to a fixed point, plus the two passes that are
//                      statements about the WRITE LIST rather than about the DAG and therefore
//                      cannot be the core's: ATTRIBUTE LIVENESS and KERNEL FUSION.
//   Slang source       `emit_slang` below. Not GLSL, not SPIR-V, not HLSL — "so VFX kernels reuse
//                      the engine's existing reflection, caching, hot-reload, and cross-backend
//                      translation rather than introducing a second shader toolchain".
//
// The optimiser is required to perform "at minimum: attribute liveness analysis, dead-code
// elimination, constant folding of parameters known at cook time, and kernel fusion of stages that
// can share a dispatch". Two of the four come from the shared core and two are here;
// `CompileReport` counts all four separately, so a claim that a pass ran is a number rather than a
// sentence.
//
// ================================================================================================
// WHY THE GENERATED UNIT IS SELF-CONTAINED
// ================================================================================================
//
// `emit_prelude` writes the attribute accessors, the parameter block, the interface samplers and
// the helper functions the kernel body calls, generated FROM THE MODULE'S OWN DECLARATIONS — which
// is the only place they can come from, because they are different for every effect. That is the
// same decision `src/rendering/material/`'s `emit_prelude` records for materials, and it is made
// here for a second reason as well: a unit that imports no authored module compiles through
// `cy::shader`'s Slang front end today, and one that imports a module which imports another does
// not (see this milestone's finding C). A generated kernel that needed a nested import would be a
// kernel blocked on a fix in `src/backends/shader/slang/`.
//
// **This was run, not asserted.** `integration.vfx_compiler` writes the assembled unit to
// `vfx-kernel.slang`; handed to the pinned Slang compiler as
// `slangc -target spirv -entry cyVfxKernel -stage compute vfx-kernel.slang` it produces SPIR-V and
// exits zero with no diagnostics. What is not yet in the tree is anything that DOES that
// automatically — see src/vfx/README.md, which says so where the evidence is.
//
// ================================================================================================
// A COMPILE ERROR NAMES A NODE AND A PIN
// ================================================================================================
//
// "WHEN a graph fails to compile THEN the error SHALL identify the offending node and pin, not only
// the generated source line." Every diagnostic this compiler reports goes through
// `cy::graph::DiagnosticSink` with a `NodeKey` and, where the failure is about a wire, a pin name.
// `CompiledEmitter::debug()` is the other direction: SSA value -> IR node -> authoring node.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/graph/emit.h>
#include <cy/graph/passes.h>
#include <cy/vfx/asset.h>
#include <cy/vfx/interfaces.h>
#include <cy/vfx/ir.h>
#include <cy/vfx/layout.h>

namespace cy::vfx {

using graph::DiagnosticSink;
using graph::GeneratedSource;
using graph::PassSwitches;

/// What the caller asks the compiler to do. Every switch is observable in the report, because a
/// switch nothing measures is a switch that becomes a placebo — which is the exact defect
/// `cy::graph::passes.h` records about the material spike's own `pass_dce`.
struct CompileOptions {
    /// The shared core's nine switches, forwarded unchanged.
    PassSwitches switches;
    /// Fuse stages that can share a dispatch. Off keeps one dispatch per stage, which is what makes
    /// `dispatches_before_fusion` and `dispatches_after_fusion` two different numbers.
    bool fuse_stages = true;
    /// Fold parameters the asset declares unexposed. Off reads every parameter from the buffer.
    bool fold_parameters = true;
    /// Drop attributes nothing reads. Off allocates every attribute any stage mentions.
    bool attribute_liveness = true;
    /// The per-emitter memory budget the reported maximum population is computed against.
    u64 memory_budget_bytes = 4ULL * 1024ULL * 1024ULL;
    /// The population an estimated cost is reported at. `vfx-system`: "estimated GPU cost at a
    /// REFERENCE POPULATION".
    u32 reference_population = 100000;
};

/// What cooking one emitter produced. `vfx-system`: "Cooking SHALL report per effect: kernel count,
/// per-particle size, estimated GPU cost at a reference population, and any features unsupported on
/// target platforms."
struct EmitterReport {
    Name emitter;
    u32 kernels = 0;
    u32 bytes_per_particle = 0;
    u32 max_population = 0;
    u32 attributes_allocated = 0;
    u32 attributes_elided = 0;
    u32 folded_parameters = 0;
    /// Dispatches the stages would need unfused, and after fusion. "reducing dispatch count" made
    /// into two numbers.
    u32 dispatches_before_fusion = 0;
    u32 dispatches_after_fusion = 0;
    /// From the shared core's own report.
    u32 nodes_before_optimisation = 0;
    u32 nodes_after_optimisation = 0;
    u32 folded_constants = 0;
    u32 merged_values = 0;
    u32 dropped_nodes = 0;
    /// Sum of `interface_cost_weight` over every sample the kernels perform.
    u32 sample_cost_weight = 0;
    /// The cost weight times the reference population. A relative number and labelled as one —
    /// this compiler does not have a device and will not pretend to have timed anything.
    u64 estimated_cost_units = 0;
    u32 generated_source_bytes = 0;
    SimulationPath path = SimulationPath::GpuPreferred;
};

/// What cooking one system produced.
struct CompileReport {
    explicit CompileReport(Allocator& allocator) noexcept : emitters(allocator) {}

    CompileReport(const CompileReport&) = delete;
    CompileReport& operator=(const CompileReport&) = delete;
    CompileReport(CompileReport&&) noexcept = default;
    CompileReport& operator=(CompileReport&&) noexcept = default;

    Array<EmitterReport> emitters;
    u32 kernels = 0;
    u32 total_bytes_per_particle = 0;
    /// True when any `PassSwitches` member is off: this is NOT the shipping program.
    bool bisection_build = false;
};

/// One cooked emitter: the layout, the kernels, the generated Slang, and the identity.
class CompiledEmitter {
public:
    CompiledEmitter(Allocator& allocator) noexcept;

    CompiledEmitter(const CompiledEmitter&) = delete;
    CompiledEmitter& operator=(const CompiledEmitter&) = delete;
    CompiledEmitter(CompiledEmitter&&) noexcept = default;
    CompiledEmitter& operator=(CompiledEmitter&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] const AttributeLayout& layout() const noexcept { return layout_; }
    [[nodiscard]] Span<const VfxKernel> kernels() const noexcept { return kernels_.span(); }
    [[nodiscard]] const VfxKernel* kernel_for(Stage stage) const noexcept;
    /// The generated Slang, one program per kernel and parallel to `kernels()`.
    [[nodiscard]] Span<const GeneratedSource> sources() const noexcept { return sources_.span(); }
    [[nodiscard]] usize source_bytes() const noexcept;
    [[nodiscard]] SimulationPath path() const noexcept { return path_; }
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }
    /// Over the kernels' digests, the layout's digest and the interface registry's digest.
    [[nodiscard]] u64 digest() const noexcept { return digest_; }

    [[nodiscard]] Allocator& allocator() const noexcept { return kernels_.allocator(); }

private:
    friend class CompiledAccess;

    Name name_;
    AttributeLayout layout_;
    Array<VfxKernel> kernels_;
    Array<GeneratedSource> sources_;
    SimulationPath path_ = SimulationPath::GpuPreferred;
    u32 capacity_ = 0;
    u64 digest_ = 0;
};

/// One cooked system. This is what the runtime loads, and it holds no graph — which is the
/// structural half of "the runtime SHALL contain no graph compiler".
class CompiledSystem {
public:
    CompiledSystem(Allocator& allocator) noexcept;

    CompiledSystem(const CompiledSystem&) = delete;
    CompiledSystem& operator=(const CompiledSystem&) = delete;
    CompiledSystem(CompiledSystem&&) noexcept = default;
    CompiledSystem& operator=(CompiledSystem&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] Span<const CompiledEmitter> emitters() const noexcept { return emitters_.span(); }
    [[nodiscard]] const CompiledEmitter* find_emitter(Name emitter) const noexcept;
    [[nodiscard]] Span<const ParameterDecl> parameters() const noexcept {
        return parameters_.span();
    }
    [[nodiscard]] Span<const EventChannelDecl> channels() const noexcept {
        return channels_.span();
    }
    [[nodiscard]] ImportanceClass importance() const noexcept { return importance_; }
    [[nodiscard]] const ScalabilityPolicy& scalability() const noexcept { return scalability_; }
    /// The content-addressed cook key: every emitter's digest, the parameters, the channels and the
    /// options the cook ran with. Two cooks that produce this key produce identical artefacts.
    [[nodiscard]] u64 cook_key() const noexcept { return cook_key_; }
    /// The largest population any one instance of this system can hold.
    [[nodiscard]] u32 capacity() const noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return emitters_.allocator(); }

private:
    friend class CompiledAccess;

    Name name_;
    Array<CompiledEmitter> emitters_;
    Array<ParameterDecl> parameters_;
    Array<EventChannelDecl> channels_;
    ImportanceClass importance_ = ImportanceClass::Ambient;
    ScalabilityPolicy scalability_;
    u64 cook_key_ = 0;
};

/// Cook one system. Diagnostics are node- and pin-precise; a failure returns the error AND leaves
/// the sink populated, because "which node" is the question an author actually has.
[[nodiscard]] Expected<CompiledSystem, Error> compile_system(
    const VfxSystemAsset& asset, const NodeRegistry& registry,
    const DataInterfaceRegistry& interfaces, const CompileOptions& options, DiagnosticSink& sink,
    CompileReport& report) noexcept;

// --- Slang emission ------------------------------------------------------------------------------

/// The compute entry point `assemble_translation_unit` appends, so a caller compiles it without
/// spelling a string twice.
inline constexpr const char* kVfxKernelEntryPoint = "cyVfxKernel";

/// The prelude a generated kernel needs to be a compilable translation unit on its own: the
/// attribute buffer accessors derived from the layout, the parameter block derived from the
/// declarations, one sampler per interface field the kernels read, and the helpers the body calls.
[[nodiscard]] Status emit_prelude(const CompiledEmitter& emitter,
                                  Span<const ParameterDecl> parameters,
                                  Span<const EventChannelDecl> channels, Array<char>& out) noexcept;

/// Prelude + the emitter's generated bodies + a compute entry point. A complete Slang translation
/// unit that imports nothing.
[[nodiscard]] Status assemble_translation_unit(const CompiledEmitter& emitter,
                                               Span<const ParameterDecl> parameters,
                                               Span<const EventChannelDecl> channels,
                                               Array<char>& out) noexcept;

}  // namespace cy::vfx
