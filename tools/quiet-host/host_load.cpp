// SPDX-License-Identifier: MIT
#include "host_load.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__linux__)
#    include <dirent.h>
#    include <unistd.h>
#endif

namespace cy::host_load {

namespace {

/// The first line of a `/proc` file, or false. Every file read here answers in its first line.
[[nodiscard]] bool first_line(const char* path, char* line, int capacity) noexcept {
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return false;
    }
    const bool got = std::fgets(line, capacity, file) != nullptr;
    std::fclose(file);
    return got;
}

/// A cursor over one line of numbers. `strtoull` and `strtod` rather than `scanf`, which reports no
/// conversion error: a field that is not a number stops the cursor, and the reading is then
/// unreadable rather than zero.
class Fields {
public:
    explicit Fields(const char* text) noexcept : at_(text) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }

    /// Step past `word` (and the spaces before it), or fail.
    void expect(const char* word) noexcept {
        skip_spaces();
        const usize length = std::strlen(word);
        if (!ok_ || std::strncmp(at_, word, length) != 0) {
            ok_ = false;
            return;
        }
        at_ += length;
    }

    [[nodiscard]] u64 whole() noexcept {
        skip_spaces();
        char* end = nullptr;
        const unsigned long long value = ok_ ? std::strtoull(at_, &end, 10) : 0;
        return static_cast<u64>(advance(end) ? value : 0);
    }

    [[nodiscard]] f64 real() noexcept {
        skip_spaces();
        char* end = nullptr;
        const double value = ok_ ? std::strtod(at_, &end) : 0.0;
        return advance(end) ? value : 0.0;
    }

    /// Step past one space-separated field of any kind.
    void skip() noexcept {
        skip_spaces();
        while (ok_ && *at_ != '\0' && *at_ != ' ') {
            ++at_;
        }
    }

private:
    void skip_spaces() noexcept {
        while (ok_ && *at_ == ' ') {
            ++at_;
        }
    }

    [[nodiscard]] bool advance(const char* end) noexcept {
        ok_ = ok_ && end != nullptr && end != at_;
        if (ok_) {
            at_ = end;
        }
        return ok_;
    }

    const char* at_;
    bool ok_ = true;
};

/// The aggregate `cpu` line: user nice system idle iowait irq softirq steal. `guest` is already
/// inside `user`, so it is not added again.
[[nodiscard]] bool read_stat(HostReading& out) noexcept {
    char line[512] = {};
    if (!first_line("/proc/stat", line, sizeof(line))) {
        return false;
    }
    Fields fields(line);
    fields.expect("cpu ");
    const u64 user = fields.whole();
    const u64 nice = fields.whole();
    const u64 system = fields.whole();
    const u64 idle = fields.whole();
    const u64 iowait = fields.whole();
    const u64 irq = fields.whole();
    const u64 softirq = fields.whole();
    const u64 steal = fields.whole();
    out.busy_ticks = user + nice + system + irq + softirq + steal;
    out.total_ticks = out.busy_ticks + idle + iowait;
    return fields.ok();
}

/// The fields of one process's `stat` line this file reads. The command name in field 2 may hold
/// spaces and parentheses, so the scan starts after the LAST ')'.
struct TaskTimes {
    /// Field 4.
    u64 ppid = 0;
    /// Field 6.
    u64 session = 0;
    /// Fields 14 and 15: the process's own user and system time, every thread included.
    u64 own = 0;
    /// Fields 16 and 17: the same for the children it has already waited for.
    u64 reaped = 0;
};

[[nodiscard]] bool parse_task_times(const char* line, TaskTimes& out) noexcept {
    const char* close = std::strrchr(line, ')');
    if (close == nullptr) {
        return false;
    }
    // After ')': state(3) ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt, then
    // utime(14) stime(15) cutime(16) cstime(17).
    Fields fields(close + 1);
    fields.skip();  // state
    out.ppid = fields.whole();
    fields.skip();  // pgrp
    out.session = fields.whole();
    constexpr int kFieldsBetweenSessionAndUtime = 7;
    for (int field = 0; field < kFieldsBetweenSessionAndUtime; ++field) {
        fields.skip();
    }
    const u64 user = fields.whole();
    const u64 system = fields.whole();
    const u64 reaped_user = fields.whole();
    const u64 reaped_system = fields.whole();
    out.own = user + system;
    out.reaped = reaped_user + reaped_system;
    return fields.ok();
}

