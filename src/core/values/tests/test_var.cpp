// `Var` — the shape, round-trip fidelity, explicit coercion and copy-on-write. Task 1.3.1.
//
// `core-type-system` — "Dynamic value type". Three of its scenarios are directly executable and are
// what this file is built around:
//
//   * "Round-trip fidelity" — a typed value converted to a `Var` and back is **bit-identical** for
//     every scalar and math type. Asserted with memcmp rather than with operator==, because the
//     requirement is about the representation and an equality operator could satisfy the weaker
//     reading of it.
//   * "Type coercion is explicit" — a `Var` holding a Float assigned to an Int field goes through
//     an API that reports narrowing. So `as_int()` on a Float must *fail*, and `coerce_to_int()`
//     must say that it narrowed.
//   * "Boundary use only" is a rule about where a `Var` appears, which no test can check. What is
//     checked instead is the property that makes the rule affordable: the value is 24 bytes and a
//     copy of a heap kind does not allocate.

#include <cy/core/values/var.h>

#include <cy/test/test.h>

#include <cstring>
#include <string>

namespace {

/// Bit-identical, which is what the round-trip requirement says. `T` is always a trivially copyable
/// aggregate here, so a memcmp over its exact size compares the representation and nothing else.
template <class T>
bool identical(const T& a, const T& b) noexcept {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

}  // namespace

CY_TEST_CASE("Var: a tag and sixteen bytes of payload") {
    CY_CHECK_EQ(sizeof(cy::Var), 24u);
    CY_CHECK_EQ(alignof(cy::Var), 8u);

    const cy::Var nil;
    CY_CHECK(nil.is_nil());
    CY_CHECK_EQ(nil.type(), cy::VarType::Nil);
    CY_CHECK_EQ(std::string(nil.type_name()), std::string("Nil"));
}

CY_TEST_CASE("Var: scalars round-trip bit-identically") {
    CY_CHECK_EQ(*cy::Var::from_bool(true).as_bool(), true);
    CY_CHECK_EQ(*cy::Var::from_int(-9007199254740993LL).as_int(), -9007199254740993LL);

    const cy::f64 pi = 3.14159265358979323846;
    const cy::f64 back = *cy::Var::from_float(pi).as_float();
    CY_CHECK(identical(pi, back));
}

CY_TEST_CASE("Var: every math shape round-trips bit-identically") {
    const cy::VarVec2 v2{1.5f, -2.5f};
    const cy::VarVec3 v3{1.0f, 2.0f, 3.0f};
    const cy::VarVec4 v4{1.0f, 2.0f, 3.0f, 4.0f};
    const cy::VarIVec2 i2{-1, 2};
    const cy::VarIVec3 i3{-1, 2, -3};
    const cy::VarIVec4 i4{-1, 2, -3, 4};
    const cy::VarQuat q{0.0f, 0.70710678f, 0.0f, 0.70710678f};
    const cy::VarColor c{0.25f, 0.5f, 0.75f, 1.0f};
    const cy::VarRect r{1.0f, 2.0f, 30.0f, 40.0f};
    const cy::VarPlane p{0.0f, 1.0f, 0.0f, -5.0f};

    CY_CHECK(identical(v2, *cy::Var::from_vec2(v2).as_vec2()));
    CY_CHECK(identical(v3, *cy::Var::from_vec3(v3).as_vec3()));
    CY_CHECK(identical(v4, *cy::Var::from_vec4(v4).as_vec4()));
    CY_CHECK(identical(i2, *cy::Var::from_ivec2(i2).as_ivec2()));
    CY_CHECK(identical(i3, *cy::Var::from_ivec3(i3).as_ivec3()));
    CY_CHECK(identical(i4, *cy::Var::from_ivec4(i4).as_ivec4()));
    CY_CHECK(identical(q, *cy::Var::from_quat(q).as_quat()));
    CY_CHECK(identical(c, *cy::Var::from_color(c).as_color()));
    CY_CHECK(identical(r, *cy::Var::from_rect(r).as_rect()));
    CY_CHECK(identical(p, *cy::Var::from_plane(p).as_plane()));

    // The four shapes too large for the inline payload take the same path through the heap.
    cy::VarMat3 m3;
    m3.columns[0][1] = 7.0f;
    cy::VarMat4 m4;
    m4.columns[3][0] = -1.0f;
    const cy::VarAabb box{cy::VarVec3{-1.0f, -2.0f, -3.0f}, cy::VarVec3{1.0f, 2.0f, 3.0f}};
    cy::VarTransform transform;
    transform.translation = cy::VarVec3{1.0f, 2.0f, 3.0f};

    CY_CHECK(identical(m3, *cy::Var::from_mat3(m3).as_mat3()));
    CY_CHECK(identical(m4, *cy::Var::from_mat4(m4).as_mat4()));
    CY_CHECK(identical(box, *cy::Var::from_aabb(box).as_aabb()));
    CY_CHECK(identical(transform, *cy::Var::from_transform(transform).as_transform()));
}

CY_TEST_CASE("Var: identity types round-trip and keep their kinds apart") {
    const cy::AssetId asset(0xdeadbeefULL, 0xfeedfaceULL);
    const cy::EntityId entity = cy::EntityId::from_slot(4, 9);
    const cy::AnyHandle handle{(3ull << 32) | 11ull, 5};

    CY_CHECK(*cy::Var::from_asset(asset).as_asset() == asset);
    CY_CHECK(*cy::Var::from_entity(entity).as_entity() == entity);
    CY_CHECK(*cy::Var::from_handle(handle).as_handle() == handle);

    // Reading one kind as another fails; it does not reinterpret the payload.
    CY_CHECK_FALSE(cy::Var::from_asset(asset).as_handle().has_value());
    CY_CHECK_FALSE(cy::Var::from_entity(entity).as_asset().has_value());
}

CY_TEST_CASE("Var: reading the wrong kind fails rather than converting") {
    const cy::Var number = cy::Var::from_float(2.5);
    const cy::Expected<cy::i64, cy::Error> as_int = number.as_int();
    CY_REQUIRE_FALSE(as_int.has_value());
    CY_CHECK(as_int.error().code == cy::ErrorCode::InvalidArgument);

    CY_CHECK_FALSE(cy::Var::from_string("7").as_int().has_value());
    CY_CHECK_FALSE(cy::Var().as_bool().has_value());
}

CY_TEST_CASE("Var: coercion is explicit and reports narrowing") {
    const cy::Expected<cy::IntCoercion, cy::Error> exact =
        cy::coerce_to_int(cy::Var::from_float(3.0));
    CY_REQUIRE(exact.has_value());
    CY_CHECK_EQ(exact->value, 3);
    CY_CHECK_FALSE(exact->narrowed);

    const cy::Expected<cy::IntCoercion, cy::Error> lossy =
        cy::coerce_to_int(cy::Var::from_float(3.75));
    CY_REQUIRE(lossy.has_value());
    CY_CHECK_EQ(lossy->value, 3);
    CY_CHECK(lossy->narrowed);

    const cy::Expected<cy::IntCoercion, cy::Error> huge =
        cy::coerce_to_int(cy::Var::from_float(1e300));
    CY_REQUIRE_FALSE(huge.has_value());
    CY_CHECK(huge.error().code == cy::ErrorCode::OutOfRange);

    // An i64 beyond 2^53 does not survive a trip through f64, and the coercion says so.
    const cy::Expected<cy::FloatCoercion, cy::Error> big =
        cy::coerce_to_float(cy::Var::from_int(9007199254740993LL));
    CY_REQUIRE(big.has_value());
    CY_CHECK(big->narrowed);

    CY_CHECK_FALSE(cy::coerce_to_int(cy::Var::from_string("3")).has_value());
    CY_CHECK_EQ(*cy::coerce_to_bool(cy::Var()), false);
    CY_CHECK_EQ(*cy::coerce_to_bool(cy::Var::from_int(2)), true);
    CY_CHECK_FALSE(cy::coerce_to_bool(cy::Var::from_string("")).has_value());
}

CY_TEST_CASE("Var: a string owns its text") {
    std::string source = "a path that will be overwritten";
    const cy::Var value = cy::Var::from_string(source);
    source = "something else entirely";

    const cy::Expected<std::string_view, cy::Error> text = value.as_string();
    CY_REQUIRE(text.has_value());
    CY_CHECK_EQ(std::string(*text), std::string("a path that will be overwritten"));
}

CY_TEST_CASE("Var: copying a heap kind shares the block; mutating detaches") {
    cy::Var array = cy::Var::empty_array();
    CY_REQUIRE(array.array_mut().has_value());
    CY_REQUIRE((*array.array_mut())->push(cy::Var::from_int(1)).has_value());

    cy::Var copy = array;  // shares
    CY_CHECK(array.is_shared());
    CY_CHECK(copy.is_shared());
    CY_CHECK_EQ(copy.array_size(), 1u);

    // Mutating through one must not be visible through the other.
    const cy::Expected<cy::VarArray*, cy::Error> mutable_copy = copy.array_mut();
    CY_REQUIRE(mutable_copy.has_value());
    CY_REQUIRE((*mutable_copy)->push(cy::Var::from_int(2)).has_value());

    CY_CHECK_EQ(copy.array_size(), 2u);
    CY_CHECK_EQ(array.array_size(), 1u);
    CY_CHECK_FALSE(array.is_shared());
    CY_CHECK_FALSE(copy.is_shared());
}

CY_TEST_CASE("Var: an inline kind is never shared") {
    const cy::Var value = cy::Var::from_int(5);
    const cy::Var copy = value;
    CY_CHECK_FALSE(value.is_shared());
    CY_CHECK_FALSE(copy.is_shared());
    CY_CHECK(value == copy);
}

CY_TEST_CASE("Var: dictionaries keep insertion order and look up by Name") {
    cy::Var dict = cy::Var::empty_dict();
    const cy::Expected<cy::VarDict*, cy::Error> entries = dict.dict_mut();
    CY_REQUIRE(entries.has_value());

    const cy::Name health = cy::Name::intern("health");
    const cy::Name armour = cy::Name::intern("armour");
    CY_REQUIRE((*entries)->set(health, cy::Var::from_int(100)).has_value());
    CY_REQUIRE((*entries)->set(armour, cy::Var::from_int(25)).has_value());
    // Replacing keeps the position rather than moving the entry to the end.
    CY_REQUIRE((*entries)->set(health, cy::Var::from_int(90)).has_value());

    CY_CHECK_EQ((*entries)->size(), 2u);
    CY_CHECK_EQ((*entries)->begin()[0].key, health);
    CY_CHECK_EQ(*(*entries)->begin()[0].value.as_int(), 90);
    CY_CHECK_EQ((*entries)->begin()[1].key, armour);

    CY_CHECK(dict.dict()->contains(armour));
    CY_CHECK_FALSE(dict.dict()->contains(cy::Name::intern("stamina")));
    CY_CHECK((*entries)->erase(armour));
    CY_CHECK_EQ((*entries)->size(), 1u);
}

CY_TEST_CASE("Var: equality is structural for the heap kinds") {
    const cy::Var a = cy::Var::from_string("same");
    const cy::Var b = cy::Var::from_string("same");
    const cy::Var c = cy::Var::from_string("other");
    CY_CHECK(a == b);
    CY_CHECK(a != c);
    CY_CHECK(cy::Var::from_int(1) != cy::Var::from_float(1.0));  // the kind is part of the value

    const cy::Var values[] = {cy::Var::from_int(1), cy::Var::from_int(2)};
    const cy::Var array_a = cy::Var::from_array(values, 2);
    const cy::Var array_b = cy::Var::from_array(values, 2);
    CY_CHECK(array_a == array_b);
    CY_CHECK_EQ(cy::var_array(array_a).size(), 2u);
}

CY_TEST_CASE("Var: bytes round-trip, including the empty buffer") {
    const cy::u8 payload[] = {0, 1, 2, 250};
    const cy::Var value = cy::Var::from_bytes(payload, sizeof(payload));

    cy::usize size = 0;
    const cy::Expected<const cy::u8*, cy::Error> data = value.as_bytes(size);
    CY_REQUIRE(data.has_value());
    CY_CHECK_EQ(size, sizeof(payload));
    CY_CHECK_EQ(std::memcmp(*data, payload, sizeof(payload)), 0);

    const cy::Var empty = cy::Var::from_bytes(nullptr, 0);
    cy::usize empty_size = 1;
    CY_CHECK(empty.as_bytes(empty_size).has_value());
    CY_CHECK_EQ(empty_size, 0u);
}

CY_TEST_CASE("Var: moving leaves the source nil and frees nothing early") {
    cy::Var source = cy::Var::from_string("moved");
    const cy::Var destination = std::move(source);
    CY_CHECK(source.is_nil());
    CY_CHECK_EQ(std::string(*destination.as_string()), std::string("moved"));
}

CY_TEST_CASE("Var: a math type crosses through var_payload_cast") {
    // Stands in for `cy::Vec3` from core-math, which does not exist yet: any trivially copyable
    // type of the same size crosses in one memcpy, checked at compile time.
    struct ExternalVec3 {
        float x, y, z;
    };
    const ExternalVec3 external{1.0f, 2.0f, 3.0f};

    const cy::Var value = cy::Var::from_vec3(cy::var_payload_cast<cy::VarVec3>(external));
    const ExternalVec3 back = cy::var_payload_cast<ExternalVec3>(*value.as_vec3());
    CY_CHECK(identical(external, back));
}
