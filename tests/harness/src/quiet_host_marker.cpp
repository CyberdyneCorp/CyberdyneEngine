// SPDX-License-Identifier: MIT
// WHETHER THIS RUN IS ON A CHECKED-QUIET HOST, and so whether the stall ceiling may fail a case.
//
// ================================================================================================
// WHY THE HARNESS ASKS, AND WHY IT DOES NOT TAKE THE ANSWER ON TRUST
// ================================================================================================
//
// The stall ceiling is a wall-clock assertion, and wall clock is the machine's as much as the
// case's. M11.c's fifth to seventh closes showed that nothing a process reads about itself says
// what the rest of the machine did to it, so the premise was moved outside the process: a
// timing-sensitive suite runs under `cy_quiet_host` (tools/quiet-host/), which checks the host
// before and across the run. The eighth and ninth closes then showed that the ledger's text is not
// where that premise can be guaranteed: three gates in a row found another route by which a
// wrapped suite also ran bare — a `&&` after the wrapper, a matrix row's `test-all`, a sanitizer
// loop — and an audit of the criteria's text cannot see every such route.
//
// So the harness itself asks whether it is inside the wrapper (the owner's option B). A stall
// FAILS the case only when it is; anywhere else the same diagnosis is printed, marked
// "not enforced: not on a quiet host", and the case passes unless its CPU budget failed. The CPU
// budget is enforced on every host, because CPU time does not grow when a neighbour spins.
//
// ================================================================================================
// THE MARKER, AND WHAT MAKES IT HARD TO FORGE BY ACCIDENT
// ================================================================================================
//
// `cy_quiet_host` sets `CY_QUIET_HOST=<its pid>:<its start time>` in the environment of the command
// it runs, and only after its pre-run check passed. The start time is field 22 of
// `/proc/<pid>/stat`, in clock ticks since boot: the kernel's own nonce for one process instance.
// The harness trusts the marker only when ALL of these hold, read from /proc:
//
//   1. the pid is an ANCESTOR of this process — so a marker exported in a shell, left over from an
//      earlier run, or copied from another terminal's wrapper names a process this one does not
//      descend from, and is refused;
//   2. that ancestor's start time is the marker's — so a pid reused by an unrelated process after
//      the wrapper exited is refused;
//   3. that ancestor's executable is `cy_quiet_host` — so a marker naming one's own shell or
//      ctest, which ARE ancestors, is refused.
//
// A test run that is a descendant of a live wrapper that passed its check is exactly what the
// premise means, so that is not a forgery. A descendant that daemonises out of the tree loses its
// ancestry and is refused: the check errs towards reporting, never towards failing a case on a
// host nobody checked. Where /proc does not exist the marker is never trusted, and the stall
// ceiling is reported and not enforced — Windows and macOS have no wrapper to be inside.

#include <cy/test/quiet_host.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#    include <unistd.h>
#    include <climits>
#endif

