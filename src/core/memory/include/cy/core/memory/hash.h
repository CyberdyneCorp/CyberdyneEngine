#pragma once
// Hashing, defined in one place. Task 2.4.
//
// `core-memory-and-containers` — "Associative containers": hashing is defined in one place
// (`core/hash`), is seeded per process in development builds to catch iteration-order dependencies,
// and is deterministic in shipping builds.
//
// THE SEED IS A TEST, NOT A SECURITY MEASURE. A development build randomises it so that code which
// depends on `HashMap` iteration order produces different results on different runs and is caught
// by a test that ran twice; a shipping build fixes it so that behaviour is reproducible. Anything
// that must iterate in a defined order uses `OrderedMap` or `FlatMap` instead — the seed makes the
// dependency visible, it does not make it acceptable.
//
// The mixer is the multiply-fold construction wyhash popularised: two 64-bit multiplies whose
// high and low halves are combined. It is not a cryptographic hash and is not claimed to be.

#include <cy/core/base/types.h>

#include <cstring>
#include <string_view>
#include <type_traits>

namespace cy {

/// The process's hash seed. Random per process in development builds, a fixed constant otherwise.
[[nodiscard]] u64 hash_seed() noexcept;

/// Force a seed. Tests only, and the one thing that makes a hash-order test reproducible when it
/// has found a failure.
void set_hash_seed(u64 seed) noexcept;

namespace detail {

/// 64x64 -> 128 multiply, folded to 64 bits by xor. The whole of the mixing.
[[nodiscard]] inline u64 mix(u64 a, u64 b) noexcept {
    const __uint128_t product = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
    return static_cast<u64>(product) ^ static_cast<u64>(product >> 64);
}

inline constexpr u64 kSecret0 = 0xa0761d6478bd642full;
inline constexpr u64 kSecret1 = 0xe7037ed1a0b428dbull;
inline constexpr u64 kSecret2 = 0x8ebc6af09c88c6e3ull;

/// Read `count` bytes (1..8) as a little-endian integer, without an unaligned load.
[[nodiscard]] inline u64 read_bytes(const u8* data, usize count) noexcept {
    u64 value = 0;
    std::memcpy(&value, data, count);
    return value;
}

}  // namespace detail

/// Hash arbitrary bytes with an explicit seed.
[[nodiscard]] u64 hash_bytes(const void* data, usize size, u64 seed) noexcept;

/// Hash arbitrary bytes with the process seed.
[[nodiscard]] inline u64 hash_bytes(const void* data, usize size) noexcept {
    return hash_bytes(data, size, hash_seed());
}

/// Fold one hash into another. Order matters, which is what makes it usable for a composite key.
[[nodiscard]] inline u64 hash_combine(u64 accumulator, u64 value) noexcept {
    return detail::mix(accumulator ^ detail::kSecret1, value ^ detail::kSecret2);
}

/// Hash an integral or enumeration value. Separate from `hash_bytes` because a scalar does not need
/// a loop, and because the mixing has to be good enough that consecutive indices — the commonest
/// key in an engine — do not collide in the low bits an open-addressed table probes with.
[[nodiscard]] inline u64 hash_integer(u64 value, u64 seed) noexcept {
    return detail::mix(value ^ seed ^ detail::kSecret0, detail::kSecret1);
}

/// The hash of `T`. Specialise for a type of your own; the defaults cover scalars, pointers and
/// the string views.
template <class T, class Enable = void>
struct Hash;

template <class T>
struct Hash<T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>>> {
    [[nodiscard]] u64 operator()(T value) const noexcept {
        return hash_integer(static_cast<u64>(value), hash_seed());
    }
};

template <class T>
struct Hash<T*, void> {
    [[nodiscard]] u64 operator()(T* value) const noexcept {
        return hash_integer(reinterpret_cast<u64>(value), hash_seed());
    }
};

template <>
struct Hash<std::string_view, void> {
    [[nodiscard]] u64 operator()(std::string_view value) const noexcept {
        return hash_bytes(value.data(), value.size());
    }
};

template <>
struct Hash<f32, void> {
    /// Zero is normalised so that -0.0 and 0.0, which compare equal, hash equal.
    [[nodiscard]] u64 operator()(f32 value) const noexcept {
        const f32 normalised = (value == 0.0f) ? 0.0f : value;
        u32 bits = 0;
        std::memcpy(&bits, &normalised, sizeof(bits));
        return hash_integer(bits, hash_seed());
    }
};

template <>
struct Hash<f64, void> {
    [[nodiscard]] u64 operator()(f64 value) const noexcept {
        const f64 normalised = (value == 0.0) ? 0.0 : value;
        u64 bits = 0;
        std::memcpy(&bits, &normalised, sizeof(bits));
        return hash_integer(bits, hash_seed());
    }
};

}  // namespace cy
