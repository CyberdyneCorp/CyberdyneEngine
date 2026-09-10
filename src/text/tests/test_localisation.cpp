// Locales, plural rules, message formatting, the string table and pseudo-localisation.
// M8.b task 9.4.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/text/localisation.h>

#include <string_view>

using namespace cy;
using namespace cy::text;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

[[nodiscard]] Locale locale_of(const char* tag) noexcept {
    auto parsed = parse_locale(tag);
    return parsed.has_value() ? parsed.value() : Locale{};
}

[[nodiscard]] std::string_view view_of(const Array<char>& buffer) noexcept {
    return {buffer.data(), buffer.size()};
}

}  // namespace

CY_TEST_CASE("text_locale: a tag parses into language, script and region") {
    const auto simple = parse_locale("en");
    CY_REQUIRE(simple.has_value());
    CY_CHECK_EQ(std::string_view(simple.value().language), "en");

    const auto full = parse_locale("zh-Hant-TW");
    CY_REQUIRE(full.has_value());
    CY_CHECK_EQ(std::string_view(full.value().language), "zh");
    CY_CHECK_EQ(std::string_view(full.value().script), "Hant");
    CY_CHECK_EQ(std::string_view(full.value().region), "TW");

    // Case is normalised, and an underscore is accepted because that is how a filename spells it.
    const auto messy = parse_locale("FR_ca");
    CY_REQUIRE(messy.has_value());
    CY_CHECK_EQ(std::string_view(messy.value().language), "fr");
    CY_CHECK_EQ(std::string_view(messy.value().region), "CA");

    char written[16] = {};
    CY_CHECK_EQ(messy.value().write(written, sizeof(written)), 5U);
    CY_CHECK_EQ(std::string_view(written), "fr-CA");
}

CY_TEST_CASE("text_locale: an unparseable tag is refused rather than becoming the root") {
    // A typo in a locale identifier otherwise shows up as an interface that is inexplicably in
    // English, which is a bug nobody can find from the symptom.
    CY_CHECK_FALSE(parse_locale("e").has_value());
    CY_CHECK_FALSE(parse_locale("en--GB").has_value());
    CY_CHECK_FALSE(parse_locale("en-GB-extra-parts").has_value());
    // The empty tag IS the root locale, and that is not an error.
    CY_CHECK(parse_locale("").has_value());
}

CY_TEST_CASE("text_locale: direction follows the locale, and the script wins where it is given") {
    CY_CHECK_EQ(direction_of(locale_of("en")), ParagraphDirection::LeftToRight);
    CY_CHECK_EQ(direction_of(locale_of("ar")), ParagraphDirection::RightToLeft);
    CY_CHECK_EQ(direction_of(locale_of("he-IL")), ParagraphDirection::RightToLeft);
    // Azerbaijani is written in both, and the script is what decides.
    CY_CHECK_EQ(direction_of(locale_of("az-Arab")), ParagraphDirection::RightToLeft);
    CY_CHECK_EQ(direction_of(locale_of("az-Latn")), ParagraphDirection::LeftToRight);
}

CY_TEST_CASE("text_plural: the categories are the language's, not English's") {
    // The rule that matters: a message written with `one` and `other` is wrong in Russian, Polish
    // and Arabic, and a plural selector that compared the count to one would ship that.
    const Locale english = locale_of("en");
    CY_CHECK_EQ(plural_category(english, 1), PluralCategory::One);
    CY_CHECK_EQ(plural_category(english, 0), PluralCategory::Other);

    const Locale french = locale_of("fr");
    CY_CHECK_EQ(plural_category(french, 0), PluralCategory::One);  // French puts zero with one

    const Locale russian = locale_of("ru");
    CY_CHECK_EQ(plural_category(russian, 1), PluralCategory::One);
    CY_CHECK_EQ(plural_category(russian, 3), PluralCategory::Few);
    CY_CHECK_EQ(plural_category(russian, 11), PluralCategory::Many);
    CY_CHECK_EQ(plural_category(russian, 21), PluralCategory::One);

    const Locale arabic = locale_of("ar");
    CY_CHECK_EQ(plural_category(arabic, 0), PluralCategory::Zero);
    CY_CHECK_EQ(plural_category(arabic, 2), PluralCategory::Two);
    CY_CHECK_EQ(plural_category(arabic, 5), PluralCategory::Few);
    CY_CHECK_EQ(plural_category(arabic, 15), PluralCategory::Many);

    const Locale japanese = locale_of("ja");
    CY_CHECK_EQ(plural_category(japanese, 1), PluralCategory::Other);

    // And a language whose rules are not implemented SAYS SO rather than being silently wrong.
    CY_CHECK(plural_rules_known(russian));
    CY_CHECK_FALSE(plural_rules_known(locale_of("cy")));  // Welsh has six categories of its own
    CY_CHECK_EQ(plural_category(locale_of("cy"), 3), PluralCategory::Other);
}

CY_TEST_CASE("text_format: arguments are substituted and a missing one stays visible") {
    Array<char> out(allocator());
    FormatReport report;
    MessageArgument arguments[1];
    arguments[0].name = Name::intern("player");
    arguments[0].value = "Ada";

    CY_REQUIRE(format_message("Welcome, {player}!", Span<const MessageArgument>(arguments, 1),
                              locale_of("en"), out, report)
                   .has_value());
    CY_CHECK_EQ(view_of(out), "Welcome, Ada!");
    CY_CHECK_EQ(report.substitutions, 1U);
    CY_CHECK_EQ(report.missing, 0U);

    // A MISSING ARGUMENT RENDERS AS ITS PLACEHOLDER. One that rendered as nothing is a bug that
    // ships; one that renders as `{score}` is a bug somebody reports.
    CY_REQUIRE(format_message("Score: {score}", Span<const MessageArgument>(arguments, 1),
                              locale_of("en"), out, report)
                   .has_value());
    CY_CHECK_EQ(view_of(out), "Score: {score}");
    CY_CHECK_EQ(report.missing, 1U);

    // A doubled brace is a literal one.
    CY_REQUIRE(format_message("{{literal}", Span<const MessageArgument>(arguments, 1),
                              locale_of("en"), out, report)
                   .has_value());
    CY_CHECK_EQ(view_of(out), "{literal}");
}

