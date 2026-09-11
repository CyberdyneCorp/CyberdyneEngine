// M9 TASK 2.4 — generated state codecs: one compiled plan per subject per purpose.
//
// THE CASE THAT MATTERS IS THE SECOND ONE. "Generated" means the hot path consults no metadata, and
// the only honest way to check that is to take the metadata away: the codec is compiled, the
// `StateSchema` it was compiled from is **destroyed**, and then the codec packs, unpacks and
// hashes. A codec that kept a pointer into the schema's arrays would read freed memory; one that
// re-walked reflection per field per tick could not run at all.

#include <cy/core/determinism/codec.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/values/name.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>

namespace {

using namespace cy;
using namespace cy::determinism;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr SchemaSubject kRobot{7};

/// Four fields across three classes, chosen so the four purposes select four different sets.
struct Robot {
    f32 health = 0.0F;        // Authoritative: hashed, rolled back, checkpointed, not saved
    u32 charge = 0;           // Persistent:    all four
    f32 muzzle_flash = 0.0F;  // Presentation:  none
    i32 prediction = 0;       // Predicted:     rolled back and checkpointed, never hashed
};

[[nodiscard]] Status declare_robot(StateSchema& schema) noexcept {
    const StateField fields[] = {
        StateField{"health", 1, static_cast<u32>(offsetof(Robot, health)), reflect::FieldKind::F32,
                   SimulationClass::Authoritative},
        StateField{"charge", 2, static_cast<u32>(offsetof(Robot, charge)), reflect::FieldKind::U32,
                   SimulationClass::Persistent},
        StateField{"muzzle_flash", 3, static_cast<u32>(offsetof(Robot, muzzle_flash)),
                   reflect::FieldKind::F32, SimulationClass::Presentation},
        StateField{"prediction", 4, static_cast<u32>(offsetof(Robot, prediction)),
                   reflect::FieldKind::I32, SimulationClass::Predicted},
    };
    return schema.declare(kRobot, "Robot", Span<const StateField>(fields, 4));
}

}  // namespace

CY_TEST_CASE("determinism: a purpose selects its own field set, and the four differ") {
    StateSchema schema(allocator());
    CY_REQUIRE(declare_robot(schema).has_value());
    schema.freeze();

    struct Expectation {
        CodecPurpose purpose;
        u32 fields;
        u32 excluded;
        u32 row_bytes;
    };
    // Hash: health + charge (Presentation and Predicted are never hashed).
    // Rollback/Checkpoint: health + charge + prediction — a prediction is rolled back because
    //   reconciliation needs the predicted value to rewind with.
    // Save: charge alone.
    const Expectation expectations[] = {
        {CodecPurpose::Hash, 2, 2, 0},
        {CodecPurpose::RollbackPack, 3, 1, 12},
        {CodecPurpose::CheckpointPack, 3, 1, 12},
        {CodecPurpose::Save, 1, 3, 4},
    };
    for (const Expectation& expectation : expectations) {
        StateCodec codec(allocator());
        CY_REQUIRE(codec.compile(schema, kRobot, expectation.purpose).has_value());
        CY_CHECK_EQ(codec.instruction_count(), expectation.fields);
        CY_CHECK_EQ(codec.excluded_fields(), expectation.excluded);
        CY_CHECK_EQ(codec.packed_row_size(), expectation.row_bytes);
        CY_CHECK(std::strcmp(codec.subject_name(), "Robot") == 0);
    }
}

CY_TEST_CASE("determinism: a compiled codec outlives the schema it came from") {
    // THE CHECK THAT "GENERATED" MEANS SOMETHING. Everything below happens after the schema is
    // gone.
    StateCodec pack(allocator());
    StateCodec hash(allocator());
    {
        StateSchema schema(allocator());
        CY_REQUIRE(declare_robot(schema).has_value());
        schema.freeze();
        CY_REQUIRE(pack.compile(schema, kRobot, CodecPurpose::RollbackPack).has_value());
        CY_REQUIRE(hash.compile(schema, kRobot, CodecPurpose::Hash).has_value());
    }

    Robot rows[3] = {};
    for (u32 index = 0; index < 3; ++index) {
        rows[index].health = 10.0F + static_cast<f32>(index);
        rows[index].charge = 100U + index;
        rows[index].muzzle_flash = 999.0F;
        rows[index].prediction = -static_cast<i32>(index);
    }

    Array<u8> packed(allocator());
    CY_REQUIRE(pack.pack_rows(rows, sizeof(Robot), 3, packed).has_value());
    CY_CHECK_EQ(packed.size(), usize{3} * pack.packed_row_size());

    Robot restored[3] = {};
    for (Robot& row : restored) {
        row.muzzle_flash = 1.0F;  // a presentation field the codec must not touch
    }
    CY_REQUIRE(pack.unpack_rows(packed.span(), restored, sizeof(Robot), 3).has_value());
    for (u32 index = 0; index < 3; ++index) {
        CY_CHECK_EQ(restored[index].health, rows[index].health);
        CY_CHECK_EQ(restored[index].charge, rows[index].charge);
        CY_CHECK_EQ(restored[index].prediction, rows[index].prediction);
        // Excluded, so left exactly as it was. A codec that packed by `sizeof` would have copied
        // it.
        CY_CHECK_EQ(restored[index].muzzle_flash, 1.0F);
    }

    const u64 entities[] = {41, 42, 43};
    StateHashTree tree(allocator());
    CY_REQUIRE(tree.begin(HashLevel::Archetype, 1, "robots").has_value());
    CY_REQUIRE(
        hash.hash_rows(tree, rows, sizeof(Robot), 3, entities, HashDetail::Fields).has_value());
    CY_REQUIRE(tree.end().has_value());
    CY_CHECK_NE(tree.root_hash(), u64{0});
}

