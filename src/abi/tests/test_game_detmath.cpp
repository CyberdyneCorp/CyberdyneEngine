// SPDX-License-Identifier: MIT
// ABI 1.8's deterministic math: the `detmath_*` entries, the CY_VAR_FIXED field kind and its typed
// fast path, and the float-write refusal of a cross-platform session.
// openspec/changes/add-deterministic-math tasks 8.1 and 8.2.
//
// The entries are held to the C++ functions they forward to over a seeded sweep of each, and to a
// few committed golden-vector lines; the kernel itself is `unit.detmath`'s and
// `integration.detmath_vectors`'s.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/abi/var.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/functions.h>
#include <cy/core/detmath/version.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/test/test.h>

#include <cstring>

namespace {

using cy::i64;
using cy::u32;
using cy::u64;
namespace dm = cy::detmath;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

/// SplitMix64, for a sweep of raw inputs spread over every binade.
struct Draws {
    u64 state = 0xAB1'0008ULL;
    u64 next() noexcept {
        state += 0x9E3779B97F4A7C15ULL;
        u64 z = state;
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    i64 scaled() noexcept {
        const auto value = static_cast<i64>(next());
        return value >> static_cast<int>(next() & 63U);
    }
};

dm::Fixed fx(i64 raw) noexcept {
    return dm::Fixed::from_raw(raw);
}

dm::Angle an(u64 raw) noexcept {
    return dm::Angle::from_raw(static_cast<u32>(raw));
}

// A component with a fixed-point field beside a float one.
struct Unit {
    i64 position_x = 0;
    float speed = 0.0F;
    float facing[3] = {0.0F, 0.0F, 0.0F};
};

const CyFieldDesc kUnitFields[] = {
    {sizeof(CyFieldDesc), CY_VAR_FIXED, 0, 8, "position_x"},
    {sizeof(CyFieldDesc), CY_VAR_F32, 8, 4, "speed"},
    {sizeof(CyFieldDesc), CY_VAR_VEC3, 12, 12, "facing"},
};

struct Bound {
    cy::ecs::World world{allocator()};
    cy::abi::World binding{allocator(), world};
    cy::abi::Host host{allocator()};
    CyComponentTypeId unit = CY_COMPONENT_TYPE_INVALID;
    CyEntity entity = CY_ENTITY_NULL;

