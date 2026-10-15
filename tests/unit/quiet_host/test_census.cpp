// SPDX-License-Identifier: MIT
// The quiet-host wrapper's session census: what counts as the wrapped command's own CPU time.
//
// THE REGRESSION. `smoke.quiet_host_nested` failed on an idle hosted runner (CI run 38508339082)
// with "other processes used 3.96 of 4 cores": its command's four spinners were reaped while the
// census was reading /proc, the census read their parent before the reap and the spinners after
// it, and their eight seconds of CPU were in neither reading. The scripted table below makes that
// reap happen at exactly that moment.

#include "host_load.h"

#include <cy/test/test.h>

#include <vector>

namespace {

using cy::u64;
using cy::host_load::census::ProcessTable;
using cy::host_load::census::Task;

constexpr u64 kSession = 100;

/// A host whose processes are a list, and which can reap one child in the middle of a look.
class ScriptedTable final : public ProcessTable {
public:
    Task wrapper{1, 0, kSession, 5, 0};
    std::vector<Task> tasks;
    /// When nonzero: the first time the parent `reaper` is read, `child` exits and is reaped by it.
    u64 reaper = 0;
    u64 child = 0;
    /// Every read makes `reaper`'s reaped time grow, so no look ever settles.
    bool always_reaping = false;
    int looks = 0;

    bool self(Task& out) override {
        out = wrapper;
        return true;
    }

    bool pids(std::vector<u64>& out) override {
        ++looks;
        for (const Task& task : tasks) {
            out.push_back(task.pid);
        }
        return true;
    }

    bool read(u64 pid, Task& out) override {
        for (Task& task : tasks) {
            if (task.pid != pid) {
                continue;
            }
            out = task;
            if (always_reaping && pid == reaper) {
                ++task.reaped;
            }
            if (pid == reaper && child != 0) {
                reap(task.pid);
            }
            return true;
        }
        return false;
    }

private:
    /// `child` exits and `parent` waits for it: the child's time moves into the parent's `reaped`.
    void reap(u64 parent) {
        u64 spent = 0;
        for (auto it = tasks.begin(); it != tasks.end(); ++it) {
            if (it->pid == child) {
                spent = it->own + it->reaped;
                tasks.erase(it);
                break;
            }
        }
        for (Task& task : tasks) {
            if (task.pid == parent) {
                task.reaped += spent;
            }
        }
        child = 0;
    }
};

}  // namespace

CY_TEST_CASE("census: the session, every descendant in a session of its own, and no one else") {
    ScriptedTable table;
    table.tasks = {
        {10, 1, kSession, 7, 3},  // the wrapped command
        {11, 10, 200, 40, 0},     // a worker that called setsid: still ours, by its parent
        {12, 11, 300, 50, 0},     // and its own child, in a third session
        {13, 1, 999, 1000, 0},    // somebody else's process
    };
    u64 ticks = 0;
    CY_REQUIRE(cy::host_load::census::session_ticks(table, kSession, ticks));
    CY_CHECK_EQ(ticks, 5ULL + 7ULL + 3ULL + 40ULL + 50ULL);
    CY_CHECK_EQ(table.looks, 1);
}

CY_TEST_CASE("census: a child reaped while the census reads is counted once, in its parent") {
    ScriptedTable table;
    table.tasks = {
        {10, 1, kSession, 7, 0},  // the command; it reaps its spinner when first read
        {11, 10, 200, 2000, 0},   // a spinner in a session of its own, two seconds of CPU
    };
    table.reaper = 10;
    table.child = 11;
    u64 ticks = 0;
    CY_REQUIRE(cy::host_load::census::session_ticks(table, kSession, ticks));
    // The first look read the parent before the reap and found the spinner gone after it: 2000
    // ticks in neither. That look is taken again, and the second one finds them in the parent.
    CY_CHECK_EQ(ticks, 5ULL + 7ULL + 2000ULL);
    CY_CHECK_EQ(table.looks, 2);
}

CY_TEST_CASE("census: a host that reaps on every read is looked at a bounded number of times") {
    ScriptedTable table;
    table.tasks = {{10, 1, kSession, 7, 0}};
    table.reaper = 10;
    table.always_reaping = true;
    u64 ticks = 0;
    CY_REQUIRE(cy::host_load::census::session_ticks(table, kSession, ticks));
    CY_CHECK_EQ(table.looks, static_cast<int>(cy::host_load::census::kAttempts));
    CY_CHECK_GE(ticks, 5ULL + 7ULL);
}