CY_TEST_CASE("determinism: the hash covers declared authoritative fields and nothing else") {
    StateSchema schema(allocator());
    CY_REQUIRE(declare_robot(schema).has_value());
    schema.freeze();
    StateCodec codec(allocator());
    CY_REQUIRE(codec.compile(schema, kRobot, CodecPurpose::Hash).has_value());

    auto hash_of = [&](const Robot* rows, u32 count, const u64* ids) noexcept {
        StateHashTree tree(allocator());
        CY_REQUIRE(tree.begin(HashLevel::Archetype, 1, "robots").has_value());
        CY_REQUIRE(
            codec.hash_rows(tree, rows, sizeof(Robot), count, ids, HashDetail::Fields).has_value());
        CY_REQUIRE(tree.end().has_value());
        return tree.root_hash();
    };

    Robot rows[2] = {};
    rows[0].health = 3.5F;
    rows[1].health = 4.5F;
    const u64 ids[] = {1, 2};
    const u64 base = hash_of(rows, 2, ids);

    // A presentation field moves: the hash does not.
    rows[0].muzzle_flash = 123.0F;
    CY_CHECK_EQ(hash_of(rows, 2, ids), base);
    // A predicted field moves: the hash does not. Two peers legitimately disagree about a
    // prediction, and hashing it would make correct prediction look like a divergence.
    rows[1].prediction = 77;
    CY_CHECK_EQ(hash_of(rows, 2, ids), base);
    // An authoritative field moves: the hash does.
    rows[0].health = 3.5001F;
    CY_CHECK_NE(hash_of(rows, 2, ids), base);

    // Identity is part of the hash: the same values under different identifiers differ.
    rows[0].health = 3.5F;
    const u64 other_ids[] = {1, 3};
    CY_CHECK_NE(hash_of(rows, 2, other_ids), base);
}

CY_TEST_CASE("determinism: -0.0 and 0.0 hash alike, because they compare alike") {
    StateSchema schema(allocator());
    CY_REQUIRE(declare_robot(schema).has_value());
    schema.freeze();
    StateCodec codec(allocator());
    CY_REQUIRE(codec.compile(schema, kRobot, CodecPurpose::Hash).has_value());

    auto hash_of = [&](f32 health) noexcept {
        Robot row;
        row.health = health;
        const u64 id = 5;
        StateHashTree tree(allocator());
        CY_REQUIRE(tree.begin(HashLevel::Archetype, 1, "robots").has_value());
        CY_REQUIRE(codec.hash_rows(tree, &row, sizeof(Robot), 1, &id, HashDetail::ComponentOnly)
                       .has_value());
        CY_REQUIRE(tree.end().has_value());
        return tree.root_hash();
    };
    CY_CHECK_EQ(hash_of(0.0F), hash_of(-0.0F));
}

CY_TEST_CASE("determinism: a codec refuses what it cannot honestly do") {
    StateSchema schema(allocator());
    CY_REQUIRE(declare_robot(schema).has_value());

    StateCodec codec(allocator());
    // Unfrozen: the field order is not yet fixed, so a codec compiled now would fold in whatever
    // order the declarations happened to arrive.
    CY_CHECK_FALSE(codec.compile(schema, kRobot, CodecPurpose::Hash).has_value());
    schema.freeze();
    CY_CHECK_FALSE(codec.compile(schema, SchemaSubject{99}, CodecPurpose::Hash).has_value());

    // An interned name cannot be packed for a save: its index is assigned in interning order and is
    // not stable across runs. Correct for a rollback ring in this process; meaningless in a file.
    struct Named {
        u32 name = 0;
    };
    const StateField named[] = {StateField{"node_name", 1, 0, reflect::FieldKind::U32,
                                           SimulationClass::Persistent,
                                           StateEncoding::InternedName}};
    StateSchema second(allocator());
    CY_REQUIRE(
        second.declare(SchemaSubject{8}, "Named", Span<const StateField>(named, 1)).has_value());
    second.freeze();
    StateCodec save(allocator());
    CY_CHECK_FALSE(save.compile(second, SchemaSubject{8}, CodecPurpose::Save).has_value());
    // ...and the same field packs perfectly well for rollback.
    StateCodec rollback(allocator());
    CY_CHECK(rollback.compile(second, SchemaSubject{8}, CodecPurpose::RollbackPack).has_value());

    // A short buffer is refused rather than partially applied: a half-restored world is worse than
    // an unrestored one, and it would hash as a divergence somewhere unrelated to the defect.
    Named rows[2] = {};
    Array<u8> packed(allocator());
    CY_REQUIRE(rollback.pack_rows(rows, sizeof(Named), 2, packed).has_value());
    CY_CHECK_FALSE(
        rollback.unpack_rows(Span<const u8>(packed.data(), 4), rows, sizeof(Named), 2).has_value());

    // A hashing codec packs nothing and a packing codec hashes nothing. Two mistakes that would
    // otherwise produce plausible-looking rubbish.
    StateCodec hash(allocator());
    CY_REQUIRE(hash.compile(schema, kRobot, CodecPurpose::Hash).has_value());
    Array<u8> nothing(allocator());
    Robot robots[1] = {};
    CY_CHECK_FALSE(hash.pack_rows(robots, sizeof(Robot), 1, nothing).has_value());
    StateHashTree tree(allocator());
    CY_REQUIRE(tree.begin(HashLevel::World, 0, "world").has_value());
    StateCodec packer(allocator());
    CY_REQUIRE(packer.compile(schema, kRobot, CodecPurpose::RollbackPack).has_value());
    CY_CHECK_FALSE(
        packer.hash_rows(tree, robots, sizeof(Robot), 1, nullptr, HashDetail::Fields).has_value());
}
