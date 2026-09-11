// THE DETERMINISM FIREWALL'S OWN TEST, IN BOTH DIRECTIONS AND AS A NEGATIVE CONTROL. M8.c task 1.4.
//
// ================================================================================================
// WHAT THIS FILE HAS TO PROVE, AND WHY IT IS NOT A TEST OF A FUNCTION
// ================================================================================================
//
// M8.b carried the criterion "VFX and inference cannot write gameplay state, proven by a test" with
// **neither subject in the tree**, which is a criterion its milestone satisfied vacuously. It moved
// to M8.c with its subjects, and `vfx-system`'s M8.c delta adds the part that makes it real:
//
//   "The test that proves the firewall SHALL fail when the firewall is removed, and that SHALL be
//    demonstrated rather than assumed."
//
// So there are four acts, and the third is the one that matters:
//
//   Act 1  CONTROL. A small deterministic simulation runs and its authoritative state is hashed.
//          Nothing else happens. This is the digest everything else is compared against.
//   Act 2  BOTH DIRECTIONS. The same simulation runs again, and on every tick a VFX readback and a
//          non-pinned inference result each attempt an authoritative write through a DIFFERENT one
//          of the firewall's doors. Every attempt is refused, each refusal names the writer, the
//          component and the path, and the presentation writes both of them also make are
//          permitted. The digest is IDENTICAL to act 1's.
//   Act 3  NEGATIVE CONTROL. The identical act-2 body runs with the enforcement point DISABLED.
//          The writes now land and the digest DIVERGES. That is what proves act 2's assertions were
//          load-bearing: if the firewall did nothing, act 2's "refused" checks and its "identical
//          digest" check would both be false, which is the definition of a test that fails when the
//          thing it guards is removed.
//   Act 4  COOK TIME. `ml-inference`'s other half: a non-pinned model behind an authoritative AI
//          node is refused when the content is COOKED, and a runtime diagnostic is not that
//          requirement.
//
// AND ACT 3 IS ONLY HALF THE DEMONSTRATION. Disabling the firewall through its own switch proves
// the assertions depend on the enforcement, but a switch is still part of the firewall. The other
// half is a SOURCE MUTATION, and four were run against this file, each rebuilt and rerun, each
// exiting 1:
//
//   `WriteFirewall::admit` made to return true      4 of 6 cases fail, 28 of 85 assertions
//   the `World::get_mut` door deleted               4 of 6 cases fail, 21 of 85 assertions
//   the `CommandBuffer::record` door deleted        1 of 6 cases fail,  4 of 85 assertions
//   the development-build report deleted            1 of 6 cases fail,  1 of 85 assertions
//
// The last is the interesting one: the digest still matches, because the firewall still refuses —
// only the report is gone, and only the case that opens a trace notices. The numbers, and the three
// diverged digests, are on `docs/design/images/m8c-determinism-firewall.png`.
//
// Both halves, because either alone is arguable.
//
// WHY THIS SUITE IS `integration` AND NOT `unit`. It builds a world, registers components, runs two
// hundred ticks several times and walks the world into a hash tree on each. The unit tier's budget
// is a millisecond and is calibrated for machine speed, contention and the optimiser; a case that
// builds a world belongs here.

#include <cy/test/test.h>

#include <cy/core/determinism/state_schema.h>
#include <cy/core/diagnostics/trace.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/command_buffer.h>
#include <cy/ecs/firewall.h>
#include <cy/ecs/query.h>
#include <cy/ecs/world.h>
#include <cy/gameplay/cook_firewall.h>
#include <cy/runtime/state_hash.h>

#include <cstddef>
#include <cstdio>
#include <thread>
#include <utility>

