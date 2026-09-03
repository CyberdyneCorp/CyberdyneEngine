// The budget tree, its platform profiles, and the startup over-subscription check. Task 2.2.

#include <cy/test/test.h>

#include <cy/core/memory/budget.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/system_allocator.h>

#include <cstring>

CY_TEST_CASE("over-subscription is caught at startup, naming the parent and the sum") {
    cy::BudgetTree tree;
    tree.set(cy::MemoryDomain::Engine, 1000, cy::BudgetKind::Hard);
    tree.set(cy::MemoryDomain::Ecs, 600, cy::BudgetKind::Soft);
    tree.set(cy::MemoryDomain::Renderer, 600, cy::BudgetKind::Soft);

    const cy::Status validated = tree.validate();
    CY_REQUIRE_FALSE(validated.has_value());
    CY_CHECK_EQ(validated.error().code, cy::ErrorCode::InvalidArgument);
    // The message has to be actionable: it names the parent, its budget and what the children add
    // up to, because "which one is too big" is the next question.
    CY_CHECK(std::strstr(validated.error().message, "engine") != nullptr);
    CY_CHECK(std::strstr(validated.error().message, "1200") != nullptr);
    CY_CHECK(std::strstr(validated.error().message, "1000") != nullptr);

    tree.set(cy::MemoryDomain::Renderer, 400, cy::BudgetKind::Soft);
    CY_CHECK(tree.validate().has_value());
}

CY_TEST_CASE("a grandchild is inside its own parent's total, not additional to it") {
    cy::BudgetTree tree;
    tree.set(cy::MemoryDomain::Engine, 1000, cy::BudgetKind::Hard);
    tree.set(cy::MemoryDomain::Renderer, 800, cy::BudgetKind::Soft);
    // Gpu is a child of Renderer, so 700 fits inside Renderer's 800 and does not count again
    // against Engine's 1000.
    tree.set(cy::MemoryDomain::Gpu, 700, cy::BudgetKind::Soft);
    CY_CHECK(tree.validate().has_value());

    tree.set(cy::MemoryDomain::Gpu, 900, cy::BudgetKind::Soft);
    CY_CHECK_FALSE(tree.validate().has_value());
}

CY_TEST_CASE("platform profiles differ, with no code change") {
    const cy::MemoryProfile* desktop = cy::find_memory_profile("desktop");
    const cy::MemoryProfile* handheld = cy::find_memory_profile("handheld");
    const cy::MemoryProfile* server = cy::find_memory_profile("server");
    CY_REQUIRE(desktop != nullptr);
    CY_REQUIRE(handheld != nullptr);
    CY_REQUIRE(server != nullptr);
    CY_CHECK(cy::find_memory_profile("nonexistent") == nullptr);

    cy::BudgetTree tree;
    CY_REQUIRE(tree.apply(*desktop).has_value());
    const cy::u64 desktop_total = tree.budget(cy::MemoryDomain::Engine);

    CY_REQUIRE(tree.apply(*handheld).has_value());
    CY_CHECK(tree.budget(cy::MemoryDomain::Engine) < desktop_total);
    // The handheld profile leaves the editor unbudgeted, which is not the same as a budget of zero.
    CY_CHECK_FALSE(tree.has_budget(cy::MemoryDomain::Editor));

    // Every shipped profile validates. A profile that did not would fail at a customer's startup
    // rather than here.
    CY_REQUIRE(tree.apply(*server).has_value());
    cy::u32 count = 0;
    const cy::MemoryProfile* profiles = cy::memory_profiles(count);
    CY_CHECK_EQ(count, 3u);
    for (cy::u32 index = 0; index < count; ++index) {
        CY_CHECK(tree.apply(profiles[index]).has_value());
    }
}

CY_TEST_CASE("a hard budget refuses, and a soft one admits so pressure can respond") {
    cy::BudgetTree tree;
    tree.set(cy::MemoryDomain::Engine, 1024ull * 1024ull * 1024ull, cy::BudgetKind::Hard);
    tree.set(cy::MemoryDomain::Streaming, 4096, cy::BudgetKind::Hard);
    tree.set(cy::MemoryDomain::Ecs, 4096, cy::BudgetKind::Soft);
    CY_REQUIRE(tree.validate().has_value());

    cy::SystemAllocator& streaming = cy::system_allocator(cy::MemoryDomain::Streaming);
    void* block = streaming.allocate(3072);
    CY_REQUIRE(block != nullptr);

    CY_CHECK(tree.admits(cy::MemoryDomain::Streaming, 512));
    CY_CHECK_FALSE(tree.admits(cy::MemoryDomain::Streaming, 8192));
    // A soft budget always admits: crossing it raises pressure, it does not stop the growth.
    CY_CHECK(tree.admits(cy::MemoryDomain::Ecs, 1024ull * 1024ull));

    streaming.deallocate(block, 3072);
}

