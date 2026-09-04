#pragma once
// Reflection-driven binding: the layouts are read out of the compiled shader. Task 3.3.
//
// `shader-system` — "Reflection-driven binding": "Descriptor set layouts, push-constant ranges, and
// vertex input layouts SHALL be derived from shader reflection, not declared separately in C++",
// and its first scenario is the whole point: "Layout mismatch is impossible — when a shader's
// bindings change, the derived layout SHALL change with it, and no separate C++ declaration can
// drift."
//
// THIS IS THE SAME ARGUMENT THAT MADE M1'S TYPE REFLECTION GENERATED RATHER THAN MACRO-DECLARED. A
// hand-maintained table describing something else's contents is correct on the day it is written
// and wrong on some later day nobody can name. The fix is not discipline; it is to stop writing the
// table.
//
// SPIR-V IS THE SOURCE, NOT SLANG. Reflection is read from the compiled SPIR-V module rather than
// from the Slang front end, for three reasons. It is the interchange form (`shader-system`'s
// pipeline step 3 reads SPIR-V, not Slang). It means a shader that arrives as SPIR-V — from a cache
// tier, from a shipped library, or from a generator that already compiled it — reflects identically
// to one compiled locally. And it means the shipping runtime, which contains no Slang compiler,
// still has everything it needs to build a pipeline layout.
//
// The parser is the engine's own (src/spirv_parser.cpp). SPIRV-Reflect and SPIRV-Cross both do this
// and both are bigger than the part of it the engine uses; the subset that matters —
// OpEntryPoint, OpExecutionMode, the type graph, and the DescriptorSet/Binding/Location/SpecId
// decorations — is a few hundred lines against a stable, versioned binary format.
//
// --- THE DESCRIPTOR SET CONVENTION ---------------------------------------------------------------
//
// `shader-system` fixes it so that reflection results are predictable:
//
//   | Set | Contents                                                          | Update frequency |
//   |-----|-------------------------------------------------------------------|------------------|
//   | 0   | Global: frame constants, samplers, bindless arrays, shadow atlases | per frame        |
//   | 1   | View: camera matrices, view constants, cluster buffers            | per view         |
//   | 2   | Pass: pass-specific resources                                     | per pass         |
//   | 3   | Draw: material data and per-draw resources (unused when bindless)  | per draw         |
//
// `validate_set_convention()` is the cook-step check behind the "Convention violated" scenario: a
// per-frame resource bound in set 3 fails the cook with a diagnostic naming the convention. What it
// can check is structural — the set index exists, is one of the four, and a runtime-sized array
// (a bindless table) appears only in set 0 — because "this is a per-frame resource" is a fact about
// intent that no binary carries. Naming the convention in the diagnostic is what makes the rest of
// it reviewable.

#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/rhi/types.h>
#include <cy/backends/shader/diagnostics.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::shader {

// --- The descriptor set convention ------------------------------------------------------------

inline constexpr u32 kSetGlobal = 0;
inline constexpr u32 kSetView = 1;
inline constexpr u32 kSetPass = 2;
inline constexpr u32 kSetDraw = 3;
inline constexpr u32 kSetCount = 4;

/// How often the contents of a set change. The fourth column of the table above, as a value, so a
/// diagnostic and a descriptor allocator can both name it.
enum class UpdateFrequency : u8 { PerFrame = 0, PerView = 1, PerPass = 2, PerDraw = 3 };

const char* set_name(u32 set) noexcept;
const char* update_frequency_name(UpdateFrequency frequency) noexcept;
[[nodiscard]] constexpr UpdateFrequency frequency_of_set(u32 set) noexcept {
    return static_cast<UpdateFrequency>(set);
}

// --- What a module declares ---------------------------------------------------------------------

struct ReflectedBinding {
    u32 set = 0;
    u32 binding = 0;
    /// Elements in the array; 1 for a scalar binding. Zero means a runtime-sized array — a bindless
    /// table — which is why `rhi::DescriptorBinding::count` uses the same encoding.
    u32 count = 1;
    rhi::DescriptorKind kind = rhi::DescriptorKind::UniformBuffer;
    /// Every stage that names this binding. Filled by `merge`, which is how a vertex and a fragment
    /// module become one pipeline layout.
    rhi::ShaderStage stages = rhi::ShaderStage::None;
    /// The variable's name from the module's debug information, or empty in a stripped module. For
    /// diagnostics and for the editor; nothing binds by it.
    Name name;
    /// True when the binding is a runtime-sized array. Redundant with `count == 0` and kept because
    /// a reader of a report should not have to know that encoding.
    bool runtime_array = false;
};

struct ReflectedPushConstant {
    u32 offset = 0;
    u32 size = 0;
    rhi::ShaderStage stages = rhi::ShaderStage::None;
};

struct ReflectedVertexInput {
    u32 location = 0;
    rhi::Format format = rhi::Format::Undefined;
    Name name;
};

struct ReflectedSpecConstant {
    u32 id = 0;
    /// The value compiled into the module, which is what a pipeline gets if nothing overrides it.
    u32 default_value = 0;
    Name name;
};

struct ReflectedEntryPoint {
    Name name;
    rhi::ShaderStage stage = rhi::ShaderStage::None;
    /// Compute only, and zero elsewhere. `rhi-and-render-graph` names workgroup size as one of the
    /// five things reflection must extract, because a dispatch that guesses it is a dispatch that
    /// is wrong on the day the shader changes.
    u32 workgroup_size[3] = {0, 0, 0};
};

