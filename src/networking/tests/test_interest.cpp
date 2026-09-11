// M9 TASK 4.4 — RELEVANCE, MEASURED AGAINST A WORLD THAT GROWS.
//
// `networking-and-replication`: "Relevance SHALL be evaluated incrementally where possible, using
// cell membership changes rather than re-evaluating every entity against every peer each tick."
//
// A test that asserted "the peer got the right 200 entities" would pass over an implementation that
// walked all 10 000 to find them — and that implementation is the one that falls over at scale, six
// months later, as a frame time nobody can attribute. So the case below asserts the number of
// entities EXAMINED, and it asserts it against the world's population: the check is that relevance
// scales with the peer's cells rather than with the world.
//
// `integration`, because ten thousand entities is a population, not a unit.

#include "fixture.h"

#include <cy/networking/interest.h>

using namespace cy::net_test;
using cy::i64;
using cy::u32;
using cy::u64;

namespace {

constexpr PeerId kPeer = PeerId::make(1, 1);
constexpr u32 kCellsAcross = 8;

/// A lattice: `kCellsAcross * kCellsAcross` cells, filled evenly.
[[nodiscard]] cy::Status populate(InterestSet& interest, NetworkIdMinter& minter,
                                  u32 population) noexcept {
    for (u32 index = 0; index < population; ++index) {
        const u32 cell_x = index % kCellsAcross;
        const u32 cell_y = (index / kCellsAcross) % kCellsAcross;
        RelevanceSubject subject;
        subject.id = minter.mint();
        subject.cell = static_cast<CellId>(cell_x) + (static_cast<CellId>(cell_y) * kCellsAcross);
        subject.position_x = (static_cast<i64>(cell_x) * 1000) + static_cast<i64>(index % 100);
        subject.position_y = static_cast<i64>(cell_y) * 1000;
        subject.team = 1 + (index % 3);
        if (cy::Status placed = interest.place(subject); !placed) {
            return placed;
        }
    }
    return cy::ok();
}

}  // namespace

CY_TEST_CASE("networking: a peer sees its cells, and the examination scales with them") {
    cy::Allocator& memory = allocator();
    InterestSet interest(memory);
    NetworkIdMinter minter(1);
    constexpr u32 kPopulation = 10000;
    CY_REQUIRE(populate(interest, minter, kPopulation).has_value());
    CY_CHECK_EQ(interest.size(), kPopulation);
    CY_CHECK_EQ(interest.cell_count(), kCellsAcross * kCellsAcross);

    // One cell of sixty-four.
    const CellId cells[1] = {0};
    CY_REQUIRE(interest.set_interest_cells(kPeer, cy::Span<const CellId>(cells, 1)).has_value());

    PeerInterest view;
    view.peer = kPeer;
    view.relevance_distance_squared = 500LL * 500LL;

    cy::Array<Candidate> candidates(memory);
    cy::Array<NetworkId> left(memory);
    const auto delta = interest.evaluate(view, candidates, left);
    CY_REQUIRE(delta.has_value());

    // THE MEASUREMENT. One cell of sixty-four holds about a sixty-fourth of the world, and that is
    // what was examined — not ten thousand. An implementation that scanned the world would report
    // ten thousand here and pass every other assertion in this file.
    CY_CHECK_LE(interest.examined_last(), u64{kPopulation / 32});
    CY_CHECK_GT(interest.examined_last(), u64{0});
    CY_CHECK_GT(candidates.size(), cy::usize{0});
    CY_CHECK_EQ(delta.value().entered, static_cast<u32>(candidates.size()));
    CY_CHECK_EQ(delta.value().left, 0U);

    // And it does not grow when the world does. Doubling the population outside the peer's cell
    // leaves the examination where it was.
    const u64 before = interest.examined_last();
    for (u32 index = 0; index < kPopulation; ++index) {
        RelevanceSubject subject;
        subject.id = minter.mint();
        subject.cell = static_cast<CellId>(8 + (index % 56));
        subject.position_x = 9000;
        CY_REQUIRE(interest.place(subject).has_value());
    }
    candidates.clear();
    CY_REQUIRE(interest.evaluate(view, candidates, left).has_value());
    CY_CHECK_EQ(interest.examined_last(), before);
}

