#ifndef CY_SHADER_REFLECTION_H
#define CY_SHADER_REFLECTION_H
// What a compiled shader says about itself, and the binding layout derived from it. Task 3.3.
//
// --- THE INVARIANT ---------------------------------------------------------------------------------
//
// `shader-system` — "Reflection-driven binding": *descriptor set layouts, push-constant ranges, and
// vertex input layouts SHALL be derived from shader reflection, not declared separately in C++.*
//
// This is the same argument that made M1's reflection generated rather than macro-declared, and it
// is worth restating because it is easy to lose. A hand-maintained binding table is a second
// description of the shader. Two descriptions of one thing drift, and the drift is silent: the
// shader compiles, the layout builds, the descriptor writes succeed, and the wrong buffer is bound.
// There is **no type in this module that a human fills in with set and binding numbers.** A
// `PipelineLayoutDesc` can only be produced by `derive_layout()`, from a `Reflection` that can only
// be produced by parsing a compiled artefact. That is what "cannot drift" has to mean.
//
// --- THE DESCRIPTOR SET CONVENTION -----------------------------------------------------------------
//
// `shader-system` fixes four sets so that reflection results are predictable:
//
//   | Set | Contents                                                          | Update frequency |
//   |-----|-------------------------------------------------------------------|------------------|
//   | 0   | Global: frame constants, samplers, bindless arrays, shadow atlases | per frame        |
//   | 1   | View: camera matrices, view constants, cluster buffers             | per view         |
//   | 2   | Pass: pass-specific resources                                     | per pass         |
//   | 3   | Draw: material data and per-draw resources (unused when bindless)  | per draw         |
//
// `validate_convention()` is the "Convention violated" scenario made executable: a shader that binds
// a per-frame resource in set 3 fails the cook with a diagnostic naming the convention. A convention
// is only enforceable if the engine knows which resources are per-frame, so the engine *reserves the
// names*: a binding called `cy_frame`, `cy_globals` or `cy_samplers` belongs in set 0 wherever it
// appears, and one called `cy_view` belongs in set 1. Those names are declared in this file and
// declared in the Slang standard library (`src/rendering/shaders/cy/globals.slang`), and the
// validator is what keeps the two in agreement.
//
// --- WHAT IS NOT HERE ------------------------------------------------------------------------------
//
// No `VkDescriptorSetLayout`, no `VkDescriptorType`, no Vulkan anything. `derive_layout()` produces
// an engine-owned description; turning that into a device object is the RHI's, one layer of
// abstraction lower and inside `src/backends/rhi/`. That split is what lets the whole of reflection
// and layout derivation be tested with no GPU and no device, which is what CI has.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/shader/shader.h>

