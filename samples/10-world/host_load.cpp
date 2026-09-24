#include "host_load.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace cy::sample::world {

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

/// Fields 14 and 15 of `/proc/self/stat`. The command name in field 2 may hold spaces and
/// parentheses, so the scan starts after the LAST ')'.
[[nodiscard]] bool read_own(HostReading& out) noexcept {
    char line[1024] = {};
    if (!first_line("/proc/self/stat", line, sizeof(line))) {
        return false;
    }
    const char* close = std::strrchr(line, ')');
    if (close == nullptr) {
        return false;
    }
    // After ')': state(3) ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt, then
    // utime(14) stime(15).
    Fields fields(close + 1);
    constexpr int kFieldsBeforeUtime = 11;
    for (int field = 0; field < kFieldsBeforeUtime; ++field) {
        fields.skip();
    }
    const u64 user = fields.whole();
    const u64 system = fields.whole();
    out.own_ticks = user + system;
    return fields.ok();
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

}  // namespace

u32 host_cores() noexcept {
    const unsigned int cores = std::thread::hardware_concurrency();
    return cores == 0 ? 1U : static_cast<u32>(cores);
}

HostReading read_host() noexcept {
    HostReading reading;
    reading.at = std::chrono::steady_clock::now();
    reading.readable = read_stat(reading) && read_own(reading);
    read_pressure(reading);
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
    const bool pressure_known = before.has_pressure && after.has_pressure && wall_us > 0.0;
    verdict.pressure =
        pressure_known
            ? static_cast<f64>(difference(after.pressure_us, before.pressure_us)) / wall_us
            : 0.0;

    const bool cores_ok = verdict.other_cores <= limits.max_other_cores;
    const bool pressure_ok = !judge_pressure || verdict.pressure <= limits.max_pressure;
    verdict.quiet = cores_ok && pressure_ok;
    std::snprintf(verdict.reason, sizeof(verdict.reason),
                  "host %s: other processes used %.2f of %u cores (limit %.2f), CPU pressure "
                  "%.1f%% (%s%.1f%%), loadavg %.2f against %u cores, over %.1f s",
                  verdict.quiet ? "quiet" : "too busy", verdict.other_cores, cores,
                  limits.max_other_cores, verdict.pressure * 100.0,
                  judge_pressure ? "limit " : "not judged while this program runs, limit ",
                  limits.max_pressure * 100.0, after.load_one_minute, cores, wall_us / 1.0e6);
    return verdict;
}

QuietVerdict wait_for_quiet(u32 wait_seconds, const QuietLimits& limits) noexcept {
    const u32 cores = host_cores();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(wait_seconds);
    QuietVerdict verdict;
    for (;;) {
        const HostReading before = read_host();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        verdict = judge_window(before, read_host(), cores, limits, true);
        if (verdict.quiet || std::chrono::steady_clock::now() >= deadline) {
            return verdict;
        }
        std::printf("  waiting for a quiet host: %s\n", verdict.reason);
        std::fflush(stdout);
    }
}

TakeWatch::TakeWatch(u32 cores, const QuietLimits& limits) noexcept
    : cores_(cores), limits_(limits) {}

void TakeWatch::begin() noexcept {
    window_start_ = read_host();
    has_previous_ = false;
    judged_ = false;
    windows_ = 0;
}

void TakeWatch::sample() noexcept {
    if (std::chrono::steady_clock::now() - window_start_.at < std::chrono::seconds(1)) {
        return;
    }
    const HostReading now = read_host();
    judge(window_start_, now);
    previous_start_ = window_start_;
    has_previous_ = true;
    window_start_ = now;
}

QuietVerdict TakeWatch::finish() noexcept {
    const HostReading now = read_host();
    const bool short_remainder =
        now.at - window_start_.at < std::chrono::milliseconds(500) && has_previous_;
    judge(short_remainder ? previous_start_ : window_start_, now);
    return busiest_;
}

void TakeWatch::judge(const HostReading& from, const HostReading& to) noexcept {
    // Pressure is not judged: this program is running flat out across the take, and its own
    // threads waiting for a core are not the host being busy.
    const QuietVerdict verdict = judge_window(from, to, cores_, limits_, false);
    ++windows_;
    // An unreadable window decides the take: "cannot tell" must not be outvoted by the others.
    const bool busier = verdict.other_cores > busiest_.other_cores;
    if (!judged_ || (busiest_.readable && (!verdict.readable || busier))) {
        busiest_ = verdict;
    }
    judged_ = true;
}

}  // namespace cy::sample::world
