// Pressure levels, the declared response, and the hysteresis that stops oscillation. Task 2.3.
//
// The scenarios in `core-memory-and-containers` under "Memory pressure levels" are "Everything
// trims together", "No oscillation" and "Pressure precedes failure"; each has a case here.

#include <cy/test/test.h>

#include <cy/core/memory/budget.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/pressure.h>
#include <cy/core/memory/system_allocator.h>

namespace {

/// A cache-holding subsystem that declares its response, in the shape a real one has: it trims at
/// Elevated and drops at Critical, and it records what it was told so the test can check the whole
/// broadcast rather than one subscriber's side effect.
class TestCache final : public cy::PressureResponder {
public:
    explicit TestCache(const char* name) noexcept : name_(name) {}

    const char* responder_name() const noexcept override { return name_; }

    void on_pressure(cy::PressureLevel level, cy::PressureLevel previous) noexcept override {
        last = level;
        last_previous = previous;
        ++notifications;
        switch (level) {
            case cy::PressureLevel::Normal:
                break;
            case cy::PressureLevel::Elevated:
                held_bytes /= 2;  // trim caches, reduce prefetch distance
                ++trims;
                break;
            case cy::PressureLevel::Critical:
                held_bytes = 0;  // drop optional caches entirely
                ++drops;
                break;
        }
    }

    cy::u64 held_bytes = 1024;
    cy::PressureLevel last = cy::PressureLevel::Normal;
    cy::PressureLevel last_previous = cy::PressureLevel::Normal;
    cy::u32 notifications = 0;
    cy::u32 trims = 0;
    cy::u32 drops = 0;

private:
    const char* name_;
};

/// A budget tree whose utilisation the test drives directly, so the pressure logic is exercised
/// without allocating hundreds of megabytes to reach a threshold.
///
/// The domain is `Scripting`, which nothing else in this suite touches. Its budget is set relative
/// to whatever it already holds — ten times that plus a kilobyte — so the starting utilisation is
/// about a tenth whatever the domain's baseline turns out to be, and the driver can move it over
/// the whole range above that. Movement is in steps of a thousandth of the range, so setting a
/// utilisation is a bounded loop rather than one iteration per byte.
class DrivenBudgets {
public:
    DrivenBudgets() noexcept {
        baseline_ = live_bytes();
        budget_ = baseline_ * 10 + 1024;
        step_ = (budget_ - baseline_) / 1024;
        step_ = (step_ == 0) ? 1 : step_;
        tree_.set(cy::MemoryDomain::Scripting, budget_, cy::BudgetKind::Soft);
    }

    ~DrivenBudgets() { set_utilisation(0.0); }

    DrivenBudgets(const DrivenBudgets&) = delete;
    DrivenBudgets& operator=(const DrivenBudgets&) = delete;

    /// Move the Scripting domain's live bytes so that its utilisation is about `fraction`.
    void set_utilisation(double fraction) noexcept {
        const auto target = static_cast<cy::u64>(fraction * static_cast<double>(budget_));
        const cy::u64 wanted = (target > baseline_) ? (target - baseline_) : 0;
        while (held_ + step_ <= wanted) {
            cy::domain_record_allocation(cy::MemoryDomain::Scripting, step_);
            held_ += step_;
        }
        while (held_ >= step_ && held_ > wanted) {
            cy::domain_record_free(cy::MemoryDomain::Scripting, step_);
            held_ -= step_;
        }
    }

    [[nodiscard]] cy::BudgetTree& tree() noexcept { return tree_; }
    [[nodiscard]] double utilisation() const noexcept {
        return static_cast<double>(live_bytes()) / static_cast<double>(budget_);
    }

private:
    [[nodiscard]] static cy::u64 live_bytes() noexcept {
        return cy::domain_stats_recursive(cy::MemoryDomain::Scripting).live_bytes;
    }

    cy::BudgetTree tree_;
    cy::u64 baseline_ = 0;
    cy::u64 budget_ = 0;
    cy::u64 held_ = 0;
    cy::u64 step_ = 1;
};

}  // namespace

CY_TEST_CASE("everything trims together: one broadcast, every subscriber's declared response") {
    cy::PressureMonitor monitor;
    TestCache textures("textures");
    TestCache geometry("geometry");
    TestCache audio("audio");

    CY_REQUIRE(monitor.subscribe(textures).has_value());
    CY_REQUIRE(monitor.subscribe(geometry).has_value());
    CY_REQUIRE(monitor.subscribe(audio).has_value());
    CY_CHECK_EQ(monitor.subscriber_count(), 3u);
    // Subscribing twice is a caller error worth reporting rather than a silent second entry.
    CY_CHECK_FALSE(monitor.subscribe(textures).has_value());

    DrivenBudgets budgets;
    budgets.set_utilisation(0.90);  // above elevated_rise, below critical_rise

    CY_CHECK_EQ(monitor.evaluate(budgets.tree()), cy::PressureLevel::Elevated);

    for (const TestCache* cache : {&textures, &geometry, &audio}) {
        CY_CHECK_EQ(cache->last, cy::PressureLevel::Elevated);
        CY_CHECK_EQ(cache->last_previous, cy::PressureLevel::Normal);
        CY_CHECK_EQ(cache->trims, 1u);
        CY_CHECK_EQ(cache->held_bytes, 512u);
    }

    CY_REQUIRE(monitor.unsubscribe(audio).has_value());
    CY_CHECK_EQ(monitor.subscriber_count(), 2u);
    CY_CHECK_FALSE(monitor.unsubscribe(audio).has_value());
}