namespace cy::test {
namespace {

#if defined(__linux__)

constexpr const char* kWrapperName = "cy_quiet_host";
/// A process tree deeper than this is a loop in a /proc that is changing under the walk.
constexpr int kMaximumDepth = 4096;

struct StatFields {
    bool read = false;
    long parent = 0;
    unsigned long long start = 0;
};

/// Fields 4 (the parent's pid) and 22 (the start time) of `/proc/<pid>/stat`. The command name in
/// field 2 is in parentheses and may contain anything, a `)` included, so the fields are counted
/// from after the LAST `)`.
StatFields read_stat(long pid) noexcept {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    std::FILE* file = std::fopen(path, "re");
    if (file == nullptr) {
        return {};
    }
    char buffer[1024];
    const std::size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[got] = '\0';
    const char* cursor = std::strrchr(buffer, ')');
    if (cursor == nullptr) {
        return {};
    }
    ++cursor;
    StatFields fields;
    for (int field = 3; field <= 22; ++field) {
        while (*cursor == ' ') {
            ++cursor;
        }
        if (*cursor == '\0') {
            return {};
        }
        if (field == 3) {  // the state letter
            while (*cursor != ' ' && *cursor != '\0') {
                ++cursor;
            }
            continue;
        }
        char* end = nullptr;
        const unsigned long long value = std::strtoull(cursor, &end, 10);
        if (end == cursor) {
            return {};
        }
        if (field == 4) {
            fields.parent = static_cast<long>(value);
        } else if (field == 22) {
            fields.start = value;
        }
        cursor = end;
    }
    fields.read = true;
    return fields;
}

/// The basename of `/proc/<pid>/exe`, with the " (deleted)" the kernel appends when the binary was
/// replaced while it ran (a build during the run). Empty when the link cannot be read.
void executable_name(long pid, char* out, std::size_t size) noexcept {
    out[0] = '\0';
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
    char target[PATH_MAX];
    const ::ssize_t got = ::readlink(path, target, sizeof(target) - 1);
    if (got <= 0) {
        return;
    }
    target[got] = '\0';
    constexpr const char* kDeleted = " (deleted)";
    const std::size_t length = std::strlen(target);
    const std::size_t suffix = std::strlen(kDeleted);
    if (length > suffix && std::strcmp(target + length - suffix, kDeleted) == 0) {
        target[length - suffix] = '\0';
    }
    const char* slash = std::strrchr(target, '/');
    std::snprintf(out, size, "%s", slash != nullptr ? slash + 1 : target);
}

/// The ancestor the marker names, judged: its start time, then its executable.
QuietHostJudgement judge_ancestor(long pid, unsigned long long marker_start,
                                  unsigned long long actual_start) noexcept {
    QuietHostJudgement out;
    out.pid = pid;
    if (actual_start != marker_start) {
        out.verdict = QuietHostMarker::Restarted;
        std::snprintf(out.reason, sizeof(out.reason),
                      "%s names pid %ld started at tick %llu, but the ancestor with that pid "
                      "started at tick %llu: the pid was reused",
                      kQuietHostMarkerVariable, pid, marker_start, actual_start);
        return out;
    }
    char name[256];
    executable_name(pid, name, sizeof(name));
    if (std::strcmp(name, kWrapperName) != 0) {
        out.verdict = QuietHostMarker::NotTheWrapper;
        std::snprintf(out.reason, sizeof(out.reason),
                      "%s names pid %ld, an ancestor that is not %s (its executable is '%.96s')",
                      kQuietHostMarkerVariable, pid, kWrapperName,
                      name[0] != '\0' ? name : "unreadable");
        return out;
    }
    out.verdict = QuietHostMarker::Trusted;
    std::snprintf(out.reason, sizeof(out.reason),
                  "%s is pid %ld, its ancestry, start time and executable verified through /proc",
                  kWrapperName, pid);
    return out;
}

#endif

}  // namespace

unsigned long long process_start_ticks(long pid) noexcept {
#if defined(__linux__)
    return read_stat(pid).start;
#else
    (void)pid;
    return 0;
#endif
}

QuietHostJudgement judge_quiet_host_marker(const char* marker) noexcept {
    QuietHostJudgement out;
    if (marker == nullptr || *marker == '\0') {
        out.verdict = QuietHostMarker::Absent;
        std::snprintf(out.reason, sizeof(out.reason),
                      "no %s marker in the environment: this run was not started by cy_quiet_host",
                      kQuietHostMarkerVariable);
        return out;
    }
#if defined(__linux__)
    char* end = nullptr;
    const long pid = std::strtol(marker, &end, 10);
    unsigned long long start = 0;
    bool parsed = end != marker && *end == ':' && pid > 0;
    if (parsed) {
        const char* digits = end + 1;
        start = std::strtoull(digits, &end, 10);
        parsed = end != digits && *end == '\0' && *digits >= '0' && *digits <= '9';
    }
    if (!parsed) {
        out.verdict = QuietHostMarker::Malformed;
        std::snprintf(out.reason, sizeof(out.reason), "%s='%.80s' is not '<pid>:<start time>'",
                      kQuietHostMarkerVariable, marker);
        return out;
    }
    long current = static_cast<long>(::getppid());
    for (int depth = 0; current > 0 && depth < kMaximumDepth; ++depth) {
        const StatFields fields = read_stat(current);
        if (!fields.read) {
            break;
        }
        if (current == pid) {
            return judge_ancestor(pid, start, fields.start);
        }
        current = fields.parent;
    }
    out.verdict = QuietHostMarker::NotAnAncestor;
    out.pid = pid;
    std::snprintf(out.reason, sizeof(out.reason),
                  "%s names pid %ld, which is not an ancestor of this process: a stray export, a "
                  "marker copied from another run, or a wrapper that has exited",
                  kQuietHostMarkerVariable, pid);
    return out;
#else
    out.verdict = QuietHostMarker::Unsupported;
    std::snprintf(out.reason, sizeof(out.reason),
                  "this platform has no /proc to verify a %s marker against, and no wrapper",
                  kQuietHostMarkerVariable);
    return out;
#endif
}

const QuietHostJudgement& quiet_host() noexcept {
    static const QuietHostJudgement judgement =
        judge_quiet_host_marker(std::getenv(kQuietHostMarkerVariable));
    return judgement;
}

bool stall_ceiling_enforced() noexcept {
    return quiet_host().verdict == QuietHostMarker::Trusted;
}

}  // namespace cy::test