namespace cy::shader {

/// The kind of a descriptor binding, in the vocabulary reflection can actually distinguish.
///
/// These are SPIR-V's distinctions, not Vulkan's: a `SampledImage` is an `OpTypeImage` with
/// `Sampled == 1`, a `StorageImage` one with `Sampled == 2`, and a uniform buffer is a `Block`-
/// decorated struct in the `Uniform` storage class. The mapping onto a `VkDescriptorType` is one
/// table in the RHI, and it is a total function — which is the property that makes it safe.
enum class BindingKind : u8 {
    UniformBuffer = 0,
    StorageBuffer = 1,
    SampledImage = 2,
    StorageImage = 3,
    Sampler = 4,
    CombinedImageSampler = 5,
    UniformTexelBuffer = 6,
    StorageTexelBuffer = 7,
    InputAttachment = 8,
    AccelerationStructure = 9,
};

const char* binding_kind_name(BindingKind kind) noexcept;

/// The four sets the convention fixes. `Count` is not a set; it is the bound a validator checks.
enum class DescriptorSet : u8 {
    Global = 0,
    View = 1,
    Pass = 2,
    Draw = 3,
};

inline constexpr u32 kDescriptorSetCount = 4;

const char* descriptor_set_name(DescriptorSet set) noexcept;
/// What the set is updated at — the fourth column of the convention table, for a diagnostic.
const char* descriptor_set_frequency(DescriptorSet set) noexcept;

/// Names the engine reserves, each pinned to the set the convention puts it in.
///
/// Reserved rather than merely conventional: these are the blocks the engine itself writes, so a
/// user shader that declares one in a different set is not expressing a preference, it is describing
/// a program that cannot be bound. Every name begins `cy_`, which is also the rule that keeps a
/// user's own uniform out of this table.
struct ReservedBinding {
    const char* name;
    DescriptorSet set;
};

/// The reserved table, in a stable order. Small enough to scan linearly, and scanned once per
/// binding at cook time rather than per draw.
Span<const ReservedBinding> reserved_bindings() noexcept;

/// One descriptor binding, as reflection found it.
struct BindingReflection {
    ShortName name;
    u32 set = 0;
    u32 binding = 0;
    BindingKind kind = BindingKind::UniformBuffer;
    /// Array length. **Zero means a runtime array** — a bindless table, declared in Slang without a
    /// bound and sized by the descriptor pool. One means a scalar binding.
    u32 count = 1;
    /// Every stage in the program that references it.
    StageMask stages = StageMask::None;
    /// Byte size of a uniform or storage block, when the type is one and its size is static.
    /// Zero for an image, a sampler, or a block ending in a runtime array.
    u32 block_size = 0;
};

/// A push-constant range. Vulkan allows one block per stage; reflection reports the union.
struct PushConstantReflection {
    ShortName name;
    u32 offset = 0;
    u32 size = 0;
    StageMask stages = StageMask::None;
};

/// A vertex attribute, as the vertex stage declares it.
///
/// `format` is the SPIR-V component type and count — `Float32x3`, `Uint32x4` — not a Vulkan format.
/// The RHI maps it; the vertex buffer layout the mesh system produces is checked against it, which
/// is how "the mesh does not match the shader" becomes a cook-time diagnostic instead of a
/// validation-layer message.
enum class VertexFormat : u8 {
    Unknown = 0,
    Float32 = 1,
    Float32x2 = 2,
    Float32x3 = 3,
    Float32x4 = 4,
    Sint32 = 5,
    Sint32x2 = 6,
    Sint32x3 = 7,
    Sint32x4 = 8,
    Uint32 = 9,
    Uint32x2 = 10,
    Uint32x3 = 11,
    Uint32x4 = 12,
};

const char* vertex_format_name(VertexFormat format) noexcept;
/// Bytes one attribute of this format occupies.
[[nodiscard]] u32 vertex_format_size(VertexFormat format) noexcept;

struct VertexInputReflection {
    ShortName name;
    u32 location = 0;
    VertexFormat format = VertexFormat::Unknown;
};

/// A specialization constant: the mechanism the specification prefers over a preprocessor
/// permutation, and therefore the one whose reflection has to be exact.
struct SpecializationConstantReflection {
    ShortName name;
    /// The SPIR-V `SpecId` decoration. This is what a pipeline's specialization info keys on.
    u32 constant_id = 0;
    /// Bytes the constant occupies: 4 for a 32-bit scalar, 1 for a bool as SPIR-V encodes it.
    u32 size = 4;
    /// The value compiled into the module, used when the pipeline does not override it.
    u64 default_value = 0;
    bool is_boolean = false;
};

/// A fragment output, so a pass can check its render-target count against the shader's.
struct FragmentOutputReflection {
    ShortName name;
    u32 location = 0;
    VertexFormat format = VertexFormat::Unknown;
};

/// One entry point in the module.
struct EntryPointReflection {
    ShortName name;
    Stage stage = Stage::Vertex;
    /// Compute, task and mesh only. `(0,0,0)` when the stage has no workgroup, and when the size is
    /// itself specialized — in which case the specialization constants that drive it are in
    /// `workgroup_spec_ids`, which is what lets a quality setting change the group size.
    u32 workgroup[3] = {0, 0, 0};
    u32 workgroup_spec_ids[3] = {0, 0, 0};
};

/// Everything a compiled module reports about itself.
///
/// Owned storage, because the SPIR-V blob it was parsed from is discarded once the artefact is
/// cached and the reflection outlives it: a pipeline created three frames later still needs to know
/// what to bind.
struct Reflection {
    explicit Reflection(Allocator& allocator) noexcept;

    Reflection(const Reflection&) = delete;
    Reflection& operator=(const Reflection&) = delete;
    Reflection(Reflection&&) noexcept = default;
    Reflection& operator=(Reflection&&) noexcept = default;

    Array<EntryPointReflection> entry_points;
    Array<BindingReflection> bindings;
    Array<PushConstantReflection> push_constants;
    Array<VertexInputReflection> vertex_inputs;
    Array<FragmentOutputReflection> fragment_outputs;
    Array<SpecializationConstantReflection> specialization_constants;

    /// Instructions in the module. `shader-system`'s diagnostics requirement asks for SPIR-V
    /// instruction counts in the build report, and it is the one cost proxy available without a
    /// vendor tool.
    u32 instruction_count = 0;
    /// The SPIR-V version the module declares, as `(major << 16) | (minor << 8)`.
    u32 spirv_version = 0;

    /// The binding with this set and index, or null.
    [[nodiscard]] const BindingReflection* find_binding(u32 set, u32 binding) const noexcept;
    /// The binding with this name, or null.
    [[nodiscard]] const BindingReflection* find_binding(std::string_view name) const noexcept;
    [[nodiscard]] const EntryPointReflection* find_entry_point(
        std::string_view name) const noexcept;

