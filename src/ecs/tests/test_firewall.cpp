// The determinism firewall AT THE ECS WRITE PATH. M8.c tasks 1.1 and 1.2, from the ECS's own side.
//
// WHY THIS FILE EXISTS, AND WHAT ITS ABSENCE COST. M8.c's closing gate mutated
// `WriteFirewall::admit` to return true unconditionally — the enforcement point removed entirely —
// and `unit.ecs` STILL PASSED, because not one of its fourteen sources named the firewall. The
// ledger's `m8c:firewall-ecs` criterion runs exactly this suite and describes "every door — create,
// add, set, remove, instantiate, the command buffer at RECORD time, the query chunk's mutable span
// and Snapshot::restore — refusing a write whose origin may not make it". A criterion that cannot
// fail when the thing it names is deleted is the shape M6 shipped four of and M8.b shipped one of.
//
// `src/gameplay/tests/test_firewall.cpp` proves the same rule end to end — both directions, the
// digest, the negative control. This file proves the DOORS, one at a time, in the module that owns
// them, so that a door added or removed here is caught here.
//
// WHY IT IS `unit` AND NOT `integration`. Every case builds one world with a handful of entities
// and asserts a returned value. Nothing starts a thread except the thread-locality case, which
// starts exactly one and joins it; nothing does I/O; the whole file is well inside the tier's
// millisecond and the gate measured it rather than assuming it.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/command_buffer.h>
#include <cy/ecs/firewall.h>
#include <cy/ecs/query.h>
#include <cy/ecs/snapshot.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

using cy::ecs::ComponentAuthority;
using cy::ecs::Entity;
using cy::ecs::WriteOrigin;
using cy::ecs::WritePath;
using cy::ecs::WriteScope;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

template <class E>
[[nodiscard]] cy::u32 as_number(E value) noexcept {
    return static_cast<cy::u32>(value);
}

/// A world with the standard fixture components, `Position` declared authoritative and `Velocity`
/// left as presentation — so every case below has both a guarded component and a permitted one and
/// a firewall that refused everything would fail as loudly as one that refused nothing.
struct Fixture {
    cy::ecs::World world{allocator()};
    cy::ecs::test::Components ids;

    [[nodiscard]] bool build() noexcept {
        if (!world.initialize().has_value()) {
            return false;
        }
        const auto registered = cy::ecs::test::register_all(world);
        if (!registered.has_value()) {
            return false;
        }
        ids = *registered;
        return world.firewall()
                   .declare(ids.position, ComponentAuthority::Authoritative)
                   .has_value() &&
               world.firewall().declare(ids.selected, ComponentAuthority::Replicated).has_value() &&
               world.firewall().declare(ids.material, ComponentAuthority::PhysicsOwned).has_value();
    }

    [[nodiscard]] Entity spawn() noexcept {
        const cy::ecs::ComponentTypeId set[] = {ids.position, ids.velocity};
        const auto created = world.create(cy::Span<const cy::ecs::ComponentTypeId>(set, 2));
        return created.has_value() ? *created : Entity{};
    }
};

}  // namespace

// ================================================================================================
// THE ORIGIN ITSELF
// ================================================================================================

CY_TEST_CASE("the write origin is a stack scope that nests and restores what it replaced") {
    CY_CHECK_EQ(as_number(cy::ecs::current_write_origin()), as_number(WriteOrigin::Simulation));
    {
        const WriteScope outer(WriteOrigin::Vfx, "vfx.outer");
        CY_CHECK_EQ(as_number(cy::ecs::current_write_origin()), as_number(WriteOrigin::Vfx));
        CY_CHECK(cy::ecs::test::same_text(cy::ecs::current_writer(), "vfx.outer"));
        {
            const WriteScope inner(WriteOrigin::Presentation, "ui.inner");
            CY_CHECK_EQ(as_number(cy::ecs::current_write_origin()),
                        as_number(WriteOrigin::Presentation));
            CY_CHECK(cy::ecs::test::same_text(cy::ecs::current_writer(), "ui.inner"));
        }
        // THE NESTING IS THE POINT: a VFX callback that calls into presentation code is still VFX
        // when it comes back, and a scope that restored `Simulation` would open a hole.
        CY_CHECK_EQ(as_number(cy::ecs::current_write_origin()), as_number(WriteOrigin::Vfx));
        CY_CHECK(cy::ecs::test::same_text(cy::ecs::current_writer(), "vfx.outer"));
    }
    CY_CHECK_EQ(as_number(cy::ecs::current_write_origin()), as_number(WriteOrigin::Simulation));
}