namespace {

using cy::ecs::ComponentAuthority;
using cy::ecs::ComponentTypeId;
using cy::ecs::Entity;
using cy::ecs::WriteOrigin;
using cy::ecs::WritePath;
using cy::ecs::WriteScope;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

/// Compare two C strings by content. `CY_CHECK_EQ` on two `const char*` compares pointers, which
/// passes only when the linker happens to merge two identical literals.
[[nodiscard]] bool same_text(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return left == right;
    }
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

/// Enumerators are compared as numbers: doctest stringifies an enum by its bytes otherwise, and the
/// rest of this tree already spells the comparison this way (src/ecs/tests/test_systems.cpp).
template <class E>
[[nodiscard]] cy::u32 as_number(E value) noexcept {
    return static_cast<cy::u32>(value);
}

// --- The world the simulation runs in ------------------------------------------------------------
//
// Three components, chosen so that the firewall's two derivations and its permitted case are each
// exercised by a real declaration rather than by a hand-set flag:
//
//   Health    carries a `Replicated` field                 -> derived `Replicated`
//   Position  carries `Persistence(RuntimeState)` fields   -> derived `Authoritative`
//   Flash     carries `Persistence(Derived)` only          -> derives nothing, stays presentation
//
// `vfx-system` names "network-replicated values" and "any value consumed by deterministic
// simulation" as the two things VFX must not be a source of truth for, and permits a presentation
// reaction — "WHEN a VFX collision readback triggers an audio one-shot THEN this SHALL be
// permitted". `Flash` is that permitted case with a component behind it.

struct Health {
    cy::i32 points = 0;
};
struct Position {
    cy::i32 x = 0;
    cy::i32 y = 0;
};
struct Flash {
    cy::f32 intensity = 0.0F;
};
/// A SPARSE component, so the sparse door is exercised by a component of the kind that actually
/// uses it rather than by an error path. Replicated, so the firewall guards it.
struct Threat {
    cy::i32 level = 0;
};

[[nodiscard]] cy::reflect::FieldAttributes replicated_attribute() noexcept {
    cy::reflect::FieldAttributes attributes;
    attributes.declared = cy::reflect::AttributeKind::Replicated;
    attributes.replicated = cy::reflect::ReplicatedAttribute{"quantised", "bits=16", "owner"};
    return attributes;
}

[[nodiscard]] cy::reflect::FieldAttributes persistence_attribute(
    cy::reflect::PersistenceKind kind) noexcept {
    cy::reflect::FieldAttributes attributes;
    attributes.declared = cy::reflect::AttributeKind::Persistence;
    attributes.persistence = kind;
    return attributes;
}

[[nodiscard]] cy::reflect::FieldInfo make_field(const char* name, cy::u32 id,
                                                cy::reflect::FieldKind kind, cy::u32 offset,
                                                cy::u32 size,
                                                cy::reflect::FieldAttributes attributes) noexcept {
    cy::reflect::FieldInfo field;
    field.name = name;
    field.id = cy::reflect::FieldId(id);
    field.kind = kind;
    field.offset = offset;
    field.size = size;
    field.attributes = attributes;
    return field;
}

[[nodiscard]] const cy::reflect::TypeInfo& health_type() noexcept {
    static const cy::reflect::FieldInfo fields[] = {
        make_field("points", 9101, cy::reflect::FieldKind::I32,
                   static_cast<cy::u32>(offsetof(Health, points)),
                   static_cast<cy::u32>(sizeof(cy::i32)), replicated_attribute()),
    };
    static cy::reflect::TypeInfo info;
    info.name = "cy::gameplay::test::Health";
    info.id = cy::reflect::TypeId(9100);
    info.size = static_cast<cy::u32>(sizeof(Health));
    info.alignment = static_cast<cy::u32>(alignof(Health));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 1;
    return info;
}

[[nodiscard]] const cy::reflect::TypeInfo& position_type() noexcept {
    static const cy::reflect::FieldInfo fields[] = {
        make_field("x", 9201, cy::reflect::FieldKind::I32,
                   static_cast<cy::u32>(offsetof(Position, x)),
                   static_cast<cy::u32>(sizeof(cy::i32)),
                   persistence_attribute(cy::reflect::PersistenceKind::RuntimeState)),
        make_field("y", 9202, cy::reflect::FieldKind::I32,
                   static_cast<cy::u32>(offsetof(Position, y)),
                   static_cast<cy::u32>(sizeof(cy::i32)),
                   persistence_attribute(cy::reflect::PersistenceKind::RuntimeState)),
    };
    static cy::reflect::TypeInfo info;
    info.name = "cy::gameplay::test::Position";
    info.id = cy::reflect::TypeId(9200);
    info.size = static_cast<cy::u32>(sizeof(Position));
    info.alignment = static_cast<cy::u32>(alignof(Position));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 2;
    return info;
}

[[nodiscard]] const cy::reflect::TypeInfo& flash_type() noexcept {
    static const cy::reflect::FieldInfo fields[] = {
        make_field("intensity", 9301, cy::reflect::FieldKind::F32,
                   static_cast<cy::u32>(offsetof(Flash, intensity)),
                   static_cast<cy::u32>(sizeof(cy::f32)),
                   persistence_attribute(cy::reflect::PersistenceKind::Derived)),
    };
    static cy::reflect::TypeInfo info;
    info.name = "cy::gameplay::test::Flash";
    info.id = cy::reflect::TypeId(9300);
    info.size = static_cast<cy::u32>(sizeof(Flash));
    info.alignment = static_cast<cy::u32>(alignof(Flash));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 1;
    return info;
}

[[nodiscard]] const cy::reflect::TypeInfo& threat_type() noexcept {
    static const cy::reflect::FieldInfo fields[] = {
        make_field("level", 9401, cy::reflect::FieldKind::I32,
                   static_cast<cy::u32>(offsetof(Threat, level)),
                   static_cast<cy::u32>(sizeof(cy::i32)), replicated_attribute()),
    };
    static cy::reflect::TypeInfo info;
    info.name = "cy::gameplay::test::Threat";
    info.id = cy::reflect::TypeId(9400);
    info.size = static_cast<cy::u32>(sizeof(Threat));
    info.alignment = static_cast<cy::u32>(alignof(Threat));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 1;
    return info;
}

struct Ids {
    ComponentTypeId health = cy::ecs::kInvalidComponent;
    ComponentTypeId position = cy::ecs::kInvalidComponent;
    ComponentTypeId flash = cy::ecs::kInvalidComponent;
    ComponentTypeId threat = cy::ecs::kInvalidComponent;
};

constexpr cy::u32 kAgents = 64;
constexpr cy::u32 kTicks = 200;

/// A world with the three components registered, `kAgents` entities in it, and the firewall's
/// authority derived from what those components already declare about themselves.
struct Fixture {
    cy::ecs::World world;
    Ids ids;
    cy::Array<Entity> agents;
    cy::ecs::AuthorityDerivationReport derivation;

