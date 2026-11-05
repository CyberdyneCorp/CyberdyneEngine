// SPDX-License-Identifier: MIT
// ISSUE #82 — REPLICATION OVER AN ECS WORLD.
//
// A server world, a replicator over it, and one or two client worlds fed the snapshots by hand, so
// a case can lose, reorder or withhold a snapshot exactly where it means to. The transport is not
// here; `test_session.cpp` runs the same path end to end over the secure transport.

#include "world_fixture.h"

#include <cy/core/reflect/demo/types.h>
#include <cy/core/reflect/demo/types.reflect.h>
#include <cy/core/reflect/registry.h>

#include <cstring>

using namespace cy::net_test;
using cy::u16;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

const cy::net::PeerId kAlice = cy::net::PeerId::make(1, 1);
const cy::net::PeerId kBob = cy::net::PeerId::make(2, 1);

/// One client: its world and its replica.
struct Client {
    GameWorld game;
    cy::net::ReplicaWorld replica;

    explicit Client(const cy::net::SchemaSet& schemas) noexcept
        : game(/*local_first=*/true), replica(allocator(), game.world, schemas) {
        fixture_requires(replica.initialize().has_value());
    }
};

/// Build a snapshot for `peer`, apply it to `client`, and acknowledge it — or, with `lose`, drop
/// it.
[[nodiscard]] cy::net::ReplicationReport deliver(cy::net::WorldReplicator& replicator,
                                                 cy::net::PeerId peer, Client& client, u64 tick,
                                                 const cy::net::BandwidthBudget& budget,
                                                 bool lose = false) noexcept {
    fixture_requires(replicator.gather(tick).has_value());
    auto bytes = replicator.build(peer, tick, budget);
    fixture_requires(bytes.has_value());
    if (!lose) {
        fixture_requires(client.replica.apply(*bytes).has_value());
        replicator.acknowledge(peer, client.replica.newest(), client.replica.previous_mask());
    }
    return replicator.last_report(peer);
}

/// Every band due every tick, so a case that steps tick by tick sees every change at once. The
/// default table spaces updates by band (20 Hz near, 2 Hz far at a 60 Hz tick), which the bandwidth
/// and lost-snapshot cases use on purpose.
[[nodiscard]] cy::net::ReplicatorConfig every_tick() noexcept {
    cy::net::ReplicatorConfig config;
    for (cy::net::BandPolicy& policy : config.bands.policies) {
        policy.interval_ticks = 1;
        policy.guaranteed_interval_ticks = 1;
    }
    return config;
}

[[nodiscard]] cy::net::BandwidthBudget unlimited() noexcept {
    cy::net::BandwidthBudget budget;
    budget.bytes_per_tick = 1U << 30;
    return budget;
}

}  // namespace

CY_TEST_CASE("replication: a schema comes from the type system's stable field ids") {
    // The generated descriptor of a real reflected type: `cy::demo::Health` declares one field
    // `Replicated("quantised", "bits=16", ...)`, one `Transient`, and three with no replication.
    const cy::reflect::TypeInfo& health = cy::reflect::type_of<cy::demo::Health>();
    cy::Array<cy::net::FieldSchema> fields(allocator());
    auto schema = cy::net::schema_from_reflection(health, fields);
    CY_REQUIRE(schema.has_value());
    CY_REQUIRE_EQ(schema->fields.size(), 1U);
    const cy::reflect::FieldInfo* current = nullptr;
    for (u32 index = 0; index < health.field_count; ++index) {
        if (std::strcmp(health.fields[index].name, "current") == 0) {
            current = &health.fields[index];
        }
    }
    CY_REQUIRE(current != nullptr);
    CY_CHECK_EQ(schema->fields[0].field, current->id);
    CY_CHECK_EQ(schema->fields[0].encoder, cy::net::FieldEncoder::QuantisedScalar);
    CY_CHECK_EQ(schema->fields[0].parameters.bits, 16U);
    CY_CHECK_EQ(schema->fields[0].parameters.maximum, 10000.0);
    cy::net::SchemaSet set(allocator());
    CY_CHECK(set.add(health, *schema).has_value());

    // The test component: four replicated fields, one of them owner-only.
    Schemas schemas;
    CY_REQUIRE_EQ(schemas.set.size(), 1U);
    CY_CHECK_EQ(schemas.set.at(0).field_count(), 4U);
    CY_CHECK_EQ(schemas.set.at(0).field(3).filter, cy::net::TargetFilter::OwnerOnly);
}

