#ifndef CY_TEXT_LOCALISATION_H
#define CY_TEXT_LOCALISATION_H
// Locales, plural rules, message formatting and the string table. M8.b task 9.4.
//
// `text-and-fonts`: "`TextServer` SHALL integrate with the localisation system so that: the active
// locale informs default language and script for shaping, plural rules and number and date
// formatting come from ICU, and layout direction follows the locale unless overridden", and "WHEN
// the locale changes at runtime THEN cached shaped runs SHALL be invalidated where
// language-dependent, and UI SHALL re-layout".
//
// ICU IS NOT IN `deps/manifest.toml`. What is here instead, and what is not:
//
//   HERE      A locale identifier (language, script, region) and its parsing; the layout DIRECTION
//             a locale implies; the CLDR plural CATEGORIES and the rules for the language families
//             `plural_category()` names; a message formatter with positional and named arguments
//             and a plural selector; a string table with a fallback chain (`fr-CA` → `fr` → root)
//             and a missing-key report; and pseudo-localisation, which is how a team finds a
//             hard-coded string before a translator does.
//   NOT HERE  Number, currency and date formatting. Those are locale DATA — digit shapes, grouping,
//             calendars, era names — and a table of them written by hand would be wrong for most of
//             the world in a way nobody here could review. `format_message` therefore substitutes
//             arguments the CALLER has already formatted, and says so.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/text/bidi.h>

#include <string_view>

namespace cy::text {

/// A locale: language, optional script, optional region. `en`, `en-GB`, `zh-Hant-TW`.
struct Locale {
    /// ISO 639 language, lowercase, at most three characters. Empty is the root locale.
    char language[4] = {};
    /// ISO 15924 script, title case, four characters. Empty when unspecified.
    char script[5] = {};
    /// ISO 3166 region, uppercase, two characters. Empty when unspecified.
    char region[3] = {};

    [[nodiscard]] bool operator==(const Locale& other) const noexcept;
    /// The identifier as it is written: `fr-CA`. Written into `out`, which needs 12 bytes.
    [[nodiscard]] u32 write(char* out, usize capacity) const noexcept;
    /// The same value as a `Name`, which is what a `ShapeKey`'s `language` field carries.
    [[nodiscard]] Name language_name() const noexcept;
};

/// Parse a BCP 47 identifier, in the subset above. An unparseable tag is refused rather than
/// silently becoming the root locale — a typo in a locale identifier is a bug that otherwise shows
/// up as an interface that is inexplicably in English.
[[nodiscard]] Expected<Locale, Error> parse_locale(std::string_view tag) noexcept;

/// The layout direction a locale implies. `text-and-fonts`: "layout direction follows the locale
/// unless overridden", so this returns the default and the override stays the caller's.
[[nodiscard]] ParagraphDirection direction_of(const Locale& locale) noexcept;

/// CLDR's plural categories. A message selects a form by one of these, never by comparing a count
/// to one — which is correct for English and wrong for most languages.
enum class PluralCategory : u8 { Zero = 0, One, Two, Few, Many, Other, Count };

[[nodiscard]] const char* plural_category_name(PluralCategory category) noexcept;

/// The category a count falls in for a locale.
///
/// The rules implemented are CLDR's for: English and the Germanic and Romance languages that share
/// its one/other split; French, which puts 0 and 1 in `One`; Russian, Ukrainian and the Slavic
/// languages with the one/few/many split; Polish; Czech and Slovak; Arabic, which uses all six; and
/// Japanese, Chinese, Korean, Thai and Vietnamese, which have one form. A language this list does
/// not name falls back to the one/other rule and `plural_rules_known()` says so — because a plural
/// silently wrong in Polish is a defect a Polish player finds and nobody else does.
[[nodiscard]] PluralCategory plural_category(const Locale& locale, i64 count) noexcept;
[[nodiscard]] bool plural_rules_known(const Locale& locale) noexcept;

/// One argument to a message. The VALUE IS ALREADY FORMATTED for numbers that need a locale's digit
/// shapes and grouping — see the header for why this module does not format them.
struct MessageArgument {
    Name name;
    std::string_view value;
    /// Used by a `{count, plural, ...}` selector. Ignored otherwise.
    i64 count = 0;
    bool has_count = false;
};

struct FormatReport {
    u32 substitutions = 0;
    /// A placeholder the arguments did not answer. The placeholder is left in the output, visible,
    /// rather than replaced with nothing: a missing argument that renders as an empty string is a
    /// bug that ships.
    u32 missing = 0;
    PluralCategory selected = PluralCategory::Other;
    bool used_plural = false;
};

/// Format a message.
///
/// The syntax is deliberately small: `{name}` substitutes an argument, `{{` is a literal brace, and
/// `{name, plural, one {…} other {…}}` selects by plural category. Anything more elaborate is a
/// message format language, and writing one here would be inventing a dialect of ICU's that no
/// translator's tool understands.
[[nodiscard]] Status format_message(std::string_view pattern, Span<const MessageArgument> arguments,
                                    const Locale& locale, Array<char>& out,
                                    FormatReport& report) noexcept;

/// A locale's strings, with a fallback chain.
class StringTable {
public:
    explicit StringTable(Allocator& allocator) noexcept;

    /// Add a string for a locale. A key already present for that locale is replaced.
    [[nodiscard]] Status add(const Locale& locale, Name key, std::string_view value) noexcept;

    /// Look a key up, walking the fallback chain: `fr-CA` → `fr` → the root. Returns an empty view
    /// when nothing in the chain has it, and counts the miss.
    [[nodiscard]] std::string_view find(const Locale& locale, Name key) noexcept;

    [[nodiscard]] usize size() const noexcept { return entries_.size(); }
    [[nodiscard]] u32 misses() const noexcept { return misses_; }
    /// The keys that were asked for and not found, for the report a localisation pass reads.
    [[nodiscard]] Span<const Name> missing_keys() const noexcept { return missing_.span(); }
    void clear_report() noexcept;

private:
    struct Entry {
        Locale locale;
        Name key;
        std::string_view value;
    };

    Array<Entry> entries_;
    Array<Name> missing_;
    u32 misses_ = 0;
};

/// How pseudo-localisation transforms a string.
struct PseudoOptions {
    /// Wrap the string in brackets so a truncation is visible.
    bool brackets = true;
    /// Lengthen by this fraction, because most translations are longer than English and a layout
    /// that only fits English is a layout that breaks on the first translation.
    f32 expansion = 0.3F;
    /// Replace Latin letters with accented forms, so a string that was never sent to translation
    /// stands out.
    bool accent = true;
};

/// Produce a pseudo-localised string. A development tool, and the cheapest way to find a hard-coded
/// string and a too-small button before a translator does.
[[nodiscard]] Status pseudo_localise(std::string_view text, const PseudoOptions& options,
                                     Array<char>& out) noexcept;

}  // namespace cy::text

#endif  // CY_TEXT_LOCALISATION_H
