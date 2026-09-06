#ifndef CY_IMPORT_OPTIONS_H
#define CY_IMPORT_OPTIONS_H
// An importer's options: what may be set, what it means, and what it currently is. M5 task 5.1.
//
// `asset-import-pipeline` — "Importer framework": an importer declares "its options schema (a
// reflected settings struct)", and "Import SHALL be a pure function of (source bytes, options,
// importer version, platform variant), so its output is cacheable and reproducible."
//
// --- WHY A DECLARED SCHEMA RATHER THAN AN ANNOTATED STRUCT ---------------------------------------
//
// The engine's reflection generator (tools/gen/reflect/) produces `reflect::TypeInfo` from
// annotated C++ declarations, and an options struct annotated that way would satisfy the letter of
// "a reflected settings struct". It would not satisfy the requirement's PURPOSE, which is three
// things this file does instead:
//
//   * The import dialog must edit options **per node** of a scene file — "Node-level options SHALL
//     be editable per node ... and stored in the `.meta`" — and there is no C++ struct for
//     "whatever nodes this particular glTF happens to contain".
//   * A custom importer registered **from Swift** must declare its options with the same weight as
//   a
//     built-in one, and Swift cannot add a C++ type to the reflection manifest.
//   * The derivation key must be computed from the options **canonically**, so that setting a value
//     to its default and never setting it produce the same key. A struct hashed field-by-field does
//     that only if every field is always written, which is exactly the discipline that fails.
//
// So an options schema is data: a list of declared options, each with a name, a type, a default and
// a sentence saying what it is for. The sentence is not decoration — `editor-agent-interface`
// requires a machine caller to be able to act on a description, and an import option is one of the
// things such a caller sets.
//
// --- THE CANONICAL FORM, AND WHY IT DECIDES CACHE CORRECTNESS ------------------------------------
//
// `ImportOptions::contribute_to` writes every DECLARED option into the derivation key, in the
// schema's declaration order, taking the default for anything unset. Two consequences, both
// deliberate:
//
//   * Setting an option to its default value and leaving it unset produce the same key, so a
//     designer who opens the import dialog and touches nothing does not invalidate the project's
//     cooked content.
//   * An option the schema does not declare CANNOT reach the key. `ImportOptions::set` refuses an
//     undeclared name outright, because an option that changed the output and not the key is the
//     one defect a cook cache cannot survive.

#include <cy/core/assets/derivation.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::import {

/// The kinds an import option can hold.
///
/// Deliberately small. An import option is something a person sets in a dialog and a machine sets
/// by name; a matrix, a callable or a nested dictionary is not one of those, and admitting them
/// would make the canonical form — which decides cache correctness — a serialisation problem.
enum class OptionType : u8 {
    Bool = 0,
    Int = 1,
    Float = 2,
    /// A free-form string: a naming convention's suffix, a profile's name.
    Text = 3,
    /// One of a declared set of names. The set is part of the schema, so a typo is refused at the
    /// point it is set rather than misinterpreted at cook time.
    Enumeration = 4,
};

/// The enumerator's own spelling. Never null.
[[nodiscard]] const char* option_type_name(OptionType type) noexcept;

/// A value of one of those kinds.
///
/// A plain tagged union rather than `cy::Var`: `Var` is reference-counted and covers twenty-five
/// kinds, twenty of which an import option may not hold, and the canonical form would then have to
/// decide what a `Var` holding a `Callable` contributes to a cache key. The answer is that it
/// cannot hold one.
class OptionValue {
public:
    constexpr OptionValue() noexcept = default;

    // Constexpr because a schema is a `constexpr OptionSpec[]` in the importer that declares it,
    // and a default value is one of its fields. A schema built at run time would be a schema that
    // can differ between two builds of the same importer.
    [[nodiscard]] static constexpr OptionValue of_bool(bool value) noexcept {
        OptionValue result;
        result.type_ = OptionType::Bool;
        result.boolean_ = value;
        return result;
    }

    [[nodiscard]] static constexpr OptionValue of_int(i64 value) noexcept {
        OptionValue result;
        result.type_ = OptionType::Int;
        result.integer_ = value;
        return result;
    }

    [[nodiscard]] static constexpr OptionValue of_float(f64 value) noexcept {
        OptionValue result;
        result.type_ = OptionType::Float;
        result.real_ = value;
        return result;
    }

    /// The text is NOT copied: it must outlive the value. Schema defaults are literals and set
    /// values come from a sidecar the caller keeps, which is every case there is.
    [[nodiscard]] static constexpr OptionValue of_text(std::string_view value) noexcept {
        OptionValue result;
        result.type_ = OptionType::Text;
        result.text_ = value;
        return result;
    }

    [[nodiscard]] static constexpr OptionValue of_enumeration(std::string_view value) noexcept {
        OptionValue result;
        result.type_ = OptionType::Enumeration;
        result.text_ = value;
        return result;
    }