    Fixture() noexcept : world(allocator()), agents(allocator()) {}

    [[nodiscard]] bool build() noexcept {
        if (!world.initialize().has_value()) {
            return false;
        }
        auto health = world.components().register_reflected(health_type());
        auto position = world.components().register_reflected(position_type());
        auto flash = world.components().register_reflected(flash_type());
        cy::ecs::ComponentOptions sparse;
        sparse.kind = cy::ecs::ComponentKind::Sparse;
        auto threat = world.components().register_reflected(threat_type(), sparse);
        if (!health.has_value() || !position.has_value() || !flash.has_value() ||
            !threat.has_value()) {
            return false;
        }
        ids.health = *health;
        ids.position = *position;
        ids.flash = *flash;
        ids.threat = *threat;

        if (!world.firewall().declare_from_reflection(world.components(), derivation).has_value()) {
            return false;
        }

        const ComponentTypeId set[] = {ids.health, ids.position, ids.flash};
        if (!world.create_many(kAgents, cy::Span<const ComponentTypeId>(set, 3), agents)
                 .has_value()) {
            return false;
        }
        for (cy::u32 index = 0; index < agents.size(); ++index) {
            auto* points = world.get_mut<Health>(agents[index], ids.health);
            auto* place = world.get_mut<Position>(agents[index], ids.position);
            if (points == nullptr || place == nullptr) {
                return false;
            }
            points->points = 1000 + static_cast<cy::i32>(index);
            place->x = static_cast<cy::i32>(index);
            place->y = 0;
        }
        return true;
    }
};

/// One tick of the authoritative simulation. Integer arithmetic on purpose: the digest has to be
/// exactly equal across runs, and this file is about the firewall rather than about floating point.
void simulate_tick(cy::ecs::World& world, const Ids& ids, cy::u32 tick) noexcept {
    cy::ecs::QueryDesc desc(allocator());
    if (!desc.write(ids.health).has_value() || !desc.write(ids.position).has_value()) {
        return;
    }
    cy::ecs::Query query(world, std::move(desc));
    (void)query.for_each_chunk([&](cy::ecs::QueryChunk& chunk) noexcept {
        const cy::Span<Health> health = chunk.write<Health>(ids.health);
        const cy::Span<Position> places = chunk.write<Position>(ids.position);
        for (cy::u32 row = 0; row < chunk.count(); ++row) {
            health[row].points -= 1;
            places[row].x += static_cast<cy::i32>((tick + row) % 3U);
            places[row].y += 1;
        }
    });
}

/// The state digest: the authoritative half of the world, walked into the hash tree the runtime
/// already uses. `simulation-and-determinism`'s own machinery rather than a number this file made
/// up, so "the digest is identical" means what the engine means by it.
[[nodiscard]] cy::u64 digest_of(const cy::ecs::World& world) noexcept {
    cy::determinism::StateSchema schema(allocator());
    cy::runtime::SchemaDeclarationReport declaration;
    if (!cy::runtime::declare_reflected_components(world, schema, declaration).has_value()) {
        return 0;
    }
    schema.freeze();
    cy::determinism::StateHashTree tree(allocator());
    cy::runtime::WorldHashReport report;
    if (!cy::runtime::hash_world(world, schema, tree, report).has_value()) {
        return 0;
    }
    return report.hash;
}

/// What one VFX readback and one non-pinned inference result try on every tick, and what each of
/// them is permitted to do.
///
/// **The two use different doors on purpose.** A firewall that only closed `World::get_mut` would
/// pass a test that only used `World::get_mut`, and the claim being made is "every authoritative
/// write passes through one point" rather than "one function checks".
struct AttemptTally {
    cy::u32 vfx_attempts = 0;
    cy::u32 inference_attempts = 0;
    cy::u32 vfx_writes_that_landed = 0;
    cy::u32 inference_writes_that_landed = 0;
    cy::u32 presentation_writes_that_landed = 0;
};

/// A VFX collision readback. Three authoritative doors and one permitted presentation write.
void vfx_readback(cy::ecs::World& world, const Ids& ids, Entity victim,
                  cy::ecs::CommandBuffer& deferred, AttemptTally& tally) noexcept {
    const WriteScope scope(WriteOrigin::Vfx, "vfx.collision-readback");

    // 1. The value door. `vfx-system`: VFX "SHALL NOT be a source of truth for: damage".
    ++tally.vfx_attempts;
    if (auto* health = world.get_mut<Health>(victim, ids.health); health != nullptr) {
        health->points -= 250;
        ++tally.vfx_writes_that_landed;
    }

    // 2. The lifetime door. `vfx-system`: "... entity creation or destruction".
    ++tally.vfx_attempts;
    if (world.create().has_value()) {
        ++tally.vfx_writes_that_landed;
    }

    // 3. The deferred door. A structural change recorded now and applied at the flush, which is the
    //    laundering route a check at flush time would miss: the flush runs under the simulation's
    //    own origin.
    //    THE BUFFER IS APPLIED BY THE CALLER, AFTER THIS SCOPE HAS CLOSED, exactly as a stage's
    //    flush applies one: by then the VFX code path is off the stack and the flushing thread is
    //    the simulation, so nothing downstream can tell where the command came from. That is why
    //    recording is the moment the question has to be asked.
    ++tally.vfx_attempts;
    if (deferred.remove(victim, ids.health).has_value()) {
        ++tally.vfx_writes_that_landed;
    }

    // 4. PERMITTED. The flash the collision produces.
    if (auto* flash = world.get_mut<Flash>(victim, ids.flash); flash != nullptr) {
        flash->intensity = 1.0F;
        ++tally.presentation_writes_that_landed;
    }
}

/// A non-pinned inference result. The bulk column door and the relationship door, and one permitted
/// presentation write — `ml-inference`: "WHEN a model drives an animation blend weight ... THEN it
/// SHALL be permitted without pinning".
void non_pinned_inference(cy::ecs::World& world, const Ids& ids, Entity subject,
                          AttemptTally& tally) noexcept {
    const WriteScope scope(WriteOrigin::Inference, "ml.threat-estimate");

    // 1. The bulk column door. Counted per chunk, because that is where the door is.
    cy::ecs::QueryDesc desc(allocator());
    if (desc.write(ids.position).has_value()) {
        cy::ecs::Query query(world, std::move(desc));
        (void)query.for_each_chunk([&](cy::ecs::QueryChunk& chunk) noexcept {
            ++tally.inference_attempts;
            const cy::Span<Position> places = chunk.write<Position>(ids.position);
            for (Position& place : places) {
                place.x += 7;
                ++tally.inference_writes_that_landed;
            }
        });
    }

    // 2. The relationship door: a model deciding a hierarchy is a model deciding hashed state —
    //    a `Parent` edge is covered by `<cy/ecs/state_schema.h>`.
    ++tally.inference_attempts;
    if (world.set_parent(subject, cy::ecs::kNoEntity).has_value()) {
        ++tally.inference_writes_that_landed;
    }

    // 3. PERMITTED.
    if (auto* flash = world.get_mut<Flash>(subject, ids.flash); flash != nullptr) {
        flash->intensity += 0.25F;
        ++tally.presentation_writes_that_landed;
    }
}

/// Run the whole simulation. `with_spectacle` decides whether VFX and inference run beside it.
[[nodiscard]] cy::u64 run_simulation(Fixture& fixture, bool with_spectacle,
                                     AttemptTally& tally) noexcept {
    for (cy::u32 tick = 0; tick < kTicks; ++tick) {
        simulate_tick(fixture.world, fixture.ids, tick);
        if (with_spectacle) {
            const Entity victim = fixture.agents[tick % fixture.agents.size()];
            cy::ecs::CommandBuffer deferred(fixture.world);
            vfx_readback(fixture.world, fixture.ids, victim, deferred, tally);
            non_pinned_inference(fixture.world, fixture.ids, victim, tally);
            // The stage flush. Outside every scope, under the simulation's own origin.
            (void)deferred.apply();
        }
        (void)fixture.world.advance_version();
    }
    return digest_of(fixture.world);
}

}  // namespace

