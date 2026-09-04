#pragma once
// The compilation pipeline, and the front ends that feed it. Tasks 3.1, 3.2 and 3.7.
//
// `shader-system` fixes the pipeline as five offline steps:
//
//   1. Slang source -> the Slang compiler -> SPIR-V, per entry point and permutation
//   2. SPIR-V -> validation and optimisation
//   3. SPIR-V -> reflection (bindings, push constants, vertex inputs, specialization constants,
//      workgroup size)
//   4. per backend: SPIR-V retained (Vulkan), or translated (MSL for Metal, DXIL for D3D12)
//   5. packaged into a shader library keyed by content hash
//
// Steps 1 and 2 belong to whichever front end is registered; step 3 is `reflection.h` and runs for
// every front end because it reads the interchange form rather than the compiler's own
// representation; steps 4 and 5 are `library.h` and `cache.h`. THE ENGINE DOES NOT AUTHOR A SHADING
// LANGUAGE AND DOES NOT WRITE A SHADER OPTIMISER — `thirdparty-dependencies` says so outright. What
// it owns is the pipeline these steps hang off, which is why this header describes a request and a
// result rather than a compiler.
//
// TWO FRONT ENDS ARE REGISTERED BY THIS MODULE ITSELF.
//
//   "slang"  the real one: Slang source -> SPIR-V, built only when CY_SHADER_SLANG is on. It lives
//            in src/backends/shader/slang/ and is the only directory in the engine that names a
//            Slang type.
//   "spirv"  the passthrough: the source unit already *is* a SPIR-V module, so there is nothing to
//            compile and everything still to reflect, cache, key and package. This is not a stub in
//            the fallback position. It is the shipping path — `shader-system`: "a shipping build
//            SHALL contain compiled backend-native shader artefacts and no Slang compiler" — and it
//            is what lets continuous integration exercise reflection, permutations, the cache, the
//            pipeline manifest and hot reload on a machine with no shader toolchain at all.
//
// The selection rule mirrors cy::rhi::create_device: ask for a name, fall back, and be told which
// one answered and why. That is the same shape `engine-architecture` requires of every backend
// choice in the engine, and repeating it here means a shader front end is configured the way a
// rendering backend is rather than in its own dialect.

#include <cy/backends/shader/diagnostics.h>
#include <cy/backends/shader/permutation.h>
#include <cy/backends/shader/reflection.h>
#include <cy/backends/shader/source.h>
#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>

namespace cy::shader {

/// The name the SPIR-V passthrough registers under, and the last link in the fallback chain.
inline constexpr const char* kSpirvBackendName = "spirv";
/// The name the Slang front end registers under when CY_SHADER_SLANG is on.
inline constexpr const char* kSlangBackendName = "slang";

/// How hard the front end is asked to work. Named rather than numbered because the third value is
/// not "more optimisation": a size-optimised module and a speed-optimised one are different
/// trade-offs, and a build that wants one should not have to know which integer means it.
enum class OptimizationLevel : u8 { None = 0, Size = 1, Performance = 2 };

/// `shader-system`: "Development builds SHALL support GPU shader debugging through RenderDoc, PIX
/// and Xcode by retaining debug information in non-shipping shader artefacts." Debug information is
/// therefore a property of the *artefact*, requested per compilation, and part of the cache key —
/// a stripped module and an annotated one are different bytes and must not share an entry.
enum class DebugInfoLevel : u8 { None = 0, LineTables = 1, Full = 2 };

/// The SPIR-V version to emit, as SPIR-V itself encodes it: (major << 16) | (minor << 8).
inline constexpr u32 kSpirv1_5 = (1U << 16) | (5U << 8);
inline constexpr u32 kSpirv1_6 = (1U << 16) | (6U << 8);

/// One compilation: one entry point, one permutation, one set of options.
struct CompileRequest {
    /// The module to compile. Authored or generated — nothing below this line can tell.
    SourceUnit source;
    /// The entry point within it. Slang names its own entry points, so this is not always "main".
    Name entry_point;
    rhi::ShaderStage stage = rhi::ShaderStage::None;

    /// The declared axes, and which variant this is. Null means the shader does not vary, which is
    /// the same thing as a set with no axes and is spelled as null so the common case allocates
    /// nothing.
    const PermutationSet* permutations = nullptr;
    PermutationKey permutation;

    OptimizationLevel optimization = OptimizationLevel::Performance;
    DebugInfoLevel debug_info = DebugInfoLevel::None;
    u32 spirv_version = kSpirv1_5;

    /// How the front end resolves an `import`. Usually `SourceRegistry::resolver()`.
    SourceResolver resolver;
};

/// What one compilation cost. `shader-system`'s "Shader diagnostics" requirement asks the build to
/// report per-shader compile time, permutation counts, and SPIR-V instruction counts; these are the
/// first two of the three, and `Reflection::instruction_count()` is the third.
struct CompileStats {
    u64 compile_ns = 0;
    u32 spirv_words = 0;
    u32 instruction_count = 0;
    /// Which front end produced it, for a report that mixes cache hits and fresh compilations.
    const char* backend = "";
};

/// A compiled entry point: the module, what it declares, and what it cost.
///
/// Move-only, like every owning container in the engine: a copy is `clone()` and says so at the
/// call site.
class CompiledShader {
public:
    explicit CompiledShader(Allocator& allocator) noexcept;

    CompiledShader(const CompiledShader&) = delete;
    CompiledShader& operator=(const CompiledShader&) = delete;
    CompiledShader(CompiledShader&&) noexcept = default;
    CompiledShader& operator=(CompiledShader&&) noexcept = default;

