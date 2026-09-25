// SPDX-License-Identifier: MIT
// cy_quiet_host — run one command on a QUIET HOST, or fail saying the host was not quiet.
//
//     cy_quiet_host [--wait-s <seconds>] -- <command> [argument...]
//
// ================================================================================================
// WHY A WRAPPER, AND NOT ANOTHER ALLOWANCE IN THE HARNESS
// ================================================================================================
//
// The test harness's stall ceiling is a wall-clock assertion: a case within its CPU budget that
// holds the suite for a hundred times that budget is waiting rather than working. Wall clock is a
// property of the machine as much as of the case, and between M11.c's fourth and seventh closes the
// harness grew three successive allowances that tried to tell the two apart from inside the case —
// the case's uninterruptible time, then that bounded by the host's I/O pressure, then that with the
// case's own process tree taken out. Each was refuted by a case that made the wait itself: its own
// vfork, its own threads reading the disk, its helpers double-forked past the census. The owner
// stopped it there: NOTHING inside a process can say what the rest of the machine was doing to it,
// so the premise is stated instead and checked from outside. A timing-sensitive suite runs on a
// quiet host, exactly as `m11a:world-budget-on-a-device` has since M11.c, and a host that is not
// quiet is a FAILURE with the numbers, never a pass and never a skip.
//
// ================================================================================================
// WHAT IT DOES
// ================================================================================================
//
//  1. BEFORE: waits up to `--wait-s` (default 600 s), a second at a time, for every other process
//     together to use at most two cores, for CPU pressure to stay at or under 10%, and for I/O
//     pressure to stay at or under the limits host_load.h states (a writeback burst from another
//     criterion's build uses no core and stalls every process on the disk), all of it for five
//     seconds running (`kSettledWindows`: a burst comes in pulses). Still busy at the deadline:
//     the command is NOT run, "host too busy: <numbers>" goes to stderr, exit 1 — and
//     "host too busy: io ..." when I/O pressure is what decided it.
//  2. RUNS the command in a session of its own (`setsid`), so that every process it starts — a
//     build, ctest, the test binaries, their children — can be told from the rest of the machine,
//     and with `CY_QUIET_HOST=<this pid>:<this process's start time>` in its environment. The
//     marker is set only here, after step 1 passed; the test harness enforces its wall-clock stall
//     ceiling only when it finds the marker AND verifies through /proc that the pid is a live
//     ancestor started at that tick whose executable is this one (tests/harness/, M11.c's ninth
//     close, the owner's option B). A marker exported by hand names no such ancestor.
//  3. ACROSS THE RUN: judges the host a second at a time with the command's whole tree subtracted
//     (host_load.h says how), and requires the BUSIEST second to be quiet. A run shorter than a
//     second is judged over a full second, because a tick's resolution decides nothing shorter.
//     Not quiet: "host too busy: <numbers>" to stderr and exit 1, whatever the command said.
//  4. Otherwise the command's own exit status is this program's. Exit 2 is a usage error.
//
// Linux only, like the check it wraps: the premise is read from /proc, and a host that cannot be
// read is reported unreadable and fails.

#include "host_load.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace cy;
using namespace cy::host_load;

constexpr u32 kDefaultWaitSeconds = 600;
constexpr int kUsageExit = 2;

struct Options {
    u32 wait_seconds = kDefaultWaitSeconds;
    /// The command, `argv`-shaped and NULL-terminated.
    char** command = nullptr;
};

void usage() {
    std::fprintf(stderr,
                 "usage: cy_quiet_host [--wait-s <seconds>] -- <command> [argument...]\n"
                 "  Waits for a quiet host, runs the command in a session of its own, and fails\n"
                 "  with \"host too busy:\" if the host was not quiet before or during the run.\n");
}

[[nodiscard]] bool parse(int argc, char** argv, Options& out) {
    int index = 1;
    while (index < argc) {
        const char* argument = argv[index];
        if (std::strcmp(argument, "--") == 0) {
            ++index;
            break;
        }
        if (std::strcmp(argument, "--wait-s") == 0 && index + 1 < argc) {
            char* end = nullptr;
            const unsigned long value = std::strtoul(argv[index + 1], &end, 10);
            if (end == argv[index + 1] || *end != '\0') {
                std::fprintf(stderr, "cy_quiet_host: --wait-s needs a whole number of seconds\n");
                return false;
            }
            out.wait_seconds = static_cast<u32>(value);
            index += 2;
            continue;
        }
        std::fprintf(stderr, "cy_quiet_host: unknown option '%s'\n", argument);
        return false;
    }
    if (index >= argc) {
        std::fprintf(stderr, "cy_quiet_host: no command after '--'\n");
        return false;
    }
    out.command = argv + index;
    return true;
}

[[nodiscard]] std::string spell(char** command) {
    std::string text;
    for (char** word = command; *word != nullptr; ++word) {
        if (!text.empty()) {
            text += ' ';
        }
        text += *word;
    }
    return text;
}

// A SIGINT or SIGTERM to this program ends the command's whole session before this program ends:
// a wrapper killed by a timeout must not leave a build or a ctest running on the host it was
// judging. Only the flag is set here; the wait loop does the killing.
volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int /*signal*/) {
    stop_requested = 1;
}

