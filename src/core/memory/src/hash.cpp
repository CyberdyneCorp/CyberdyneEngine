// The one hash function, and the per-process seed. Task 2.4.

#include <cy/core/memory/hash.h>

#include <atomic>
#include <chrono>
#include <random>

namespace cy {
namespace {

/// The shipping seed. A constant, so that a shipping build's iteration order is reproducible from
/// one run to the next and a bug report is diagnosable.
constexpr u64 kFixedSeed = 0x9e3779b97f4a7c15ull;

std::atomic<u64> g_seed{0};
std::atomic<bool> g_seeded{false};

[[nodiscard]] u64 initial_seed() noexcept {
#if defined(CY_DEVELOPMENT)
    // Randomised per process, so that code depending on iteration order gives different answers on
    // different runs. std::random_device rather than the clock alone: two processes started in the
    // same millisecond would otherwise agree, which is exactly the case a test harness produces.
    std::random_device device;
    const u64 high = static_cast<u64>(device()) << 32;
    const u64 low = static_cast<u64>(device());
    const auto now = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
    u64 seed = (high | low) ^ detail::mix(now, kFixedSeed);
    return (seed == 0) ? kFixedSeed : seed;
#else
    return kFixedSeed;
#endif
}

}  // namespace

u64 hash_seed() noexcept {
    if (!g_seeded.load(std::memory_order_acquire)) {
        // A race here costs one extra call to initial_seed(); whichever store lands last wins, and
        // both are valid seeds. Making this a mutex would put a lock on every hash of every key.
        g_seed.store(initial_seed(), std::memory_order_relaxed);
        g_seeded.store(true, std::memory_order_release);
    }
    return g_seed.load(std::memory_order_relaxed);
}

void set_hash_seed(u64 seed) noexcept {
    g_seed.store(seed, std::memory_order_relaxed);
    g_seeded.store(true, std::memory_order_release);
}

u64 hash_bytes(const void* data, usize size, u64 seed) noexcept {
    const auto* bytes = static_cast<const u8*>(data);
    u64 accumulator = seed ^ detail::mix(seed ^ detail::kSecret0, detail::kSecret1);
    usize remaining = size;

    // Sixteen bytes at a time: two independent multiplies per iteration, which keeps the dependency
    // chain short enough that the loop is limited by the loads rather than by the multiplier.
    while (remaining >= 16) {
        const u64 low = detail::read_bytes(bytes, 8);
        const u64 high = detail::read_bytes(bytes + 8, 8);
        accumulator = detail::mix(low ^ detail::kSecret1, high ^ accumulator);
        bytes += 16;
        remaining -= 16;
    }
    if (remaining >= 8) {
        accumulator = detail::mix(detail::read_bytes(bytes, 8) ^ detail::kSecret2, accumulator);
        bytes += 8;
        remaining -= 8;
    }
    if (remaining > 0) {
        accumulator = detail::mix(detail::read_bytes(bytes, remaining) ^ detail::kSecret0,
                                  accumulator ^ detail::kSecret1);
    }
    // The length is folded in last so that inputs differing only in trailing zero bytes differ.
    return detail::mix(accumulator ^ static_cast<u64>(size), detail::kSecret2);
}

}  // namespace cy
