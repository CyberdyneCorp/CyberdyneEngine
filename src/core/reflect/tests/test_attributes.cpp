// Field attributes as typed data. Task 1.1.3.
//
// The specification's acceptance scenarios for attributes are "inspector renders from attributes
// alone", "transient field is not saved", "malformed attribute fails the build" and "custom
// attribute is typed". The first, second and fourth are here; the third cannot be a C++ test,
// because a build error is the observable — it is in test_generator.py, which runs the generator
// over a malformed annotation and reads the diagnostic.

#include <cy/core/reflect/demo/types.reflect.h>
#include <cy/core/reflect/reflect.h>
#include <cy/test/test.h>

namespace {

const cy::reflect::FieldInfo& field_of(const cy::reflect::TypeInfo& type, const char* name) {
    const cy::reflect::FieldInfo* found = type.find_field(name);
    CY_REQUIRE(found != nullptr);
    return *found;
}

}  // namespace

CY_TEST_CASE("a bounded field carries its bounds, not a string describing them") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    const auto& maximum = field_of(health, "maximum");

    CY_REQUIRE(maximum.attributes.declares(cy::reflect::AttributeKind::Range));
    CY_CHECK_EQ(maximum.attributes.range.minimum, 0.0);
    CY_CHECK_EQ(maximum.attributes.range.maximum, 10000.0);
    CY_CHECK_EQ(maximum.attributes.range.step, 1.0);

    // An inspector renders a slider from those three numbers with no code for this specific type,
    // which is what the attribute is for.
    CY_CHECK_EQ(maximum.attributes.unit, cy::reflect::UnitKind::Percent);
    CY_CHECK_EQ(cy::reflect::unit_name(maximum.attributes.unit), doctest::String("percent"));
}

CY_TEST_CASE("presentation text is text, and absence is distinguishable from emptiness") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    const auto& maximum = field_of(health, "maximum");
    CY_CHECK_EQ(maximum.attributes.category.text, doctest::String("Combat"));
    CY_CHECK_EQ(maximum.attributes.tooltip.text,
                doctest::String("The value healing cannot exceed"));

    const auto& icon = field_of(health, "icon");
    CY_CHECK_FALSE(icon.attributes.declares(cy::reflect::AttributeKind::Tooltip));
    CY_CHECK_EQ(icon.attributes.tooltip.text, doctest::String(""));
}

CY_TEST_CASE("a transient field is declared transient and nothing else has to know") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    CY_CHECK(field_of(health, "displayed").attributes.transient());
    CY_CHECK(field_of(health, "displayed").attributes.hidden());
    CY_CHECK_FALSE(field_of(health, "maximum").attributes.transient());
}

CY_TEST_CASE("the field classification is one declaration every consumer reads") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    using cy::reflect::PersistenceKind;
    CY_CHECK_EQ(field_of(health, "maximum").attributes.persistence, PersistenceKind::Authoring);
    CY_CHECK_EQ(field_of(health, "current").attributes.persistence, PersistenceKind::RuntimeState);
    CY_CHECK_EQ(field_of(health, "displayed").attributes.persistence, PersistenceKind::Derived);
}

CY_TEST_CASE("enumerators carry their persistent values") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    const auto& damage = field_of(health, "last_damage");
    CY_REQUIRE(damage.attributes.declares(cy::reflect::AttributeKind::Enum));
    CY_REQUIRE_EQ(damage.attributes.enumeration.count, 4U);
    CY_CHECK_EQ(damage.attributes.enumeration.values[0].name, doctest::String("Physical"));
    CY_CHECK_EQ(damage.attributes.enumeration.values[2].value, 2);
    CY_CHECK(damage.attributes.read_only());

    const auto& placement = cy::reflect::type_of<cy::demo::Placement>();
    const auto& flags = field_of(placement, "flags");
    CY_REQUIRE(flags.attributes.declares(cy::reflect::AttributeKind::Flags));
    CY_CHECK_EQ(flags.attributes.enumeration.values[3].value, 4);  // a bit, not an ordinal
}

CY_TEST_CASE("replication and asset references are typed, not parsed") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    const auto& current = field_of(health, "current");
    CY_REQUIRE(current.attributes.declares(cy::reflect::AttributeKind::Replicated));
    CY_CHECK_EQ(current.attributes.replicated.encoder, doctest::String("quantised"));
    CY_CHECK_EQ(current.attributes.replicated.parameters, doctest::String("bits=16"));

    const auto& icon = field_of(health, "icon");
    CY_REQUIRE(icon.attributes.declares(cy::reflect::AttributeKind::AssetRef));
    CY_CHECK_EQ(icon.attributes.asset_ref.kind, doctest::String("Texture"));
}

CY_TEST_CASE("a module's own attribute is a struct with named members") {
    const auto& placement = cy::reflect::type_of<cy::demo::Placement>();
    const auto& tile = field_of(placement, "tile");
    CY_REQUIRE_EQ(tile.attributes.custom_count, 1U);

    const auto* streaming =
        cy::reflect::find_custom<cy::demo::StreamingAttribute>(tile.attributes, "Streaming");
    CY_REQUIRE(streaming != nullptr);
    CY_CHECK_EQ(streaming->priority, 2);
    CY_CHECK(streaming->prefetch);

    // Asking for an attribute the field does not carry is a null, not a misread.
    CY_CHECK(cy::reflect::find_custom<cy::demo::StreamingAttribute>(tile.attributes, "Nope") ==
             nullptr);
    const auto& x = field_of(placement, "x");
    CY_CHECK(cy::reflect::find_custom<cy::demo::StreamingAttribute>(x.attributes, "Streaming") ==
             nullptr);
}