[[nodiscard]] bool host_quiet_before(const std::string& command, u32 wait_seconds) {
    std::printf("=== the host, before `%s` (waiting up to %u s for it to be quiet) ===\n",
                command.c_str(), wait_seconds);
    std::fflush(stdout);
    const QuietVerdict verdict = wait_for_quiet(wait_seconds, QuietLimits{});
    if (!verdict.quiet) {
        std::fprintf(stderr,
                     "%s.\nThe command was not run: a wall-clock ceiling on a loaded machine "
                     "measures the machine, so this run FAILS rather than passing or skipping.\n",
                     verdict.reason);
        return false;
    }
    std::printf("  %s\n", verdict.reason);
    return true;
}

/// Field 22 of /proc/self/stat, this process's start time in clock ticks since boot: the nonce
/// in the marker, which the harness compares with the same field of the ancestor the marker
/// names, so that a pid reused after this wrapper exited is not trusted. 0 when unreadable.
[[nodiscard]] unsigned long long own_start_ticks() {
    std::FILE* file = std::fopen("/proc/self/stat", "re");
    if (file == nullptr) {
        return 0;
    }
    char buffer[1024];
    const std::size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[got] = '\0';
    // The command name in field 2 may contain anything, so fields are counted after the LAST ')':
    // the state is field 3, and the start time is the nineteenth number after it.
    const char* cursor = std::strrchr(buffer, ')');
    if (cursor == nullptr || cursor[1] != ' ' || cursor[2] == '\0') {
        return 0;
    }
    cursor += 3;
    unsigned long long value = 0;
    for (int field = 4; field <= 22; ++field) {
        char* end = nullptr;
        value = std::strtoull(cursor, &end, 10);
        if (end == cursor) {
            return 0;
        }
        cursor = end;
    }
    return value;
}

/// `CY_QUIET_HOST=<pid>:<start ticks>`, built before the fork so the child only calls `putenv`.
[[nodiscard]] std::string quiet_host_marker() {
    return "CY_QUIET_HOST=" + std::to_string(static_cast<long>(::getpid())) + ":" +
           std::to_string(own_start_ticks());
}

[[nodiscard]] pid_t start(char** command, std::string& marker) {
    const pid_t child = ::fork();
    if (child == 0) {
        // Its own session, so that host_load can count the whole tree as ours; and the default
        // signal dispositions, so that the session dies of the SIGTERM below like any other.
        (void)::setsid();
        // THE MARKER, and only now: the pre-run check has passed. It replaces any marker this
        // wrapper inherited, so a wrapper nested inside another run names itself.
        (void)::putenv(marker.data());
        ::execvp(command[0], command);
        std::fprintf(stderr, "cy_quiet_host: cannot run '%s': %s\n", command[0],
                     std::strerror(errno));
        ::_exit(127);
    }
    return child;
}

/// The command's exit status as this program should report it: its own code, or 128 plus the
/// signal that ended it, as a shell would.
[[nodiscard]] int exit_status(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

/// Waits for the command while the watch samples the host, then holds the watch open until it has
/// seen at least one full second, so that a short command is judged at a tick's resolution rather
/// than a fraction of one.
[[nodiscard]] int run_watched(pid_t child, TakeWatch& watch) {
    int status = 0;
    bool exited = false;
    for (;;) {
        if (!exited) {
            const pid_t reaped = ::waitpid(child, &status, WNOHANG);
            if (reaped == child) {
                exited = true;
            } else if (reaped < 0 && errno != EINTR) {
                exited = true;
                status = 0;
            }
        }
        if (stop_requested != 0 && !exited) {
            (void)::kill(-child, SIGTERM);
            stop_requested = 0;
        }
        const bool window_seen =
            std::chrono::steady_clock::now() - watch.started() >= std::chrono::seconds(1);
        if (exited && window_seen) {
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        watch.sample();
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        usage();
        return kUsageExit;
    }
    const std::string command = spell(options.command);

    if (!host_quiet_before(command, options.wait_seconds)) {
        return 1;
    }

    std::printf("=== running: %s ===\n", command.c_str());
    // Flushed so a reader on a pipe sees the run START before the command's own output: the
    // regression test loads the host at exactly this line.
    std::fflush(stdout);

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    std::string marker = quiet_host_marker();
    const pid_t child = start(options.command, marker);
    if (child < 0) {
        std::fprintf(stderr, "cy_quiet_host: cannot fork: %s\n", std::strerror(errno));
        return 1;
    }
    TakeWatch watch(host_cores(), QuietLimits{}, OwnScope{static_cast<i32>(child)});
    watch.begin();
    const int status = run_watched(child, watch);
    const QuietVerdict verdict = watch.finish();
    const int command_exit = exit_status(status);

    std::printf("=== the host, across `%s` (the busiest of %u windows) ===\n  %s\n",
                command.c_str(), watch.windows(), verdict.reason);
    std::fflush(stdout);
    if (!verdict.quiet) {
        std::fprintf(stderr,
                     "%s.\nThe host was not quiet while `%s` ran (it exited %d), so its verdict "
                     "measures the machine and this run FAILS whatever it said.\n",
                     verdict.reason, command.c_str(), command_exit);
        return 1;
    }
    if (command_exit != 0) {
        std::fprintf(stderr,
                     "cy_quiet_host: `%s` exited %d on a quiet host; the verdict is its own.\n",
                     command.c_str(), command_exit);
    }
    return command_exit;
}