CY_TEST_CASE("networking: entering and leaving relevance are both reported") {
    cy::Allocator& memory = allocator();
    InterestSet interest(memory);
    NetworkIdMinter minter(1);
    CY_REQUIRE(populate(interest, minter, 256).has_value());

    const CellId first[1] = {0};
    CY_REQUIRE(interest.set_interest_cells(kPeer, cy::Span<const CellId>(first, 1)).has_value());

    PeerInterest view;
    view.peer = kPeer;
    view.relevance_distance_squared = 500LL * 500LL;

    cy::Array<Candidate> candidates(memory);
    cy::Array<NetworkId> left(memory);
    const auto entered = interest.evaluate(view, candidates, left);
    CY_REQUIRE(entered.has_value());
    const u32 first_count = entered.value().entered;
    CY_CHECK_GT(first_count, 0U);

    // The peer moves to a different cell. Everything it had leaves, and "SHALL be explicitly
    // dropped so the client can clean up" is the `left` array rather than a silent forget.
    const CellId second[1] = {9};
    CY_REQUIRE(interest.set_interest_cells(kPeer, cy::Span<const CellId>(second, 1)).has_value());
    view.viewpoint_x = 1000;
    view.viewpoint_y = 1000;
    candidates.clear();
    left.clear();
    const auto moved = interest.evaluate(view, candidates, left);
    CY_REQUIRE(moved.has_value());
    CY_CHECK_EQ(moved.value().left, first_count);
    CY_CHECK_EQ(left.size(), static_cast<cy::usize>(first_count));
    CY_CHECK_GT(moved.value().entered, 0U);
}

CY_TEST_CASE("networking: an entity outside the peer's distance leaks nothing") {
    cy::Allocator& memory = allocator();
    InterestSet interest(memory);
    NetworkIdMinter minter(1);

    RelevanceSubject near;
    near.id = minter.mint();
    near.cell = 0;
    near.position_x = 10;
    CY_REQUIRE(interest.place(near).has_value());

    RelevanceSubject far;
    far.id = minter.mint();
    far.cell = 0;
    far.position_x = 100000;
    CY_REQUIRE(interest.place(far).has_value());

    const CellId cells[1] = {0};
    CY_REQUIRE(interest.set_interest_cells(kPeer, cy::Span<const CellId>(cells, 1)).has_value());
    PeerInterest view;
    view.peer = kPeer;
    view.relevance_distance_squared = 1000LL * 1000LL;

    cy::Array<Candidate> candidates(memory);
    cy::Array<NetworkId> left(memory);
    CY_REQUIRE(interest.evaluate(view, candidates, left).has_value());
    CY_REQUIRE_EQ(candidates.size(), cy::usize{1});
    CY_CHECK_EQ(candidates[0].id.value(), near.id.value());
    CY_CHECK(candidates[0].rule == RelevanceRule::CellMembership);
}