CY_TEST_CASE("replication: making an entity replicated changes no existing query") {
    Schemas schemas;
    GameWorld server;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set, every_tick());
    CY_REQUIRE(replicator.initialize().has_value());
    for (u32 index = 0; index < 10; ++index) {
        (void)server.spawn(static_cast<float>(index), 0.0F);
    }
    auto count = [&]() noexcept {
        cy::ecs::QueryDesc desc(allocator());
        (void)desc.read(server.body);
        (void)desc.read(server.local);
        cy::ecs::Query query(server.world, std::move(desc));
        return query.entity_count();
    };
    CY_CHECK_EQ(count(), 10U);
    cy::ecs::QueryDesc all(allocator());
    (void)all.read(server.body);
    cy::ecs::Query bodies(server.world, std::move(all));
    cy::Array<cy::ecs::Entity> entities(allocator());
    CY_REQUIRE(bodies
                   .for_each_chunk([&](cy::ecs::QueryChunk& chunk) noexcept {
                       for (const cy::ecs::Entity entity : chunk.entities()) {
                           (void)entities.push_back(entity);
                       }
                   })
                   .has_value());
    for (const cy::ecs::Entity entity : entities) {
        CY_REQUIRE(replicator.replicate(entity, 7, cy::net::PeerId{}).has_value());
    }
    // The same entities, matched by the same query that never heard of NetworkIdentity.
    CY_CHECK_EQ(count(), 10U);
    CY_CHECK(server.world.has(entities[0], replicator.identity_component()));
    CY_CHECK_FALSE(replicator.replicate(entities[0], 7, cy::net::PeerId{}).has_value());
}

CY_TEST_CASE("replication: a thousand entities reach a client world from the chunk columns") {
    Schemas schemas;
    GameWorld server;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set, every_tick());
    CY_REQUIRE(replicator.initialize().has_value());
    for (u32 index = 0; index < 1000; ++index) {
        const u32 row = index / 100;
        const cy::ecs::Entity entity =
            server.spawn(static_cast<float>(index % 100) * 3.5F, static_cast<float>(row),
                         static_cast<u16>(index % 4));
        CY_REQUIRE(replicator.replicate(entity, 1 + (index % 3), kAlice).has_value());
    }
    CY_REQUIRE(replicator.add_peer(kAlice).has_value());
    Client alice(schemas.set);

    const auto report = deliver(replicator, kAlice, alice, 1, unlimited());
    CY_TEST_MESSAGE("a full baseline of 1000 entities: " << report.bytes << " bytes, "
                                                         << report.spawns << " spawns, "
                                                         << report.updates << " updates");
    CY_CHECK_EQ(report.spawns, 1000U);
    CY_CHECK_EQ(report.updates, 1000U);
    CY_CHECK_EQ(alice.replica.entity_count(), 1000U);
    CY_CHECK_EQ(cy::net::replicated_digest(alice.game.world, schemas.set),
                cy::net::replicated_digest(server.world, schemas.set));
    // The client's world holds the component as the client registered it, and the owner's own
    // field reached the owner.
    const cy::net::NetworkId first = cy::net::NetworkId::make(0, 1);
    const cy::ecs::Entity mirrored = alice.replica.entity_of(first);
    CY_REQUIRE(mirrored.valid());
    CY_CHECK(alice.game.world.has(mirrored, alice.game.body));
    CY_CHECK_FALSE(alice.game.world.has(mirrored, alice.game.local));
}

