// An importer's options, and the canonical form the cook cache's correctness rests on. M5 task 5.1.

#include <cy/core/assets/derivation.h>
#include <cy/import/importer.h>
#include <cy/import/options.h>
#include <cy/test/test.h>

#include <string_view>

using namespace cy::import;
using cy::assets::DerivationKey;
using cy::assets::DerivationKeyBuilder;

namespace {

constexpr std::string_view kChoices[] = {"low", "high"};

constexpr OptionSpec kOptions[] = {
    {"quality", OptionType::Enumeration, OptionValue::of_enumeration("low"),
     "How hard the importer works, traded against how long it takes.",
     cy::Span<const std::string_view>(kChoices), 0.0, 0.0},
    {"scale",
     OptionType::Float,
     OptionValue::of_float(1.0),
     "A uniform scale applied at import so nothing downstream accounts for the source's units.",
     {},
     0.01,
     100.0},
    {"mips",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to produce the full mip chain rather than the base level alone.",
     {},
     0.0,
     0.0},
};

OptionsSchema schema() noexcept {
    return OptionsSchema(cy::Span<const OptionSpec>(kOptions));
}

DerivationKey key_of(const ImportOptions& options) {
    DerivationKeyBuilder builder;
    builder.producer(cy::assets::DerivedKind::Import, "test", 1);
    options.contribute_to(schema(), builder);
    auto key = builder.finish();
    CY_REQUIRE(key.has_value());
    return key.value();
}

}  // namespace

CY_TEST_CASE("options: an unset option reads its default") {
    ImportOptions options;
    CY_CHECK(options.get(schema(), "scale").value().as_float() == 1.0);
    CY_CHECK(options.get(schema(), "quality").value().as_text() == "low");
    CY_CHECK(!options.is_set("scale"));
}

CY_TEST_CASE("options: an option the schema does not declare cannot be set") {
    // The refusal that keeps the cache honest. An option that changed the output and not the
    // derivation key is the one defect content addressing cannot survive, and there is no way to
    // spell one.
    ImportOptions options;
    const auto refused = options.set(schema(), "quality-level", OptionValue::of_bool(true));
    CY_CHECK(!refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::NotFound);
}

CY_TEST_CASE("options: the wrong type, a choice outside the set, and a number out of bounds") {
    ImportOptions options;
    CY_CHECK(!options.set(schema(), "scale", OptionValue::of_bool(true)).has_value());
    CY_CHECK(!options.set(schema(), "quality", OptionValue::of_enumeration("medium")).has_value());
    CY_CHECK(!options.set(schema(), "scale", OptionValue::of_float(1000.0)).has_value());
    CY_CHECK(options.set(schema(), "scale", OptionValue::of_float(2.0)).has_value());
}

CY_TEST_CASE("options: setting a value to its default is the same key as not setting it") {
    // The property the header argues for at length, and the reason the canonical form writes every
    // DECLARED option rather than every SET one: a designer who opens the import dialog and touches
    // nothing must not invalidate the project's cooked content.
    ImportOptions untouched;
    ImportOptions explicit_default;
    CY_REQUIRE(explicit_default.set(schema(), "scale", OptionValue::of_float(1.0)).has_value());
    CY_REQUIRE(explicit_default.set(schema(), "mips", OptionValue::of_bool(true)).has_value());
    CY_CHECK(key_of(untouched) == key_of(explicit_default));
}

CY_TEST_CASE("options: a changed option changes the key") {
    ImportOptions changed;
    CY_REQUIRE(changed.set(schema(), "scale", OptionValue::of_float(2.0)).has_value());
    CY_CHECK(key_of(ImportOptions{}) != key_of(changed));
}