    Bound() {
        CY_REQUIRE(world.initialize().has_value());
        host.bind_world(&binding);
        CyComponentTypeDesc desc{};
        desc.struct_size = sizeof(desc);
        desc.size = sizeof(Unit);
        desc.alignment = alignof(Unit);
        desc.field_count = 3;
        desc.name = "DetmathUnit";
        desc.fields = kUnitFields;
        unit = table().world_register_component(&binding, &desc);
        CY_REQUIRE(unit != CY_COMPONENT_TYPE_INVALID);
        entity = table().world_create_entity(&binding);
        CY_REQUIRE_EQ(table().world_add_component(&binding, entity, unit, nullptr), CY_RESULT_OK);
    }
};

}  // namespace

CY_TEST_CASE("detmath: every entry is the engine's function, raw value for raw value") {
    const CyInterface& iface = table();
    CY_CHECK_EQ(iface.detmath_kernel_version(), dm::kKernelVersion);
    Draws draws;
    for (u32 index = 0; index < 64; ++index) {
        const i64 x = draws.scaled();
        const i64 y = draws.scaled();
        const u64 a = draws.next();
        CY_CHECK_EQ(iface.detmath_sqrt(x), dm::sqrt(fx(x)).raw);
        CY_CHECK_EQ(iface.detmath_sin(static_cast<u32>(a)), dm::sin(an(a)).raw);
        CY_CHECK_EQ(iface.detmath_cos(static_cast<u32>(a)), dm::cos(an(a)).raw);
        CY_CHECK_EQ(iface.detmath_tan(static_cast<u32>(a)), dm::tan(an(a)).raw);
        CY_CHECK_EQ(iface.detmath_atan(x), dm::atan(fx(x)).raw);
        CY_CHECK_EQ(iface.detmath_atan2(y, x), dm::atan2(fx(y), fx(x)).raw);
        CY_CHECK_EQ(iface.detmath_asin(x >> 32), dm::asin(fx(x >> 32)).raw);
        CY_CHECK_EQ(iface.detmath_acos(x >> 32), dm::acos(fx(x >> 32)).raw);
        CY_CHECK_EQ(iface.detmath_exp2(x >> 28), dm::exp2(fx(x >> 28)).raw);
        CY_CHECK_EQ(iface.detmath_log2(x), dm::log2(fx(x)).raw);
        CY_CHECK_EQ(iface.detmath_exp(x >> 28), dm::exp(fx(x >> 28)).raw);
        CY_CHECK_EQ(iface.detmath_log(x), dm::log(fx(x)).raw);
        CY_CHECK_EQ(iface.detmath_pow(x & INT64_MAX, y >> 30),
                    dm::pow(fx(x & INT64_MAX), fx(y >> 30)).raw);
    }
    // Committed golden lines (tools/detmath/vectors/sin.txt and atan2.txt): a quarter turn's sine
    // is exactly one, and the angle of (0, 1) is exactly a quarter turn.
    CY_CHECK_EQ(iface.detmath_sin(1U << 30U), i64{1} << 32U);
    CY_CHECK_EQ(iface.detmath_atan2(i64{1} << 32U, 0), 1U << 30U);
    CY_CHECK_EQ(iface.detmath_sqrt(i64{4} << 32U), i64{2} << 32U);
}

CY_TEST_CASE("detmath: the span entry applies one function to every slot, angles in the low bits") {
    const CyInterface& iface = table();
    Draws draws;
    CyFixed x[16] = {};
    CyFixed y[16] = {};
    CyFixed out[16] = {};
    for (u32 index = 0; index < 16; ++index) {
        x[index] = draws.scaled();
        y[index] = draws.scaled();
    }
    CY_REQUIRE_EQ(iface.detmath_evaluate(CY_DETMATH_ATAN2, x, y, out, 16), CY_RESULT_OK);
    for (u32 index = 0; index < 16; ++index) {
        CY_CHECK_EQ(out[index], static_cast<CyFixed>(iface.detmath_atan2(x[index], y[index])));
    }
    CY_REQUIRE_EQ(iface.detmath_evaluate(CY_DETMATH_SIN, x, nullptr, out, 16), CY_RESULT_OK);
    for (u32 index = 0; index < 16; ++index) {
        // Only the low 32 bits of the slot are the angle.
        CY_CHECK_EQ(out[index],
                    iface.detmath_sin(static_cast<CyAngle>(static_cast<u64>(x[index]))));
    }
    // Refused, having written nothing: an unknown function, a missing span, pow without y.
    out[0] = 77;
    CY_CHECK_EQ(iface.detmath_evaluate(13, x, y, out, 16), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.detmath_evaluate(CY_DETMATH_SQRT, nullptr, nullptr, out, 16),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.detmath_evaluate(CY_DETMATH_POW, x, nullptr, out, 16),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(out[0], 77);
    // An empty span needs no pointers.
    CY_CHECK_EQ(iface.detmath_evaluate(CY_DETMATH_POW, nullptr, nullptr, nullptr, 0), CY_RESULT_OK);
}

CY_TEST_CASE("a fixed-point field round-trips its raw integer, never through a float") {
    // `native-abi`: "A fixed value round-trips".
    Bound bound;
    const CyInterface& iface = table();
    CyWorld world = &bound.binding;
    // A raw value no f64 holds exactly: 2^62 + 1.
    const i64 raw = (i64{1} << 62U) + 1;
    CY_REQUIRE_EQ(iface.component_set_fixed(world, bound.entity, bound.unit, 0, raw), CY_RESULT_OK);
    CyFixed back = 0;
    CY_REQUIRE_EQ(iface.component_get_fixed(world, bound.entity, bound.unit, 0, &back),
                  CY_RESULT_OK);
    CY_CHECK_EQ(back, raw);
    CyVar read = cy::abi::var_nil();
    CY_REQUIRE_EQ(iface.component_get_var(world, bound.entity, bound.unit, 0, &read), CY_RESULT_OK);
    CY_CHECK_EQ(read.type, static_cast<u32>(CY_VAR_FIXED));
    CY_CHECK_EQ(read.payload.as_i64, raw);
    CyVar write = cy::abi::var_i64(-5);
    write.type = CY_VAR_FIXED;
    CY_REQUIRE_EQ(iface.component_set_var(world, bound.entity, bound.unit, 0, &write),
                  CY_RESULT_OK);
    CY_REQUIRE_EQ(iface.component_get_fixed(world, bound.entity, bound.unit, 0, &back),
                  CY_RESULT_OK);
    CY_CHECK_EQ(back, -5);
    // The typed path names its kind: a float field is not fixed, and a fixed field is not a float.
    CY_CHECK_EQ(iface.component_get_fixed(world, bound.entity, bound.unit, 1, &back),
                CY_RESULT_INVALID_ARGUMENT);
    float speed = 0.0F;
    CY_CHECK_EQ(iface.component_get_f32(world, bound.entity, bound.unit, 0, &speed),
                CY_RESULT_INVALID_ARGUMENT);
    // An integer tag is not the fixed tag: the tag names the format.
    CyVar integer = cy::abi::var_i64(3);
    CY_CHECK_EQ(iface.component_set_var(world, bound.entity, bound.unit, 0, &integer),
                CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("under Lockstep a float write to a fixed-point field is refused, naming the field") {
    // `native-abi`: "A float write to a fixed field is refused".
    for (const cy::determinism::DeterminismProfile profile :
         {cy::determinism::DeterminismProfile::Lockstep,
          cy::determinism::DeterminismProfile::CrossPlatform}) {
        Bound bound;
        bound.binding.set_determinism_profile(profile);
        const CyInterface& iface = table();
        CyWorld world = &bound.binding;
        CY_CHECK_EQ(iface.component_set_f32(world, bound.entity, bound.unit, 0, 1.5F),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK(std::strstr(cy::abi::last_error_message(), "position_x") != nullptr);
        const float xyz[3] = {1.0F, 2.0F, 3.0F};
        CY_CHECK_EQ(iface.component_set_vec3(world, bound.entity, bound.unit, 0, xyz),
                    CY_RESULT_PERMISSION_DENIED);
        for (CyVar value : {cy::abi::var_f64(1.5), cy::abi::var_floats(CY_VAR_VEC2, xyz, 2)}) {
            CY_CHECK_EQ(iface.component_set_var(world, bound.entity, bound.unit, 0, &value),
                        CY_RESULT_PERMISSION_DENIED);
            CY_CHECK(std::strstr(cy::abi::last_error_message(), "position_x") != nullptr);
        }
        CyFixed back = 99;
        CY_REQUIRE_EQ(iface.component_get_fixed(world, bound.entity, bound.unit, 0, &back),
                      CY_RESULT_OK);
        CY_CHECK_EQ(back, 0);  // nothing was written
        // The raw write is the way in, and a float FIELD still takes a float.
        CY_CHECK_EQ(iface.component_set_fixed(world, bound.entity, bound.unit, 0, 42),
                    CY_RESULT_OK);
        CY_CHECK_EQ(iface.component_set_f32(world, bound.entity, bound.unit, 1, 2.5F),
                    CY_RESULT_OK);
    }
    // Under SamePlatform the same write is the ordinary type mismatch.
    Bound bound;
    CY_CHECK_EQ(table().component_set_f32(&bound.binding, bound.entity, bound.unit, 0, 1.5F),
                CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}
