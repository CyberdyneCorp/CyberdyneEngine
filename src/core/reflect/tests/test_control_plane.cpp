// Reflection is control plane, not hot path — checked rather than asserted. Task 1.1.4.
//
// Two mechanisms, and this file exercises both.
//
// The *structural* one: a per-frame path holds a TypedAccessor, which is a byte offset. It cannot
// perform a lookup, because it has nothing to look anything up in. The million-iteration loop below
// is written the way a real system is written, and it makes no reflected call at all.
//
// The *detected* one: a hot region is declared, and any reflected lookup inside it increments a
// counter. That counter is a plain atomic, not an assertion, so it is live in Profile and Shipping
// as well — which is the point. CY_ASSERT is compiled out of exactly the two configurations where
// nobody would notice the violation.
//
// The deliberate violation at the end is what keeps the check honest: a check that can only pass is
// not a check.

#include <cy/core/reflect/demo/types.reflect.h>
#include <cy/core/reflect/reflect.h>
#include <cy/test/test.h>

namespace {

constexpr cy::usize entity_count = 4096;

}  // namespace

CY_TEST_CASE("a typed per-entity loop performs no reflected lookup") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();

    // Setup time: one lookup per field, resolved to an offset. This is the "resolved once" shape
    // `core-type-system` requires of anything dynamic that then runs at scale.
    auto current = cy::reflect::resolve_field<cy::f32>(health, health.find_field("current")->id);
    auto maximum = cy::reflect::resolve_field<cy::f32>(health, health.find_field("maximum")->id);
    CY_REQUIRE(current.has_value());
    CY_REQUIRE(maximum.has_value());

    cy::demo::Health entities[entity_count];
    cy::usize seed = 0;
    for (cy::demo::Health& entity : entities) {
        entity.maximum = static_cast<cy::f32>(++seed);
        entity.current = 0.0F;
    }

    cy::reflect::reset_control_plane_violations();
    cy::f32 total = 0.0F;
    {
        CY_REFLECT_HOT_REGION("test: per-entity health update");
        for (cy::demo::Health& entity : entities) {
            (*current)(&entity) = (*maximum)(&entity) * 0.5F;
            total += (*current)(&entity);
        }
    }

    CY_CHECK_EQ(cy::reflect::control_plane_violations(), 0U);
    CY_CHECK_GT(total, 0.0F);
    CY_CHECK_EQ(entities[3].current, 2.0F);
}

CY_TEST_CASE("a reflected lookup inside a hot region is counted") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    cy::reflect::reset_control_plane_violations();
    CY_REQUIRE_EQ(cy::reflect::control_plane_violations(), 0U);

    {
        CY_REFLECT_HOT_REGION("test: the violation this check exists to notice");
        const cy::reflect::FieldInfo* found = health.find_field("current");
        CY_CHECK(found != nullptr);
    }

    CY_CHECK_EQ(cy::reflect::control_plane_violations(), 1U);
    CY_REQUIRE(cy::reflect::last_violation_label() != nullptr);
    CY_CHECK_EQ(cy::reflect::last_violation_label(),
                doctest::String("test: the violation this check exists to notice"));
    cy::reflect::reset_control_plane_violations();
}

CY_TEST_CASE("control-plane work outside a hot region is free of suspicion") {
    cy::reflect::TypeRegistry registry;
    CY_REQUIRE(cy::reflect::register_generated_types(registry).has_value());
    cy::reflect::reset_control_plane_violations();

    // Exactly the work an inspector does: enumerate the registry, enumerate every field, look each
    // one up again by identifier. Dozens of reflected lookups, and not one of them a violation.
    for (const cy::reflect::TypeInfo* type : registry) {
        CY_CHECK(registry.find(type->name) == type);
        for (cy::u32 index = 0; index < type->field_count; ++index) {
            CY_CHECK(type->find_field(type->fields[index].id) != nullptr);
        }
    }

    CY_CHECK_EQ(cy::reflect::control_plane_violations(), 0U);
    CY_CHECK_FALSE(cy::reflect::in_hot_region());
}

CY_TEST_CASE("hot regions nest, and the innermost label is the one reported") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    cy::reflect::reset_control_plane_violations();
    {
        CY_REFLECT_HOT_REGION("outer");
        CY_CHECK(cy::reflect::in_hot_region());
        {
            cy::reflect::HotRegion inner{"inner"};
            CY_CHECK_EQ(cy::reflect::hot_region_label(), doctest::String("inner"));
            (void)health.find_field("icon");
        }
        CY_CHECK_EQ(cy::reflect::hot_region_label(), doctest::String("outer"));
    }
    CY_CHECK_FALSE(cy::reflect::in_hot_region());
    CY_CHECK_EQ(cy::reflect::control_plane_violations(), 1U);
    CY_CHECK_EQ(cy::reflect::last_violation_label(), doctest::String("inner"));
    cy::reflect::reset_control_plane_violations();
}

CY_TEST_CASE("a typed accessor refuses a field that does not hold its type") {
    const auto& health = cy::reflect::type_of<cy::demo::Health>();
    const cy::reflect::FieldId icon = health.find_field("icon")->id;

    // icon is a u64. Asking for it as an f32 is a setup-time error, not a reinterpretation.
    const auto wrong = cy::reflect::resolve_field<cy::f32>(health, icon);
    CY_REQUIRE_FALSE(wrong.has_value());
    CY_CHECK_EQ(wrong.error().code, cy::ErrorCode::InvalidArgument);

    const auto missing = cy::reflect::resolve_field<cy::f32>(health, cy::reflect::FieldId{9999});
    CY_REQUIRE_FALSE(missing.has_value());
    CY_CHECK_EQ(missing.error().code, cy::ErrorCode::NotFound);
}