CY_TEST_CASE("no oscillation: utilisation hovering at a threshold changes nothing") {
    cy::PressureMonitor monitor;
    TestCache cache("hovering");
    CY_REQUIRE(monitor.subscribe(cache).has_value());

    DrivenBudgets budgets;
    const cy::PressureThresholds thresholds = monitor.thresholds();

    budgets.set_utilisation(thresholds.elevated_rise + 0.01);
    CY_CHECK_EQ(monitor.evaluate(budgets.tree()), cy::PressureLevel::Elevated);
    const cy::u32 after_rise = cache.notifications;
    CY_CHECK_EQ(after_rise, 1u);

    // Between the fall and the rise thresholds, in both directions, repeatedly. A single-threshold
    // implementation would trim and refill on every step; this one does nothing at all.
    for (int step = 0; step < 8; ++step) {
        budgets.set_utilisation(thresholds.elevated_rise - 0.01);
        CY_CHECK_EQ(monitor.evaluate(budgets.tree()), cy::PressureLevel::Elevated);
        budgets.set_utilisation(thresholds.elevated_fall + 0.01);
        CY_CHECK_EQ(monitor.evaluate(budgets.tree()), cy::PressureLevel::Elevated);
    }
    CY_CHECK_EQ(cache.notifications, after_rise);

    // Below the fall threshold it does drop back — hysteresis is a delay, not a latch.
    budgets.set_utilisation(thresholds.elevated_fall - 0.05);
    CY_CHECK_EQ(monitor.evaluate(budgets.tree()), cy::PressureLevel::Normal);
    CY_CHECK_EQ(cache.notifications, after_rise + 1);
}

CY_TEST_CASE("pressure precedes failure, and the transition is recorded with its cause") {
    cy::PressureMonitor monitor;
    DrivenBudgets budgets;

    budgets.set_utilisation(0.99);
    CY_CHECK_EQ(monitor.evaluate(budgets.tree()), cy::PressureLevel::Critical);
    CY_CHECK_EQ(monitor.level(), cy::PressureLevel::Critical);
    CY_CHECK_EQ(monitor.transition_count(), 1u);

    cy::PressureTransition history[4];
    const cy::u32 count = monitor.history(history, 4);
    CY_REQUIRE_EQ(count, 1u);
    CY_CHECK_EQ(history[0].from, cy::PressureLevel::Normal);
    CY_CHECK_EQ(history[0].to, cy::PressureLevel::Critical);
    CY_CHECK_EQ(history[0].cause, cy::MemoryDomain::Scripting);
    CY_CHECK(history[0].utilisation > 0.9);
    CY_CHECK_EQ(history[0].sequence, 1u);
}

CY_TEST_CASE("the platform's own condition is a floor the engine never reports below") {
    cy::PressureMonitor monitor;
    DrivenBudgets budgets;
    budgets.set_utilisation(0.10);

    CY_CHECK_EQ(monitor.evaluate(budgets.tree()), cy::PressureLevel::Normal);

    // A low-memory notification from the operating system: it knows about memory the budget tree
    // does not describe, so it can only raise the level.
    monitor.report_platform_level(cy::PressureLevel::Critical);
    CY_CHECK_EQ(monitor.evaluate(budgets.tree()), cy::PressureLevel::Critical);

    monitor.report_platform_level(cy::PressureLevel::Normal);
    CY_CHECK_EQ(monitor.evaluate(budgets.tree()), cy::PressureLevel::Normal);
}

CY_TEST_CASE("an over-budget domain raises pressure") {
    cy::PressureMonitor monitor;
    TestCache cache("streaming");
    CY_REQUIRE(monitor.subscribe(cache).has_value());

    const cy::u64 baseline = cy::domain_stats_recursive(cy::MemoryDomain::Network).live_bytes;
    cy::BudgetTree tree;
    tree.set(cy::MemoryDomain::Network, baseline + 4096, cy::BudgetKind::Soft);
    CY_CHECK_EQ(monitor.evaluate(tree), cy::PressureLevel::Normal);

    cy::SystemAllocator& network = cy::system_allocator(cy::MemoryDomain::Network);
    void* block = network.allocate(8192);
    CY_REQUIRE(block != nullptr);

    CY_CHECK(tree.over_budget(cy::MemoryDomain::Network));
    CY_CHECK_EQ(monitor.evaluate(tree), cy::PressureLevel::Critical);
    CY_CHECK_EQ(cache.drops, 1u);
    CY_CHECK_EQ(cache.held_bytes, 0u);

    network.deallocate(block, 8192);
    CY_CHECK_EQ(monitor.evaluate(tree), cy::PressureLevel::Normal);
}
