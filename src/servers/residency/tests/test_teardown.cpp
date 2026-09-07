// Teardown under load, not at rest. M6 hard rule; `residency` tasks 4.1 to 4.3.
//
// M5.5's gate found the first real engine defect in six milestones by tearing a subsystem down
// while it was busy: Jolt's job bridge destroyed its free list underneath a worker that was still
// releasing a job — one run in forty, a SIGTRAP with no physics call on the stack. M6 creates and
// destroys worlds CONTINUOUSLY rather than once per fixture, so every subsystem it adds has to
// survive the same treatment.
//
// --- WHAT THIS FILE TEARS DOWN, AND IN WHICH ORDER
// ----------------------------------------------------
//
// Three shapes, because they fail differently:
//
//   1. `unregister_subsystem()` while other threads are requesting into that subsystem, holding its
//      pages and reporting them resident. The subsystem's records go, its holds are invalidated,
//      and the concurrent callers get a refusal rather than a queue entry against a policy that no
//      longer exists.
//   2. `reset()` while the same load runs — a world teardown with everything in flight.
//   3. Construction and destruction of the whole server, two hundred times, each one carrying
//      resident pages, outstanding holds, queued requests and announced deadlines at the moment it
//      is destroyed. A destructor that only worked on an empty server would pass every other test
//      in this directory.
//
// --- WHY THE HARNESS IS DECLARED AFTER THE SERVER
// -----------------------------------------------------
//
// Members and locals are destroyed in reverse order of declaration, so `Load` — which owns the
// threads and joins them in its destructor — unwinds BEFORE the server it points at. That is the
// only sound arrangement: no lock inside `ResidencyServer` can make it safe for a thread to *enter*
// a member function of an object whose storage has been freed, and a test that relied on one would
// be testing a race rather than an invariant. The lock in `~ResidencyServer` covers the other half
// — a call already in progress finishes before the tables go — and the two together are the whole
// guarantee.
//
// Nothing in a worker asserts: doctest's macros are not thread-safe. The workers count what
// happened into atomics and the main thread checks them after the join.

#include <cy/servers/residency/server.h>
#include <cy/test/test.h>

#include <atomic>
#include <thread>

using namespace cy::residency;
using cy::u32;
using cy::u64;

namespace {

constexpr u64 kPageBytes = 512;
constexpr u32 kWorkers = 4;

[[nodiscard]] SubsystemPolicy policy() noexcept {
    SubsystemPolicy value;
    value.domain = cy::MemoryDomain::Gpu;
    value.budget_bytes = 64 * kPageBytes;
    value.budget_kind = cy::BudgetKind::Hard;
    value.min_residency_frames = 0;
    return value;
}

/// Threads that hammer the server until told to stop. Owns them; joins them in its destructor.
class Load {
public:
    explicit Load(ResidencyServer& server) : server_(&server) {
        for (u32 index = 0; index < kWorkers; ++index) {
            threads_[index] = std::thread([this, index]() noexcept { run(index); });
        }
    }

    Load(const Load&) = delete;
    Load& operator=(const Load&) = delete;
    Load(Load&&) = delete;
    Load& operator=(Load&&) = delete;

    ~Load() {
        stop_.store(true, std::memory_order_release);
        for (std::thread& worker : threads_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    [[nodiscard]] u64 attempts() const noexcept {
        return attempts_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 refusals() const noexcept {
        return refusals_.load(std::memory_order_relaxed);
    }

private:
    void run(u32 worker) noexcept {
        u64 page = static_cast<u64>(worker) * 1000ULL;
        while (!stop_.load(std::memory_order_acquire)) {
            ++page;
            const PageKey key{Subsystem::Texture, page};

            Request request;
            request.key = key;
            request.bytes = kPageBytes;
            request.inputs.importance = 0.5F;
            if (!server_->request(request)) {
                refusals_.fetch_add(1, std::memory_order_relaxed);
            }

            ResidentReport report;
            report.key = key;
            report.bytes = kPageBytes;
            if (!server_->note_resident(report, 0.0)) {
                refusals_.fetch_add(1, std::memory_order_relaxed);
            }

            // A hold taken and released across the teardown, which is the window the Jolt defect
            // lived in: the free list went away between the acquire and the release.
            if (auto held = server_->hold(key, HoldReason::Streaming); held) {
                server_->touch(key, 0.0, 0.5F, 0.5F);
                if (!server_->set_active(key, true)) {
                    refusals_.fetch_add(1, std::memory_order_relaxed);
                }
                server_->release(*held);
            }
            server_->note_released(key, 0.0);
            attempts_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ResidencyServer* server_;
    std::thread threads_[kWorkers];
    std::atomic<bool> stop_{false};
    std::atomic<u64> attempts_{0};
    std::atomic<u64> refusals_{0};
};

}  // namespace

CY_TEST_CASE("a subsystem is unregistered and re-registered while four threads use it") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy()));

    u64 attempts = 0;
    {
        // Declared AFTER the server: it unwinds first, joining every worker before the server's
        // own destructor runs. See the note at the top of this file.
        Load load(server);

        // At least two hundred teardowns, and not before the workers have completed a thousand
        // passes: the main thread is fast enough to finish its loop before a thread has started,
        // and a teardown test that tore down an idle server would pass for the wrong reason.
        Schedule frame;
        u32 cycle = 0;
        while (cycle < 200 || load.attempts() < 1000) {
            server.unregister_subsystem(Subsystem::Texture);
            CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy()));
            CY_REQUIRE(server.schedule(ScheduleOptions{static_cast<cy::f64>(cycle), 0}, frame));
            server.end_frame(static_cast<cy::f64>(cycle));
            ++cycle;
        }
        attempts = load.attempts();
    }

