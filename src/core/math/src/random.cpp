// Random — PCG32 and the distributions on top of it. Task 3.1.6.
// See include/cy/core/math/random.h.

#include <cy/core/math/random.h>

#include <cy/core/base/assert.h>

#include <chrono>
#include <cmath>
#include <thread>

namespace cy {
namespace {

/// The PCG-recommended 64-bit LCG multiplier. Not a tunable: the generator's statistical properties
/// are established for this constant and not for a nearby one.
constexpr u64 kMultiplier = 6364136223846793005ull;

/// SplitMix64's finaliser, used only to whisk together the entropy sources in `from_entropy`. It is
/// a mixing function, not a generator.
[[nodiscard]] u64 mix64(u64 x) noexcept {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

}  // namespace

void Random::seed(u64 seed_value, u64 stream_value) noexcept {
    // The increment must be odd for the LCG to have full period; the low bit is therefore not part
    // of the stream number, which is why `stream()` shifts it back out.
    state_.state = 0;
    state_.increment = (stream_value << 1u) | 1u;
    state_.draws = 0;
    state_.cached_normal = 0.0f;
    state_.has_cached_normal = false;
    // The standard PCG seeding dance: step, add the seed, step again, so that two nearby seeds do
    // not produce two nearby first outputs. The two steps are not counted as draws — `draws()` is a
    // count of values the *caller* has taken.
    const u64 draws_before = state_.draws;
    (void)next_u32();
    state_.state += seed_value;
    (void)next_u32();
    state_.draws = draws_before;
}

Random Random::from_entropy() noexcept {
    // Deliberately not `std::random_device`: its constructor reports failure by throwing, and the
    // engine is built with -fno-exceptions, so a machine without an entropy source would abort
    // inside a function whose whole job is to be a convenience. Three weak sources mixed are ample
    // for the uses this has — a session id, a fuzz seed, a procedural seed offered to a player.
    // It is not cryptographic and is not claimed to be.
    static u64 counter = 0;
    const u64 now = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
    const u64 wall = static_cast<u64>(std::chrono::system_clock::now().time_since_epoch().count());
    const u64 thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const u64 tick = ++counter;
    return Random(mix64(now ^ (wall << 1)), mix64(thread ^ (tick * kMultiplier)));
}

u32 Random::next_u32() noexcept {
    const u64 previous = state_.state;
    state_.state = previous * kMultiplier + state_.increment;
    ++state_.draws;
    // XSH-RR: xorshift the high bits down, then rotate by an amount taken from the very highest
    // bits. The data-dependent rotation is what defeats the lattice structure a bare LCG has.
    const u32 xorshifted = static_cast<u32>(((previous >> 18u) ^ previous) >> 27u);
    const u32 rotation = static_cast<u32>(previous >> 59u);
    return (xorshifted >> rotation) | (xorshifted << ((32u - rotation) & 31u));
}

u64 Random::next_u64() noexcept {
    const u64 high = static_cast<u64>(next_u32());
    const u64 low = static_cast<u64>(next_u32());
    return (high << 32) | low;
}

f32 Random::next_float() noexcept {
    // 24 bits, the mantissa's width, scaled by 2^-24. Using all 32 bits would round the largest
    // values to exactly 1.0 and break the half-open interval every caller assumes.
    return static_cast<f32>(next_u32() >> 8) * 0x1.0p-24f;
}

f32 Random::next_float_in(f32 low, f32 high) noexcept {
    return low + (high - low) * next_float();
}

u32 Random::next_u32_below(u32 bound) noexcept {
    CY_ASSERT_MSG(bound > 0, "Random::next_u32_below(0) has no value to return");
    if (bound == 0) {
        return 0;
    }
    // Reject the leading partial block so that every residue is equally likely. `-bound` in
    // unsigned arithmetic is 2^32 - bound, so this threshold is 2^32 mod bound.
    const u32 threshold = (~bound + 1u) % bound;
    for (;;) {
        const u32 value = next_u32();
        if (value >= threshold) {
            return value % bound;
        }
    }
}

i32 Random::next_int_in(i32 low, i32 high) noexcept {
    CY_ASSERT_MSG(low <= high, "Random::next_int_in() called with an inverted range");
    if (low >= high) {
        return low;
    }
    const u32 span = static_cast<u32>(static_cast<i64>(high) - static_cast<i64>(low) + 1);
    if (span == 0) {
        // The whole 32-bit range: every value is in it, so there is nothing to reject.
        return static_cast<i32>(next_u32());
    }
    return static_cast<i32>(static_cast<i64>(low) + static_cast<i64>(next_u32_below(span)));
}

bool Random::next_bool(f32 probability) noexcept {
    return next_float() < probability;
}

f32 Random::next_normal() noexcept {
    if (state_.has_cached_normal) {
        state_.has_cached_normal = false;
        return state_.cached_normal;
    }
    // Marsaglia's polar method: rejection sampling in the unit disk, then one log and one sqrt for
    // two independent normals. Preferred over the trigonometric Box-Muller because it needs no sine
    // or cosine, and the rejection rate is only 1 - pi/4.
    f32 u = 0.0f;
    f32 v = 0.0f;
    f32 s = 0.0f;
    do {
        u = next_float() * 2.0f - 1.0f;
        v = next_float() * 2.0f - 1.0f;
        s = u * u + v * v;
    } while (s >= 1.0f || s == 0.0f);

    const f32 factor = std::sqrt(-2.0f * std::log(s) / s);
    state_.cached_normal = v * factor;
    state_.has_cached_normal = true;
    return u * factor;
}

f32 Random::next_normal_in(f32 mean, f32 standard_deviation) noexcept {
    return mean + standard_deviation * next_normal();
}

Vec3 Random::on_unit_sphere() noexcept {
    // Archimedes: the projection of a sphere onto its axis is uniform, so sampling z uniformly in
    // [-1, 1] and the azimuth uniformly gives a distribution uniform in area. Sampling two angles
    // uniformly would crowd the poles, which shows up immediately as a bright spot in any lighting
    // that uses it.
    const f32 z = next_float() * 2.0f - 1.0f;
    const f32 azimuth = next_float() * math::kTwoPi;
    const f32 radius = std::sqrt(math::max(0.0f, 1.0f - z * z));
    return Vec3{radius * std::cos(azimuth), radius * std::sin(azimuth), z};
}

Vec3 Random::in_unit_sphere() noexcept {
    // The cube root is what makes it uniform in volume: without it the samples bunch toward the
    // centre, because a shell's volume grows as r².
    const f32 radius = std::cbrt(next_float());
    return on_unit_sphere() * radius;
}

Vec3 Random::on_hemisphere(Vec3 normal) noexcept {
    const Vec3 v = on_unit_sphere();
    return dot(v, normal) < 0.0f ? -v : v;
}

Vec3 Random::on_cosine_hemisphere(Vec3 normal) noexcept {
    // Malley's method: a uniform disk sample lifted onto the hemisphere is cosine-distributed. Two
    // draws and no rejection, and the resulting weight cancels the cosine in a diffuse estimator.
    const Vec2 disk = in_unit_disk();
    const f32 height = std::sqrt(math::max(0.0f, 1.0f - length_squared(disk)));
    const Vec3 tangent = any_perpendicular(normal);
    const Vec3 bitangent = cross(normal, tangent);
    return tangent * disk.x + bitangent * disk.y + normal * height;
}

Vec2 Random::in_unit_disk() noexcept {
    // sqrt on the radius, for the same reason the sphere needs a cube root: area grows as r.
    const f32 radius = std::sqrt(next_float());
    const f32 angle = next_float() * math::kTwoPi;
    return Vec2{radius * std::cos(angle), radius * std::sin(angle)};
}

Vec2 Random::on_unit_circle() noexcept {
    const f32 angle = next_float() * math::kTwoPi;
    return Vec2{std::cos(angle), std::sin(angle)};
}

}  // namespace cy