// THE THREAD-LOCALITY HALF IS NOT HERE, AND THAT IS THE TAXONOMY'S RULE RATHER THAN AN OMISSION:
// this file's own header above says a unit test starts no thread, which is why `ecs_scheduling` is
// integration. `integration.gameplay_firewall` covers it with eight threads writing at once against
// one world, asserting the refusal counts exactly and that the main thread — which never opened a
// scope — still writes.

// ================================================================================================
// WHAT A COMPONENT IS
// ================================================================================================

CY_TEST_CASE("authority is raised but never lowered, and what could not be derived is reported") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());

    CY_CHECK_EQ(as_number(fixture.world.firewall().authority_of(fixture.ids.position)),
                as_number(ComponentAuthority::Authoritative));
    CY_CHECK_EQ(as_number(fixture.world.firewall().authority_of(fixture.ids.velocity)),
                as_number(ComponentAuthority::Presentation));
    CY_CHECK(fixture.world.firewall().guards(fixture.ids.position));
    CY_CHECK_FALSE(fixture.world.firewall().guards(fixture.ids.velocity));
    CY_CHECK_EQ(fixture.world.firewall().guarded_count(), 3u);

    // Idempotent for the same value; raising is allowed.
    CY_CHECK(fixture.world.firewall()
                 .declare(fixture.ids.position, ComponentAuthority::Authoritative)
                 .has_value());
    CY_CHECK(fixture.world.firewall()
                 .declare(fixture.ids.position, ComponentAuthority::Replicated)
                 .has_value());
    // LOWERING IS REFUSED. A component that was once replicated does not become presentation
    // because a second caller said so.
    CY_CHECK_FALSE(fixture.world.firewall()
                       .declare(fixture.ids.position, ComponentAuthority::Presentation)
                       .has_value());
    CY_CHECK_EQ(as_number(fixture.world.firewall().authority_of(fixture.ids.position)),
                as_number(ComponentAuthority::Replicated));

    // AND THE DERIVATION REPORTS WHAT IT COULD NOT ANSWER. These fixture types declare no
    // attributes at all, so every one of them is `underived` — which is the number
    // `AuthorityDerivationReport` exists to stop being a silence.
    cy::ecs::AuthorityDerivationReport report;
    CY_REQUIRE(fixture.world.firewall()
                   .declare_from_reflection(fixture.world.components(), report)
                   .has_value());
    CY_CHECK_GT(report.components_examined, 0u);
    CY_CHECK_EQ(report.guarded_by_replication, 0u);
    CY_CHECK_EQ(report.guarded_by_persistence, 0u);
    CY_CHECK_GT(report.underived, 0u);
    CY_CHECK_EQ(report.components_examined, report.guarded_by_replication +
                                                report.guarded_by_persistence +
                                                report.already_declared + report.underived);
}

// ================================================================================================
// EVERY DOOR — the list `firewall.h` enumerates, one case per door
// ================================================================================================

CY_TEST_CASE("the value door: World::get_mut hands back nothing to a restricted origin") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    const Entity entity = fixture.spawn();

    {
        const WriteScope scope(WriteOrigin::Vfx, "vfx.impact");
        CY_CHECK(fixture.world.get_mut<cy::ecs::test::Position>(entity, fixture.ids.position) ==
                 nullptr);
        // The permitted half, in the same scope: presentation state is what VFX may write.
        CY_CHECK(fixture.world.get_mut<cy::ecs::test::Velocity>(entity, fixture.ids.velocity) !=
                 nullptr);
        // And reading is never a door.
        CY_CHECK(fixture.world.get<cy::ecs::test::Position>(entity, fixture.ids.position) !=
                 nullptr);
    }
    CY_CHECK_EQ(fixture.world.firewall().refusals(), 1u);
    const cy::ecs::FirewallViolation violation = fixture.world.firewall().last_violation();
    CY_CHECK_EQ(as_number(violation.path), as_number(WritePath::GetMut));
    CY_CHECK_EQ(as_number(violation.origin), as_number(WriteOrigin::Vfx));
    CY_CHECK(cy::ecs::test::same_text(violation.writer, "vfx.impact"));
    CY_CHECK_EQ(violation.component, fixture.ids.position);
    CY_CHECK_EQ(violation.entity.index(), entity.index());
    CY_CHECK_EQ(violation.ordinal, 1u);
}

