// The engine's fixed-width type vocabulary. Task 3.1.3.
//
// This is deliberately the whole of it. `core-type-system` defines containers, allocators, string
// types and math at M1, and writing any of them here would mean writing them before the
// specification that governs them exists (design.md §9). Until then, engine code that needs a
// sequence uses the standard library and says so.

#pragma once

#include <cstddef>
#include <cstdint>

namespace cy {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

// The size and the difference of an object in memory. usize is std::size_t rather than u64 so that
// it stays the platform's index type on a 32-bit target.
using usize = std::size_t;
using isize = std::ptrdiff_t;

static_assert(sizeof(f32) == 4, "f32 must be a 32-bit IEEE-754 binary float");
static_assert(sizeof(f64) == 8, "f64 must be a 64-bit IEEE-754 binary float");

// Nanoseconds on the monotonic clock. Signed so that a difference of two readings is representable
// without the caller reasoning about wraparound; 63 bits of nanoseconds is 292 years.
using Nanoseconds = i64;

}  // namespace cy
