// The commit boundary, the provider registry, and stable tie-breaking.
// Tasks 4.2.3, 4.2.6 (providers) and 4.2.7.

#include <cy/core/determinism/commit.h>
#include <cy/core/determinism/ordering.h>
#include <cy/core/determinism/provider.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <string_view>

namespace {

using namespace cy;
using namespace cy::determinism;

/// One of the five consumers `simulation-and-determinism` names — hashing, rollback capture, replay
/// checkpointing, save snapshotting, network send — reduced to what they have in common: it is told
/// a tick committed and is given the record.
class RecordingObserver final : public CommitObserver {
public:
    explicit RecordingObserver(const char* name) noexcept : name_(name) {}

    [[nodiscard]] const char* name() const noexcept override { return name_; }
    [[nodiscard]] Status on_commit(const CommitRecord& record) noexcept override {
        ++calls;
        last = record;
        return fails ? fail(ErrorCode::Io, "the save could not be written") : ok();
    }

    u32 calls = 0;
    CommitRecord last;
    bool fails = false;

private:
    const char* name_;
};

class SeedProvider final : public StateProvider {
public:
    SeedProvider(const char* name, u64 seed) noexcept : name_(name), seed_(seed) {}

    [[nodiscard]] const char* name() const noexcept override { return name_; }
    [[nodiscard]] Participates participation() const noexcept override {
        return Participates::Hash | Participates::Checkpoint;
    }
    [[nodiscard]] Status hash(StateHashTree& tree) const noexcept override {
        tree.mix_u64(seed_);
        return ok();
    }

private:
    const char* name_;
    u64 seed_;
};

/// Declares that it hashes and does not implement it. The base class refuses, which is what stops
/// the omission from being a silently missing subtree.
class ForgetfulProvider final : public StateProvider {
public:
    [[nodiscard]] const char* name() const noexcept override { return "forgetful"; }
    [[nodiscard]] Participates participation() const noexcept override {
        return Participates::Hash;
    }
};

}  // namespace

CY_TEST_CASE("one moment, many consumers") {
    // `simulation-and-determinism`: "WHEN a tick completes THEN its hash, its snapshot, its replay
    // record, and its network state SHALL describe the same state."
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    CommitBoundary boundary(allocator);

    RecordingObserver hasher("hash");
    RecordingObserver saver("save");
    RecordingObserver sender("network");
    CY_REQUIRE(static_cast<bool>(boundary.observe(hasher)));
    CY_REQUIRE(static_cast<bool>(boundary.observe(saver)));
    CY_REQUIRE(static_cast<bool>(boundary.observe(sender)));
    CY_CHECK_EQ(boundary.observer_count(), 3U);

    CommitRecord tick;
    tick.point = SimulationPoint{Epoch{1}, 42};
    tick.hash = 0xABCD;
    tick.hashed = true;
    const auto committed = boundary.commit(tick);
    CY_REQUIRE(static_cast<bool>(committed));

    CY_CHECK_EQ(boundary.state_version(), 1ULL);
    CY_CHECK_EQ(committed.value().state_version, 1ULL);
    for (const RecordingObserver* observer : {&hasher, &saver, &sender}) {
        CY_REQUIRE_EQ(observer->calls, 1U);
        CY_CHECK(observer->last.point == tick.point);
        CY_CHECK_EQ(observer->last.state_version, 1ULL);
        CY_CHECK_EQ(observer->last.hash, 0xABCDULL);
    }
}

CY_TEST_CASE("nothing observes a partial tick") {
    // The record is published before the observers run, so an observer asking which tick it is
    // looking at sees this one; and `last()` is only ever written by `commit()`, so a caller
    // between ticks sees the previous committed tick and never one in progress.
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    CommitBoundary boundary(allocator);
    CY_CHECK_EQ(boundary.last().state_version, 0ULL);

    CommitRecord tick;
    tick.point = SimulationPoint{Epoch{0}, 1};
    CY_REQUIRE(static_cast<bool>(boundary.commit(tick)));
    CY_CHECK_EQ(boundary.last().point.tick, 1ULL);
    CY_CHECK_EQ(boundary.last().state_version, 1ULL);

    tick.point = SimulationPoint{Epoch{0}, 2};
    CY_REQUIRE(static_cast<bool>(boundary.commit(tick)));
    CY_CHECK_EQ(boundary.last().point.tick, 2ULL);
    CY_CHECK_EQ(boundary.last().state_version, 2ULL);
}

