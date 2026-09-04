#pragma once
// Shader variation: the axes, their cardinality, and the key that names one variant. Task 3.2.
//
// `shader-system` orders the three mechanisms and the order is the requirement, not a preference:
//
//   1. SPECIALIZATION CONSTANTS — resolved at pipeline creation. One SPIR-V module serves every
//      value, so an axis of this kind multiplies the number of *pipelines* and not the number of
//      *compilations*.
//   2. SLANG GENERICS AND INTERFACES — compile-time polymorphism producing distinct entry points
//      only where the code genuinely differs.
//   3. PREPROCESSOR PERMUTATIONS — last resort, and only with the axis and its cardinality
//      declared.
//
// THE DISTINCTION IS MEASURED HERE RATHER THAN ASSERTED IN PROSE. `compiled_variants()` counts only
// axes of kinds 2 and 3; `pipeline_variants()` counts all of them. A shader that moves an axis from
// Preprocessor to Specialization halves its compile count and changes nothing else, and the two
// numbers say so. That is what makes "prefer specialization" a thing a build report can show rather
// than a thing a reviewer has to notice.
//
// EVERY AXIS DECLARES ITS ALLOWED VALUES. `shader-system`: "Every permutation axis SHALL declare
// its allowed values so the total permutation count is known and reportable at build time." So an
// axis is a name, a kind, and an explicit list — not an open integer range — and the product over
// the list is a number the build can print and compare against a budget before compile times become
// somebody's afternoon.
//
// THE KEY IS MIXED-RADIX AND STABLE. Variant *i* of an axis contributes `i * stride`, with strides
// derived from the axes in declaration order. Two properties follow and both are load-bearing: the
// key is dense (so a variant table is an array, not a hash map), and it is reproducible across runs
// and machines (so it can be part of a content-addressed cache key — see cache.h).

#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/shader/diagnostics.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::shader {

/// How a variation axis is realised. See the ordering above: lower is better.
enum class VariationKind : u8 {
    /// A specialization constant. One module, many pipelines.
    Specialization = 0,
    /// A Slang generic or interface argument. A distinct entry point, compiled separately.
    Generic = 1,
    /// A preprocessor define. A distinct compilation of the whole module, and the last resort.
    Preprocessor = 2,
};

const char* variation_kind_name(VariationKind kind) noexcept;

/// Whether an axis of this kind costs a separate compilation.
[[nodiscard]] constexpr bool requires_compilation(VariationKind kind) noexcept {
    return kind != VariationKind::Specialization;
}

/// How many axes one shader may declare, and how many values one axis may take.
///
/// Both are small on purpose. Sixteen binary axes are already 65,536 variants; an axis with more
/// than 64 declared values is a parameter that wants to be a uniform, not a permutation.
inline constexpr u32 kMaxPermutationAxes = 16;
inline constexpr u32 kMaxAxisValues = 64;

/// The default ceiling `check_budget` compares against. Not a hard limit — exceeding it is a
/// warning with the breakdown, which is what `shader-system`'s "Permutation explosion is visible"
/// scenario asks for: the build says so early, rather than the compile getting slower every week.
inline constexpr u64 kDefaultPermutationBudget = 1024;

/// One declared axis of variation.
struct PermutationAxis {
    Name name;
    VariationKind kind = VariationKind::Specialization;
    /// The values this axis may take, in the order a key indexes them. At least two: an axis with
    /// one value varies nothing and is a constant that took a wrong turn.
    Span<const u32> values;
    /// The SPIR-V specialization-constant id this axis feeds. Required for
    /// `VariationKind::Specialization` and meaningless otherwise; `reflection.h` reports the ids a
    /// module actually declares, and `PermutationSet::validate_against` checks that they agree.
    u32 specialization_id = 0;
};

/// A variant, as an index per axis. `value` is dense over `[0, pipeline_variants())`.
struct PermutationKey {
    u64 value = 0;

    friend constexpr bool operator==(PermutationKey a, PermutationKey b) noexcept {
        return a.value == b.value;
    }
    friend constexpr bool operator!=(PermutationKey a, PermutationKey b) noexcept {
        return a.value != b.value;
    }
};