CY_TEST_CASE("replication: an unchanged world sends nothing and compares nothing") {
    Schemas schemas;
    GameWorld server;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set, every_tick());
    CY_REQUIRE(replicator.initialize().has_value());
    cy::Array<cy::ecs::Entity> entities(allocator());
    for (u32 index = 0; index < 1000; ++index) {
        const cy::ecs::Entity entity = server.spawn(static_cast<float>(index), 1.0F);
        CY_REQUIRE(replicator.replicate(entity, 1, cy::net::PeerId{}).has_value());
        CY_REQUIRE(entities.push_back(entity).has_value());
    }
    CY_REQUIRE(replicator.add_peer(kAlice).has_value());
    Client alice(schemas.set);
    server.world.advance_version();
    const auto full = deliver(replicator, kAlice, alice, 1, unlimited());
    server.world.advance_version();
    const auto confirm = deliver(replicator, kAlice, alice, 2, unlimited());
    (void)confirm;

    // Nothing written since the acknowledged baseline: every chunk is older than it.
    server.world.advance_version();
    const auto idle = deliver(replicator, kAlice, alice, 3, unlimited());
    CY_CHECK_EQ(idle.updates, 0U);
    CY_CHECK_EQ(idle.entities_compared, 0U);
    CY_CHECK_EQ(idle.skipped_by_version, 1000U);
    CY_TEST_MESSAGE("idle snapshot: " << idle.bytes << " bytes");
    CY_CHECK_LE(idle.bytes, 13U);  // the header and the terminator

    // One entity's health. Its chunk is written, so its chunk is compared — and only the one field
    // of the one entity is sent.
    server.world.advance_version();
    server.body_of(entities[500])->health = 42;
    const auto one = deliver(replicator, kAlice, alice, 4, unlimited());
    CY_TEST_MESSAGE("one field of one entity: " << one.bytes << " bytes against a " << full.bytes
                                                << "-byte baseline; " << one.entities_compared
                                                << " compared");
    CY_CHECK_EQ(one.updates, 1U);
    CY_CHECK_GT(one.entities_compared, 0U);
    CY_CHECK_LT(one.entities_compared, 1000U);
    CY_CHECK_LE(one.bytes, 28U);
    CY_CHECK_LT(one.bytes * 500U, full.bytes);
    CY_CHECK_EQ(cy::net::replicated_digest(alice.game.world, schemas.set),
                cy::net::replicated_digest(server.world, schemas.set));
}

CY_TEST_CASE("replication: a change in a lost snapshot is sent again, because acks are exact") {
    // The default band table, deliberately: an entity sent at tick 2 is not due again until tick 5,
    // so snapshot 3 carries nothing for it — and is acknowledged. A cumulative acknowledgement of 3
    // would promote what the lost snapshot 2 carried, the entity would compare equal to the
    // promoted baseline from then on, and the client would keep the old value for good.
    Schemas schemas;
    GameWorld server;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set);
    CY_REQUIRE(replicator.initialize().has_value());
    const cy::ecs::Entity moving = server.spawn(1.0F, 1.0F);
    CY_REQUIRE(replicator.replicate(moving, 1, cy::net::PeerId{}).has_value());
    CY_REQUIRE(replicator.add_peer(kAlice).has_value());
    Client alice(schemas.set);
    server.world.advance_version();
    CY_REQUIRE_EQ(deliver(replicator, kAlice, alice, 1, unlimited()).spawns, 1U);

    server.world.advance_version();
    server.body_of(moving)->health = 7;
    CY_CHECK_EQ(deliver(replicator, kAlice, alice, 4, unlimited(), /*lose=*/true).updates, 1U);
    server.world.advance_version();
    const auto between = deliver(replicator, kAlice, alice, 5, unlimited());
    CY_CHECK_EQ(between.updates, 0U);  // not due: sent one tick ago
    u32 resent = 0;
    for (u64 tick = 6; tick <= 12; ++tick) {
        server.world.advance_version();
        resent += deliver(replicator, kAlice, alice, tick, unlimited()).updates;
    }
    CY_CHECK_EQ(resent, 1U);
    const cy::ecs::Entity mirrored = alice.replica.entity_of(cy::net::NetworkId::make(0, 1));
    CY_REQUIRE(mirrored.valid());
    CY_CHECK_EQ(alice.game.read(mirrored)->health, 7U);
    CY_CHECK_EQ(cy::net::replicated_digest(alice.game.world, schemas.set),
                cy::net::replicated_digest(server.world, schemas.set));
}