// ================================================================================================
// ACTS 1 AND 2 — BOTH DIRECTIONS, AND THE DIGEST THAT DOES NOT MOVE
// ================================================================================================

CY_TEST_CASE(
    "the firewall refuses a VFX readback and a non-pinned inference write, and the re-simulation "
    "reaches the identical digest") {
    Fixture control;
    CY_REQUIRE(control.build());
    // The derivation found something. A firewall guarding nothing would pass every check below.
    CY_REQUIRE_EQ(control.derivation.guarded_by_replication, 2u);
    CY_REQUIRE_EQ(control.derivation.guarded_by_persistence, 1u);
    CY_CHECK_EQ(control.world.firewall().guarded_count(), 3u);
    CY_CHECK_EQ(as_number(control.world.firewall().authority_of(control.ids.health)),
                as_number(ComponentAuthority::Replicated));
    CY_CHECK_EQ(as_number(control.world.firewall().authority_of(control.ids.position)),
                as_number(ComponentAuthority::Authoritative));
    // Flash derives nothing and stays writable by presentation code, which is the permitted case.
    CY_CHECK_EQ(as_number(control.world.firewall().authority_of(control.ids.flash)),
                as_number(ComponentAuthority::Presentation));

    AttemptTally control_tally;
    const cy::u64 control_digest = run_simulation(control, false, control_tally);
    CY_REQUIRE_NE(control_digest, 0u);
    CY_CHECK_EQ(control.world.firewall().refusals(), 0u);

    Fixture spectacle;
    CY_REQUIRE(spectacle.build());
    AttemptTally tally;
    const cy::u64 spectacle_digest = run_simulation(spectacle, true, tally);

    // BOTH DIRECTIONS WERE ATTEMPTED, AND NOTHING LANDED.
    CY_CHECK_EQ(tally.vfx_attempts, kTicks * 3u);
    CY_CHECK_GE(tally.inference_attempts, kTicks * 2u);
    CY_CHECK_EQ(tally.vfx_writes_that_landed, 0u);
    CY_CHECK_EQ(tally.inference_writes_that_landed, 0u);
    // And the permitted presentation writes DID land — a firewall that refused everything would
    // also produce an identical digest, and would be useless.
    CY_CHECK_EQ(tally.presentation_writes_that_landed, kTicks * 2u);

    // THE CRITERION. `vfx-system`: "the result SHALL be identical regardless of what VFX did".
    CY_CHECK_EQ(spectacle_digest, control_digest);

    // Every attempt produced exactly one refusal, on both sides, and the simulation produced none.
    CY_CHECK_EQ(spectacle.world.firewall().refusals(),
                static_cast<cy::u64>(tally.vfx_attempts) + tally.inference_attempts);
    CY_CHECK_EQ(spectacle.world.firewall().refusals_by(WriteOrigin::Vfx),
                static_cast<cy::u64>(tally.vfx_attempts));
    CY_CHECK_EQ(spectacle.world.firewall().refusals_by(WriteOrigin::Inference),
                static_cast<cy::u64>(tally.inference_attempts));
    CY_CHECK_EQ(spectacle.world.firewall().refusals_by(WriteOrigin::Simulation), 0u);
}

