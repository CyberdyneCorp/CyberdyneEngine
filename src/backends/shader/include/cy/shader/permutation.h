#ifndef CY_SHADER_PERMUTATION_H
#define CY_SHADER_PERMUTATION_H
// Permutation axes, their cardinality, and the key that names one variant. Task 3.2.
//
// `shader-system` — "Permutations and specialization" states an **order of preference**, and this
// file makes that order a property of the type rather than a paragraph somebody remembers:
//
//   1. **Specialization constants** — resolved at pipeline creation. A quality change makes a new
//      pipeline from the *same* SPIR-V.
//   2. **Slang generics and interfaces** — compile-time polymorphism, producing distinct entry
//      points only where genuinely different code is needed.
//   3. **Preprocessor permutations** — last resort, requiring an explicit declaration of the axis
//      and its cardinality.
//
// THE DISTINCTION THAT MATTERS, AND THE ONE THIS FILE ENFORCES. A specialization axis multiplies the
// number of *pipelines*; a generic or preprocessor axis multiplies the number of *compilations*.
// Those two numbers are wildly different in cost — one is a driver call, the other is a Slang
// invocation and a cache entry — and a permutation budget that adds them together is a budget that
// punishes the cheap choice. `PermutationDomain` reports them separately, `compiled_variants()` is
// what the budget is checked against, and that is the whole mechanism by which the preference order
// above becomes something a build can fail on.
//
// `shader-system`: "Every permutation axis SHALL declare its allowed values so the total permutation
// count is known and reportable at build time." An axis therefore carries its values, not just its
// arity: a report that says "axis `shadow_quality` has 4 values" is actionable and one that says
// "some axis has 4 values" is not.
//
// --- THE KEY IS A MIXED-RADIX NUMBER --------------------------------------------------------------
//
// A variant is identified by one `u64`: the axis values in declaration order, in mixed radix. It is
// small enough to sit in a cache key and a library entry, it is dense (so a domain's variants can be
// enumerated by counting), and it is **stable** — the same declaration produces the same key on
// every machine, which is what lets a cooked shader library from CI be indexed by a developer's
// build. The cost is that adding an axis renumbers every key; the cache key includes the domain's
// own hash, so that renumbering invalidates rather than mismatches.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/shader/shader.h>

#include <string_view>

namespace cy::shader {

/// How an axis is realised, in the specification's order of preference. The enumerator values are
/// the preference order, so a report can sort by them and a lint can compare them.
enum class PermutationKind : u8 {
    /// A SPIR-V specialization constant. Costs one pipeline per value, zero extra compilations.
    Specialization = 0,
    /// A Slang generic or interface parameter. Costs one compilation per value, and is how genuinely
    /// different code is selected without the preprocessor.
    Generic = 1,
    /// A preprocessor define. Costs one compilation per value and carries no type checking; the
    /// last resort the specification names.
    Preprocessor = 2,
};

const char* permutation_kind_name(PermutationKind kind) noexcept;

/// The largest number of values one axis may declare. An axis wider than this is a parameter, not a
/// permutation — the shape of the mistake is a `float` someone turned into 256 variants.
inline constexpr u16 kMaxAxisValues = 64;

/// The largest number of axes one domain may declare. Sixteen axes of two values each is 65,536
/// compilations, which is already past any sane budget; the bound exists so that the key's mixed
/// radix cannot overflow a `u64` in any legal domain.
inline constexpr u32 kMaxAxes = 16;

/// One variant, as a mixed-radix number over a domain's axes. Meaningless without its domain.
struct PermutationKey {
    u64 value = 0;

    friend bool operator==(PermutationKey a, PermutationKey b) noexcept {
        return a.value == b.value;
    }
    friend bool operator!=(PermutationKey a, PermutationKey b) noexcept {
        return a.value != b.value;
    }
};

/// The default variant: every axis at value 0. An axis's first declared value is its default, which
/// is why declaration order within an axis is part of the interface.
inline constexpr PermutationKey kDefaultPermutation{0};

/// What a budget check found, whether or not it passed.
struct PermutationBudget {
    /// Compilations the domain implies: the product of the generic and preprocessor axes.
    u64 compiled_variants = 1;
    /// Pipelines the domain implies: `compiled_variants` times the specialization axes.
    u64 pipeline_variants = 1;
    /// The configured ceiling on `compiled_variants`.
    u64 budget = 0;
    /// The axis whose contribution is largest — the one to argue about first.
    u32 largest_axis = 0;
    bool within_budget = true;
};

/// The declared variation of one shader: its axes, their values, and the arithmetic over them.
///
/// Copyable, because a library entry keeps the domain it was compiled against and comparing two
/// domains is how "the axes changed" is detected.
class PermutationDomain {
public:
    explicit PermutationDomain(Allocator& allocator) noexcept;