CY_TEST_CASE("replication: a spawn and a despawn repeat until a snapshot carrying them is acked") {
    Schemas schemas;
    GameWorld server;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set, every_tick());
    CY_REQUIRE(replicator.initialize().has_value());
    const cy::ecs::Entity entity = server.spawn(5.0F, 5.0F);
    CY_REQUIRE(replicator.replicate(entity, 3, cy::net::PeerId{}).has_value());
    CY_REQUIRE(replicator.add_peer(kAlice).has_value());
    Client alice(schemas.set);

    // Two lost snapshots: the spawn goes out in both, and again in the third, which arrives.
    CY_CHECK_EQ(deliver(replicator, kAlice, alice, 1, unlimited(), true).spawns, 1U);
    CY_CHECK_EQ(deliver(replicator, kAlice, alice, 2, unlimited(), true).spawns, 1U);
    CY_CHECK_EQ(deliver(replicator, kAlice, alice, 3, unlimited()).spawns, 1U);
    CY_CHECK_EQ(alice.replica.entity_count(), 1U);
    CY_CHECK_EQ(deliver(replicator, kAlice, alice, 4, unlimited()).spawns, 0U);

    // The server destroys it; the despawn repeats through a loss and stops once acknowledged.
    CY_REQUIRE(server.world.destroy(entity).has_value());
    CY_CHECK_EQ(deliver(replicator, kAlice, alice, 5, unlimited(), true).despawns, 1U);
    CY_CHECK_EQ(alice.replica.entity_count(), 1U);
    CY_CHECK_EQ(deliver(replicator, kAlice, alice, 6, unlimited()).despawns, 1U);
    CY_CHECK_EQ(alice.replica.entity_count(), 0U);
    CY_CHECK_EQ(alice.game.world.entity_count(), 0U);
    CY_CHECK_EQ(deliver(replicator, kAlice, alice, 7, unlimited()).despawns, 0U);
}

CY_TEST_CASE("replication: an older snapshot arriving late is not applied over a newer one") {
    Schemas schemas;
    GameWorld server;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set, every_tick());
    CY_REQUIRE(replicator.initialize().has_value());
    const cy::ecs::Entity entity = server.spawn(5.0F, 5.0F);
    CY_REQUIRE(replicator.replicate(entity, 3, cy::net::PeerId{}).has_value());
    CY_REQUIRE(replicator.add_peer(kAlice).has_value());
    Client alice(schemas.set);

    server.world.advance_version();
    server.body_of(entity)->health = 50;
    CY_REQUIRE(replicator.gather(1).has_value());
    cy::Array<u8> older(allocator());
    CY_REQUIRE(older.append(*replicator.build(kAlice, 1, unlimited())).has_value());
    server.world.advance_version();
    server.body_of(entity)->health = 60;
    CY_REQUIRE(replicator.gather(2).has_value());
    cy::Array<u8> newer(allocator());
    CY_REQUIRE(newer.append(*replicator.build(kAlice, 2, unlimited())).has_value());

    CY_REQUIRE(alice.replica.apply(newer.span()).has_value());
    CY_REQUIRE(alice.replica.apply(older.span()).has_value());
    CY_CHECK_EQ(alice.replica.stale_dropped(), 1U);
    const cy::ecs::Entity mirrored = alice.replica.entity_of(cy::net::NetworkId::make(0, 1));
    CY_CHECK_EQ(alice.game.read(mirrored)->health, 60U);
    // Only the newer one is acknowledged; the older one was never applied.
    CY_CHECK_EQ(alice.replica.newest(), 2U);
    CY_CHECK_EQ(alice.replica.previous_mask(), 0U);
}

