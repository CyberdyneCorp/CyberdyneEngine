// M9 TASK 4.2 — EXACTLY ONE AUTHORITY, INCLUDING DURING A HANDOVER.
//
// `networking-and-replication`: "the protocol SHALL ensure exactly one peer considers itself
// authoritative at any time, with the transition acknowledged". The case below walks the handover
// window step by step and asserts the count at every point — including the point where the
// acknowledgement is lost, which is the only interesting one.

#include "fixture.h"

#include <cy/networking/authority.h>

using namespace cy::net_test;
using cy::u32;
using cy::u64;

namespace {

constexpr PeerId kServer = PeerId::make(0, 1);
constexpr PeerId kAlice = PeerId::make(1, 1);
constexpr PeerId kBob = PeerId::make(2, 1);

}  // namespace

CY_TEST_CASE("networking: a network id carries its minter, so two peers cannot collide") {
    NetworkIdMinter alice(7);
    NetworkIdMinter bob(9);
    const NetworkId first = alice.mint();
    const NetworkId second = bob.mint();

    CY_CHECK(first.valid());
    CY_CHECK_NE(first.value(), second.value());
    CY_CHECK_EQ(first.minter(), cy::u16{7});
    CY_CHECK_EQ(second.minter(), cy::u16{9});
    CY_CHECK_EQ(first.counter(), second.counter());  // both minted their first
    CY_CHECK_FALSE(NetworkId{}.valid());
}

CY_TEST_CASE("networking: the topology decides who owns a spawn") {
    CY_CHECK(authority_for_spawn(Topology::DedicatedServer, kServer, kAlice) == kServer);
    CY_CHECK(authority_for_spawn(Topology::ListenServer, kServer, kAlice) == kServer);
    CY_CHECK(authority_for_spawn(Topology::PeerToPeerDistributed, kServer, kAlice) == kAlice);
}

CY_TEST_CASE("networking: a client's write on server-owned state is local-only and counted") {
    AuthorityRegistry registry(allocator(), Topology::DedicatedServer, kAlice);
    NetworkIdMinter minter(1);
    const NetworkId id = minter.mint();
    CY_REQUIRE(registry.track(id, kServer, cy::ecs::Entity::make(4, 1)).has_value());

    CY_CHECK(registry.classify_write(id, kServer) == WriteVerdict::Authoritative);
    CY_CHECK(registry.classify_write(id, kAlice) == WriteVerdict::LocalOnly);
    CY_CHECK(registry.classify_write(minter.mint(), kAlice) == WriteVerdict::Unknown);
    // Counted rather than printed: prediction is exactly this write, and a line per frame per
    // predicted entity is a diagnostic nobody leaves on.
    CY_CHECK_EQ(registry.local_only_writes(), u64{1});

    // Two records for one id is the two-authorities failure in local form.
    CY_CHECK_FALSE(registry.track(id, kAlice, cy::ecs::Entity::make(5, 1)).has_value());
}

CY_TEST_CASE("networking: a handover has exactly one authority at every step") {
    AuthorityRegistry registry(allocator(), Topology::PeerToPeerDistributed, kAlice);
    NetworkIdMinter minter(1);
    const NetworkId id = minter.mint();
    CY_REQUIRE(registry.track(id, kAlice, cy::ecs::Entity::make(4, 1)).has_value());
    CY_CHECK_EQ(registry.self_declared_authorities(id), 1U);

    const auto token = registry.begin_handover(id, kBob, /*expires_at_tick=*/100);
    CY_REQUIRE(token.has_value());
    // The pending phase. Alice is still authoritative — pessimistic on purpose, because the
    // alternative has a window in which both peers believe they own the entity.
    CY_CHECK(registry.authority_of(id) == kAlice);
    CY_CHECK_EQ(registry.self_declared_authorities(id), 1U);
    CY_CHECK(registry.classify_write(id, kBob) == WriteVerdict::LocalOnly);

    // A second transfer while one is pending would put two peers in the incoming position.
    CY_CHECK_FALSE(registry.begin_handover(id, kServer, 100).has_value());
    // A stale acknowledgement for a different handover does not complete this one.
    CY_CHECK_FALSE(registry.acknowledge_handover(id, token.value() + 1).has_value());
    CY_CHECK(registry.authority_of(id) == kAlice);

    CY_REQUIRE(registry.acknowledge_handover(id, token.value()).has_value());
    CY_CHECK(registry.authority_of(id) == kBob);
    CY_CHECK_EQ(registry.self_declared_authorities(id), 1U);
    CY_CHECK_EQ(registry.handovers_completed(), u64{1});
    CY_CHECK(registry.classify_write(id, kBob) == WriteVerdict::Authoritative);
}

CY_TEST_CASE("networking: a handover nobody acknowledges expires back, never to nobody") {
    AuthorityRegistry registry(allocator(), Topology::PeerToPeerDistributed, kAlice);
    NetworkIdMinter minter(1);
    const NetworkId id = minter.mint();
    CY_REQUIRE(registry.track(id, kAlice, cy::ecs::Entity::make(4, 1)).has_value());

    const auto token = registry.begin_handover(id, kBob, /*expires_at_tick=*/50);
    CY_REQUIRE(token.has_value());
    CY_CHECK_EQ(registry.expire_handovers(49), 0U);
    CY_CHECK_EQ(registry.self_declared_authorities(id), 1U);

    CY_CHECK_EQ(registry.expire_handovers(50), 1U);
    CY_CHECK(registry.authority_of(id) == kAlice);
    CY_CHECK_EQ(registry.self_declared_authorities(id), 1U);
    CY_CHECK_EQ(registry.handovers_expired(), u64{1});

    // And the acknowledgement that arrives after the expiry does not resurrect it: the entity would
    // otherwise be handed to a peer that has stopped expecting it.
    CY_CHECK_FALSE(registry.acknowledge_handover(id, token.value()).has_value());
    CY_CHECK(registry.authority_of(id) == kAlice);
}

CY_TEST_CASE("networking: a handover is refused to nobody and to the current owner") {
    AuthorityRegistry registry(allocator(), Topology::PeerToPeerDistributed, kAlice);
    NetworkIdMinter minter(1);
    const NetworkId id = minter.mint();
    CY_REQUIRE(registry.track(id, kAlice, cy::ecs::Entity::make(4, 1)).has_value());

    CY_CHECK_FALSE(registry.begin_handover(id, PeerId{}, 10).has_value());
    CY_CHECK_FALSE(registry.begin_handover(id, kAlice, 10).has_value());
    CY_CHECK_FALSE(registry.begin_handover(minter.mint(), kBob, 10).has_value());
    CY_CHECK_EQ(registry.self_declared_authorities(id), 1U);

    CY_REQUIRE(registry.forget(id).has_value());
    CY_CHECK_EQ(registry.self_declared_authorities(id), 0U);
    CY_CHECK_FALSE(registry.forget(id).has_value());
}