    PermutationDomain(const PermutationDomain&) = delete;
    PermutationDomain& operator=(const PermutationDomain&) = delete;
    PermutationDomain(PermutationDomain&&) noexcept = default;
    PermutationDomain& operator=(PermutationDomain&&) noexcept = default;

    /// Declare an axis and its allowed values. The first value is the default.
    ///
    /// Rejects: a duplicate name, an empty value list, more than `kMaxAxisValues` values, more than
    /// `kMaxAxes` axes, and a value list whose product would overflow the key. Each rejection names
    /// the axis, because a domain is assembled from several places — the shader declares some, the
    /// renderer's quality settings declare others — and "which one" is the first question.
    [[nodiscard]] Status add_axis(const char* name, PermutationKind kind,
                                  Span<const char* const> values) noexcept;

    [[nodiscard]] u32 axis_count() const noexcept { return static_cast<u32>(axes_.size()); }
    [[nodiscard]] std::string_view axis_name(u32 axis) const noexcept;
    [[nodiscard]] PermutationKind axis_kind(u32 axis) const noexcept;
    [[nodiscard]] u16 axis_cardinality(u32 axis) const noexcept;
    [[nodiscard]] std::string_view axis_value_name(u32 axis, u16 value) const noexcept;
    /// The axis with this name, or `axis_count()` when there is none.
    [[nodiscard]] u32 find_axis(const char* name) const noexcept;

    /// Compilations implied by the generic and preprocessor axes.
    [[nodiscard]] u64 compiled_variants() const noexcept;
    /// Pipelines implied by every axis, specialization included.
    [[nodiscard]] u64 pipeline_variants() const noexcept;

    /// Check `compiled_variants()` against a ceiling and report the breakdown.
    ///
    /// `shader-system`'s "Permutation explosion is visible" scenario: the build warns *with the axis
    /// breakdown*, "before compile times become a problem". The breakdown is the point — a bare
    /// count tells an author that something is wrong and not what to change.
    [[nodiscard]] PermutationBudget check_budget(u64 budget) const noexcept;

    /// Build a key from one value per axis, in declaration order.
    [[nodiscard]] Expected<PermutationKey, Error> key_of(Span<const u16> values) const noexcept;
    /// The value one axis takes in a key.
    [[nodiscard]] u16 value_of(PermutationKey key, u32 axis) const noexcept;
    /// The key with one axis changed, every other axis kept.
    [[nodiscard]] Expected<PermutationKey, Error> with_value(PermutationKey key, u32 axis,
                                                             u16 value) const noexcept;

    /// The key with every specialization axis set to zero.
    ///
    /// **This is the compilation key**: two variants that differ only in specialization values share
    /// one SPIR-V blob and one cache entry, which is the whole reason the specification prefers
    /// specialization constants. The cache is keyed on this, the pipeline on the full key.
    [[nodiscard]] PermutationKey compilation_key(PermutationKey key) const noexcept;

    /// Write `axis=value` pairs, comma separated, into `out`. Returns the length written, excluding
    /// the terminator. Truncates rather than failing: this is a diagnostic string.
    [[nodiscard]] usize format(PermutationKey key, char* out, usize capacity) const noexcept;

    /// A hash over the declaration — names, kinds and value names, in order. Part of the cache key,
    /// so that adding an axis (which renumbers every key) invalidates rather than aliases.
    [[nodiscard]] ContentHash declaration_hash() const noexcept;

private:
    struct Axis {
        u32 name_offset = 0;
        u32 name_length = 0;
        /// The index of this axis's first value in `values_`.
        u32 first_value = 0;
        u16 cardinality = 0;
        PermutationKind kind = PermutationKind::Preprocessor;
        /// The place value of this axis in the mixed-radix key: the product of every earlier axis's
        /// cardinality. Cached because `value_of` is on the pipeline-lookup path.
        u64 radix = 1;
    };

    struct Value {
        u32 offset = 0;
        u32 length = 0;
    };

    [[nodiscard]] std::string_view text_at(u32 offset, u32 length) const noexcept {
        return {text_.data() + offset, length};
    }
    [[nodiscard]] Expected<u32, Error> intern(std::string_view text) noexcept;

    Array<char> text_;
    Array<Axis> axes_;
    Array<Value> values_;
};

}  // namespace cy::shader

#endif  // CY_SHADER_PERMUTATION_H
