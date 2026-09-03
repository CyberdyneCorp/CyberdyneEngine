// The opt-in registry, and the promise that reflecting a type does not change it. Task 1.1.1.
//
// `core-type-system` has a scenario named "Component stays a POD": registering a struct for
// reflection leaves `sizeof` and the memory layout unchanged, and the type stays usable in packed
// chunk storage. That is asserted here against an unannotated twin of cy::demo::Health with the
// same members in the same order — if the annotation ever grew a hidden member or a base class, the
// static_asserts below stop compiling.

#include <cy/core/reflect/demo/types.reflect.h>
#include <cy/core/reflect/reflect.h>
#include <cy/test/test.h>

#include <cstddef>
#include <type_traits>

namespace {

// The same members, in the same order, with no annotation. Whatever CY_REFLECT_TYPE and
// CY_REFLECT_FIELD do to a struct, they must do nothing that this twin does not also have.
struct HealthTwin {
    cy::f32 maximum;
    cy::f32 current;
    cy::f32 displayed;
    cy::u8 last_damage;
    cy::u64 icon;
};

using Health = cy::demo::Health;

static_assert(sizeof(Health) == sizeof(HealthTwin), "annotation changed the size of the struct");
static_assert(alignof(Health) == alignof(HealthTwin), "annotation changed the alignment");
static_assert(offsetof(Health, icon) == offsetof(HealthTwin, icon),
              "annotation changed the layout");
static_assert(std::is_standard_layout_v<Health>, "a reflected type stays standard-layout");
static_assert(std::is_trivially_copyable_v<Health>, "a reflected component stays relocatable");
static_assert(!std::is_polymorphic_v<Health>, "a reflected type has no vtable");

cy::reflect::TypeRegistry make_registry() {
    cy::reflect::TypeRegistry registry;
    const auto registered = cy::reflect::register_generated_types(registry);
    CY_REQUIRE(registered.has_value());
    return registry;
}

}  // namespace

CY_TEST_CASE("registration is opt-in and idempotent") {
    cy::reflect::TypeRegistry registry;
    CY_CHECK_EQ(registry.size(), 0U);  // nothing is reflected until something registers it

    CY_REQUIRE(cy::reflect::register_generated_types(registry).has_value());
    const auto after_first = registry.size();
    CY_CHECK_GE(after_first, 2U);

    CY_REQUIRE(cy::reflect::register_generated_types(registry).has_value());
    CY_CHECK_EQ(registry.size(), after_first);
}

CY_TEST_CASE("a type is found by its identifier and by its name") {
    const auto registry = make_registry();
    const cy::reflect::TypeId id = cy::reflect::type_id_of<Health>();
    CY_REQUIRE(id.valid());

    const cy::reflect::TypeInfo* by_id = registry.find(id);
    CY_REQUIRE(by_id != nullptr);
    const cy::reflect::TypeInfo* by_name = registry.find("cy::demo::Health");
    CY_CHECK_EQ(by_id, by_name);
    CY_CHECK_EQ(by_id, &cy::reflect::type_of<Health>());
}

CY_TEST_CASE("an unknown identifier resolves to nothing rather than to something") {
    const auto registry = make_registry();
    CY_CHECK(registry.find(cy::reflect::TypeId{0xFFFFFFFFU}) == nullptr);
    CY_CHECK(registry.find(cy::reflect::TypeId{}) == nullptr);
    CY_CHECK(registry.find("cy::demo::NoSuchType") == nullptr);
}

CY_TEST_CASE("the descriptor reports the type the compiler built") {
    const cy::reflect::TypeInfo& info = cy::reflect::type_of<Health>();
    CY_CHECK_EQ(info.size, static_cast<cy::u32>(sizeof(Health)));
    CY_CHECK_EQ(info.alignment, static_cast<cy::u32>(alignof(Health)));
    CY_CHECK(info.trivially_relocatable);
    CY_CHECK_EQ(info.field_count, 5U);
    CY_CHECK(info.construct != nullptr);
    CY_CHECK(info.destruct != nullptr);

    // The header is recorded relative to the source root: an absolute path would differ between
    // checkouts and generated output has to be byte-reproducible.
    CY_CHECK_EQ(info.header[0], 's');
    CY_CHECK(cy::reflect::type_of<cy::demo::Placement>().id.valid());
}

CY_TEST_CASE("fields carry an identifier, an offset, and a name that is only metadata") {
    const cy::reflect::TypeInfo& info = cy::reflect::type_of<Health>();
    const cy::reflect::FieldInfo* maximum = info.find_field("maximum");
    CY_REQUIRE(maximum != nullptr);

    CY_CHECK(maximum->id.valid());
    CY_CHECK_EQ(maximum->kind, cy::reflect::FieldKind::F32);
    CY_CHECK_EQ(maximum->offset, static_cast<cy::u32>(offsetof(Health, maximum)));
    CY_CHECK_EQ(maximum->size, 4U);

    // Every field's identifier is distinct within the type, and the lookup by identifier agrees
    // with the lookup by name.
    for (cy::u32 outer = 0; outer < info.field_count; ++outer) {
        CY_CHECK_EQ(info.find_field(info.fields[outer].id), &info.fields[outer]);
        for (cy::u32 inner = outer + 1; inner < info.field_count; ++inner) {
            CY_CHECK_NE(info.fields[outer].id, info.fields[inner].id);
        }
    }
}

CY_TEST_CASE("construction and destruction thunks build the real type") {
    const cy::reflect::TypeInfo& info = cy::reflect::type_of<Health>();
    alignas(Health) unsigned char storage[sizeof(Health)];
    info.construct(storage);
    const auto* built = reinterpret_cast<const Health*>(storage);
    CY_CHECK_EQ(built->maximum, 100.0F);  // the default member initialiser ran
    info.destruct(storage);
}

CY_TEST_CASE("a second type may not claim an identifier that is taken") {
    cy::reflect::TypeRegistry registry;
    CY_REQUIRE(registry.add(cy::reflect::type_of<Health>()).has_value());

    // The shape a recycled identifier takes at run time: a different descriptor, the same number.
    // The manifest's tombstones are what stop it happening; this is the last place it is catchable.
    cy::reflect::TypeInfo impostor = cy::reflect::type_of<cy::demo::Placement>();
    impostor.id = cy::reflect::type_of<Health>().id;
    const auto added = registry.add(impostor);
    CY_REQUIRE_FALSE(added.has_value());
    CY_CHECK_EQ(added.error().code, cy::ErrorCode::AlreadyExists);
}

CY_TEST_CASE("the null identifier is never a valid registration") {
    cy::reflect::TypeRegistry registry;
    cy::reflect::TypeInfo nameless = cy::reflect::type_of<Health>();
    nameless.id = cy::reflect::TypeId{};
    const auto added = registry.add(nameless);
    CY_REQUIRE_FALSE(added.has_value());
    CY_CHECK_EQ(added.error().code, cy::ErrorCode::InvalidArgument);
}
