// SPDX-License-Identifier: MIT
// COOKED TERRAIN HEIGHTS. Task 6.1, design §8: heights converted once to Fixed16, read per tick by
// a shift, a mask and a bilinear blend, never as floats.

#include <cy/movement/height_field.h>

#include "movement_fixture.h"

namespace {

using cy::f32;
using cy::movement::FixedHeightField;
using namespace cy::movement_test;

constexpr f32 kHeights[] = {
    0.0F,  2.0F,  4.0F,   // z = 0
    0.0F,  2.0F,  4.0F,   // z = 2
    10.0F, 10.0F, 10.0F,  // z = 4
};

}  // namespace

CY_TEST_CASE("movement heights: samples blend bilinearly between cooked values") {
    FixedHeightField field(allocator());
    CY_REQUIRE(field.cook(cy::Span<const f32>(kHeights), 3, 3, at(0, 0), 1).has_value());
    CY_CHECK_EQ(field.width(), 3U);
    CY_CHECK(field.height_at(at(0, 0)) == metres(0));
    CY_CHECK(field.height_at(at(2, 0)) == metres(2));
    CY_CHECK(field.height_at(at(1, 0)) == metres(1));
    CY_CHECK(field.height_at(at(3, 1)) == metres(3));
    CY_CHECK(field.height_at(at(2, 3)) == metres(6));
    // Outside the field: the nearest edge sample.
    CY_CHECK(field.height_at(at(-5, -5)) == metres(0));
    CY_CHECK(field.height_at(at(100, 100)) == metres(10));
    CY_CHECK(field.height_at(at(100, 0)) == metres(4));
}

CY_TEST_CASE("movement heights: a sub-metre spacing is a negative shift") {
    FixedHeightField field(allocator());
    CY_REQUIRE(field.cook(cy::Span<const f32>(kHeights), 3, 3, at(0, 0), -1).has_value());
    CY_CHECK(field.height_at(FixedVec2{fraction(1, 2), Fixed::zero()}) == metres(1));
    CY_CHECK(field.height_at(FixedVec2{fraction(1, 1), fraction(1, 1)}) == metres(2));
}

CY_TEST_CASE("movement heights: a cooked sample is Q16.16, and what it cannot hold is refused") {
    FixedHeightField field(allocator());
    const f32 fine[] = {0.1F, 0.1F, 0.1F, 0.1F};
    CY_REQUIRE(field.cook(cy::Span<const f32>(fine), 2, 2, at(0, 0), 0).has_value());
    // 0.1 is stored to the nearest 2^-16: what a tick reads is that, exactly, everywhere.
    CY_CHECK_EQ(field.samples()[0].raw, 6554);
    CY_CHECK(field.height_at(at(0, 0)) == Fixed::from_raw(cy::i64{6554} << 16));

    const f32 too_high[] = {40000.0F};
    CY_CHECK_FALSE(field.cook(cy::Span<const f32>(too_high), 1, 1, at(0, 0), 0).has_value());
    CY_CHECK_FALSE(field.cook(cy::Span<const f32>(fine), 3, 1, at(0, 0), 0).has_value());
    CY_CHECK_FALSE(field.cook(cy::Span<const f32>(fine), 2, 2, at(0, 0), 20).has_value());
}
