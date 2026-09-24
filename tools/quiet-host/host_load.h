// SPDX-License-Identifier: MIT
#pragma once
// Whether the host is quiet enough for a wall-clock measurement to measure what it claims to.
// tools/quiet-host: shared by `cy_quiet_host` and by samples/10-world.
//
// ================================================================================================
// WHY A TIMED RUN ASKS THIS AT ALL
// ================================================================================================
//
// A frame budget on a loaded machine measures the machine. `m11a:world-budget-on-a-device` held at
// 9.0 ms mean and 10.8 ms worst on a quiet host and missed at 58 to 286 ms worst beside 24 spinning
// processes, with the same binary and the same world. Neither number says anything about the other
// one's host. So the criterion states the host it assumes, and this is where that host is checked:
// before the take (waiting for it, up to a bound) and across the take itself. A busy host is a
// FAILURE whose reason says so, never a pass and never a skip.
//
// M11.c's seventh close extended the same premise to the test harness's stall ceiling. The harness
// had grown three successive allowances that excused a case's uninterruptible waiting as the host's
// I/O, and every one was refuted by a case that made the wait itself; the owner's decision was to
// stop patching the allowance and run every timing-sensitive suite on a quiet host instead, judged
// exactly as the world sample is. `cy_quiet_host` (main.cpp beside this file) is that: it waits for
// a quiet host, runs one command in a session of its own, and judges the host across the run with
// the command's whole process tree subtracted.
//
// ================================================================================================
// WHAT "QUIET" MEANS HERE, AND HOW THE PROGRAM'S OWN LOAD IS KEPT OUT OF IT
// ================================================================================================
//
// The measure is CPU TIME SPENT BY EVERY OTHER PROCESS over a window: the machine's busy ticks from
// `/proc/stat`, minus OUR OWN ticks, in cores. What "our own" means is `OwnScope`: this process's
// user and system time from `/proc/self/stat` (a program measuring itself), or every process in one
// session as well (a program wrapping a command, whose build and test processes are all its own).
// Subtracting our own ticks is what lets the check run DURING the take, when the measured program
// keeps every core it has busy on purpose. `loadavg` is reported and not judged: it counts this
// program's own workers and decays over a minute, so it would call the host busy for a minute after
// the host went quiet. CPU pressure (`/proc/pressure/cpu`, the share of time some task waited for a
// core) is judged only BEFORE the take, while this program is idle and cannot be what is waiting.
//
// Linux only. Where `/proc` cannot be read the verdict is "cannot tell", and that FAILS too: a
// check that passes whenever it cannot look is the silent pass the criterion exists to refuse.

#include <cy/core/base/types.h>

#include <chrono>

namespace cy::host_load {

/// Everything one look at the host reads. Two of these, a window apart, make a verdict.
struct HostReading {
    bool readable = false;
    /// The aggregate `cpu` line of `/proc/stat`, in USER_HZ ticks across every core.
    u64 busy_ticks = 0;
    u64 total_ticks = 0;
    /// The ticks that are OURS, in the sense `OwnScope` gives: this process's user plus system
    /// time, plus — for a session scope — every process of that session, with the children each of
    /// them has already reaped.
    u64 own_ticks = 0;
    /// `/proc/pressure/cpu`'s `some total`, in microseconds. Absent on kernels without PSI.
    bool has_pressure = false;
    u64 pressure_us = 0;
    f64 load_one_minute = 0.0;
    std::chrono::steady_clock::time_point at;
};

/// Whose CPU time is subtracted from the host's as our own.
struct OwnScope {
    /// A session id (the pid of a process that called `setsid`): every process in it, every
    /// descendant of one of them — a wrapped command that starts a session of its own, such as
    /// this wrapper run inside a suite it wraps — plus this process and whatever it reaped, is
    /// ours. Zero means this process alone. A process that leaves the session AND the tree — an
    /// orphan that then calls `setsid` — is no longer ours and counts against the host, which errs
    /// towards failing, never towards a silent pass.
    i32 session = 0;
};

/// What counts as quiet. The defaults are the criterion's; a test may not loosen them, because the
/// verdict carries them and the criterion reads the verdict.
struct QuietLimits {
    /// Two cores' worth of everyone else: the two cores the machine keeps for itself.
    f64 max_other_cores = 2.0;
    /// The share of the window in which some task on the host waited for a core.
    f64 max_pressure = 0.10;
};

struct QuietVerdict {
    bool quiet = false;
    /// False when `/proc` could not be read over the window, and the verdict is "cannot tell".
    bool readable = false;
    f64 other_cores = 0.0;
    f64 pressure = 0.0;
    /// "host too busy: ...", "host quiet: ..." or "host load unreadable: ...", with the numbers
    /// that decided it.
    char reason[320] = {};
};

/// The host's cores, as the process sees them.
[[nodiscard]] u32 host_cores() noexcept;

/// One look at `/proc`. `readable` is false when any required file could not be parsed.
[[nodiscard]] HostReading read_host(const OwnScope& own = {}) noexcept;

/// The verdict over the window between two readings. `judge_pressure` is false for a window in
/// which the measured program was itself running flat out, since its own threads waiting for a
/// core would otherwise be counted against the host.
[[nodiscard]] QuietVerdict judge_window(const HostReading& before, const HostReading& after,
                                        u32 cores, const QuietLimits& limits,
                                        bool judge_pressure) noexcept;

/// Look for up to `wait_seconds`, a second at a time, until one window is quiet. Every busy window
/// is printed as it is seen, so a run that waited says what it waited for. Returns the last
/// verdict, quiet or not.
[[nodiscard]] QuietVerdict wait_for_quiet(u32 wait_seconds, const QuietLimits& limits) noexcept;

/// THE HOST ACROSS THE TAKE, JUDGED BY ITS BUSIEST SECOND. An average over the whole take would let
/// one second of twenty busy cores pass as under one core across a 25-second take, and one busy
/// second is all it takes to make the worst frame, which is the number the budget judges. So the
/// take is cut into consecutive windows of at least a second, and the verdict is the busiest one.
/// `sample()` is called between frames, outside anything a frame's cost is measured over; it reads
/// `/proc` only when a second has passed, so it costs a clock read on every other call.
class TakeWatch {
public:
    TakeWatch(u32 cores, const QuietLimits& limits, const OwnScope& own = {}) noexcept;

    void begin() noexcept;
    void sample() noexcept;
    /// Closes the last window and returns the busiest. A remainder shorter than half a second is
    /// judged together with the window before it rather than alone, where a few ticks would decide.
    [[nodiscard]] QuietVerdict finish() noexcept;
    [[nodiscard]] u32 windows() const noexcept { return windows_; }
    /// When `begin()` was called.
    [[nodiscard]] std::chrono::steady_clock::time_point started() const noexcept {
        return started_;
    }

private:
    void judge(const HostReading& from, const HostReading& to) noexcept;

    u32 cores_;
    QuietLimits limits_;
    OwnScope own_;
    std::chrono::steady_clock::time_point started_;
    HostReading window_start_;
    HostReading previous_start_;
    bool has_previous_ = false;
    QuietVerdict busiest_;
    bool judged_ = false;
    u32 windows_ = 0;
};

}  // namespace cy::host_load