CY_TEST_CASE("replication: the owner's own field reaches the owner and nobody else") {
    Schemas schemas;
    GameWorld server;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set, every_tick());
    CY_REQUIRE(replicator.initialize().has_value());
    const cy::ecs::Entity alices = server.spawn(1.0F, 1.0F);
    server.body_of(alices)->orders = 0xA11CE;
    CY_REQUIRE(replicator.replicate(alices, 1, kAlice).has_value());
    CY_REQUIRE(replicator.add_peer(kAlice).has_value());
    CY_REQUIRE(replicator.add_peer(kBob).has_value());
    Client alice(schemas.set);
    Client bob(schemas.set);
    (void)deliver(replicator, kAlice, alice, 1, unlimited());
    (void)deliver(replicator, kBob, bob, 1, unlimited());
    const cy::net::NetworkId id = cy::net::NetworkId::make(0, 1);
    CY_CHECK_EQ(alice.game.read(alice.replica.entity_of(id))->orders, 0xA11CEU);
    CY_CHECK_EQ(bob.game.read(bob.replica.entity_of(id))->orders, 0U);
    // What "the same world" means excludes what is private by design: both digests agree.
    CY_CHECK_EQ(cy::net::replicated_digest(alice.game.world, schemas.set),
                cy::net::replicated_digest(bob.game.world, schemas.set));
}

namespace {

void position_subject(void* user, const cy::ecs::World& world, cy::ecs::Entity entity,
                      cy::net::RelevanceSubject& subject) noexcept {
    const auto component = *static_cast<const cy::ecs::ComponentTypeId*>(user);
    const Body* body = world.get<Body>(entity, component);
    subject.position_x = static_cast<cy::i64>(body->position[0]);
    subject.position_y = static_cast<cy::i64>(body->position[1]);
    subject.cell = static_cast<cy::net::CellId>(body->position[0] / 100.0F);
}

}  // namespace

CY_TEST_CASE("replication: each peer is sent what is relevant to it, and told when it leaves") {
    Schemas schemas;
    GameWorld server;
    cy::net::ReplicatorConfig config = every_tick();
    config.subject = &position_subject;
    config.subject_user = &server.body;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set, config);
    CY_REQUIRE(replicator.initialize().has_value());
    // Forty entities along a line from 0 to 780.
    cy::ecs::Entity travelling;
    for (u32 index = 0; index < 40; ++index) {
        const cy::ecs::Entity entity = server.spawn(static_cast<float>(index) * 20.0F, 0.0F);
        CY_REQUIRE(replicator.replicate(entity, 1, cy::net::PeerId{}).has_value());
        if (index == 2) {
            travelling = entity;
        }
    }
    CY_REQUIRE(replicator.add_peer(kAlice).has_value());
    CY_REQUIRE(replicator.add_peer(kBob).has_value());
    cy::net::PeerInterest near_origin;
    near_origin.relevance_distance_squared = cy::i64{150} * 150;
    const cy::net::CellId alice_cells[] = {0, 1};
    CY_REQUIRE(replicator.set_interest(kAlice, near_origin, alice_cells).has_value());
    cy::net::PeerInterest far_end;
    far_end.viewpoint_x = 700;
    far_end.relevance_distance_squared = cy::i64{150} * 150;
    const cy::net::CellId bob_cells[] = {6, 7};
    CY_REQUIRE(replicator.set_interest(kBob, far_end, bob_cells).has_value());
    Client alice(schemas.set);
    Client bob(schemas.set);

    (void)deliver(replicator, kAlice, alice, 1, unlimited());
    (void)deliver(replicator, kBob, bob, 1, unlimited());
    CY_TEST_MESSAGE("alice sees " << alice.replica.entity_count() << ", bob sees "
                                  << bob.replica.entity_count() << " of 40");
    CY_CHECK_GT(alice.replica.entity_count(), 0U);
    CY_CHECK_LT(alice.replica.entity_count(), 40U);
    CY_CHECK_GT(bob.replica.entity_count(), 0U);
    // Nothing near the far end reaches the peer at the origin.
    CY_CHECK_FALSE(alice.replica.entity_of(cy::net::NetworkId::make(0, 40)).valid());
    CY_CHECK(bob.replica.entity_of(cy::net::NetworkId::make(0, 36)).valid());

    // One entity travels to the far end: it leaves Alice's world and enters Bob's.
    const cy::net::NetworkId travelling_id = cy::net::NetworkId::make(0, 3);
    CY_REQUIRE(alice.replica.entity_of(travelling_id).valid());
    server.world.advance_version();
    server.body_of(travelling)->position[0] = 710.0F;
    (void)deliver(replicator, kAlice, alice, 2, unlimited());
    (void)deliver(replicator, kBob, bob, 2, unlimited());
    CY_CHECK_FALSE(alice.replica.entity_of(travelling_id).valid());
    CY_CHECK(bob.replica.entity_of(travelling_id).valid());
}