CY_TEST_CASE("options: the key does not depend on the order options were set") {
    ImportOptions first;
    CY_REQUIRE(first.set(schema(), "scale", OptionValue::of_float(2.0)).has_value());
    CY_REQUIRE(first.set(schema(), "mips", OptionValue::of_bool(false)).has_value());
    ImportOptions second;
    CY_REQUIRE(second.set(schema(), "mips", OptionValue::of_bool(false)).has_value());
    CY_REQUIRE(second.set(schema(), "scale", OptionValue::of_float(2.0)).has_value());
    CY_CHECK(key_of(first) == key_of(second));
}

CY_TEST_CASE("options: setting the same option twice keeps its position") {
    // So that a record rewritten after one edit is a one-line diff rather than a reordering.
    ImportOptions options;
    CY_REQUIRE(options.set(schema(), "scale", OptionValue::of_float(2.0)).has_value());
    CY_REQUIRE(options.set(schema(), "mips", OptionValue::of_bool(false)).has_value());
    CY_REQUIRE(options.set(schema(), "scale", OptionValue::of_float(3.0)).has_value());
    CY_CHECK(options.size() == 2);
    CY_CHECK(options.name_at(0) == "scale");
    CY_CHECK(options.value_at(0).as_float() == 3.0);
}

CY_TEST_CASE("options: a schema a caller could not act on is refused") {
    constexpr OptionSpec kNoDescription[] = {
        {"quality", OptionType::Bool, OptionValue::of_bool(true), "", {}, 0.0, 0.0}};
    CY_CHECK(!OptionsSchema(cy::Span<const OptionSpec>(kNoDescription)).validate().has_value());

    constexpr OptionSpec kDuplicated[] = {{"a",
                                           OptionType::Bool,
                                           OptionValue::of_bool(true),
                                           "A long enough description.",
                                           {},
                                           0.0,
                                           0.0},
                                          {"a",
                                           OptionType::Bool,
                                           OptionValue::of_bool(true),
                                           "A long enough description.",
                                           {},
                                           0.0,
                                           0.0}};
    CY_CHECK(!OptionsSchema(cy::Span<const OptionSpec>(kDuplicated)).validate().has_value());

    constexpr OptionSpec kBadDefault[] = {{"quality", OptionType::Enumeration,
                                           OptionValue::of_enumeration("medium"),
                                           "A quality that is not one of its own choices.",
                                           cy::Span<const std::string_view>(kChoices), 0.0, 0.0}};
    CY_CHECK(!OptionsSchema(cy::Span<const OptionSpec>(kBadDefault)).validate().has_value());

    CY_CHECK(schema().validate().has_value());
}

CY_TEST_CASE("importers: two importers cannot claim one extension") {
    // An ambiguity nothing downstream can resolve, so it is a registration error naming both rather
    // than a silent race in which whichever registered first wins.
    struct Fake final : Importer {
        Fake(std::string_view importer_name, cy::Span<const std::string_view> handled) noexcept
            : name(importer_name), extensions(handled) {}

        std::string_view name;
        cy::Span<const std::string_view> extensions;

        [[nodiscard]] ImporterInfo info() const noexcept override {
            ImporterInfo declared;
            declared.name = name;
            declared.version = 1;
            declared.extensions = extensions;
            declared.description = "A test importer that produces nothing at all.";
            return declared;
        }
        [[nodiscard]] OptionsSchema schema() const noexcept override { return {}; }
        [[nodiscard]] cy::Status import(const ImportRequest&, ImportResult&) noexcept override {
            return cy::ok();
        }
    };

    constexpr std::string_view kFirst[] = {".zzz"};
    constexpr std::string_view kSecond[] = {".ZZZ"};
    Fake first("first", cy::Span<const std::string_view>(kFirst));
    Fake second("second", cy::Span<const std::string_view>(kSecond));

    ImporterRegistry registry;
    CY_REQUIRE(registry.register_importer(&first).has_value());
    // Case-insensitively the same extension, because `.ZZZ` off a Windows filesystem is the same
    // file.
    CY_CHECK(!registry.register_importer(&second).has_value());
    CY_CHECK(registry.find_for_extension(".zzz") == &first);
    CY_CHECK(registry.find_for_extension(".ZZZ") == &first);
}
