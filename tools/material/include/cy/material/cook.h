#ifndef CY_MATERIAL_COOK_H
#define CY_MATERIAL_COOK_H
// Material cooking, as a node in the derivation graph. M7 task 6.3.
//
// `material-compiler` — "Cooking, caching, and versioning": "Material compilation SHALL be an
// offline cook step producing: the IR, the generated Slang, the compiled programs per target and
// tier, reflection data, the cost report, and pipeline state metadata. Cook keys SHALL include the
// material graph, the compiler version, the IR version, the target profile, the feature set, and
// the quality tier ... Cooked material outputs SHALL use the content-addressed cook cache defined
// in `asset-import-pipeline`, including its shared and CI-populated tiers."
//
// ================================================================================================
// A GRAPH NODE, AND NOT A SECOND CACHE
// ================================================================================================
//
// M6's closing gate recorded that "the real cooks are not nodes in the build graph", and M7 task
// 1.4 put `import` and `cook` into it. This is the third, and it follows that pattern exactly
// rather than inventing one: a `ProducerBody` handed a `NodeContext`, reading its source through
// `context.read` and writing its output through `context.write`, with the derivation key, the
// content-addressed artefact store, precise invalidation and the two-run determinism gate all
// supplied by `cy::build` and none of them reimplemented here.
//
// design.md §5 lists "every new cook is a graph node with a two-run determinism gate" among the
// things that must not be retrofitted, and gives the reason: "M6's spike measured what a
// non-deterministic step plus a cache costs: the artefact that ships is decided by a race."
//
// THE PRODUCER'S VERSION IS THE COMPILER'S VERSION. `kMaterialProducerVersion` is defined as
// `cy::rendering::material::kCompilerVersion`, so "WHEN the material compiler version increases
// THEN compiled programs SHALL be recooked and the authored material assets SHALL be untouched" is
// a fact about the key rather than a note somebody has to remember. A constant here that had to be
// bumped alongside another constant is a constant that would drift.

#include <cy/build/producer.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/material/compiler.h>

#include <string_view>

namespace cy::material {

using rendering::material::CompiledMaterial;
using rendering::material::CompileOptions;

/// Moves with the compiler, deliberately. See the note above.
inline constexpr u32 kMaterialProducerVersion = rendering::material::kCompilerVersion;

/// The cooked bundle's own framing version.
inline constexpr u32 kBundleVersion = 1;

/// What one cooked material carries, read back out of a bundle.
struct CookedProgram {
    rendering::material::ProgramKind kind = rendering::material::ProgramKind::Primary;
    rendering::material::QualityTier tier = rendering::material::QualityTier::High;
    u64 program_digest = 0;
    bool absent = false;
    u32 texture_samples = 0;
    u32 arithmetic = 0;
    u32 closures = 0;
    u32 permutation_count = 0;
    f32 full_screen_ms = 0.0F;
    /// The generated Slang, as a view into the bundle the caller decoded.
    std::string_view source;
};

/// A decoded bundle. Views point into the bytes the caller passed in and are valid for as long as
/// they are.
struct CookedBundle {
    explicit CookedBundle(Allocator& allocator) noexcept : programs(allocator) {}

    CookedBundle(const CookedBundle&) = delete;
    CookedBundle& operator=(const CookedBundle&) = delete;
    CookedBundle(CookedBundle&&) noexcept = default;
    CookedBundle& operator=(CookedBundle&&) noexcept = default;

    u64 cook_key = 0;
    u32 compiler_version = 0;
    /// The serialised material IR, so an editor or a later cook reads the module the programs came
    /// from rather than re-parsing the source.
    Span<const u8> ir;
    Array<CookedProgram> programs;
};

/// Compile a text material definition and encode the bundle.
///
/// Deterministic: two calls with the same source and options produce identical bytes. That is the
/// property the graph's two-run determinism gate measures, and it is the reason the bundle carries
/// no timestamp, no path and no allocator address.
[[nodiscard]] Status cook_material(std::string_view source, const CompileOptions& options,
                                   Allocator& allocator, Array<u8>& out,
                                   Array<char>& report) noexcept;

/// Read a bundle back.
[[nodiscard]] Expected<CookedBundle, Error> decode_bundle(Span<const u8> bytes,
                                                          Allocator& allocator) noexcept;

/// Register the `material` producer. `cy_material` calls it; so does any tool that wants material
/// cooking in its graph.
[[nodiscard]] Status add_material_producers(build::ProducerRegistry& registry) noexcept;

/// The profile a node's `profile` option names. Unknown names are refused rather than defaulted,
/// because a mistyped profile that silently cooked for the desktop would ship.
[[nodiscard]] Expected<rendering::material::Profile, Error> profile_from_name(
    std::string_view name) noexcept;

}  // namespace cy::material

#endif  // CY_MATERIAL_COOK_H