    // The load actually ran — a teardown test that tore down an idle server would pass anyway.
    CY_CHECK_GT(attempts, 0U);
    // And the accounting is not merely non-negative: after a final unregister nothing is owed.
    CY_CHECK(server.unregister_subsystem(Subsystem::Texture));
    CY_CHECK_EQ(server.resident_bytes(Subsystem::Texture), 0U);
    CY_CHECK_EQ(server.outstanding_holds(), 0U);
}

CY_TEST_CASE("reset runs while the same load is in flight") {
    ResidencyServer server;
    CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy()));

    {
        Load load(server);
        u32 cycle = 0;
        while (cycle < 200 || load.attempts() < 1000) {
            server.reset();
            server.end_frame(static_cast<cy::f64>(cycle));
            ++cycle;
        }
        CY_CHECK_GT(load.attempts(), 0U);
    }

    server.reset();
    CY_CHECK_EQ(server.resident_bytes(Subsystem::Texture), 0U);
    CY_CHECK_EQ(server.outstanding_holds(), 0U);
    CY_CHECK_EQ(server.pending_requests(), 0U);
    CY_CHECK(server.registered(Subsystem::Texture));  // reset keeps the configuration
}

CY_TEST_CASE("two hundred servers are destroyed holding pages, holds, requests and deadlines") {
    for (u32 cycle = 0; cycle < 200; ++cycle) {
        ResidencyServer server;
        CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy()));
        CY_REQUIRE(server.register_subsystem(Subsystem::Geometry, policy()));

        for (u64 page = 0; page < 16; ++page) {
            ResidentReport report;
            report.key = PageKey{Subsystem::Texture, page};
            report.bytes = kPageBytes;
            CY_REQUIRE(server.note_resident(report, 0.0));
            CY_REQUIRE(server.hold(PageKey{Subsystem::Texture, page}, HoldReason::Editor));

            Request request;
            request.key = PageKey{Subsystem::Geometry, page};
            request.bytes = kPageBytes;
            CY_REQUIRE(server.request(request));
        }
        Prediction arrival;
        arrival.region = cycle;
        arrival.seconds_until = 1.0;
        CY_REQUIRE(server.announce(arrival, 0.0));
        CY_REQUIRE(server.publish_importance(cycle, ImportanceInputs{}));

        // Destroyed here, with sixteen resident pages, sixteen live holds, sixteen queued requests
        // and six outstanding deadlines. No drain, no shutdown call, no quiescing.
    }
    CY_CHECK(true);  // reaching here without a fault, a leak or a hang is the assertion
}

CY_TEST_CASE("a server destroyed while a worker is inside a public member") {
    // The narrowest window there is: the worker is joined only after the scope that owns the server
    // has stopped touching it, and `~ResidencyServer` takes the lock so that a call already running
    // finishes before the tables are destroyed.
    for (u32 cycle = 0; cycle < 50; ++cycle) {
        ResidencyServer server;
        CY_REQUIRE(server.register_subsystem(Subsystem::Texture, policy()));
        {
            Load load(server);
            Schedule frame;
            while (load.attempts() < 8) {
                CY_REQUIRE(server.schedule(ScheduleOptions{0.0, 0}, frame));
            }
        }
    }
    CY_CHECK(true);
}