    [[nodiscard]] constexpr OptionType type() const noexcept { return type_; }
    [[nodiscard]] bool as_bool() const noexcept;
    [[nodiscard]] i64 as_int() const noexcept;
    [[nodiscard]] f64 as_float() const noexcept;
    [[nodiscard]] std::string_view as_text() const noexcept;

    friend bool operator==(const OptionValue& a, const OptionValue& b) noexcept;
    friend bool operator!=(const OptionValue& a, const OptionValue& b) noexcept {
        return !(a == b);
    }

private:
    OptionType type_ = OptionType::Bool;
    bool boolean_ = false;
    i64 integer_ = 0;
    f64 real_ = 0.0;
    std::string_view text_;
};

/// One option an importer declares.
struct OptionSpec {
    /// The name it is set by. Lower-case with hyphens, like a command identifier: a person types it
    /// into a sidecar and a machine caller types it into a request.
    std::string_view name;
    OptionType type = OptionType::Bool;
    /// The value used when nothing sets it. Part of the canonical form, so changing a default
    /// re-cooks — which is correct, and is why a default is changed with the importer's version.
    OptionValue default_value;
    /// What it is for, in a sentence, written for a caller that cannot see the dialog.
    std::string_view description;
    /// For `Enumeration`, the permitted names. Empty for every other type.
    Span<const std::string_view> choices;
    /// Inclusive bounds for `Int` and `Float`. Ignored for the other types, and equal when
    /// unbounded.
    f64 minimum = 0.0;
    f64 maximum = 0.0;
};

/// What an importer accepts.
///
/// A view over the importer's own static table rather than an owning container: an importer's
/// schema is fixed at compile time for a built-in and fixed at registration for a plugin, and
/// copying it per import would allocate once per asset for data that never changes.
class OptionsSchema {
public:
    constexpr OptionsSchema() noexcept = default;
    constexpr explicit OptionsSchema(Span<const OptionSpec> options) noexcept : options_(options) {}

    [[nodiscard]] Span<const OptionSpec> options() const noexcept { return options_; }
    [[nodiscard]] usize size() const noexcept { return options_.size(); }

    /// The declaration for a name, or null.
    [[nodiscard]] const OptionSpec* find(std::string_view name) const noexcept;

    /// Refuse a schema a caller could not act on: a duplicate name, an option with no description,
    /// an enumeration with no choices or a default outside them, a numeric default outside its own
    /// bounds. Called by `ImporterRegistry::register_importer`, so none of them can be registered.
    [[nodiscard]] Status validate() const noexcept;

private:
    Span<const OptionSpec> options_;
};

/// The values set for one import.
///
/// Sparse: it holds what was set, and reads through to the schema's defaults for the rest. That is
/// what makes "setting a value to its default and never setting it" the same cache key, and it is
/// also what makes a sidecar a short diff rather than a dump of every option in the importer.
class ImportOptions {
public:
    ImportOptions() noexcept = default;

    /// Set an option, refusing a name the schema does not declare, a value of the wrong type, a
    /// choice outside an enumeration's set, and a number outside its bounds. Each failure names the
    /// option and what would have been accepted.
    [[nodiscard]] Status set(const OptionsSchema& schema, std::string_view name,
                             const OptionValue& value) noexcept;

    /// The value in force: what was set, or the schema's default. Fails with `NotFound` for an
    /// option the schema does not declare.
    [[nodiscard]] Expected<OptionValue, Error> get(const OptionsSchema& schema,
                                                   std::string_view name) const noexcept;

    /// Whether a value was set explicitly, as opposed to defaulted. For the dialog, which shows a
    /// modified option differently, and for a sidecar, which writes only what was set.
    [[nodiscard]] bool is_set(std::string_view name) const noexcept;

    /// The options that were set, in the order they were set. What a sidecar writes.
    [[nodiscard]] usize size() const noexcept { return values_.size(); }
    [[nodiscard]] std::string_view name_at(usize index) const noexcept;
    [[nodiscard]] const OptionValue& value_at(usize index) const noexcept;

    void clear() noexcept { values_.clear(); }

    /// Write every DECLARED option into a derivation key, in the schema's order, defaults included.
    ///
    /// This is the function the cache's correctness rests on. See the header: taking the schema's
    /// order rather than the set order means the key does not depend on which order a dialog wrote
    /// its fields, and writing defaults means an unset option and an explicitly-defaulted one are
    /// one key.
    void contribute_to(const OptionsSchema& schema,
                       assets::DerivationKeyBuilder& builder) const noexcept;

private:
    struct Entry {
        std::string_view name;
        OptionValue value;
    };

    Array<Entry> values_;
};

}  // namespace cy::import

#endif  // CY_IMPORT_OPTIONS_H
