#ifndef CY_SHADER_COMPILER_H
#define CY_SHADER_COMPILER_H
// The compilation pipeline, and the interface the third-party compiler sits behind. Task 3.2.
//
// `shader-system` — "Compilation pipeline" is five steps, and they are worth keeping in front of
// the reader because this file is where four of them are ordered:
//
//   1. Slang source → Slang compiler → SPIR-V per entry point and permutation   (`ShaderCompiler`)
//   2. SPIR-V → validation and optimisation                                     (the compiler's own)
//   3. SPIR-V → reflection                                                      (`spirv.h`)
//   4. Per backend: SPIR-V retained, or cross-compiled                          (M11)
//   5. Package into a shader library keyed by content hash                      (`library.h`)
//
// **The engine owns none of step 1 or 2.** `thirdparty-dependencies` is explicit and design.md §5
// repeats it: the engine does not author a shading language and does not write a shader optimiser.
// What it owns is the *request* — which source, which entry point, which permutation, which target,
// which feature set — and everything that happens to the answer.
//
// --- WHY THERE IS AN INTERFACE HERE AT ALL --------------------------------------------------------
//
// Three reasons, and the third is the one that pays for it:
//
//   * A shipping build contains **no compiler**. `shader-system`: "Runtime shader compilation SHALL
//     exist only in development builds, for hot reload." The interface is what lets the Slang
//     implementation be absent from a link rather than present and unreachable.
//   * The Slang library is a 3 GB build tree and a hard dependency for anyone who touches this
//     module. Behind an interface, the cache, the library format, the permutation model, the
//     reflection parser and the pipeline manager are all testable without it — which is what CI has.
//   * M7's material compiler and M11's Metal backend each add a step to this pipeline. A concrete
//     compiler class would grow a flag per addition; an interface grows an implementation.
//
// The default implementation is `unavailable_compiler()`, which fails every request with a message
// naming the build option. A missing compiler is a *reported* condition, never a silently empty
// artefact — the failure mode that would otherwise reach a golden-image test as a black frame.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/shader/permutation.h>
#include <cy/shader/reflection.h>
#include <cy/shader/source.h>

namespace cy::shader {

/// How bad a compiler message is. `Note` carries the "in module imported from here" chain, which is
/// what makes an error inside the standard library traceable back to the shader that imported it.
enum class Severity : u8 {
    Note = 0,
    Warning = 1,
    Error = 2,
};

const char* severity_name(Severity severity) noexcept;

/// One compiler message with its source location.
///
/// `shader-system`'s "Compile error surfaces with source location" scenario: the error carries the
/// Slang source file, line and column, and appears in the editor's shader editor and the build log.
/// The file is a `ShortName` rather than a pointer because the message outlives the compilation that
/// produced it — a failed hot reload keeps its diagnostic on screen until the next successful one.
struct Diagnostic {
    Severity severity = Severity::Error;
    ShortName file;
    u32 line = 0;
    u32 column = 0;
    /// The compiler's own code, `E00100` and the like, kept verbatim so it can be searched for.
    ShortName code;
    ShortName message;
};

/// Everything that identifies one compilation. Every field is part of the cache key.
///
/// `shader-system` — "Shader compilation service and tiered cache": the key includes the source, the
/// compiler version, the IR version, the target platform, the renderer profile, and the feature set,
/// "so a compiler change invalidates derived data without invalidating authored assets". `key()`
/// below is that sentence, executable.
struct CompileRequest {
    /// The module holding the entry point. Its imports are resolved through the same store.
    SourceId source;
    ShortName entry_point;
    Stage stage = Stage::Vertex;
    Target target = Target::SpirV;
    Optimisation optimisation = Optimisation::Default;
    RendererProfile profile = RendererProfile::Desktop;
    FeatureSet features = FeatureSet::None;
    /// The permutation, already reduced by `PermutationDomain::compilation_key()`: two variants that
    /// differ only in specialization constants are one compilation and must be one cache entry.
    PermutationKey permutation = kDefaultPermutation;
    /// Retained in non-shipping artefacts so RenderDoc, PIX and Xcode have something to show —
    /// `shader-system`'s GPU-debugging requirement. It changes the output, so it is in the key.
    bool debug_info = false;
};

/// What a compilation produced.
///
/// `code` is SPIR-V words for `Target::SpirV`; for a future backend-native target it is that
/// target's bytes and nothing above this module needs to know the difference.
struct CompileResult {
    explicit CompileResult(Allocator& allocator) noexcept;

