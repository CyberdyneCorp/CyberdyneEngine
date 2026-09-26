// SPDX-License-Identifier: MIT
// cy/test/quiet_host.h — whether this run is inside `cy_quiet_host`, and so whether the stall
// ceiling fails a case.
//
// M11.c's ninth close, the owner's option B. The wall-clock stall ceiling (cy/test/test.h,
// `kStallMultiplier`) FAILS a case only when the run is inside the quiet-host wrapper
// (tools/quiet-host/), which it learns from the `CY_QUIET_HOST` marker the wrapper sets for the
// command it runs after its pre-run check passed. Anywhere else a stall is reported — the same
// diagnosis, marked "not enforced: not on a quiet host" — and the case passes unless its CPU
// budget failed. The CPU budget is enforced everywhere. How the marker is verified, and why it is
// not simply believed, is in src/quiet_host_marker.cpp.

#ifndef CY_TEST_QUIET_HOST_H
#define CY_TEST_QUIET_HOST_H

namespace cy::test {

/// The environment variable `cy_quiet_host` sets for its command: `<its pid>:<its start time>`,
/// the start time being field 22 of `/proc/<pid>/stat`.
inline constexpr const char* kQuietHostMarkerVariable = "CY_QUIET_HOST";

/// What the harness concluded about a marker.
enum class QuietHostMarker {
    /// A live ancestor, started at the marker's tick, whose executable is the `cy_quiet_host` this
    /// build produced — the same device and inode, not merely the same name.
    Trusted,
    /// No marker at all: the run was not started by the wrapper.
    Absent,
    /// Not `<pid>:<start time>`.
    Malformed,
    /// The pid is not an ancestor of this process: exported by hand, copied from another run, or a
    /// wrapper that has since exited.
    NotAnAncestor,
    /// The pid is an ancestor, but it started at another tick: the wrapper's pid was reused.
    Restarted,
    /// The pid is an ancestor with the right start time, but its executable is not the built
    /// `cy_quiet_host`: a shell, ctest, or another binary that is only NAMED `cy_quiet_host`.
    NotTheWrapper,
    /// No /proc to verify against (not Linux): never trusted.
    Unsupported,
};

struct QuietHostJudgement {
    QuietHostMarker verdict = QuietHostMarker::Absent;
    /// The pid the marker named, or 0.
    long pid = 0;
    /// Why, in a sentence the stall message quotes.
    char reason[320] = {};
};

/// Judges one marker's text against this process's ancestry and the `cy_quiet_host` this build
/// produced (its path is compiled in). `nullptr` and "" are `Absent`.
QuietHostJudgement judge_quiet_host_marker(const char* marker) noexcept;

/// The same, against the executable at `wrapper` instead of the built one — so that a test can
/// name a file whose identity it knows. `nullptr` means no wrapper, and nothing is trusted.
QuietHostJudgement judge_quiet_host_marker(const char* marker, const char* wrapper) noexcept;

/// The `cy_quiet_host` this build produced, whose device and inode a trusted marker's ancestor must
/// run — the path tests/harness/CMakeLists.txt compiles in — or `nullptr` when the tree has none
/// (not Linux, or tools not built), in which case no marker is trusted.
const char* built_quiet_host_wrapper() noexcept;

/// This process's judgement of its own `CY_QUIET_HOST`, taken once, at first use.
const QuietHostJudgement& quiet_host() noexcept;

/// True when `quiet_host()` trusted the marker: a stall fails the case.
bool stall_ceiling_enforced() noexcept;

/// Field 22 of `/proc/<pid>/stat` — the process's start time in clock ticks since boot — or 0 when
/// it cannot be read. The wrapper writes its own into the marker; exposed so that a test can
/// forge a marker that is right in every respect but one.
unsigned long long process_start_ticks(long pid) noexcept;

}  // namespace cy::test

#endif  // CY_TEST_QUIET_HOST_H