CY_TEST_CASE("networking: ownership, always-relevant and a custom predicate each admit") {
    cy::Allocator& memory = allocator();
    InterestSet interest(memory);
    NetworkIdMinter minter(1);

    RelevanceSubject owned;
    owned.id = minter.mint();
    owned.cell = 0;
    owned.owner = kPeer;
    owned.position_x = 999999;  // far away, and the peer's own
    CY_REQUIRE(interest.place(owned).has_value());

    RelevanceSubject objective;
    objective.id = minter.mint();
    objective.cell = 40;  // a cell the peer is not interested in
    objective.always_relevant = true;
    objective.position_x = 888888;
    CY_REQUIRE(interest.place(objective).has_value());

    RelevanceSubject exotic;
    exotic.id = minter.mint();
    exotic.cell = 0;
    exotic.position_x = 777777;
    CY_REQUIRE(interest.place(exotic).has_value());

    const CellId cells[1] = {0};
    CY_REQUIRE(interest.set_interest_cells(kPeer, cy::Span<const CellId>(cells, 1)).has_value());
    PeerInterest view;
    view.peer = kPeer;
    view.relevance_distance_squared = 100LL * 100LL;

    cy::Array<Candidate> candidates(memory);
    cy::Array<NetworkId> left(memory);
    CY_REQUIRE(interest.evaluate(view, candidates, left).has_value());
    CY_REQUIRE_EQ(candidates.size(), cy::usize{2});

    bool saw_ownership = false;
    bool saw_always = false;
    for (auto& candidate : candidates) {
        saw_ownership |= candidate.rule == RelevanceRule::Ownership;
        saw_always |= candidate.rule == RelevanceRule::AlwaysRelevant;
    }
    CY_CHECK(saw_ownership);
    CY_CHECK(saw_always);

    // The project's own rule admits the third, and the candidate says which rule did it — which is
    // what the profiler answers "why was this replicated" with.
    interest.set_predicate(
        [](void*, const PeerInterest&, const RelevanceSubject&) noexcept { return true; }, nullptr);
    candidates.clear();
    CY_REQUIRE(interest.evaluate(view, candidates, left).has_value());
    CY_CHECK_EQ(candidates.size(), cy::usize{3});
}

CY_TEST_CASE("networking: replication cells are derived from world cells by arithmetic") {
    // "Networking MAY aggregate world cells into coarser replication cells where relevance
    // granularity differs from streaming granularity, but the aggregation SHALL be derived from
    // world cell identity, so client and server refer to the same content by the same identifiers."
    CY_CHECK_EQ(InterestSet::aggregate(0, 2), CellId{0});
    CY_CHECK_EQ(InterestSet::aggregate(3, 2), CellId{0});
    CY_CHECK_EQ(InterestSet::aggregate(4, 2), CellId{1});
    CY_CHECK_EQ(InterestSet::aggregate(kNoCell, 2), kNoCell);
    // A shift of zero is the identity, which is what "no aggregation" means.
    CY_CHECK_EQ(InterestSet::aggregate(12345, 0), CellId{12345});
}

CY_TEST_CASE("networking: moving an entity between cells is an index update, not a rebuild") {
    cy::Allocator& memory = allocator();
    InterestSet interest(memory);
    NetworkIdMinter minter(1);
    CY_REQUIRE(populate(interest, minter, 512).has_value());
    const u32 before = interest.size();

    RelevanceSubject moved;
    moved.id = NetworkId::make(1, 1);  // the first minted
    moved.cell = 63;
    moved.position_x = 63000;
    CY_REQUIRE(interest.place(moved).has_value());
    // Placed again rather than added again: the population is unchanged.
    CY_CHECK_EQ(interest.size(), before);

    const CellId cells[1] = {0};
    CY_REQUIRE(interest.set_interest_cells(kPeer, cy::Span<const CellId>(cells, 1)).has_value());
    PeerInterest view;
    view.peer = kPeer;
    view.relevance_distance_squared = 100000LL * 100000LL;
    cy::Array<Candidate> candidates(memory);
    cy::Array<NetworkId> left(memory);
    CY_REQUIRE(interest.evaluate(view, candidates, left).has_value());
    for (auto& candidate : candidates) {
        CY_CHECK_NE(candidate.id.value(), moved.id.value());
    }

    interest.remove(moved.id);
    candidates.clear();
    CY_REQUIRE(interest.evaluate(view, candidates, left).has_value());
    for (auto& candidate : candidates) {
        CY_CHECK_NE(candidate.id.value(), moved.id.value());
    }
}