// ================================================================================================
// TASK 1.2 — THE REPORT NAMES THE WRITER, THE COMPONENT AND THE PATH
// ================================================================================================

CY_TEST_CASE("a refused write is reported with the writer, the component and the path") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    const Entity victim = fixture.agents[0];

    {
        const WriteScope scope(WriteOrigin::Vfx, "vfx.impact-decal");
        CY_CHECK(fixture.world.get_mut<Health>(victim, fixture.ids.health) == nullptr);
    }
    CY_REQUIRE_EQ(fixture.world.firewall().refusals(), 1u);

    const cy::ecs::FirewallViolation first = fixture.world.firewall().last_violation();
    CY_CHECK_EQ(as_number(first.origin), as_number(WriteOrigin::Vfx));
    CY_CHECK(same_text(first.writer, "vfx.impact-decal"));
    CY_CHECK_EQ(as_number(first.path), as_number(WritePath::GetMut));
    CY_CHECK_EQ(first.component, fixture.ids.health);
    CY_CHECK(same_text(first.component_name, "cy::gameplay::test::Health"));
    CY_CHECK_EQ(as_number(first.authority), as_number(ComponentAuthority::Replicated));
    CY_CHECK_EQ(first.entity.index(), victim.index());
    CY_CHECK_EQ(first.ordinal, 1u);

    // Each door reports itself, so "the path" is the path and not a constant.
    const Threat threat{9};
    {
        const WriteScope scope(WriteOrigin::Inference, "ml.pathfinder");
        CY_CHECK_FALSE(fixture.world.set_sparse(victim, fixture.ids.threat, &threat).has_value());
    }
    CY_CHECK_EQ(as_number(fixture.world.firewall().last_violation().path),
                as_number(WritePath::SparseWrite));
    CY_CHECK(same_text(fixture.world.firewall().last_violation().writer, "ml.pathfinder"));

    {
        const WriteScope scope(WriteOrigin::Vfx, "vfx.spawner");
        CY_CHECK_FALSE(fixture.world.create().has_value());
    }
    CY_CHECK_EQ(as_number(fixture.world.firewall().last_violation().path),
                as_number(WritePath::EntityLifetime));

    // A pinned inference session is NOT restricted — `ml-inference` permits exactly that, and a
    // firewall that refused it would refuse the case its own specification allows.
    const cy::u64 before = fixture.world.firewall().refusals();
    {
        const WriteScope scope(WriteOrigin::PinnedInference, "ml.pinned-threat");
        CY_CHECK(fixture.world.get_mut<Health>(victim, fixture.ids.health) != nullptr);
    }
    CY_CHECK_EQ(fixture.world.firewall().refusals(), before);
}

