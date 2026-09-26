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
//   3. that ancestor's executable IS the `cy_quiet_host` this build produced — the same file, by
//      device and inode, as the path CMake handed this library at configure time — so a marker
//      naming one's own shell or ctest, which ARE ancestors, is refused, and so is any other
//      binary merely NAMED `cy_quiet_host`.
//
// A test run that is a descendant of a live wrapper that passed its check is exactly what the
// premise means, so that is not a forgery. A descendant that daemonises out of the tree loses its
// ancestry and is refused: the check errs towards reporting, never towards failing a case on a
// host nobody checked. Where /proc does not exist the marker is never trusted, and the stall
// ceiling is reported and not enforced — Windows and macOS have no wrapper to be inside.
//
// WHY THE IDENTITY AND NOT THE NAME (M11.d task 9.7). Until then rule 3 compared the BASENAME of
// the ancestor's `/proc/<pid>/exe`, so a copy of `sh` renamed `cy_quiet_host` that set the marker
// to its own pid and start time and ran a suite as its child was trusted, and the stall ceiling was
// enforced on a host nothing had checked. `smoke.quiet_host_marker` runs exactly that impostor. The
// kernel resolves `/proc/<pid>/exe` to the file the process is running, even once that file has
// been unlinked, so `stat` on it gives the device and inode of the very binary; comparing them with
// the built wrapper's needs no name at all. What this refuses that a name did not: a wrapper from
// ANOTHER build tree, a copy of the wrapper, and a wrapper relinked while it ran (its running image
// is the old inode). Each is reported, not failed — the direction this check always errs in.

#include <cy/test/quiet_host.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace cy::test {
namespace {

#if defined(__linux__)

constexpr const char* kWrapperName = "cy_quiet_host";
/// The wrapper this build produced, as tests/harness/CMakeLists.txt passes it. Absent when the tree
/// has no wrapper, and then no marker is trusted.
#    if defined(CY_QUIET_HOST_WRAPPER)
constexpr const char* kBuiltWrapper = CY_QUIET_HOST_WRAPPER;
#    else
constexpr const char* kBuiltWrapper = nullptr;
#    endif
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

/// Where `/proc/<pid>/exe` points, for the reason a refusal prints; " (deleted)" is kept, since a
/// wrapper relinked while it ran is one of the things refused. Empty when it cannot be read.
void executable_path(long pid, char* out, std::size_t size) noexcept {
    out[0] = '\0';
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
    const ::ssize_t got = ::readlink(path, out, size - 1);
    out[got > 0 ? got : 0] = '\0';
}

/// Whether `/proc/<pid>/exe` is the file at `wrapper`: the same device and inode. `stat` follows
/// the link to the image the process runs, so no name — the ancestor's or the wrapper's — enters.
bool runs_executable(long pid, const char* wrapper) noexcept {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
    struct ::stat running{};
    struct ::stat built{};
    if (::stat(path, &running) != 0 || ::stat(wrapper, &built) != 0) {
        return false;
    }
    return running.st_dev == built.st_dev && running.st_ino == built.st_ino;
}

/// The ancestor the marker names, judged: its start time, then its executable's identity.
QuietHostJudgement judge_ancestor(long pid, unsigned long long marker_start,
                                  unsigned long long actual_start, const char* wrapper) noexcept {
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
    if (wrapper == nullptr || *wrapper == '\0') {
        out.verdict = QuietHostMarker::NotTheWrapper;
        std::snprintf(out.reason, sizeof(out.reason),
                      "%s names pid %ld, but this build produced no %s to compare it with",
                      kQuietHostMarkerVariable, pid, kWrapperName);
        return out;
    }
    if (!runs_executable(pid, wrapper)) {
        char running[256];
        executable_path(pid, running, sizeof(running));
        out.verdict = QuietHostMarker::NotTheWrapper;
        std::snprintf(out.reason, sizeof(out.reason),
                      "%s names pid %ld, an ancestor that is not the %s this build produced (it "
                      "runs '%.64s', not the file at '%.64s')",
                      kQuietHostMarkerVariable, pid, kWrapperName,
                      running[0] != '\0' ? running : "unreadable", wrapper);
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

const char* built_quiet_host_wrapper() noexcept {
#if defined(__linux__)
    return kBuiltWrapper;
#else
    return nullptr;
#endif
}

QuietHostJudgement judge_quiet_host_marker(const char* marker) noexcept {
    return judge_quiet_host_marker(marker, built_quiet_host_wrapper());
}

QuietHostJudgement judge_quiet_host_marker(const char* marker, const char* wrapper) noexcept {
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
            return judge_ancestor(pid, start, fields.start, wrapper);
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
    (void)wrapper;
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
