// Hierarchical state hashing, the schema that decides what is in it, and the narrowing.
// Tasks 4.2.5 and 4.2.6.

#include <cy/core/determinism/hash.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <string_view>

namespace {

using namespace cy;
using namespace cy::determinism;

/// A component-shaped struct with one field of each interesting class. Deliberately padded, so that
/// "raw memory is not the hash" is a claim the test can falsify.
struct Robot {
    f32 health = 100.0F;
    u8 team = 1;
    // Three bytes of padding here, containing whatever the allocator left.
    u32 charge = 7;
    f32 muzzle_flash = 0.0F;  // presentation
};

constexpr SchemaSubject kRobot{1};

Status declare_robot(StateSchema& schema) {
    const StateField fields[] = {
        StateField{"health", 1, offsetof(Robot, health), reflect::FieldKind::F32,
                   SimulationClass::Authoritative},
        StateField{"team", 2, offsetof(Robot, team), reflect::FieldKind::U8,
                   SimulationClass::Persistent},
        StateField{"charge", 3, offsetof(Robot, charge), reflect::FieldKind::U32,
                   SimulationClass::Authoritative},
        StateField{"muzzle_flash", 4, offsetof(Robot, muzzle_flash), reflect::FieldKind::F32,
                   SimulationClass::Presentation},
    };
    return schema.declare(kRobot, "Robot", Span<const StateField>(fields, 4));
}

/// Hash one robot into a world/entity/component/field tree, the shape the ECS walk produces.
Status hash_one(StateHashTree& tree, const StateSchema& schema, u64 entity, const Robot& robot) {
    const SubjectSchema* subject = schema.find(kRobot);
    CY_REQUIRE(subject != nullptr);
    if (Status entity_open = tree.begin(HashLevel::Entity, entity, "entity"); !entity_open) {
        return entity_open;
    }
    if (Status component_open = tree.begin(HashLevel::Component, kRobot.value, subject->name);
        !component_open) {
        return component_open;
    }
    for (const StateField& field : schema.fields_of(*subject)) {
        if (!participation_of(field.classification).hashed) {
            continue;
        }
        if (Status field_open = tree.begin(HashLevel::Field, field.id, field.name); !field_open) {
            return field_open;
        }
        hash_field(tree, field, &robot);
        if (Status field_closed = tree.end(); !field_closed) {
            return field_closed;
        }
    }
    if (Status component_closed = tree.end(); !component_closed) {
        return component_closed;
    }
    return tree.end();
}

}  // namespace

CY_TEST_CASE("the schema decides what is hashed, and refuses what it cannot read") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    StateSchema schema(allocator);
    CY_REQUIRE(static_cast<bool>(declare_robot(schema)));

    const SubjectSchema* robot = schema.find(kRobot);
    CY_REQUIRE(robot != nullptr);
    CY_CHECK_EQ(robot->field_count, 4U);
    // Three of the four: the presentation field is declared and does not participate.
    CY_CHECK_EQ(robot->hashed_field_count, 3U);

    // A second declaration of one subject is refused: two schemas for one component would make the
    // hash depend on which one a walk happened to find.
    CY_CHECK_FALSE(static_cast<bool>(declare_robot(schema)));

    const StateField duplicate_ids[] = {
        StateField{"a", 9, 0, reflect::FieldKind::U32, SimulationClass::Authoritative},
        StateField{"b", 9, 4, reflect::FieldKind::U32, SimulationClass::Authoritative},
    };
    CY_CHECK_FALSE(static_cast<bool>(
        schema.declare(SchemaSubject{2}, "Dup", Span<const StateField>(duplicate_ids, 2))));

    const StateField unreadable[] = {
        StateField{"x", 1, 0, reflect::FieldKind::Unsupported, SimulationClass::Authoritative},
    };
    CY_CHECK_FALSE(static_cast<bool>(
        schema.declare(SchemaSubject{3}, "Bad", Span<const StateField>(unreadable, 1))));

    // Reclassification moves a field in and out of the hash, and a typo is refused rather than
    // silently leaving the field where it was.
    CY_REQUIRE(static_cast<bool>(
        schema.override_classification(kRobot, 3, SimulationClass::Presentation)));
    CY_CHECK_EQ(schema.find(kRobot)->hashed_field_count, 2U);
    CY_CHECK_FALSE(
        static_cast<bool>(schema.override_classification(kRobot, 99, SimulationClass::Derived)));

    schema.freeze();
    CY_CHECK(schema.frozen());
    CY_CHECK_FALSE(static_cast<bool>(declare_robot(schema)));
}

CY_TEST_CASE("padding and presentation fields are not in the hash") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    StateSchema schema(allocator);
    CY_REQUIRE(static_cast<bool>(declare_robot(schema)));
    schema.freeze();

    Robot left;
    Robot right;
    // Fill the padding differently on each side. A hash of raw memory would differ here;
    // `simulation-and-determinism` lists that among the forbidden patterns, so this must not.
    auto* left_bytes = reinterpret_cast<u8*>(&left);
    auto* right_bytes = reinterpret_cast<u8*>(&right);
    left_bytes[offsetof(Robot, team) + 1] = 0xAB;
    right_bytes[offsetof(Robot, team) + 1] = 0x00;

    // And change a presentation field, which is declared and does not participate.
    right.muzzle_flash = 12.0F;

    StateHashTree a(allocator);
    StateHashTree b(allocator);
    CY_REQUIRE(static_cast<bool>(a.begin(HashLevel::World, 0, "w")));
    CY_REQUIRE(static_cast<bool>(hash_one(a, schema, 1, left)));
    CY_REQUIRE(static_cast<bool>(a.end()));
    CY_REQUIRE(static_cast<bool>(b.begin(HashLevel::World, 0, "w")));
    CY_REQUIRE(static_cast<bool>(hash_one(b, schema, 1, right)));
    CY_REQUIRE(static_cast<bool>(b.end()));

    CY_CHECK_EQ(a.root_hash(), b.root_hash());

    Divergence divergence;
    StateHashTree::compare(a, b, divergence);
    CY_CHECK_FALSE(divergence.diverged);
}