CY_TEST_CASE("text_format: a plural selector picks the arm the locale's rules choose") {
    Array<char> out(allocator());
    FormatReport report;
    MessageArgument arguments[1];
    arguments[0].name = Name::intern("count");
    arguments[0].value = "5";
    arguments[0].count = 5;
    arguments[0].has_count = true;

    const std::string_view pattern =
        "{count, plural, one {one item} few {a few items} other {{count} items}}";

    CY_REQUIRE(format_message(pattern, Span<const MessageArgument>(arguments, 1), locale_of("en"),
                              out, report)
                   .has_value());
    CY_CHECK_EQ(view_of(out), "5 items");
    CY_CHECK(report.used_plural);
    CY_CHECK_EQ(report.selected, PluralCategory::Other);

    // The same message in Russian picks `few` for five... which Russian's rules do not: five is
    // `many`, and with no `many` arm the formatter falls back to `other`. That fallback is the
    // behaviour a translator relies on when a language has more categories than the message does.
    CY_REQUIRE(format_message(pattern, Span<const MessageArgument>(arguments, 1), locale_of("ru"),
                              out, report)
                   .has_value());
    CY_CHECK_EQ(report.selected, PluralCategory::Many);
    CY_CHECK_EQ(view_of(out), "5 items");

    arguments[0].count = 3;
    arguments[0].value = "3";
    CY_REQUIRE(format_message(pattern, Span<const MessageArgument>(arguments, 1), locale_of("ru"),
                              out, report)
                   .has_value());
    CY_CHECK_EQ(report.selected, PluralCategory::Few);
    CY_CHECK_EQ(view_of(out), "a few items");

    arguments[0].count = 1;
    arguments[0].value = "1";
    CY_REQUIRE(format_message(pattern, Span<const MessageArgument>(arguments, 1), locale_of("en"),
                              out, report)
                   .has_value());
    CY_CHECK_EQ(view_of(out), "one item");
}

CY_TEST_CASE("text_table: a lookup walks the fallback chain and records what it could not find") {
    StringTable table(allocator());
    const Locale root;
    const Locale french = locale_of("fr");
    const Locale canadian = locale_of("fr-CA");

    CY_REQUIRE(table.add(root, Name::intern("greeting"), "Hello").has_value());
    CY_REQUIRE(table.add(french, Name::intern("greeting"), "Bonjour").has_value());
    CY_REQUIRE(table.add(canadian, Name::intern("greeting"), "Allo").has_value());
    CY_REQUIRE(table.add(french, Name::intern("farewell"), "Au revoir").has_value());

    CY_CHECK_EQ(table.find(canadian, Name::intern("greeting")), "Allo");
    // Not in fr-CA: the chain falls back to fr.
    CY_CHECK_EQ(table.find(canadian, Name::intern("farewell")), "Au revoir");
    // Not in fr either: the root answers.
    CY_CHECK_EQ(table.find(locale_of("de"), Name::intern("greeting")), "Hello");

    // A key nowhere in the chain is a MISS, and it is recorded — which is the report a localisation
    // pass reads to find the strings nobody translated.
    CY_CHECK_EQ(table.find(canadian, Name::intern("nothing")), "");
    CY_CHECK_EQ(table.misses(), 1U);
    CY_REQUIRE_EQ(table.missing_keys().size(), 1U);
    CY_CHECK_EQ(table.missing_keys()[0], Name::intern("nothing"));

    // The same missing key twice is one entry in the report, so a string looked up every frame does
    // not fill the report with copies of itself.
    CY_CHECK_EQ(table.find(canadian, Name::intern("nothing")), "");
    CY_CHECK_EQ(table.missing_keys().size(), 1U);
    table.clear_report();
    CY_CHECK_EQ(table.misses(), 0U);
}

CY_TEST_CASE(
    "text_pseudo: pseudo-localisation lengthens and marks, and leaves placeholders alone") {
    Array<char> out(allocator());
    PseudoOptions options;
    CY_REQUIRE(pseudo_localise("Save {slot}", options, out).has_value());
    const std::string_view text = view_of(out);

    CY_CHECK_EQ(text[0], '[');
    CY_CHECK_EQ(text[text.size() - 1], ']');
    // THE PLACEHOLDER SURVIVES. Accenting the inside of `{slot}` would break the formatter, and a
    // pseudo-localisation that breaks the string it is testing tests nothing.
    CY_CHECK_NE(text.find("{slot}"), std::string_view::npos);
    // And the string got longer, which is what finds a button that only fits English.
    CY_CHECK_GT(text.size(), std::string_view("Save {slot}").size());
    // The letters were accented, so a string that never went to translation stands out.
    CY_CHECK_EQ(text.find("Save"), std::string_view::npos);

    PseudoOptions plain;
    plain.brackets = false;
    plain.accent = false;
    plain.expansion = 0.0F;
    Array<char> untouched(allocator());
    CY_REQUIRE(pseudo_localise("Save", plain, untouched).has_value());
    CY_CHECK_EQ(view_of(untouched), "Save");
}