    [[nodiscard]] Span<const u32> spirv() const noexcept { return {spirv_.data(), spirv_.size()}; }
    [[nodiscard]] const Reflection& reflection() const noexcept { return reflection_; }
    [[nodiscard]] Reflection& reflection() noexcept { return reflection_; }
    [[nodiscard]] const CompileStats& stats() const noexcept { return stats_; }
    [[nodiscard]] CompileStats& stats() noexcept { return stats_; }
    /// BLAKE3 over the SPIR-V. What the library is keyed by, and what makes two materials that
    /// lower to the same code share one entry.
    [[nodiscard]] const assets::ContentHash& hash() const noexcept { return hash_; }
    [[nodiscard]] Name entry_point() const noexcept { return entry_point_; }
    [[nodiscard]] rhi::ShaderStage stage() const noexcept { return stage_; }

    /// Take ownership of a module, reflect it, and record its hash. The one way a `CompiledShader`
    /// is filled, so a front end cannot produce one whose reflection disagrees with its bytes.
    [[nodiscard]] Status adopt(Array<u32>&& spirv, Name entry_point, rhi::ShaderStage stage,
                               DiagnosticLog& diagnostics) noexcept;

    /// Restore a shader from a library artefact, where the code and the reflection were both
    /// stored and nothing is re-derived.
    ///
    /// This is the shipping path, and it is what keeps the library format correct for a payload
    /// that cannot be reflected at all — a Metal library, when M7 adds one. `adopt` is the
    /// compile-time path and derives the reflection from the bytes; the two must not be merged,
    /// because merging them would make the format's reflection records decorative.
    [[nodiscard]] Status restore(Array<u32>&& code, Reflection&& reflection, Name entry_point,
                                 rhi::ShaderStage stage, const CompileStats& stats,
                                 const assets::ContentHash& hash) noexcept;

    /// The `rhi::ShaderModuleDescription` this module would be created from. The name is borrowed
    /// from `entry_point`'s interned text and outlives any device call.
    [[nodiscard]] rhi::ShaderModuleDescription module_description() const noexcept;

    [[nodiscard]] Expected<CompiledShader, Error> clone(Allocator& allocator) const noexcept;

private:
    Array<u32> spirv_;
    Reflection reflection_;
    CompileStats stats_;
    assets::ContentHash hash_;
    Name entry_point_;
    rhi::ShaderStage stage_ = rhi::ShaderStage::None;
};

/// A shader front end. One per authoring path; there is exactly one authoring language.
class ShaderCompiler {
public:
    ShaderCompiler() noexcept = default;
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;
    virtual ~ShaderCompiler();

    /// The registered name: "slang", "spirv".
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// False when the front end cannot turn source text into a module — the SPIR-V passthrough's
    /// answer. A cook step checks it before reporting "no shader compiler in this build" rather
    /// than reporting a hundred identical compilation failures.
    [[nodiscard]] virtual bool compiles_source() const noexcept = 0;

    /// A version string that goes into the cache key. `shader-system`: "The cache key SHALL include
    /// ... the compiler version ... so a compiler change invalidates derived data without
    /// invalidating authored assets."
    [[nodiscard]] virtual const char* version() const noexcept = 0;

    /// Compile one entry point of one permutation. Diagnostics go into `diagnostics` whether it
    /// succeeds or not: a warning on a successful compile is exactly the thing a build report
    /// exists to surface.
    [[nodiscard]] virtual Expected<CompiledShader, Error> compile(
        const CompileRequest& request, DiagnosticLog& diagnostics) noexcept = 0;
};

// --- The registry ---------------------------------------------------------------------------

using CompilerFactory = Expected<ShaderCompiler*, Error> (*)(Allocator& allocator) noexcept;
using CompilerDestructor = void (*)(Allocator& allocator, ShaderCompiler* compiler) noexcept;

struct CompilerRegistration {
    const char* name = "";
    CompilerFactory create = nullptr;
    CompilerDestructor destroy = nullptr;
    /// Answered without constructing one, so selection skips a front end whose library is missing
    /// rather than reporting its construction failure as the reason nothing compiled. Null means
    /// "always available", which is the passthrough's answer.
    bool (*is_available)() noexcept = nullptr;
};

/// Register a front end. Idempotent by name, so a test can substitute one without unregistering.
Status register_compiler(const CompilerRegistration& registration) noexcept;
[[nodiscard]] Span<const CompilerRegistration> registered_compilers() noexcept;
[[nodiscard]] const CompilerRegistration* find_compiler(const char* name) noexcept;

/// How `create_compiler` chose. Reported rather than inferred: "asked for slang, ran the SPIR-V
/// passthrough" is the line a bug report needs and is not recoverable from the compiler alone.
struct CompilerSelection {
    const char* requested = "";
    const char* selected = "";
    bool fell_back = false;
    /// Why the requested front end was not used. Empty when it was.
    const char* reason = "";
};

/// Create a front end, falling back to the SPIR-V passthrough.
///
/// `requested` may be null or empty, meaning "the default": the first available registered front
/// end that can compile source, and the passthrough when there is none. A build with
/// CY_SHADER_SLANG off therefore needs no configuration change to run.
Expected<ShaderCompiler*, Error> create_compiler(Allocator& allocator, const char* requested,
                                                 CompilerSelection& selection) noexcept;
void destroy_compiler(Allocator& allocator, ShaderCompiler* compiler) noexcept;

/// Register the SPIR-V passthrough explicitly. It registers itself when this module is linked; this
/// exists so a host can make it a statement rather than a link-order property.
Status register_spirv_backend() noexcept;

}  // namespace cy::shader
