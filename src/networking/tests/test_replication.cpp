// M9 TASK 4.3 — BASELINES, DELTAS, AND THE LOSS THAT MAKES THE DIFFERENCE VISIBLE.
//
// The case worth reading is "a delta is against what the peer acknowledged". A system that deltas
// against the last *send* passes every test where nothing is lost and produces silent garbage the
// first time a packet goes missing — there is no error anywhere, just a field that is wrong. So the
// case loses a snapshot on purpose and checks that the next delta is computed against the older,
// acknowledged state.
//
// `integration`, because every case builds a peer's baseline over a population.

#include "fixture.h"

#include <cy/networking/replication.h>

#include <cstring>

using namespace cy::net_test;
using cy::u32;
using cy::u64;
using cy::u8;

namespace {

struct Applied {
    NetworkId id;
    u32 schema = 0;
    ChangeMask mask = 0;
    SampleTransform value;
};

struct Sink {
    cy::Array<Applied> applied;

    explicit Sink(cy::Allocator& allocator) noexcept : applied(allocator) {}
};

cy::Status collect(void* user, NetworkId id, u32 schema, cy::Span<const u8> decoded,
                   ChangeMask mask) noexcept {
    auto* sink = static_cast<Sink*>(user);
    Applied one;
    one.id = id;
    one.schema = schema;
    one.mask = mask;
    std::memcpy(static_cast<void*>(&one.value), static_cast<const void*>(decoded.data()),
                sizeof(SampleTransform));
    return sink->applied.push_back(one);
}

[[nodiscard]] SampleTransform at(float x) noexcept {
    SampleTransform value;
    value.position[0] = x;
    return value;
}

}  // namespace

CY_TEST_CASE("networking: a first update is a full baseline and the next is a delta") {
    SchemaSet schemas(allocator());
    CY_REQUIRE(schemas.add(sample_type(), sample_schema()).has_value());
    PeerBaseline baseline(allocator(), schemas);
    SnapshotWriter writer(allocator(), schemas);
    NetworkIdMinter minter(1);
    const NetworkId id = minter.mint();

    SampleTransform value = at(10.0F);
    CY_REQUIRE(writer.open(1, /*tick=*/100, /*full_baseline=*/false).has_value());
    const auto first = writer.add_update(id, 0, &value, /*owner=*/true, baseline);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(writer.close().has_value());
    // Nothing acknowledged, so every field is present.
    const u32 full_bits = first.value();
    CY_CHECK_GT(full_bits, 100U);

    baseline.note_sent(1);
    baseline.acknowledge(1);

    value.health = 55;
    CY_REQUIRE(writer.open(2, 101, false).has_value());
    const auto second = writer.add_update(id, 0, &value, true, baseline);
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(writer.close().has_value());
    // One field changed, so the delta is the tag, the id, the schema, the mask and ten bits.
    CY_CHECK_LT(second.value(), full_bits);
    CY_CHECK_EQ(second.value(), 2U + 64U + 8U + 4U + 10U);

    // And an unchanged instance costs nothing at all.
    baseline.note_sent(2);
    baseline.acknowledge(2);
    CY_REQUIRE(writer.open(3, 102, false).has_value());
    const auto third = writer.add_update(id, 0, &value, true, baseline);
    CY_REQUIRE(third.has_value());
    CY_CHECK_EQ(third.value(), 0U);
    CY_CHECK_EQ(writer.cost().components, 0U);
}

