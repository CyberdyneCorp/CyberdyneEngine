// M9 TASK 4.3 — RPCs ARE NOT THE CHANNEL FOR GAMEPLAY INTENT.
//
// `networking-and-replication`: "**RPCs SHALL NOT be the channel for gameplay intent** ... Intent
// sent by RPC would bypass prediction, replay, and command validation." That last clause is this
// milestone's subject, so the first case is the one to read: a declaration that admits to carrying
// intent does not register, and there is no overload that lets it.

#include "fixture.h"

#include <cy/networking/rpc.h>

#include <cstring>

using namespace cy::net_test;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

constexpr PeerId kServer = PeerId::make(0, 1);
constexpr PeerId kAlice = PeerId::make(1, 1);
constexpr PeerId kBob = PeerId::make(2, 1);

[[nodiscard]] RpcDeclaration chat() noexcept {
    RpcDeclaration declaration;
    declaration.name = cy::Name::intern("SendChatMessage");
    declaration.stable_id = 4001;
    declaration.direction = RpcDirection::ToServer;
    declaration.authority = RpcAuthority::AnyPeer;
    declaration.bounds.payload_size = 0;
    declaration.rate_limit_per_second = 3;
    return declaration;
}

}  // namespace

CY_TEST_CASE("networking: an RPC that carries gameplay intent does not register") {
    RpcRegistry registry(allocator());
    RpcDeclaration firing;
    firing.name = cy::Name::intern("FireWeapon");
    firing.stable_id = 4100;
    firing.direction = RpcDirection::ToServer;

    const auto refused = registry.declare(firing, /*carries_gameplay_intent=*/true);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error() == RpcRefusal::IntentMustBeACommand);
    CY_CHECK_EQ(registry.size(), 0U);

    // Declared without the admission it still registers — and the development-build heuristic
    // reports it, because a heuristic that could refuse would eventually refuse something real.
    CY_CHECK(registry.declare(firing, false).has_value());
    CY_CHECK_EQ(registry.suspected_intent(), 1U);
    CY_CHECK(RpcRegistry::looks_like_intent(firing));

    RpcDeclaration notification;
    notification.name = cy::Name::intern("MatchTimerExpired");
    notification.stable_id = 4101;
    notification.direction = RpcDirection::ToClients;
    CY_CHECK_FALSE(RpcRegistry::looks_like_intent(notification));
}

CY_TEST_CASE("networking: an unauthorised call is rejected, logged, and counted") {
    RpcRegistry registry(allocator());
    AuthorityRegistry authority(allocator(), Topology::DedicatedServer, kServer);
    NetworkIdMinter minter(1);
    const NetworkId owned_by_alice = minter.mint();
    CY_REQUIRE(authority.track(owned_by_alice, kAlice, cy::ecs::Entity::make(1, 1)).has_value());

    RpcDeclaration server_only;
    server_only.name = cy::Name::intern("KickPeer");
    server_only.stable_id = 4200;
    server_only.authority = RpcAuthority::ServerOnly;
    CY_REQUIRE(registry.declare(server_only, false).has_value());

    RpcDeclaration owner_only;
    owner_only.name = cy::Name::intern("RenameSquad");
    owner_only.stable_id = 4201;
    owner_only.authority = RpcAuthority::EntityOwner;
    CY_REQUIRE(registry.declare(owner_only, false).has_value());

    RpcCall call;
    call.stable_id = 4200;
    call.sender = kAlice;
    CY_CHECK(registry.validate(call, authority, kServer, 0) == RpcRefusal::NotPermitted);
    call.sender = kServer;
    CY_CHECK(registry.validate(call, authority, kServer, 0) == RpcRefusal::None);

    call.stable_id = 4201;
    call.target = owned_by_alice;
    call.sender = kBob;
    CY_CHECK(registry.validate(call, authority, kServer, 0) == RpcRefusal::NotPermitted);
    call.sender = kAlice;
    CY_CHECK(registry.validate(call, authority, kServer, 0) == RpcRefusal::None);

    call.stable_id = 9999;
    CY_CHECK(registry.validate(call, authority, kServer, 0) == RpcRefusal::UnknownRpc);
    CY_CHECK_EQ(registry.rejected_total(), u64{3});

    const PeerRpcRecord* bob = registry.record_for(kBob);
    CY_REQUIRE(bob != nullptr);
    CY_CHECK_EQ(bob->rejected, u64{1});
}

CY_TEST_CASE("networking: parameters are checked against their declared bounds") {
    RpcRegistry registry(allocator());
    AuthorityRegistry authority(allocator(), Topology::DedicatedServer, kServer);

    RpcDeclaration bounded;
    bounded.name = cy::Name::intern("SetTeamNumber");
    bounded.stable_id = 4300;
    bounded.bounds.payload_size = static_cast<cy::u16>(sizeof(cy::i64));
    bounded.bounds.checked = true;
    bounded.bounds.minimum = 0;
    bounded.bounds.maximum = 3;
    CY_REQUIRE(registry.declare(bounded, false).has_value());

    cy::i64 value = 2;
    u8 payload[sizeof(cy::i64)];
    std::memcpy(static_cast<void*>(payload), static_cast<const void*>(&value), sizeof(value));

    RpcCall call;
    call.stable_id = 4300;
    call.sender = kAlice;
    call.payload = cy::Span<const u8>(payload, sizeof(payload));
    CY_CHECK(registry.validate(call, authority, kServer, 0) == RpcRefusal::None);

    value = 9;
    std::memcpy(static_cast<void*>(payload), static_cast<const void*>(&value), sizeof(value));
    CY_CHECK(registry.validate(call, authority, kServer, 0) == RpcRefusal::ParameterOutOfBounds);

    call.payload = cy::Span<const u8>(payload, 4);
    CY_CHECK(registry.validate(call, authority, kServer, 0) == RpcRefusal::PayloadSize);
}

CY_TEST_CASE("networking: a peer sending faster than its limit is clamped and flagged") {
    RpcRegistry registry(allocator());
    AuthorityRegistry authority(allocator(), Topology::DedicatedServer, kServer);
    CY_REQUIRE(registry.declare(chat(), false).has_value());

    RpcCall call;
    call.stable_id = 4001;
    call.sender = kAlice;

    for (u32 index = 0; index < 3; ++index) {
        CY_CHECK(registry.validate(call, authority, kServer, /*now_seconds=*/10) ==
                 RpcRefusal::None);
    }
    CY_CHECK(registry.validate(call, authority, kServer, 10) == RpcRefusal::RateLimited);

    const PeerRpcRecord* alice = registry.record_for(kAlice);
    CY_REQUIRE(alice != nullptr);
    CY_CHECK_EQ(alice->accepted, u64{3});
    CY_CHECK_EQ(alice->rate_limited, u64{1});

    // The window is per second, so the next second starts again — a limit that never reset would be
    // a disconnect with extra steps.
    CY_CHECK(registry.validate(call, authority, kServer, 11) == RpcRefusal::None);

    // And another peer is unaffected: the limit is per peer, or one loud client silences the room.
    call.sender = kBob;
    CY_CHECK(registry.validate(call, authority, kServer, 11) == RpcRefusal::None);
}