/// Everything one compiled module — or several merged — declares.
class Reflection {
public:
    explicit Reflection(Allocator& allocator) noexcept;

    Reflection(const Reflection&) = delete;
    Reflection& operator=(const Reflection&) = delete;
    Reflection(Reflection&&) noexcept = default;
    Reflection& operator=(Reflection&&) noexcept = default;

    [[nodiscard]] Span<const ReflectedBinding> bindings() const noexcept;
    [[nodiscard]] Span<const ReflectedPushConstant> push_constants() const noexcept;
    [[nodiscard]] Span<const ReflectedVertexInput> vertex_inputs() const noexcept;
    [[nodiscard]] Span<const ReflectedSpecConstant> spec_constants() const noexcept;
    [[nodiscard]] Span<const ReflectedEntryPoint> entry_points() const noexcept;

    /// Every stage present across the entry points.
    [[nodiscard]] rhi::ShaderStage stages() const noexcept { return stages_; }
    /// Instructions in the module, which is `shader-system`'s cheapest cost signal and the number
    /// its "Expensive shader is identified" scenario thresholds against.
    [[nodiscard]] u32 instruction_count() const noexcept { return instruction_count_; }
    [[nodiscard]] u32 spirv_version() const noexcept { return spirv_version_; }

    /// Fold another module's reflection into this one. Bindings that agree are unioned by stage
    /// mask; a binding that disagrees — same (set, binding), different kind or count — is a real
    /// defect and is reported, because it is a pipeline that would be created with a layout one of
    /// its stages does not accept.
    [[nodiscard]] Status merge(const Reflection& other, DiagnosticLog& diagnostics) noexcept;

    /// The bindings of one set, in ascending binding order — the shape `rhi::Device` wants for a
    /// descriptor set layout. Appended to `out`.
    [[nodiscard]] Status set_layout(u32 set, Array<rhi::DescriptorBinding>& out) const noexcept;
    /// The push-constant ranges, merged per stage mask. Appended to `out`.
    [[nodiscard]] Status push_constant_ranges(Array<rhi::PushConstantRange>& out) const noexcept;
    /// True when the module declares anything in `set`.
    [[nodiscard]] bool uses_set(u32 set) const noexcept;
    /// The lowest set index this reflection does *not* use, which is how many set layouts a
    /// pipeline layout needs.
    [[nodiscard]] u32 set_count() const noexcept;

    [[nodiscard]] const ReflectedEntryPoint* find_entry_point(Name name) const noexcept;

    void clear() noexcept;

    // Filled by the parser; public so that a back end that gets reflection from somewhere other
    // than SPIR-V can populate the same object rather than a parallel one.
    [[nodiscard]] Status add_binding(const ReflectedBinding& binding) noexcept;
    [[nodiscard]] Status add_push_constant(const ReflectedPushConstant& range) noexcept;
    [[nodiscard]] Status add_vertex_input(const ReflectedVertexInput& input) noexcept;
    [[nodiscard]] Status add_spec_constant(const ReflectedSpecConstant& constant) noexcept;
    [[nodiscard]] Status add_entry_point(const ReflectedEntryPoint& entry) noexcept;
    /// Stamp every binding and push-constant range with the stages the entry points declare.
    /// Called once, after both have been added — the entry points are known before the variables
    /// are walked, so the stamp cannot happen as each binding arrives.
    void assign_stages_to_resources() noexcept;
    void set_instruction_count(u32 count) noexcept { instruction_count_ = count; }
    void set_spirv_version(u32 version) noexcept { spirv_version_ = version; }

    /// Sort bindings, vertex inputs and specialization constants into their canonical order.
    ///
    /// Called by the parser once it has walked the module. Ordering is by (set, binding), by
    /// location, and by id — never by the order ids happened to appear in the binary, because a
    /// reflection that depends on the emitter's internal ordering is one that changes when the
    /// compiler is updated and takes every derived cache key with it.
    void sort() noexcept;

private:
    Array<ReflectedBinding> bindings_;
    Array<ReflectedPushConstant> push_constants_;
    Array<ReflectedVertexInput> vertex_inputs_;
    Array<ReflectedSpecConstant> spec_constants_;
    Array<ReflectedEntryPoint> entry_points_;
    rhi::ShaderStage stages_ = rhi::ShaderStage::None;
    u32 instruction_count_ = 0;
    u32 spirv_version_ = 0;
};

/// Read a SPIR-V module. `words` is the module as 32-bit words, in host order.
///
/// Fails on a bad magic number, a truncated instruction stream, or an id bound the module exceeds —
/// the three ways a buffer that is not SPIR-V, or is SPIR-V that was cut short, reaches this
/// function. It does not attempt full validation: that is `spirv-val`'s job in the cook step, and
/// duplicating it here would be a second, worse validator.
[[nodiscard]] Expected<Reflection, Error> reflect_spirv(Allocator& allocator,
                                                        Span<const u32> words) noexcept;

/// Check a reflection against the descriptor set convention. Returns true when it holds; every
/// violation is reported into `diagnostics` naming the convention and the offending binding.
[[nodiscard]] bool validate_set_convention(const Reflection& reflection, DiagnosticLog& diagnostics,
                                           std::string_view shader_name) noexcept;

}  // namespace cy::shader