CY_TEST_CASE("the bulk door: a query chunk's mutable span is EMPTY rather than short") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    const cy::ecs::ComponentTypeId set[] = {fixture.ids.position, fixture.ids.velocity};
    cy::Array<Entity> created(allocator());
    CY_REQUIRE(
        fixture.world.create_many(8, cy::Span<const cy::ecs::ComponentTypeId>(set, 2), created)
            .has_value());

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.write(fixture.ids.position).has_value());
    CY_REQUIRE(desc.write(fixture.ids.velocity).has_value());
    cy::ecs::Query query(fixture.world, std::move(desc));

    cy::u32 guarded_rows = 0;
    cy::u32 permitted_rows = 0;
    {
        const WriteScope scope(WriteOrigin::Inference, "ml.threat-estimate");
        CY_REQUIRE(query
                       .for_each_chunk([&](cy::ecs::QueryChunk& chunk) noexcept {
                           guarded_rows += static_cast<cy::u32>(
                               chunk.write<cy::ecs::test::Position>(fixture.ids.position).size());
                           permitted_rows += static_cast<cy::u32>(
                               chunk.write<cy::ecs::test::Velocity>(fixture.ids.velocity).size());
                       })
                       .has_value());
    }
    // AN EMPTY SPAN, NOT A SHORT ONE: the caller's next statement is a range-for, so refusal has to
    // mean "walk zero rows" rather than "return an error nobody reads".
    CY_CHECK_EQ(guarded_rows, 0u);
    CY_CHECK_EQ(permitted_rows, 8u);
    CY_CHECK_GT(fixture.world.firewall().refusals(), 0u);
    CY_CHECK_EQ(as_number(fixture.world.firewall().last_violation().path),
                as_number(WritePath::QueryWrite));
}

CY_TEST_CASE("the structural doors: add, remove and set_shared are refused") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    const Entity entity = fixture.spawn();
    const cy::ecs::test::Position moved{1.0F, 2.0F, 3.0F};
    // Interned first, by the simulation: `set_shared` refuses an un-interned value before it ever
    // reaches the firewall, and a refusal for the wrong reason would read as a pass here.
    const cy::ecs::test::Material stone{7};
    const auto interned = fixture.world.intern_shared(fixture.ids.material, &stone);
    CY_REQUIRE(interned.has_value());

    {
        const WriteScope scope(WriteOrigin::Vfx, "vfx.spawn-callback");
        CY_CHECK_FALSE(fixture.world.remove(entity, fixture.ids.position).has_value());
        CY_CHECK_FALSE(
            fixture.world.set_shared(entity, fixture.ids.material, *interned).has_value());
        // Adding a PRESENTATION component is not a door the firewall closes.
        CY_CHECK(fixture.world.add(entity, fixture.ids.frozen, nullptr).has_value());
    }
    CY_CHECK_EQ(fixture.world.firewall().refusals(), 2u);
    CY_CHECK(fixture.world.get<cy::ecs::test::Position>(entity, fixture.ids.position) != nullptr);
    // And the simulation may do what the restricted origin could not.
    CY_CHECK(fixture.world.set(entity, fixture.ids.position, &moved).has_value());
}

CY_TEST_CASE("the sparse doors: set_sparse and remove_sparse are refused") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    const Entity entity = fixture.spawn();
    const cy::ecs::test::Selected marked{42};
    CY_REQUIRE(fixture.world.set_sparse(entity, fixture.ids.selected, &marked).has_value());

    {
        const WriteScope scope(WriteOrigin::Inference, "ml.unpinned");
        const cy::ecs::test::Selected other{99};
        CY_CHECK_FALSE(fixture.world.set_sparse(entity, fixture.ids.selected, &other).has_value());
        CY_CHECK_FALSE(fixture.world.remove_sparse(entity, fixture.ids.selected).has_value());
    }
    CY_CHECK_EQ(fixture.world.firewall().refusals(), 2u);
    CY_CHECK_EQ(fixture.world.firewall().refusals_by(WriteOrigin::Inference), 2u);
    // Read defensively rather than with a REQUIRE: this tree builds doctest with
    // DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS, so a failed REQUIRE does not stop the case
    // and a bare dereference here turns a mutated build's clean FAILURE into a SIGSEGV. A test that
    // crashes instead of failing reports nothing, which fixtures.h says in as many words.
    const void* raw = fixture.world.get_sparse(entity, fixture.ids.selected);
    CY_CHECK(raw != nullptr);
    const cy::u64 tick =
        (raw == nullptr) ? 0u : static_cast<const cy::ecs::test::Selected*>(raw)->tick;
    CY_CHECK_EQ(tick, 42u);
}