CY_TEST_CASE("every observer runs even when one fails") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    CommitBoundary boundary(allocator);
    RecordingObserver first("a");
    RecordingObserver second("b");
    first.fails = true;
    CY_REQUIRE(static_cast<bool>(boundary.observe(first)));
    CY_REQUIRE(static_cast<bool>(boundary.observe(second)));

    const auto committed = boundary.commit(CommitRecord{});
    CY_CHECK_FALSE(static_cast<bool>(committed));
    // The second still ran: a save that failed to write must not silence the network send.
    CY_CHECK_EQ(second.calls, 1U);
    // And the tick still happened — a failed observer is not an undone commit.
    CY_CHECK_EQ(boundary.state_version(), 1ULL);
}

CY_TEST_CASE("an observer cannot be added twice or during a commit") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    CommitBoundary boundary(allocator);
    RecordingObserver first("save");
    RecordingObserver duplicate("save");
    CY_REQUIRE(static_cast<bool>(boundary.observe(first)));
    CY_CHECK_FALSE(static_cast<bool>(boundary.observe(duplicate)));

    // Registering from inside another observer's on_commit would make which ticks the newcomer sees
    // depend on where it landed in the array.
    class Reentrant final : public CommitObserver {
    public:
        Reentrant(CommitBoundary& boundary, CommitObserver& other) noexcept
            : boundary_(&boundary), other_(&other) {}
        [[nodiscard]] const char* name() const noexcept override { return "reentrant"; }
        [[nodiscard]] Status on_commit(const CommitRecord&) noexcept override {
            refused = !boundary_->observe(*other_);
            return ok();
        }
        bool refused = false;

    private:
        CommitBoundary* boundary_;
        CommitObserver* other_;
    };

    RecordingObserver latecomer("late");
    Reentrant reentrant(boundary, latecomer);
    CY_REQUIRE(static_cast<bool>(boundary.observe(reentrant)));
    CY_REQUIRE(static_cast<bool>(boundary.commit(CommitRecord{})));
    CY_CHECK(reentrant.refused);
}

CY_TEST_CASE("a provider registry is ordered by name, not by registration") {
    // `simulation-and-determinism`: "Registries whose contents affect simulation SHALL be finalised
    // in a deterministic order derived from stable identifiers before simulation begins."
    Allocator& allocator = system_allocator(MemoryDomain::Engine);

    StateHashTree first_tree(allocator);
    StateHashTree second_tree(allocator);
    SeedProvider rules("rules", 1);
    SeedProvider teams("teams", 2);
    SeedProvider clock("clock", 3);

    {
        StateProviderRegistry registry(allocator);
        CY_REQUIRE(static_cast<bool>(registry.add(rules)));
        CY_REQUIRE(static_cast<bool>(registry.add(teams)));
        CY_REQUIRE(static_cast<bool>(registry.add(clock)));
        // Hashing before finalisation is refused: plugin load order would otherwise be visible in
        // the result.
        CY_CHECK_FALSE(static_cast<bool>(registry.hash_all(first_tree)));
        registry.finalize();
        CY_CHECK_FALSE(static_cast<bool>(registry.add(rules)));

        CY_REQUIRE(static_cast<bool>(first_tree.begin(HashLevel::World, 0, "w")));
        CY_REQUIRE(static_cast<bool>(registry.hash_all(first_tree)));
        CY_REQUIRE(static_cast<bool>(first_tree.end()));
    }
    {
        // The same three, registered in the opposite order — a different plugin load order.
        StateProviderRegistry registry(allocator);
        CY_REQUIRE(static_cast<bool>(registry.add(clock)));
        CY_REQUIRE(static_cast<bool>(registry.add(teams)));
        CY_REQUIRE(static_cast<bool>(registry.add(rules)));
        registry.finalize();
        CY_REQUIRE(static_cast<bool>(second_tree.begin(HashLevel::World, 0, "w")));
        CY_REQUIRE(static_cast<bool>(registry.hash_all(second_tree)));
        CY_REQUIRE(static_cast<bool>(second_tree.end()));
    }

    CY_CHECK_EQ(first_tree.root_hash(), second_tree.root_hash());
}