CY_TEST_CASE("negative zero hashes as zero") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    StateHashTree a(allocator);
    StateHashTree b(allocator);
    CY_REQUIRE(static_cast<bool>(a.begin(HashLevel::Field, 1, "x")));
    a.mix_f32(0.0F);
    CY_REQUIRE(static_cast<bool>(a.end()));
    CY_REQUIRE(static_cast<bool>(b.begin(HashLevel::Field, 1, "x")));
    b.mix_f32(-0.0F);
    CY_REQUIRE(static_cast<bool>(b.end()));
    CY_CHECK_EQ(a.root_hash(), b.root_hash());
}

CY_TEST_CASE("a divergence narrows to a named field on a named entity") {
    // `simulation-and-determinism`: "WHEN two runs disagree at a tick THEN descending the hash
    // hierarchy SHALL identify the entity, component, and field that differ."
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    StateSchema schema(allocator);
    CY_REQUIRE(static_cast<bool>(declare_robot(schema)));
    schema.freeze();

    Robot first;
    Robot second;
    Robot third;
    third.charge = 8;  // the one difference, on the third entity

    StateHashTree left(allocator);
    StateHashTree right(allocator);
    for (StateHashTree* tree : {&left, &right}) {
        CY_REQUIRE(static_cast<bool>(tree->begin(HashLevel::World, 0, "world")));
        CY_REQUIRE(static_cast<bool>(tree->begin(HashLevel::Archetype, 77, "archetype")));
        CY_REQUIRE(static_cast<bool>(hash_one(*tree, schema, 1, first)));
        CY_REQUIRE(static_cast<bool>(hash_one(*tree, schema, 2, second)));
        CY_REQUIRE(static_cast<bool>(hash_one(*tree, schema, 3, tree == &right ? third : Robot{})));
        CY_REQUIRE(static_cast<bool>(tree->end()));
        CY_REQUIRE(static_cast<bool>(tree->end()));
    }

    CY_REQUIRE_NE(left.root_hash(), right.root_hash());

    Divergence divergence;
    StateHashTree::compare(left, right, divergence);
    CY_REQUIRE(divergence.diverged);
    CY_CHECK_FALSE(divergence.shape_mismatch);

    // world -> archetype -> entity -> component -> field
    CY_REQUIRE_EQ(divergence.depth, 5U);
    CY_CHECK(divergence.levels[0] == HashLevel::World);
    CY_CHECK(divergence.levels[2] == HashLevel::Entity);
    CY_CHECK_EQ(divergence.ids[2], 3ULL);  // the entity
    CY_CHECK(divergence.levels[3] == HashLevel::Component);
    CY_CHECK(divergence.levels[4] == HashLevel::Field);
    CY_CHECK_EQ(divergence.ids[4], 3ULL);  // "charge"
    CY_CHECK(std::string_view(divergence.names[4]) == "charge");
}

CY_TEST_CASE("an entity present in one run and absent in the other is a shape mismatch") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    StateSchema schema(allocator);
    CY_REQUIRE(static_cast<bool>(declare_robot(schema)));
    schema.freeze();

    const Robot robot;
    StateHashTree left(allocator);
    StateHashTree right(allocator);
    CY_REQUIRE(static_cast<bool>(left.begin(HashLevel::World, 0, "world")));
    CY_REQUIRE(static_cast<bool>(hash_one(left, schema, 1, robot)));
    CY_REQUIRE(static_cast<bool>(hash_one(left, schema, 2, robot)));
    CY_REQUIRE(static_cast<bool>(left.end()));

    CY_REQUIRE(static_cast<bool>(right.begin(HashLevel::World, 0, "world")));
    CY_REQUIRE(static_cast<bool>(hash_one(right, schema, 1, robot)));
    CY_REQUIRE(static_cast<bool>(right.end()));

    Divergence divergence;
    StateHashTree::compare(left, right, divergence);
    CY_CHECK(divergence.diverged);
    CY_CHECK(divergence.shape_mismatch);
    CY_CHECK_EQ(divergence.depth, 1U);
}

CY_TEST_CASE("the hash schedule says when, and a period of zero is not every tick") {
    HashSchedule never;
    CY_CHECK_FALSE(never.due(0));
    CY_CHECK_FALSE(never.due(1));

    HashSchedule every;
    every.frequency = HashFrequency::EveryTick;
    CY_CHECK(every.due(0));
    CY_CHECK(every.due(12345));

    HashSchedule periodic;
    periodic.frequency = HashFrequency::Periodic;
    periodic.period = 60;
    CY_CHECK(periodic.due(120));
    CY_CHECK_FALSE(periodic.due(121));

    periodic.period = 0;
    CY_CHECK_FALSE(periodic.due(0));

    HashSchedule on_demand;
    on_demand.frequency = HashFrequency::OnDemand;
    CY_CHECK_FALSE(on_demand.due(0));
}

CY_TEST_CASE("nesting deeper than the hierarchy is refused") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    StateHashTree tree(allocator);
    for (u32 depth = 0; depth < kHashDepth; ++depth) {
        CY_REQUIRE(static_cast<bool>(tree.begin(HashLevel::Field, depth, "n")));
    }
    CY_CHECK_FALSE(static_cast<bool>(tree.begin(HashLevel::Field, 99, "too deep")));
    CY_CHECK_FALSE(static_cast<bool>(StateHashTree(allocator).end()));
}