CY_TEST_CASE("replication: the bandwidth budget holds with two thousand moving entities") {
    Schemas schemas;
    GameWorld server;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set);
    CY_REQUIRE(replicator.initialize().has_value());
    cy::Array<cy::ecs::Entity> entities(allocator());
    for (u32 index = 0; index < 2000; ++index) {
        const u32 row = index / 200;
        const cy::ecs::Entity entity =
            server.spawn(static_cast<float>(index % 200), static_cast<float>(row));
        CY_REQUIRE(replicator.replicate(entity, 1, cy::net::PeerId{}).has_value());
        CY_REQUIRE(entities.push_back(entity).has_value());
    }
    CY_REQUIRE(replicator.add_peer(kAlice).has_value());
    Client alice(schemas.set);
    cy::net::BandwidthBudget budget;
    budget.bytes_per_tick = 2000;

    u32 peak = 0;
    for (u64 tick = 1; tick <= 120; ++tick) {
        server.world.advance_version();
        for (const cy::ecs::Entity entity : entities) {
            server.body_of(entity)->position[2] = static_cast<float>(tick) * 0.25F;
        }
        const auto report = deliver(replicator, kAlice, alice, tick, budget);
        CY_CHECK_LE(report.bytes, budget.bytes_per_tick);
        peak = report.bytes > peak ? report.bytes : peak;
    }
    CY_TEST_MESSAGE("peak " << peak << " of " << budget.bytes_per_tick << " bytes per tick");
    CY_CHECK_GT(peak, budget.bytes_per_tick / 2);
    // Every entity reached the client despite the budget: deferral, not starvation.
    CY_CHECK_EQ(alice.replica.entity_count(), 2000U);
    // Once the world stops moving, what was deferred drains and the client converges.
    for (u64 tick = 121; tick <= 400; ++tick) {
        server.world.advance_version();
        (void)deliver(replicator, kAlice, alice, tick, budget);
    }
    CY_CHECK_EQ(cy::net::replicated_digest(alice.game.world, schemas.set),
                cy::net::replicated_digest(server.world, schemas.set));
}

CY_TEST_CASE("replication: an exact acknowledgement promotes only what its snapshot carried") {
    // `PeerBaseline` on its own: the regression behind `acknowledge_exactly`.
    SchemaSet set(allocator());
    CY_REQUIRE(set.add(sample_type(), sample_schema()).has_value());
    cy::net::PeerBaseline cumulative(allocator(), set);
    cy::net::PeerBaseline exact(allocator(), set);
    const cy::net::NetworkId id = cy::net::NetworkId::make(1, 1);
    SampleTransform value;
    value.health = 10;
    for (cy::net::PeerBaseline* baseline : {&cumulative, &exact}) {
        CY_REQUIRE(baseline->record_sent(id, 0, 11, &value, sizeof(value)).has_value());
    }
    // Snapshot 11 was lost; snapshot 12 — which did not carry this entity — was acknowledged.
    cumulative.acknowledge(12);
    exact.acknowledge_exactly(12);
    CY_CHECK(cumulative.acknowledged(id, 0) != nullptr);  // believes 11 arrived: wrong
    CY_CHECK(exact.acknowledged(id, 0) == nullptr);       // knows it did not
    exact.acknowledge_exactly(11);
    CY_CHECK(exact.acknowledged(id, 0) != nullptr);
}

