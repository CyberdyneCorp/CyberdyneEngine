#include <cy/platform/host_loop.h>

#include <chrono>
#include <csignal>
#include <thread>

namespace cy {
namespace {

// The only thing a signal handler may safely touch, and the only thing this one does touch. The
// loop reads it between frames, where the process is in a state it understands.
volatile std::sig_atomic_t g_interrupt = 0;

using SignalHandler = void (*)(int);

SignalHandler g_previous_int = nullptr;
SignalHandler g_previous_term = nullptr;

extern "C" void on_interrupt(int /*signal*/) {
    g_interrupt = 1;
}

}  // namespace

// std::this_thread::sleep_for rather than a platform call: the wait is the host's own pacing, not a
// service the engine offers, and the standard library's sleep is the same code on all three desktop
// platforms. It sleeps for slightly less than the remainder and yields the rest, because a sleep
// that overshoots by a scheduler quantum every frame is a frame rate nobody asked for.
void wait_for_next_frame(const Platform& platform, Nanoseconds frame_started_ns,
                         Nanoseconds interval_ns) noexcept {
    if (interval_ns <= 0) {
        return;
    }
    constexpr Nanoseconds kSpinMargin = 1000000;  // 1 ms

    const Nanoseconds deadline = frame_started_ns + interval_ns;
    const Nanoseconds remaining = deadline - platform.monotonic_nanoseconds();
    if (remaining > kSpinMargin) {
        std::this_thread::sleep_for(std::chrono::nanoseconds{remaining - kSpinMargin});
    }
    while (platform.monotonic_nanoseconds() < deadline) {
        std::this_thread::yield();
    }
}

bool interrupt_requested() noexcept {
    return g_interrupt != 0;
}

void clear_interrupt() noexcept {
    g_interrupt = 0;
}

// std::signal rather than sigaction: it is standard C++ and therefore the same code on Windows,
// where the CRT raises SIGINT for Ctrl+C. What the handler does — set one sig_atomic_t — is inside
// what the standard guarantees for a signal handler, so the portable form costs nothing here.
InterruptScope::InterruptScope(bool enabled) noexcept : enabled_(enabled) {
    if (!enabled_) {
        return;
    }
    clear_interrupt();
    g_previous_int = std::signal(SIGINT, on_interrupt);
    g_previous_term = std::signal(SIGTERM, on_interrupt);
}

InterruptScope::~InterruptScope() noexcept {
    if (!enabled_) {
        return;
    }
    if (g_previous_int != SIG_ERR && g_previous_int != nullptr) {
        (void)std::signal(SIGINT, g_previous_int);
    } else {
        (void)std::signal(SIGINT, SIG_DFL);
    }
    if (g_previous_term != SIG_ERR && g_previous_term != nullptr) {
        (void)std::signal(SIGTERM, g_previous_term);
    } else {
        (void)std::signal(SIGTERM, SIG_DFL);
    }
    g_previous_int = nullptr;
    g_previous_term = nullptr;
}

}  // namespace cy