    /// Sort every array into a canonical order — bindings by (set, binding), inputs by location,
    /// constants by id, entry points by name. Called by the parser before it returns.
    ///
    /// **Determinism, not tidiness.** `rendering-architecture` requires deterministic submission and
    /// design.md §6 spells out why; a reflection whose binding order came from the order SPIR-V
    /// happened to declare variables would put that non-determinism into every derived layout, every
    /// cache key computed over one, and every golden image that depends on a descriptor write order.
    void canonicalise() noexcept;
};

/// One set's derived layout: what the RHI needs to create a descriptor set layout.
struct DescriptorSetLayoutDesc {
    u32 set = 0;
    /// Bindings in ascending binding order — `canonicalise()` guarantees it.
    Array<BindingReflection> bindings;
    /// True when any binding in the set is a runtime array, which is what makes the set a bindless
    /// one and changes how it is allocated.
    bool has_runtime_array = false;

    explicit DescriptorSetLayoutDesc(Allocator& allocator) noexcept;
    DescriptorSetLayoutDesc(const DescriptorSetLayoutDesc&) = delete;
    DescriptorSetLayoutDesc& operator=(const DescriptorSetLayoutDesc&) = delete;
    DescriptorSetLayoutDesc(DescriptorSetLayoutDesc&&) noexcept = default;
    DescriptorSetLayoutDesc& operator=(DescriptorSetLayoutDesc&&) noexcept = default;
};

/// The whole program's binding layout: four sets and the push-constant ranges.
///
/// **There is no public constructor that takes bindings.** The only way to obtain one is
/// `derive_layout()`, and that is the invariant this header exists to hold.
class PipelineLayoutDesc {
public:
    explicit PipelineLayoutDesc(Allocator& allocator) noexcept;

    PipelineLayoutDesc(const PipelineLayoutDesc&) = delete;
    PipelineLayoutDesc& operator=(const PipelineLayoutDesc&) = delete;
    PipelineLayoutDesc(PipelineLayoutDesc&&) noexcept = default;
    PipelineLayoutDesc& operator=(PipelineLayoutDesc&&) noexcept = default;

    [[nodiscard]] const DescriptorSetLayoutDesc& set(DescriptorSet which) const noexcept {
        return sets_[static_cast<u32>(which)];
    }
    [[nodiscard]] Span<const PushConstantReflection> push_constants() const noexcept {
        return push_constants_.span();
    }
    /// True when a set has no bindings at all. An empty set is still created — Vulkan requires set
    /// numbers to be contiguous in a pipeline layout — but knowing it is empty is what lets the
    /// renderer skip binding it.
    [[nodiscard]] bool set_is_empty(DescriptorSet which) const noexcept;
    /// A hash over the whole layout. Two programs with equal hashes share a pipeline layout object,
    /// which is most of them: every forward pass has the same set 0 and set 1.
    [[nodiscard]] ContentHash hash() const noexcept;

private:
    friend Expected<PipelineLayoutDesc, Error> derive_layout(const Reflection&,
                                                             Allocator&) noexcept;

    Array<DescriptorSetLayoutDesc> sets_;
    Array<PushConstantReflection> push_constants_;
};

/// Derive the binding layout from reflection. The only way to make a `PipelineLayoutDesc`.
///
/// Fails when the reflection violates the convention — see `validate_convention()`, which this
/// calls — and when two entry points disagree about a binding: the same (set, binding) declared as
/// two different kinds is a program that cannot be bound, and a layout that silently picked one
/// would fail at the descriptor write instead, three layers away from the mistake.
[[nodiscard]] Expected<PipelineLayoutDesc, Error> derive_layout(const Reflection& reflection,
                                                                Allocator& allocator) noexcept;

/// What a convention check found. A violation names the binding, the set it is in, and the set the
/// convention puts it in, because a diagnostic that says "convention violated" and stops is one
/// nobody can act on.
struct ConventionViolation {
    ShortName binding;
    u32 set = 0;
    u32 expected_set = 0;
    /// Filled by `format_violation()`; a static string, never allocated.
    const char* reason = "";
};

/// Check the reflection against the descriptor set convention.
///
/// Three rules, all of them from `shader-system`'s table:
///   * a set index above 3 does not exist,
///   * a reserved `cy_` name belongs in the set the table pins it to,
///   * a runtime array — a bindless table — belongs in set 0, because a per-draw bindless table is
///     a contradiction: the point of bindless is that the set is not rebound per draw.
///
/// Returns the number of violations written to `out`. Zero is a shader that passes the cook.
[[nodiscard]] u32 validate_convention(const Reflection& reflection,
                                      Array<ConventionViolation>& out) noexcept;

/// A one-line diagnostic for a violation, into `out`. Returns the length written.
[[nodiscard]] usize format_violation(const ConventionViolation& violation, char* out,
                                     usize capacity) noexcept;

}  // namespace cy::shader

#endif  // CY_SHADER_REFLECTION_H
