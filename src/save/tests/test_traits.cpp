// Persistence traits: derived from the one declaration, and agreeing with it. Task 6.1.

#include <cy/save/traits.h>
#include <cy/test/test.h>

#include "fixtures.h"

#include <string_view>

using namespace cy;
using namespace cy::save;
using namespace cy::save::test;

namespace {

const reflect::FieldInfo& field_of(const reflect::TypeInfo& type, const char* name) noexcept {
    for (u32 index = 0; index < type.field_count; ++index) {
        const reflect::FieldInfo& field = type.fields[index];
        const char* a = field.name;
        const char* b = name;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') {
            return field;
        }
    }
    CY_REQUIRE(false);  // the fixture has no such field
    return type.fields[0];
}

}  // namespace

CY_TEST_CASE("the schema decides what a save captures, and only PersistentState is captured") {
    // save-and-persistence, "Persistence traits": "WHEN a component gains a field THEN whether it
    // is saved SHALL follow from its declared traits with no save code change."
    const reflect::TypeInfo& health = health_type();
    CY_CHECK_FALSE(field_is_saved(field_of(health, "maximum")));
    CY_CHECK_FALSE(field_is_saved(field_of(health, "current")));
    CY_CHECK(field_is_saved(field_of(health, "revives")));
    CY_CHECK_FALSE(field_is_saved(field_of(health, "fraction")));
    CY_CHECK_FALSE(field_is_saved(field_of(health, "debug_counter")));
}

CY_TEST_CASE("derived data is absent from every column, which is what 'rebuilt' means") {
    // "Derived state SHALL NOT be saved: spatial indexes, cached transforms, GPU scene contents,
    // residency state ... are reconstructed after load."
    const reflect::FieldInfo& derived = field_of(health_type(), "fraction");
    CY_CHECK(has_trait(traits_of(derived), Trait::Derived));
    CY_CHECK_FALSE(has_trait(traits_of(derived), Trait::SaveGame));
    CY_CHECK_FALSE(has_trait(traits_of(derived), Trait::ReplayRelevant));
    CY_CHECK_FALSE(field_is_saved(derived));
}

CY_TEST_CASE("a transient field carries no trait at all") {
    const reflect::FieldInfo& transient = field_of(health_type(), "debug_counter");
    CY_CHECK_EQ(static_cast<u16>(traits_of(transient)), static_cast<u16>(Trait::None));
}

CY_TEST_CASE("replay relevance and persistence are different questions") {
    // A RuntimeState field is not saved and IS part of a verbatim capture. Conflating the two would
    // make a snapshot restore silently lose the current health of every robot in the world.
    const reflect::FieldInfo& runtime = field_of(health_type(), "current");
    CY_CHECK(has_trait(traits_of(runtime), Trait::RuntimeOnly));
    CY_CHECK(has_trait(traits_of(runtime), Trait::ReplayRelevant));
    CY_CHECK_FALSE(has_trait(traits_of(runtime), Trait::SaveGame));
}

CY_TEST_CASE("the trait table and the classification table cannot drift apart") {
    // The whole argument for deriving traits rather than declaring them a second time: this case
    // fails the moment the two answers differ, for every combination either can be built from.
    using reflect::PersistenceKind;
    const PersistenceKind kinds[] = {PersistenceKind::Authoring, PersistenceKind::RuntimeState,
                                     PersistenceKind::PersistentState, PersistenceKind::Derived};
    for (const PersistenceKind kind : kinds) {
        for (const bool transient : {false, true}) {
            const reflect::FieldInfo field =
                make_field("probe", 1, reflect::FieldKind::U32, 0, sizeof(u32), kind, transient);
            const bool saved = field_is_saved(field);
            CY_CHECK_EQ(saved, serialize::field_is_written(field, serialize::Purpose::Persistence));
            CY_CHECK_EQ(saved, has_trait(traits_of(field), Trait::SaveGame));
        }
    }
}

CY_TEST_CASE("a field routes to the world scope unless it declares another") {
    CY_CHECK_EQ(scope_of(field_of(health_type(), "revives")), Scope::World);
    CY_CHECK_EQ(scope_of(field_of(settings_type(), "gamma")), Scope::Profile);
    CY_CHECK(has_trait(traits_of(field_of(settings_type(), "gamma")), Trait::Profile));
    CY_CHECK_FALSE(has_trait(traits_of(field_of(health_type(), "revives")), Trait::Profile));
}

CY_TEST_CASE("an attribute whose generated struct changed size reads as absent, not as noise") {
    // find_custom<T>() checks the size before handing back a T. A project that changed its
    // attribute schema and rebuilt gets the default scope rather than a reinterpretation of
    // unrelated bytes.
    struct WiderAttribute {
        u8 scope;
        u64 padding;
    };
    static const WiderAttribute wider{static_cast<u8>(Scope::Profile), 0};
    static const reflect::CustomAttribute custom[] = {
        reflect::CustomAttribute{kSaveScopeAttribute, &wider, sizeof(WiderAttribute)},
    };
    reflect::FieldInfo field = make_field("probe", 1, reflect::FieldKind::U32, 0, sizeof(u32),
                                          reflect::PersistenceKind::PersistentState);
    field.attributes.custom = custom;
    field.attributes.custom_count = 1;
    CY_CHECK_EQ(scope_of(field), Scope::World);
}

CY_TEST_CASE("scopes have stable numbers and readable names") {
    // Persistent: the numbers are written into manifests.
    CY_CHECK_EQ(static_cast<u8>(Scope::Profile), 0);
    CY_CHECK_EQ(static_cast<u8>(Scope::World), 4);
    CY_CHECK_EQ(static_cast<u8>(Scope::Count), 6);
    CY_CHECK(is_known_scope(5));
    CY_CHECK_FALSE(is_known_scope(6));
    CY_CHECK_EQ(std::string_view(scope_name(Scope::Campaign)), std::string_view("campaign"));
}