[[nodiscard]] bool read_task(const char* path, TaskTimes& out) noexcept {
    char line[1024] = {};
    return first_line(path, line, sizeof(line)) && parse_task_times(line, out);
}

/// This process alone: its own user and system time.
[[nodiscard]] bool read_own_process(HostReading& out) noexcept {
    TaskTimes self;
    if (!read_task("/proc/self/stat", self)) {
        return false;
    }
    out.own_ticks = self.own;
    return true;
}

#if defined(__linux__)
/// One process of the host, as the census reads it.
struct Task {
    u64 pid = 0;
    u64 ppid = 0;
    u64 session = 0;
    u64 ticks = 0;
    bool ours = false;
};

/// Every process of `session` AND EVERY DESCENDANT OF ONE, plus this process and the children it
/// has reaped. A process's own time is counted while it lives and moves into its parent's
/// `cutime`/`cstime` once reaped, so summing both over the living members counts each tick once —
/// provided the reaper is a member, which the wrapper that started the session is.
///
/// WHY DESCENDANTS, AND NOT THE SESSION ALONE. `just test-all` runs `smoke.quiet_host_own`, which
/// is this wrapper again, around a command that burns four cores; the inner wrapper puts that
/// command in a session of its own, exactly as the outer one did, and a census by session id alone
/// then counted those four cores as OTHER processes and failed `m0:test` on a host nobody else was
/// using. Membership is therefore the session's, or an ancestor's: a process whose parent chain
/// reaches a member is ours whatever session it started. An orphan re-parented to init keeps its
/// session id and so stays ours; one that ALSO started its own session is lost from the sum, as is
/// a member that init has already reaped, and the host then looks busier than it was: the direction
/// that fails, never the one that passes.
[[nodiscard]] bool read_own_session(HostReading& out, i32 session) noexcept {
    TaskTimes self;
    if (!read_task("/proc/self/stat", self)) {
        return false;
    }
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr) {
        return false;
    }
    const auto own_pid = static_cast<long>(::getpid());
    const auto wanted = static_cast<u64>(session);
    std::vector<Task> tasks;
    while (const dirent* entry = ::readdir(proc)) {
        char* end = nullptr;
        const long pid = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || pid <= 0 || pid == own_pid) {
            continue;
        }
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        TaskTimes task;
        // A process that exited between the listing and this read is nothing to count.
        if (read_task(path, task)) {
            tasks.push_back(Task{static_cast<u64>(pid), task.ppid, task.session,
                                 task.own + task.reaped, task.session == wanted});
        }
    }
    ::closedir(proc);
    // Membership flows down the parent chain until nothing changes: a pass marks every child of a
    // member, and the tree is no deeper than the number of passes it takes.
    for (bool changed = true; changed;) {
        changed = false;
        for (Task& task : tasks) {
            if (task.ours) {
                continue;
            }
            for (const Task& candidate : tasks) {
                if (candidate.ours && candidate.pid == task.ppid) {
                    task.ours = true;
                    changed = true;
                    break;
                }
            }
        }
    }
    u64 ticks = self.own + self.reaped;
    for (const Task& task : tasks) {
        if (task.ours) {
            ticks += task.ticks;
        }
    }
    out.own_ticks = ticks;
    return true;
}
#endif

[[nodiscard]] bool read_own(HostReading& out, const OwnScope& own) noexcept {
#if defined(__linux__)
    if (own.session > 0) {
        return read_own_session(out, own.session);
    }
#else
    (void)own;
#endif
    return read_own_process(out);
}

/// `some avg10=.. avg60=.. avg300=.. total=<microseconds>`.
void read_pressure(HostReading& out) noexcept {
    char line[256] = {};
    if (!first_line("/proc/pressure/cpu", line, sizeof(line))) {
        return;
    }
    const char* total = std::strstr(line, "total=");
    if (total == nullptr) {
        return;
    }
    Fields fields(total);
    fields.expect("total=");
    out.pressure_us = fields.whole();
    out.has_pressure = fields.ok();
}

