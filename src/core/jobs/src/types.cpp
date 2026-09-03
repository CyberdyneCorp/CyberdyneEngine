// The vocabulary's out-of-line half: the enumerator names, and the one clock.

#include <cy/core/jobs/types.h>

#include <chrono>

namespace cy::jobs {

const char* priority_name(Priority priority) noexcept {
    switch (priority) {
        case Priority::Critical:
            return "Critical";
        case Priority::High:
            return "High";
        case Priority::Normal:
            return "Normal";
        case Priority::Background:
            return "Background";
        case Priority::Idle:
            return "Idle";
    }
    return "Unknown";
}

// steady_clock, never system_clock: a duration measured against a clock the user can set backwards
// is a duration that occasionally comes out negative, and every consumer here — the critical path,
// the watchdog, the queue latency — is measuring a duration.
i64 monotonic_now_ns() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<i64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

}  // namespace cy::jobs