// ================================================================================================
// TASK 1.2 — AND THE REPORT IS EMITTED, NOT MERELY COMPILED
// ================================================================================================
//
// The retained `FirewallViolation` above is a convenience for a test. The requirement is a
// development-build *report*, and M8.c's delta adds "not a diagnostic that a caller may choose to
// read" — so the refusal writes onto the one trace at the moment it happens. This case opens that
// trace and checks that something arrived, because a `CY_LOG` inside an `#if` that nobody ever
// executed reads exactly like one that works.
//
// AND IT CHECKS BOTH CONFIGURATIONS. In a Shipping build `CY_DEVELOPMENT` is undefined, the report
// is compiled out, and the firewall still REFUSES and still COUNTS — that is the part that is not
// a development-build feature. The assertion below says which build it is in rather than assuming.

CY_TEST_CASE("a refusal reports itself onto the one trace in a development build") {
    cy::diag::TraceConfig config;
    config.path = "gameplay_firewall.cytrace";
    config.consumer_thread = false;
    CY_REQUIRE(cy::diag::trace_open(config).has_value());

    cy::u64 refusals = 0;
    {
        Fixture fixture;
        CY_REQUIRE(fixture.build());
        const Entity victim = fixture.agents[0];
        const WriteScope scope(WriteOrigin::Vfx, "vfx.trace-probe");
        for (cy::u32 attempt = 0; attempt < 4; ++attempt) {
            CY_CHECK(fixture.world.get_mut<Health>(victim, fixture.ids.health) == nullptr);
        }
        refusals = fixture.world.firewall().refusals();
    }

    cy::diag::trace_flush();
    const auto stats = cy::diag::trace_close();
    CY_REQUIRE(stats.has_value());

    // The refusal itself is not a development-build feature.
    CY_CHECK_EQ(refusals, 4u);
    // Every field the report carries is classified; nothing was dropped as unclassified.
    CY_CHECK_EQ(stats->unclassified_fields, 0u);
#if defined(CY_DEVELOPMENT)
    CY_CHECK_GT(stats->events_written, 0u);
#else
    CY_TEST_MESSAGE("Shipping build: the firewall refused and counted; the report is compiled out");
#endif
    (void)std::remove("gameplay_firewall.cytrace");
}

// ================================================================================================
// ACT 3 — THE NEGATIVE CONTROL
// ================================================================================================