    CompileResult(const CompileResult&) = delete;
    CompileResult& operator=(const CompileResult&) = delete;
    CompileResult(CompileResult&&) noexcept = default;
    CompileResult& operator=(CompileResult&&) noexcept = default;

    Array<u32> code;
    Reflection reflection;
    Array<Diagnostic> diagnostics;
    /// Wall-clock nanoseconds the compiler spent. `shader-system` requires per-shader compile time
    /// in the build report, and it is measured here because this is the only place that sees it.
    u64 compile_ns = 0;
    /// The resolved source hash — the entry-point module plus every module it imported. This is
    /// what the cache key is computed over, and it is returned because the caller cannot compute it
    /// without knowing which imports the compiler followed.
    ContentHash resolved_source_hash;

    [[nodiscard]] bool has_errors() const noexcept;
};

/// The third-party compiler, behind one function.
///
/// Implementations: `cy::shader::slang::compiler()` when `CY_SHADER_SLANG` is on, and
/// `unavailable_compiler()` otherwise. There is deliberately no in-tree "simple" implementation —
/// a second shading-language front end is exactly what `thirdparty-dependencies` forbids.
class ShaderCompiler {
public:
    virtual ~ShaderCompiler() = default;

    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;
    ShaderCompiler(ShaderCompiler&&) = delete;
    ShaderCompiler& operator=(ShaderCompiler&&) = delete;

    /// The implementation's name, for a report: `slang`, `unavailable`.
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// True when this implementation can actually compile. False for the default one, and the thing
    /// a development build checks before offering hot reload.
    [[nodiscard]] virtual bool available() const noexcept = 0;

    /// A hash over everything about the compiler that changes its output: its name, its version, and
    /// the version of the intermediate form it emits.
    ///
    /// **This is what makes "a compiler change invalidates derived data" true rather than hoped
    /// for.** It is part of every cache key, so a Slang upgrade turns every cached artefact into a
    /// miss without touching a single authored asset.
    [[nodiscard]] virtual ContentHash identity() const noexcept = 0;

    /// Compile one entry point of one module for one permutation.
    ///
    /// Returns an error only when the compilation could not be *attempted* — no compiler, no such
    /// module, an entry point the module does not declare. A shader that fails to compile is a
    /// **value**: a `CompileResult` with `has_errors()` and the diagnostics that say why, because a
    /// failed compile is an ordinary outcome of editing a shader and the diagnostics are the point.
    [[nodiscard]] virtual Expected<CompileResult, Error> compile(SourceStore& sources,
                                                                 const CompileRequest& request,
                                                                 Allocator& allocator) noexcept = 0;

protected:
    ShaderCompiler() = default;
};

/// The compiler a build without Slang gets. Never null; fails every request with a message naming
/// `CY_SHADER_SLANG`.
[[nodiscard]] ShaderCompiler& unavailable_compiler() noexcept;

/// The process-wide compiler. `unavailable_compiler()` until something installs one.
///
/// A single global rather than a parameter threaded through the library, the cache and hot reload,
/// for the same reason `servers.h` has a registry: the compiler is a property of the build, not of
/// any one call, and passing it everywhere would mean every test constructing one.
[[nodiscard]] ShaderCompiler& current_compiler() noexcept;
void set_current_compiler(ShaderCompiler* compiler) noexcept;

/// The cache key for a request, given the resolved source hash and the domain it permutes over.
///
/// Every input the specification names is folded in, in a fixed order. Two builds that agree about
/// all of them agree about the key — which is what makes a CI-populated cache usable by a developer,
/// and it is checked by a test that computes the same key twice from independently built inputs.
[[nodiscard]] ContentHash compile_cache_key(const CompileRequest& request,
                                            const ContentHash& resolved_source_hash,
                                            const ContentHash& compiler_identity,
                                            const ContentHash& domain_hash) noexcept;

/// Write a diagnostic the way a build log and the editor both want it:
/// `path:line:column: severity: [code] message`. Returns the length written.
[[nodiscard]] usize format_diagnostic(const Diagnostic& diagnostic, char* out,
                                      usize capacity) noexcept;

}  // namespace cy::shader

#endif  // CY_SHADER_COMPILER_H