CY_TEST_CASE("networking: a delta is against what the peer acknowledged, not the last send") {
    SchemaSet schemas(allocator());
    CY_REQUIRE(schemas.add(sample_type(), sample_schema()).has_value());
    PeerBaseline baseline(allocator(), schemas);
    SnapshotWriter writer(allocator(), schemas);
    SnapshotReader reader(allocator(), schemas);
    NetworkIdMinter minter(1);
    const NetworkId id = minter.mint();

    // Snapshot 1: the full state, acknowledged.
    SampleTransform value = at(1.0F);
    CY_REQUIRE(writer.open(1, 1, false).has_value());
    CY_REQUIRE(writer.add_update(id, 0, &value, true, baseline).has_value());
    CY_REQUIRE(writer.close().has_value());
    baseline.note_sent(1);
    baseline.acknowledge(1);

    // Snapshot 2: health moves. THIS SNAPSHOT IS LOST — never acknowledged.
    value.health = 70;
    CY_REQUIRE(writer.open(2, 2, false).has_value());
    CY_REQUIRE(writer.add_update(id, 0, &value, true, baseline).has_value());
    CY_REQUIRE(writer.close().has_value());
    baseline.note_sent(2);

    // Snapshot 3: position moves too. A system deltaing against the last SEND would send only the
    // position, and the client — which never received snapshot 2 — would keep the old health for
    // ever, with no error anywhere.
    value.position[0] = 2.0F;
    CY_REQUIRE(writer.open(3, 3, false).has_value());
    CY_REQUIRE(writer.add_update(id, 0, &value, true, baseline).has_value());
    CY_REQUIRE(writer.close().has_value());

    Sink sink(allocator());
    SnapshotHeader header;
    cy::Array<SpawnEvent> spawns(allocator());
    cy::Array<NetworkId> despawns(allocator());
    CY_REQUIRE(reader.read(writer.bytes(), header, spawns, despawns, collect, &sink).has_value());
    CY_REQUIRE_EQ(sink.applied.size(), cy::usize{1});

    // Both fields are present, because both differ from what the peer acknowledged.
    CY_CHECK_NE(sink.applied[0].mask & (ChangeMask{1} << 0), ChangeMask{0});
    CY_CHECK_NE(sink.applied[0].mask & (ChangeMask{1} << 2), ChangeMask{0});
    CY_CHECK_EQ(sink.applied[0].value.health, cy::u16{70});
    CY_CHECK_NEAR(sink.applied[0].value.position[0], 2.0F, 0.01F);
    CY_CHECK_EQ(header.tick, u64{3});
    CY_CHECK_EQ(header.updates, 1U);
}

CY_TEST_CASE("networking: an acknowledgement older than the history asks for a fresh baseline") {
    SchemaSet schemas(allocator());
    CY_REQUIRE(schemas.add(sample_type(), sample_schema()).has_value());
    PeerBaseline baseline(allocator(), schemas);

    // Nothing acknowledged at all — a late joiner, by definition.
    CY_CHECK(baseline.needs_baseline());

    baseline.note_sent(1);
    baseline.acknowledge(1);
    CY_CHECK_FALSE(baseline.needs_baseline());

    baseline.note_sent(1 + kSnapshotHistory);
    CY_CHECK_FALSE(baseline.needs_baseline());
    baseline.note_sent(2 + kSnapshotHistory);
    // Past the retained history: the next update must be a full baseline rather than an
    // undecodable delta.
    CY_CHECK(baseline.needs_baseline());
}

CY_TEST_CASE("networking: spawns, despawns and a late joiner's baseline") {
    SchemaSet schemas(allocator());
    CY_REQUIRE(schemas.add(sample_type(), sample_schema()).has_value());
    PeerBaseline baseline(allocator(), schemas);
    SnapshotWriter writer(allocator(), schemas);
    SnapshotReader reader(allocator(), schemas);
    NetworkIdMinter minter(3);

    SpawnEvent spawn;
    spawn.id = minter.mint();
    spawn.prefab = 77;
    spawn.owner = PeerId::make(2, 1);
    const NetworkId leaving = minter.mint();

    SampleTransform value = at(5.0F);
    CY_REQUIRE(writer.open(9, 42, /*full_baseline=*/true).has_value());
    CY_REQUIRE(writer.add_spawn(spawn).has_value());
    CY_REQUIRE(writer.add_despawn(leaving).has_value());
    CY_REQUIRE(writer.add_update(spawn.id, 0, &value, false, baseline).has_value());
    CY_REQUIRE(writer.close().has_value());

    Sink sink(allocator());
    SnapshotHeader header;
    cy::Array<SpawnEvent> spawns(allocator());
    cy::Array<NetworkId> despawns(allocator());
    CY_REQUIRE(reader.read(writer.bytes(), header, spawns, despawns, collect, &sink).has_value());

    CY_CHECK(header.full_baseline);
    CY_CHECK_EQ(header.snapshot, 9U);
    CY_REQUIRE_EQ(spawns.size(), cy::usize{1});
    CY_CHECK_EQ(spawns[0].id.value(), spawn.id.value());
    CY_CHECK_EQ(spawns[0].prefab, 77U);
    CY_CHECK(spawns[0].owner == spawn.owner);
    CY_REQUIRE_EQ(despawns.size(), cy::usize{1});
    CY_CHECK_EQ(despawns[0].value(), leaving.value());

    // Owner-only fields are absent for a non-owner even in a full baseline: "so other clients never
    // receive it" is about the peer, not about the kind of update.
    CY_REQUIRE_EQ(sink.applied.size(), cy::usize{1});
    CY_CHECK_EQ(sink.applied[0].mask & (ChangeMask{1} << 3), ChangeMask{0});
}