CY_TEST_CASE("with the enforcement point disabled the same writes land and the digest diverges") {
    Fixture control;
    CY_REQUIRE(control.build());
    AttemptTally control_tally;
    const cy::u64 control_digest = run_simulation(control, false, control_tally);
    CY_REQUIRE_NE(control_digest, 0u);

    Fixture disarmed;
    CY_REQUIRE(disarmed.build());
    disarmed.world.firewall().disarm_for_negative_control(
        "M8.c task 1.4: proving the firewall's own test depends on the firewall");
    CY_REQUIRE_FALSE(disarmed.world.firewall().armed());

    AttemptTally tally;
    const cy::u64 disarmed_digest = run_simulation(disarmed, true, tally);

    // Exactly the assertions act 2 makes, inverted. Every one of these would have to be false for
    // act 2 to pass with the firewall removed, which is what "the test fails when the firewall is
    // removed" means expressed from inside one process.
    CY_CHECK_EQ(disarmed.world.firewall().refusals(), 0u);
    CY_CHECK_GT(tally.vfx_writes_that_landed, 0u);
    CY_CHECK_GT(tally.inference_writes_that_landed, 0u);
    CY_CHECK_NE(disarmed_digest, control_digest);

    // And the switch says why it is down, so a capture taken with the firewall disabled is not
    // mistaken for a clean one.
    CY_CHECK_FALSE(same_text(disarmed.world.firewall().disarm_reason(), ""));

    disarmed.world.firewall().rearm();
    CY_CHECK(disarmed.world.firewall().armed());
}

// ================================================================================================
// TEARDOWN UNDER LOAD
// ================================================================================================