/// The axes one shader varies over.
class PermutationSet {
public:
    explicit PermutationSet(Allocator& allocator) noexcept;

    PermutationSet(const PermutationSet&) = delete;
    PermutationSet& operator=(const PermutationSet&) = delete;

    /// Declare an axis. Order matters: it fixes the key's radix layout, so a set built in a
    /// different order produces different keys for the same choices and would miss in the cache.
    [[nodiscard]] Status add_axis(Name name, VariationKind kind, Span<const u32> values,
                                  u32 specialization_id = 0) noexcept;

    [[nodiscard]] usize axis_count() const noexcept { return axes_.size(); }
    [[nodiscard]] PermutationAxis axis_at(usize index) const noexcept;
    /// The axis by name, or `axis_count()` when there is none. A caller sets a value by name and
    /// this is how the name becomes the index the key is built from.
    [[nodiscard]] usize index_of(Name name) const noexcept;

    /// Every combination, including the specialization axes: the number of *pipelines*.
    [[nodiscard]] u64 pipeline_variants() const noexcept;
    /// The combinations that need their own SPIR-V: the number of *compilations*.
    [[nodiscard]] u64 compiled_variants() const noexcept;

    /// Pack a choice per axis, in declaration order, into a key. Fails when the count does not
    /// match the axes or an index is out of range — both of which are the caller asking for a
    /// variant that was never declared.
    [[nodiscard]] Expected<PermutationKey, Error> encode(Span<const u32> indices) const noexcept;
    /// Unpack a key into one index per axis. `out` must have `axis_count()` elements.
    [[nodiscard]] Status decode(PermutationKey key, Span<u32> indices) const noexcept;

    /// The key of the variant that differs from `key` only in `axis`, which is how a quality
    /// setting changes one axis without rebuilding the whole choice vector.
    [[nodiscard]] Expected<PermutationKey, Error> with_axis(PermutationKey key, usize axis,
                                                            u32 index) const noexcept;

    /// The key of the compilation `key` belongs to: the same choices with every specialization axis
    /// zeroed. Two variants differing only in a specialization constant share this, and therefore
    /// share one cache entry and one SPIR-V module.
    [[nodiscard]] Expected<PermutationKey, Error> compilation_key(
        PermutationKey key) const noexcept;

    /// The specialization constants a pipeline for `key` is created with — the id and value of
    /// every specialization axis. This is where "a quality change creates a new pipeline from the
    /// same SPIR-V rather than a new compilation" stops being a claim.
    [[nodiscard]] Status specialization_constants(
        PermutationKey key, Array<rhi::SpecializationConstant>& out) const noexcept;

    /// The preprocessor defines a compilation for `key` is invoked with, as `NAME=value` pairs
    /// appended to `out`. Only `VariationKind::Preprocessor` axes appear; a generic axis is an
    /// entry-point argument and a specialization axis is not a compile-time input at all.
    [[nodiscard]] Status preprocessor_defines(PermutationKey key, Array<Name>& names,
                                              Array<u32>& values) const noexcept;

    /// Warn when the compilation count exceeds `budget`, with the per-axis breakdown the scenario
    /// asks for. Returns true when it is within budget. Never an error: a project may knowingly
    /// exceed it, and turning that into a build failure would only teach people to raise the
    /// number.
    [[nodiscard]] bool check_budget(u64 budget, DiagnosticLog& diagnostics,
                                    std::string_view shader_name) const noexcept;

    /// Check the declared specialization ids against the ids a compiled module reports. Every
    /// specialization axis must have a matching constant, or the axis silently does nothing —
    /// which looks exactly like a shader that ignores a quality setting.
    [[nodiscard]] bool validate_specialization_ids(Span<const u32> declared_ids,
                                                   DiagnosticLog& diagnostics,
                                                   std::string_view shader_name) const noexcept;

private:
    struct Axis {
        Name name;
        VariationKind kind = VariationKind::Specialization;
        u32 specialization_id = 0;
        u32 first_value = 0;  ///< offset into `values_`
        u32 value_count = 0;
        u64 stride = 1;  ///< the mixed-radix weight, fixed when the axis is added
    };

    Array<Axis> axes_;
    Array<u32> values_;
};

}  // namespace cy::shader