/// `/proc/pressure/io`: a `some` line and a `full` line, each ending in `total=<microseconds>`.
/// Both are required: a kernel that reports one and not the other is not one this reads.
void read_io_pressure(HostReading& out) noexcept {
    std::FILE* file = std::fopen("/proc/pressure/io", "r");
    if (file == nullptr) {
        return;
    }
    bool some = false;
    bool full = false;
    char line[256] = {};
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        const char* total = std::strstr(line, "total=");
        if (total == nullptr) {
            continue;
        }
        Fields fields(total);
        fields.expect("total=");
        const u64 value = fields.whole();
        if (!fields.ok()) {
            continue;
        }
        if (std::strncmp(line, "some ", 5) == 0) {
            out.io_some_us = value;
            some = true;
        } else if (std::strncmp(line, "full ", 5) == 0) {
            out.io_full_us = value;
            full = true;
        }
    }
    std::fclose(file);
    out.has_io_pressure = some && full;
}

void read_load(HostReading& out) noexcept {
    char line[256] = {};
    if (!first_line("/proc/loadavg", line, sizeof(line))) {
        return;
    }
    Fields fields(line);
    const f64 one = fields.real();
    if (fields.ok()) {
        out.load_one_minute = one;
    }
}

[[nodiscard]] u64 difference(u64 later, u64 earlier) noexcept {
    return later > earlier ? later - earlier : 0;
}

/// The share of the window a PSI `total` counter advanced by, or zero when either end lacks it.
[[nodiscard]] f64 pressure_share(bool known, u64 later_us, u64 earlier_us, f64 wall_us) noexcept {
    return known && wall_us > 0.0 ? static_cast<f64>(difference(later_us, earlier_us)) / wall_us
                                  : 0.0;
}

}  // namespace

u32 host_cores() noexcept {
    const unsigned int cores = std::thread::hardware_concurrency();
    return cores == 0 ? 1U : static_cast<u32>(cores);
}

HostReading read_host(const OwnScope& own) noexcept {
    HostReading reading;
    reading.at = std::chrono::steady_clock::now();
    reading.readable = read_stat(reading) && read_own(reading, own);
    read_pressure(reading);
    read_io_pressure(reading);
    read_load(reading);
    return reading;
}

QuietVerdict judge_window(const HostReading& before, const HostReading& after, u32 cores,
                          const QuietLimits& limits, bool judge_pressure) noexcept {
    QuietVerdict verdict;
    const u64 total = difference(after.total_ticks, before.total_ticks);
    if (!before.readable || !after.readable || total == 0) {
        std::snprintf(verdict.reason, sizeof(verdict.reason),
                      "host load unreadable: cannot tell whether the host is quiet, because "
                      "/proc/stat or /proc/self/stat could not be read over the window");
        return verdict;
    }
    verdict.readable = true;
    const u64 busy = difference(after.busy_ticks, before.busy_ticks);
    const u64 own = difference(after.own_ticks, before.own_ticks);
    const u64 others = busy > own ? busy - own : 0;
    verdict.other_cores =
        (static_cast<f64>(others) / static_cast<f64>(total)) * static_cast<f64>(cores);

    const f64 wall_us = std::chrono::duration<f64, std::micro>(after.at - before.at).count();
    verdict.pressure = pressure_share(before.has_pressure && after.has_pressure, after.pressure_us,
                                      before.pressure_us, wall_us);
    const bool io_known = before.has_io_pressure && after.has_io_pressure;
    verdict.io_some = pressure_share(io_known, after.io_some_us, before.io_some_us, wall_us);
    verdict.io_full = pressure_share(io_known, after.io_full_us, before.io_full_us, wall_us);

    const bool cores_ok = verdict.other_cores <= limits.max_other_cores;
    const bool pressure_ok = !judge_pressure || verdict.pressure <= limits.max_pressure;
    verdict.io_busy = judge_pressure && (verdict.io_some > limits.max_io_some ||
                                         verdict.io_full > limits.max_io_full);
    verdict.quiet = cores_ok && pressure_ok && !verdict.io_busy;

    // THE I/O CLAUSE LEADS WHEN IT DECIDED THE VERDICT, so that "host too busy: io" names the cause
    // the way the eighth close had to be told it: by a reader of the log, not of /proc.
    char io[160] = {};
    if (judge_pressure) {
        std::snprintf(io, sizeof(io),
                      "io pressure some %.1f%% (limit %.1f%%), full %.1f%% (limit %.1f%%)",
                      verdict.io_some * 100.0, limits.max_io_some * 100.0, verdict.io_full * 100.0,
                      limits.max_io_full * 100.0);
    } else {
        std::snprintf(io, sizeof(io),
                      "io pressure some %.1f%%, full %.1f%% (not judged while this program runs, "
                      "limits %.1f%% and %.1f%%)",
                      verdict.io_some * 100.0, verdict.io_full * 100.0, limits.max_io_some * 100.0,
                      limits.max_io_full * 100.0);
    }
    char cpu[240] = {};
    std::snprintf(cpu, sizeof(cpu),
                  "other processes used %.2f of %u cores (limit %.2f), CPU pressure %.1f%% "
                  "(%s%.1f%%)",
                  verdict.other_cores, cores, limits.max_other_cores, verdict.pressure * 100.0,
                  judge_pressure ? "limit " : "not judged while this program runs, limit ",
                  limits.max_pressure * 100.0);
    const char* state = verdict.quiet ? "quiet" : "too busy";
    const f64 seconds = wall_us / 1.0e6;
    if (verdict.io_busy) {
        std::snprintf(verdict.reason, sizeof(verdict.reason),
                      "host %s: %s; %s, loadavg %.2f against %u cores, over %.1f s", state, io, cpu,
                      after.load_one_minute, cores, seconds);
    } else {
        std::snprintf(verdict.reason, sizeof(verdict.reason),
                      "host %s: %s, %s, loadavg %.2f against %u cores, over %.1f s", state, cpu, io,
                      after.load_one_minute, cores, seconds);
    }
    return verdict;
}

