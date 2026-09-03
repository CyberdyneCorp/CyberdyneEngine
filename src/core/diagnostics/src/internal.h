#pragma once
// The clock the whole trace is stamped against.
//
// `diagnostics-profiling-and-crash` — "Correlating across subsystems": one timeline with one clock.
// Monotonic, in nanoseconds, never the wall clock: a capture's timings must not move when the
// system clock is adjusted. The wall clock appears once, in the file header, so a capture can be
// placed in time without any event depending on it.

#include <cy/core/diagnostics/prelude.h>

#include <chrono>

namespace cy::diag {

inline u64 monotonic_now_ns() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

}  // namespace cy::diag