CY_TEST_CASE("the firewall counts and tears down correctly with every thread refused at once") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());

    constexpr cy::u32 kThreads = 8;
    constexpr cy::u32 kAttemptsPerThread = 500;
    std::thread workers[kThreads];
    for (cy::u32 index = 0; index < kThreads; ++index) {
        workers[index] = std::thread([&fixture, index]() noexcept {
            // Every thread declares its own origin, which is the whole reason the frame is
            // thread-local: one world, eight call stacks, eight answers.
            const WriteOrigin origin = (index % 2 == 0) ? WriteOrigin::Vfx : WriteOrigin::Inference;
            const WriteScope scope(origin, "load.worker");
            for (cy::u32 attempt = 0; attempt < kAttemptsPerThread; ++attempt) {
                const Entity victim = fixture.agents[attempt % fixture.agents.size()];
                (void)fixture.world.get_mut<Health>(victim, fixture.ids.health);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    CY_CHECK_EQ(fixture.world.firewall().refusals(),
                static_cast<cy::u64>(kThreads) * kAttemptsPerThread);
    CY_CHECK_EQ(fixture.world.firewall().refusals_by(WriteOrigin::Vfx),
                static_cast<cy::u64>(kThreads / 2) * kAttemptsPerThread);
    CY_CHECK_EQ(fixture.world.firewall().violation_count(),
                cy::ecs::WriteFirewall::kMaxRecordedViolations);

    // The main thread never opened a scope, so it is still the simulation and still writes.
    CY_CHECK(fixture.world.get_mut<Health>(fixture.agents[0], fixture.ids.health) != nullptr);
    // And the world tears down with eight threads' worth of refusals recorded in it.
}

// ================================================================================================
// ACT 4 — COOK TIME. `ml-inference`: "Multiplayer desync is prevented at cook time"
// ================================================================================================

namespace {

[[nodiscard]] cy::gameplay::ModelPinning pinned_model(cy::u64 configuration) noexcept {
    cy::gameplay::ModelPinning model;
    model.model = cy::Name::intern("threat-estimator");
    model.backend = cy::Name::intern("onnxruntime");
    model.precision = cy::Name::intern("fp32");
    model.verified_configuration = configuration;
    return model;
}

[[nodiscard]] cy::gameplay::ModelPinning unpinned_model() noexcept {
    cy::gameplay::ModelPinning model;
    model.model = cy::Name::intern("threat-estimator");
    return model;
}

[[nodiscard]] cy::gameplay::InferenceBinding make_binding(bool authoritative,
                                                          cy::u64 configuration) noexcept {
    cy::gameplay::InferenceBinding bound;
    bound.graph = cy::Name::intern("enemy-brain");
    bound.node = cy::Name::intern("estimate-threat");
    bound.model = cy::Name::intern("threat-estimator");
    bound.authoritative = authoritative;
    bound.cook_configuration = configuration;
    return bound;
}

}  // namespace

CY_TEST_CASE("a non-pinned model feeding an authoritative node is refused at cook time") {
    constexpr cy::u64 kConfiguration = 0x5EED'C0DE'1234'5678ULL;

    cy::Array<cy::gameplay::CookRefusal> refusals(allocator());
    cy::gameplay::CookFirewallReport report;

    const cy::gameplay::ModelPinning unpinned[] = {unpinned_model()};
    const cy::gameplay::InferenceBinding authoritative[] = {make_binding(true, kConfiguration)};

    // 1. THE REFUSAL. A non-pinned model behind an authoritative node fails the cook.
    const cy::Status refused = cy::gameplay::check_inference_bindings(
        cy::Span<const cy::gameplay::ModelPinning>(unpinned, 1),
        cy::Span<const cy::gameplay::InferenceBinding>(authoritative, 1), refusals, report);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(as_number(refused.error().code), as_number(cy::ErrorCode::PermissionDenied));
    CY_REQUIRE_EQ(report.refusals, 1u);
    CY_CHECK_EQ(report.bindings_examined, 1u);
    CY_CHECK_EQ(report.authoritative_bindings, 1u);
    CY_REQUIRE_EQ(refusals.size(), 1u);
    CY_CHECK_EQ(as_number(refusals[0].tag),
                as_number(cy::gameplay::CookRefusalTag::ModelNotPinned));
    CY_CHECK_EQ(refusals[0].node.index(), cy::Name::intern("estimate-threat").index());

    // 2. THE SAME MODEL BEHIND A PRESENTATION NODE IS FINE. `ml-inference`: "Presentation use is
    //    unrestricted" — a blend weight needs no pinning.
    refusals.clear();
    const cy::gameplay::InferenceBinding presentation[] = {make_binding(false, kConfiguration)};
    CY_CHECK(cy::gameplay::check_inference_bindings(
                 cy::Span<const cy::gameplay::ModelPinning>(unpinned, 1),
                 cy::Span<const cy::gameplay::InferenceBinding>(presentation, 1), refusals, report)
                 .has_value());
    CY_CHECK_EQ(report.refusals, 0u);
    CY_CHECK_EQ(report.presentation_bindings, 1u);

    // 3. A PINNED MODEL ON THE CONFIGURATION BEING COOKED PASSES.
    refusals.clear();
    const cy::gameplay::ModelPinning pinned[] = {pinned_model(kConfiguration)};
    CY_CHECK(cy::gameplay::check_inference_bindings(
                 cy::Span<const cy::gameplay::ModelPinning>(pinned, 1),
                 cy::Span<const cy::gameplay::InferenceBinding>(authoritative, 1), refusals, report)
                 .has_value());
    CY_CHECK_EQ(report.refusals, 0u);
    CY_CHECK_EQ(report.pinned_bindings, 1u);

    // 4. PINNED TO A CONFIGURATION NOBODY IS BUILDING IS STILL A REFUSAL — the same desync with an
    //    extra step, and the reason both digests are reported rather than a boolean.
    refusals.clear();
    const cy::gameplay::ModelPinning elsewhere[] = {pinned_model(kConfiguration ^ 1ULL)};
    CY_REQUIRE_FALSE(cy::gameplay::check_inference_bindings(
                         cy::Span<const cy::gameplay::ModelPinning>(elsewhere, 1),
                         cy::Span<const cy::gameplay::InferenceBinding>(authoritative, 1), refusals,
                         report)
                         .has_value());
    CY_REQUIRE_EQ(refusals.size(), 1u);
    CY_CHECK_EQ(as_number(refusals[0].tag),
                as_number(cy::gameplay::CookRefusalTag::PinnedConfigurationMismatch));
    CY_CHECK_EQ(refusals[0].verified_configuration, kConfiguration ^ 1ULL);
    CY_CHECK_EQ(refusals[0].cook_configuration, kConfiguration);

    // 5. AN AUTHORITATIVE NODE BOUND TO A MODEL NOTHING DESCRIBES IS REFUSED RATHER THAN ASSUMED
    //    PINNED. An unknown model is the case where guessing costs the most.
    refusals.clear();
    CY_REQUIRE_FALSE(cy::gameplay::check_inference_bindings(
                         cy::Span<const cy::gameplay::ModelPinning>{},
                         cy::Span<const cy::gameplay::InferenceBinding>(authoritative, 1), refusals,
                         report)
                         .has_value());
    CY_REQUIRE_EQ(refusals.size(), 1u);
    CY_CHECK_EQ(as_number(refusals[0].tag), as_number(cy::gameplay::CookRefusalTag::UnknownModel));
}