namespace {

/// A cache that holds its budget the way the specification requires: it asks before it grows, and
/// when the answer is no it evicts rather than allocating, and records the eviction.
class ToyCache {
public:
    ToyCache(cy::BudgetTree& budgets, cy::u64 entry_bytes) noexcept
        : budgets_(&budgets), entry_bytes_(entry_bytes) {}

    void insert() noexcept {
        while (!budgets_->admits(cy::MemoryDomain::Assets, entry_bytes_) && held_ >= entry_bytes_) {
            held_ -= entry_bytes_;
            cy::domain_record_free(cy::MemoryDomain::Assets, entry_bytes_);
            budgets_->record_eviction(cy::MemoryDomain::Assets, entry_bytes_);
            ++evictions_;
        }
        held_ += entry_bytes_;
        cy::domain_record_allocation(cy::MemoryDomain::Assets, entry_bytes_);
    }

    void drain() noexcept {
        while (held_ >= entry_bytes_) {
            held_ -= entry_bytes_;
            cy::domain_record_free(cy::MemoryDomain::Assets, entry_bytes_);
        }
    }

    [[nodiscard]] cy::u64 held() const noexcept { return held_; }
    [[nodiscard]] cy::u32 evictions() const noexcept { return evictions_; }

private:
    cy::BudgetTree* budgets_;
    cy::u64 entry_bytes_;
    cy::u64 held_ = 0;
    cy::u32 evictions_ = 0;
};

}  // namespace

CY_TEST_CASE("caches cannot take everything: the eviction is recorded and reported") {
    const cy::u64 baseline = cy::domain_stats_recursive(cy::MemoryDomain::Assets).live_bytes;

    cy::BudgetTree tree;
    tree.set(cy::MemoryDomain::Assets, baseline + 8192, cy::BudgetKind::Hard);

    ToyCache cache(tree, 2048);
    for (int index = 0; index < 10; ++index) {
        cache.insert();
    }

    CY_CHECK(cache.held() <= 8192u);
    CY_CHECK(cache.evictions() > 0);
    CY_CHECK_FALSE(tree.over_budget(cy::MemoryDomain::Assets));

    cy::BudgetRow rows[cy::kMemoryDomainCount];
    const cy::u32 written = tree.report(rows, cy::kMemoryDomainCount);
    CY_REQUIRE_EQ(written, 1u);
    CY_CHECK_EQ(rows[0].domain, cy::MemoryDomain::Assets);
    CY_CHECK_EQ(rows[0].kind, cy::BudgetKind::Hard);
    CY_CHECK_EQ(rows[0].evictions, cache.evictions());
    CY_CHECK_EQ(rows[0].evicted_bytes, static_cast<cy::u64>(cache.evictions()) * 2048u);
    CY_CHECK(rows[0].utilisation > 0.0);

    cache.drain();
    CY_CHECK_EQ(cy::domain_stats_recursive(cy::MemoryDomain::Assets).live_bytes, baseline);
}

CY_TEST_CASE("a shipping build can attribute memory: the counters are not development-only") {
    // This test compiles and runs identically in all four configurations. There is no
    // CY_DEVELOPMENT here on purpose — the requirement is that per-domain live bytes are queryable
    // without a development build, and a test guarded on the profile would not check that.
    const cy::u64 before = cy::domain_stats(cy::MemoryDomain::World).live_bytes;
    cy::SystemAllocator& world = cy::system_allocator(cy::MemoryDomain::World);
    void* block = world.allocate(1024);
    CY_REQUIRE(block != nullptr);
    CY_CHECK_EQ(cy::domain_stats(cy::MemoryDomain::World).live_bytes, before + 1024);
    world.deallocate(block, 1024);

    CY_CHECK(std::strcmp(cy::domain_name(cy::MemoryDomain::World), "world") == 0);
    CY_CHECK(std::strcmp(cy::budget_kind_name(cy::BudgetKind::Hard), "hard") == 0);
}
