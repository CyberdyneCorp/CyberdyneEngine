// The desktop host's loop. Task 3.4.3.
//
// THE LOOP LIVES HERE, and it is the only `while` in the engine that drives frames.
// `core-platform-abstraction` requires that a platform which drives frames itself — mobile, web —
// is not precluded: such a platform replaces this file with its own callback and calls the same
// `frame` it is given. The runtime is not involved in that decision and does not know which host it
// is running under.
//
// This is layer 3, so it cannot name the runtime (layer 5) and does not: it takes any callable and
// calls it once per frame. The sample passes `[&] { runtime.tick(); }`, which is what makes the
// host a caller of tick() rather than its owner.
//
//   cy::HostLoopOptions options;
//   options.frame_limit = 600;                       // 0 runs until something asks to exit
//   options.frame_interval_ns = 16666667;            // 0 runs as fast as the machine allows
//   const cy::HostLoopResult result =
//       cy::run_host_loop(platform, options, [&] { (void)runtime.tick(); });
//
// THE THREE WAYS A RUN ENDS, and all three end the same way — Platform::request_exit() records the
// intent, the loop observes it, and main() returns (task 3.6.3):
//
//   the window closed     the frame body sees the CloseRequested event and requests exit
//   SIGINT / SIGTERM      the handler below sets a flag; the loop requests exit before the frame
//   the frame limit       the loop requests exit after the last frame — what CI runs

#pragma once

#include <cy/core/base/types.h>
#include <cy/core/platform/platform.h>

namespace cy {

struct HostLoopOptions {
    /// Stop after this many frames. Zero runs until the window is closed or the process is
    /// interrupted; a non-zero limit is what makes a sample runnable in CI.
    u64 frame_limit = 0;
    /// Take over SIGINT and SIGTERM for the loop's lifetime, restoring the previous handlers when
    /// it returns. A host embedded in a larger process sets this false and drives exit itself.
    bool handle_interrupt = true;
    /// The shortest a frame may take: the loop waits out the remainder before starting the next
    /// one. Zero runs as fast as the machine allows.
    ///
    /// Something has to pace the loop, and until M3 nothing does — there is no presentation and so
    /// no v-sync to wait on. A frame that returns immediately would otherwise spin a core flat for
    /// a window that is only sitting there, and would run thousands of frames between two
    /// simulation steps.
    Nanoseconds frame_interval_ns = 0;
};

struct HostLoopResult {
    u64 frames = 0;
    i32 exit_code = 0;
    bool interrupted = false;
    bool frame_limit_reached = false;
};

/// True when SIGINT or SIGTERM has been received since the flag was last cleared. Readable from
/// anywhere; the handler itself does nothing but set it, because a signal handler may do nothing
/// else safely.
[[nodiscard]] bool interrupt_requested() noexcept;
void clear_interrupt() noexcept;

/// Wait until `interval_ns` has passed since `frame_started_ns`, and return immediately if it
/// already has. Does nothing when the interval is zero.
void wait_for_next_frame(const Platform& platform, Nanoseconds frame_started_ns,
                         Nanoseconds interval_ns) noexcept;

/// Installs the interrupt handlers on construction and restores the previous ones on destruction.
/// Held by run_host_loop(); exposed for a host that owns its own loop and wants the same behaviour.
class InterruptScope {
public:
    explicit InterruptScope(bool enabled) noexcept;
    ~InterruptScope() noexcept;

    InterruptScope(const InterruptScope&) = delete;
    InterruptScope& operator=(const InterruptScope&) = delete;

private:
    bool enabled_;
};

/// Run frames until the platform is asked to exit.
///
/// A template rather than a function pointer so that the call in the loop body is the caller's own
/// code — `runtime.tick()` — with nothing between them.
template <class Frame>
HostLoopResult run_host_loop(Platform& platform, const HostLoopOptions& options, Frame&& frame) {
    const InterruptScope interrupts{options.handle_interrupt};
    HostLoopResult result;

    while (!platform.exit_requested()) {
        if (options.handle_interrupt && interrupt_requested()) {
            result.interrupted = true;
            platform.request_exit(0);
            break;
        }

        const Nanoseconds started = platform.monotonic_nanoseconds();
        frame();
        ++result.frames;

        if (options.frame_limit != 0 && result.frames >= options.frame_limit) {
            result.frame_limit_reached = true;
            platform.request_exit(0);
            break;
        }

        wait_for_next_frame(platform, started, options.frame_interval_ns);
    }

    result.exit_code = platform.exit_code();
    return result;
}

}  // namespace cy