CY_TEST_CASE("a provider that declares hashing and does not implement it is refused") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    StateProviderRegistry registry(allocator);
    ForgetfulProvider provider;
    CY_REQUIRE(static_cast<bool>(registry.add(provider)));
    registry.finalize();

    StateHashTree tree(allocator);
    CY_REQUIRE(static_cast<bool>(tree.begin(HashLevel::World, 0, "w")));
    CY_CHECK_FALSE(static_cast<bool>(registry.hash_all(tree)));
}

CY_TEST_CASE("participation is declared, not assumed") {
    SeedProvider provider("p", 0);
    CY_CHECK(participates_in(provider.participation(), Participates::Hash));
    CY_CHECK(participates_in(provider.participation(), Participates::Checkpoint));
    CY_CHECK_FALSE(participates_in(provider.participation(), Participates::Rollback));
    CY_CHECK_FALSE(participates_in(provider.participation(), Participates::Save));
}

CY_TEST_CASE("equal candidates resolve identically, by stable identity") {
    // `simulation-and-determinism`: "WHEN two targets score identically THEN the tie-break SHALL
    // select the same one on every machine."
    //
    // Two arrays holding the same candidates in different orders — which is what two runs with
    // different chunk assignment produce — must select the same identity.
    struct Candidate {
        u64 id;
        f32 utility;
    };
    const Candidate forward[] = {{7, 0.5F}, {3, 0.9F}, {11, 0.9F}, {2, 0.1F}};
    const Candidate backward[] = {{2, 0.1F}, {11, 0.9F}, {3, 0.9F}, {7, 0.5F}};

    const usize a = select_best(
        4, [&](usize i) noexcept { return forward[i].utility; },
        [&](usize i) noexcept { return forward[i].id; });
    const usize b = select_best(
        4, [&](usize i) noexcept { return backward[i].utility; },
        [&](usize i) noexcept { return backward[i].id; });

    CY_CHECK_EQ(forward[a].id, 3ULL);
    CY_CHECK_EQ(backward[b].id, 3ULL);

    // The same rule with the comparison reversed — path cost rather than utility — breaks ties the
    // same way. A tie-break that changed direction with the comparison would be two rules.
    const usize cheapest = select_best(
        4, [&](usize i) noexcept { return forward[i].utility; },
        [&](usize i) noexcept { return forward[i].id; }, /*prefer_lower=*/true);
    CY_CHECK_EQ(forward[cheapest].id, 2ULL);

    CY_CHECK_EQ(select_best(
                    0, [](usize) noexcept { return 0; }, [](usize) noexcept { return u64{0}; }),
                usize{0});
}

CY_TEST_CASE("sorting by a key breaks ties by identity") {
    u64 ids[] = {5, 1, 9, 3};
    u32 keys[] = {2, 2, 1, 2};
    sort_by_key(
        4, [&](usize i) noexcept { return keys[i]; }, [&](usize i) noexcept { return ids[i]; },
        [&](usize a, usize b) noexcept {
            const u64 held_id = ids[a];
            const u32 held_key = keys[a];
            ids[a] = ids[b];
            keys[a] = keys[b];
            ids[b] = held_id;
            keys[b] = held_key;
        });
    CY_CHECK_EQ(ids[0], 9ULL);  // key 1
    CY_CHECK_EQ(ids[1], 1ULL);  // key 2, lowest id
    CY_CHECK_EQ(ids[2], 3ULL);
    CY_CHECK_EQ(ids[3], 5ULL);
}

CY_TEST_CASE("every tick phase has a name") {
    for (u32 phase = 0; phase < kTickPhaseCount; ++phase) {
        CY_CHECK(std::string_view(tick_phase_name(static_cast<TickPhase>(phase))) != "unknown");
    }
    CY_CHECK(std::string_view(ordering_name(Ordering::Stable)) == "stable");
}