CY_TEST_CASE("replication: despawns and updates share one budget") {
    // The scheduler plans updates against the budget and knows nothing of despawns, which the
    // replicator writes first. So when a hundred and fifty entities die in one tick while the rest
    // keep moving, the plan alone would overrun the budget by the despawns' size; the replicator
    // checks every record against what is left and defers the updates that no longer fit.
    Schemas schemas;
    GameWorld server;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set, every_tick());
    CY_REQUIRE(replicator.initialize().has_value());
    cy::Array<cy::ecs::Entity> entities(allocator());
    for (u32 index = 0; index < 400; ++index) {
        const cy::ecs::Entity entity = server.spawn(static_cast<float>(index), 0.0F);
        CY_REQUIRE(replicator.replicate(entity, 1, cy::net::PeerId{}).has_value());
        CY_REQUIRE(entities.push_back(entity).has_value());
    }
    CY_REQUIRE(replicator.add_peer(kAlice).has_value());
    Client alice(schemas.set);
    cy::net::BandwidthBudget budget;
    budget.bytes_per_tick = 1500;
    u32 deferred = 0;
    u32 despawns = 0;
    for (u64 tick = 1; tick <= 80; ++tick) {
        server.world.advance_version();
        if (tick == 60) {
            for (u32 index = 0; index < 150; ++index) {
                CY_REQUIRE(server.world.destroy(entities[index]).has_value());
            }
        }
        for (u32 index = tick >= 60 ? 150U : 0U; index < entities.size(); ++index) {
            server.body_of(entities[index])->position[1] = static_cast<float>(tick);
        }
        const auto report = deliver(replicator, kAlice, alice, tick, budget);
        CY_CHECK_LE(report.bytes, budget.bytes_per_tick);
        despawns += report.despawns;
        deferred += tick >= 60 ? report.deferred : 0U;
    }
    CY_TEST_MESSAGE("despawns " << despawns << ", updates deferred for them " << deferred);
    CY_CHECK_EQ(despawns, 150U);
    CY_CHECK_GT(deferred, 0U);
    CY_CHECK_EQ(alice.replica.entity_count(), 250U);
}

CY_TEST_CASE("replication: an entity that dies before any peer saw it leaves the relevance index") {
    Schemas schemas;
    GameWorld server;
    cy::net::ReplicatorConfig config = every_tick();
    config.subject = &position_subject;
    config.subject_user = &server.body;
    cy::net::WorldReplicator replicator(allocator(), server.world, schemas.set, config);
    CY_REQUIRE(replicator.initialize().has_value());
    cy::Array<cy::ecs::Entity> entities(allocator());
    for (u32 index = 0; index < 10; ++index) {
        const cy::ecs::Entity entity = server.spawn(static_cast<float>(index), 0.0F);
        CY_REQUIRE(replicator.replicate(entity, 1, cy::net::PeerId{}).has_value());
        CY_REQUIRE(entities.push_back(entity).has_value());
    }
    CY_REQUIRE(replicator.gather(1).has_value());
    CY_CHECK_EQ(replicator.relevance_subjects(), 10U);
    for (u32 index = 0; index < 5; ++index) {
        CY_REQUIRE(server.world.destroy(entities[index]).has_value());
    }
    // No peer has joined, so no peer's knowledge can name the dead: the index has to.
    CY_REQUIRE(replicator.gather(2).has_value());
    CY_CHECK_EQ(replicator.replicated_count(), 5U);
    CY_CHECK_EQ(replicator.relevance_subjects(), 5U);
}