QuietVerdict wait_for_quiet(u32 wait_seconds, const QuietLimits& limits) noexcept {
    const u32 cores = host_cores();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait_seconds);
    u32 settled = 0;
    for (;;) {
        const HostReading before = read_host();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const QuietVerdict verdict = judge_window(before, read_host(), cores, limits, true);
        if (verdict.quiet) {
            if (++settled >= kSettledWindows) {
                return verdict;
            }
            continue;
        }
        settled = 0;
        if (std::chrono::steady_clock::now() >= deadline) {
            return verdict;
        }
        std::printf("  waiting for a quiet host: %s\n", verdict.reason);
        std::fflush(stdout);
    }
}

TakeWatch::TakeWatch(u32 cores, const QuietLimits& limits, const OwnScope& own) noexcept
    : cores_(cores), limits_(limits), own_(own) {}

void TakeWatch::begin() noexcept {
    window_start_ = read_host(own_);
    started_ = window_start_.at;
    has_previous_ = false;
    judged_ = false;
    windows_ = 0;
}

void TakeWatch::sample() noexcept {
    if (std::chrono::steady_clock::now() - window_start_.at < std::chrono::seconds(1)) {
        return;
    }
    const HostReading now = read_host(own_);
    judge(window_start_, now);
    previous_start_ = window_start_;
    has_previous_ = true;
    window_start_ = now;
}

QuietVerdict TakeWatch::finish() noexcept {
    const HostReading now = read_host(own_);
    const bool short_remainder =
        now.at - window_start_.at < std::chrono::milliseconds(500) && has_previous_;
    judge(short_remainder ? previous_start_ : window_start_, now);
    return busiest_;
}

void TakeWatch::judge(const HostReading& from, const HostReading& to) noexcept {
    // Pressure is not judged: the measured program is running flat out across the take, and its
    // own threads waiting for a core are not the host being busy.
    const QuietVerdict verdict = judge_window(from, to, cores_, limits_, false);
    ++windows_;
    // An unreadable window decides the take: "cannot tell" must not be outvoted by the others.
    const bool busier = verdict.other_cores > busiest_.other_cores;
    if (!judged_ || (busiest_.readable && (!verdict.readable || busier))) {
        busiest_ = verdict;
    }
    judged_ = true;
}

}  // namespace cy::host_load