CY_TEST_CASE("networking: a snapshot carries one tick, and the reader sees which") {
    SchemaSet schemas(allocator());
    CY_REQUIRE(schemas.add(sample_type(), sample_schema()).has_value());
    PeerBaseline baseline(allocator(), schemas);
    SnapshotWriter writer(allocator(), schemas);

    // There is no way to add an instance sampled at another tick, because the writer never asks for
    // one: the tick is `open()`'s and every `add_*` is against it.
    CY_REQUIRE(writer.open(4, /*tick=*/999, false).has_value());
    CY_CHECK_EQ(writer.tick(), u64{999});
    SampleTransform value = at(1.0F);
    CY_REQUIRE(writer.add_update(NetworkId::make(1, 1), 0, &value, true, baseline).has_value());
    CY_REQUIRE(writer.close().has_value());

    // And nothing may be added after it is closed.
    CY_CHECK_FALSE(writer.add_despawn(NetworkId::make(1, 2)).has_value());
    CY_CHECK_FALSE(writer.add_update(NetworkId::make(1, 3), 0, &value, true, baseline).has_value());
}

CY_TEST_CASE("networking: a truncated or unknown snapshot is refused rather than misread") {
    SchemaSet schemas(allocator());
    CY_REQUIRE(schemas.add(sample_type(), sample_schema()).has_value());
    PeerBaseline baseline(allocator(), schemas);
    SnapshotWriter writer(allocator(), schemas);
    SnapshotReader reader(allocator(), schemas);

    SampleTransform value = at(3.0F);
    CY_REQUIRE(writer.open(1, 1, true).has_value());
    CY_REQUIRE(writer.add_update(NetworkId::make(1, 1), 0, &value, true, baseline).has_value());
    CY_REQUIRE(writer.close().has_value());

    cy::Array<u8> truncated(allocator());
    CY_REQUIRE(truncated.append(writer.bytes()).has_value());
    CY_REQUIRE(truncated.resize(truncated.size() / 2).has_value());

    Sink sink(allocator());
    SnapshotHeader header;
    cy::Array<SpawnEvent> spawns(allocator());
    cy::Array<NetworkId> despawns(allocator());
    CY_CHECK_FALSE(
        reader.read(truncated.span(), header, spawns, despawns, collect, &sink).has_value());

    const u8 nothing[2] = {0, 0};
    CY_CHECK_FALSE(
        reader.read(cy::Span<const u8>(nothing, 2), header, spawns, despawns, collect, &sink)
            .has_value());
}

CY_TEST_CASE("networking: an unresolved reference resolves when its target arrives") {
    ReferenceResolver resolver(allocator());
    NetworkIdMinter minter(1);
    const NetworkId holder = minter.mint();
    const NetworkId target = minter.mint();

    // "the reference SHALL resolve to null and re-resolve when the target arrives, rather than
    // failing" — a dangling reference is the normal case when two spawns are two datagrams.
    CY_CHECK_FALSE(resolver.resolve(target).valid());
    CY_CHECK_EQ(resolver.unresolved_lookups(), u64{1});
    CY_REQUIRE(resolver.defer(holder, /*field_offset=*/12, target).has_value());
    CY_CHECK_EQ(resolver.deferred(), 1U);

    CY_REQUIRE(resolver.bind(target, cy::ecs::Entity::make(41, 2)).has_value());
    CY_CHECK(resolver.resolve(target).valid());
    CY_CHECK_EQ(resolver.resolve(target).index(), 41U);

    cy::Array<u32> offsets(allocator());
    cy::Array<NetworkId> holders(allocator());
    CY_CHECK_EQ(resolver.resolve_pending(target, offsets, holders), 1U);
    CY_REQUIRE_EQ(offsets.size(), cy::usize{1});
    CY_CHECK_EQ(offsets[0], 12U);
    CY_CHECK_EQ(holders[0].value(), holder.value());
    CY_CHECK_EQ(resolver.deferred(), 0U);

    resolver.unbind(target);
    CY_CHECK_FALSE(resolver.resolve(target).valid());
}