CY_TEST_CASE("the lifetime and relationship doors are refused whatever components are involved") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    const Entity parent = fixture.spawn();
    const Entity child = fixture.spawn();

    {
        const WriteScope scope(WriteOrigin::Vfx, "vfx.death-burst");
        // `vfx-system` names "entity creation or destruction" itself, so these are refused with no
        // component in the question at all.
        CY_CHECK_FALSE(fixture.world.create().has_value());
        CY_CHECK_FALSE(fixture.world.destroy(child).has_value());
        // A Parent edge is hashed, so re-parenting is an authoritative write.
        CY_CHECK_FALSE(fixture.world.set_parent(child, parent).has_value());
        // `instantiate` is the bulk lifetime door, and the firewall is consulted BEFORE the block
        // is examined — so an empty block comes back PermissionDenied here and succeeds for the
        // simulation below. Comparing the two is what separates "the firewall refused it" from
        // "something refused it".
        cy::Array<Entity> spawned(allocator());
        const cy::ecs::World::ArchetypeBlock block;
        const cy::Status refused = fixture.world.instantiate(block, spawned);
        CY_CHECK_FALSE(refused.has_value());
        // Read the code only when there is one: a failed REQUIRE does not stop a case in this
        // tree's no-exceptions doctest build, and `.error()` on a value aborts.
        CY_CHECK_EQ(as_number(refused.has_value() ? cy::ErrorCode::None : refused.error().code),
                    as_number(cy::ErrorCode::PermissionDenied));
    }
    CY_CHECK_EQ(fixture.world.firewall().refusals(), 4u);
    {
        // The same call from the simulation SUCCEEDS, which is what makes the refusal above the
        // firewall's rather than the block's.
        cy::Array<Entity> spawned(allocator());
        const cy::ecs::World::ArchetypeBlock block;
        CY_CHECK(fixture.world.instantiate(block, spawned).has_value());
    }
    CY_CHECK(fixture.world.is_alive(child));
}

CY_TEST_CASE("the deferred door is checked at RECORD time, not at flush time") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    const Entity entity = fixture.spawn();

    cy::ecs::CommandBuffer buffer(fixture.world);
    {
        const WriteScope scope(WriteOrigin::Vfx, "vfx.deferred");
        // THE LAUNDERING HOLE. The flush runs under the simulation's own origin, so a check at
        // flush time would admit everything a restricted origin had recorded.
        CY_CHECK_FALSE(buffer.remove(entity, fixture.ids.position).has_value());
    }
    CY_CHECK_EQ(as_number(fixture.world.firewall().last_violation().path),
                as_number(WritePath::DeferredRecord));
    CY_REQUIRE(buffer.apply().has_value());
    CY_CHECK(fixture.world.get<cy::ecs::test::Position>(entity, fixture.ids.position) != nullptr);
}

CY_TEST_CASE("the wholesale door: Snapshot::restore is refused for a restricted origin") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    const Entity entity = fixture.spawn();

    cy::ecs::Snapshot snapshot(allocator());
    CY_REQUIRE(snapshot.capture(fixture.world).has_value());
    CY_REQUIRE(fixture.world.destroy(entity).has_value());
    CY_CHECK_FALSE(fixture.world.is_alive(entity));

    {
        const WriteScope scope(WriteOrigin::Presentation, "ui.scrub");
        CY_CHECK_FALSE(snapshot.restore(fixture.world).has_value());
    }
    CY_CHECK_FALSE(fixture.world.is_alive(entity));
    CY_CHECK_EQ(fixture.world.firewall().refusals_by(WriteOrigin::Presentation), 1u);

    // And the simulation restores it, so this is a refusal rather than a broken snapshot.
    CY_REQUIRE(snapshot.restore(fixture.world).has_value());
    CY_CHECK(fixture.world.is_alive(entity));
}

// ================================================================================================
// ARMING — the switch `vfx-system`'s delta requires so a test can be a negative control
// ================================================================================================

CY_TEST_CASE("disarming the enforcement point lets the same write land, and it is greppable") {
    Fixture fixture;
    CY_REQUIRE(fixture.build());
    const Entity entity = fixture.spawn();

    CY_CHECK(fixture.world.firewall().armed());
    fixture.world.firewall().disarm_for_negative_control("unit.ecs: the firewall's own control");
    CY_CHECK_FALSE(fixture.world.firewall().armed());
    CY_CHECK(cy::ecs::test::same_text(fixture.world.firewall().disarm_reason(),
                                      "unit.ecs: the firewall's own control"));
    {
        const WriteScope scope(WriteOrigin::Vfx, "vfx.readback");
        CY_CHECK(fixture.world.get_mut<cy::ecs::test::Position>(entity, fixture.ids.position) !=
                 nullptr);
    }
    CY_CHECK_EQ(fixture.world.firewall().refusals(), 0u);

    fixture.world.firewall().rearm();
    CY_CHECK(fixture.world.firewall().armed());
    {
        const WriteScope scope(WriteOrigin::Vfx, "vfx.readback");
        CY_CHECK(fixture.world.get_mut<cy::ecs::test::Position>(entity, fixture.ids.position) ==
                 nullptr);
    }
    CY_CHECK_EQ(fixture.world.firewall().refusals(), 1u);
}
