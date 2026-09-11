// M9 TASK 1.4b — ARMING THE FIREWALL M8.c BUILT, AND REPORTING WHAT IT ACTUALLY GUARDS.
//
// M8.c's closing gate recorded that the runtime firewall is armed and guards nothing: the only
// callers of `declare`/`declare_from_reflection` in the tree are test files, so a game built on
// this engine has a firewall that has been told nothing is authoritative.
//
// ================================================================================================
// WHAT THIS SUITE IS AND WHAT IT IS NOT
// ================================================================================================
//
// It is the mechanism's test — the derivation, the by-hand declarations, the refusals, and the
// report's arithmetic — over a registry of TWELVE components rather than three, chosen so that the
// guarded fraction is a number between the extremes and a report that computed it wrongly could not
// pass by accident.
//
// **It is not the check task 1.4b asks for**, and saying so is the point. The task's own words: the
// check is "a startup report (`guarded_count` beside `AuthorityDerivationReport::underived`)
// asserted by an artefact, not by a unit test with three components in it". A test that builds its
// own twelve components and then asserts a percentage over them proves the arithmetic and nothing
// about the engine's real component set — which is exactly the state M8.c found. The artefact is
// `samples/09-multiplayer` (M9 section 6, a different phase); `format_arming_report()` is the line
// it prints and this suite is what makes sure the line is not a lie.
//
// `integration`, because every case builds a world.

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/gameplay/firewall_arming.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>

namespace {

using namespace cy;
using namespace cy::gameplay;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Ecs);
}

struct Replicated32 {
    i32 value = 0;
};

[[nodiscard]] reflect::FieldAttributes replicated_attribute() noexcept {
    reflect::FieldAttributes attributes;
    attributes.declared = reflect::AttributeKind::Replicated;
    attributes.replicated = reflect::ReplicatedAttribute{"quantised", "bits=16", "owner"};
    return attributes;
}

[[nodiscard]] reflect::FieldAttributes persistence_attribute(
    reflect::PersistenceKind kind) noexcept {
    reflect::FieldAttributes attributes;
    attributes.declared = reflect::AttributeKind::Persistence;
    attributes.persistence = kind;
    return attributes;
}

/// One reflected component, built at a distinct type id. Storage is static per call site, which is
/// what a generated descriptor is.
template <int kSlot>
[[nodiscard]] const reflect::TypeInfo& component_type(
    const char* name, reflect::FieldAttributes attributes) noexcept {
    static reflect::FieldInfo field;
    static reflect::TypeInfo info;
    field.name = "value";
    field.id = reflect::FieldId(9500 + (kSlot * 10) + 1);
    field.kind = reflect::FieldKind::I32;
    field.offset = 0;
    field.size = static_cast<u32>(sizeof(i32));
    field.attributes = attributes;
    info.name = name;
    info.id = reflect::TypeId(9500 + (kSlot * 10));
    info.size = static_cast<u32>(sizeof(Replicated32));
    info.alignment = static_cast<u32>(alignof(Replicated32));
    info.trivially_relocatable = true;
    info.fields = &field;
    info.field_count = 1;
    return info;
}

/// Ten reflected components — four replicated, three authoritative by persistence, three that
/// derive nothing — plus two registered with `register_builtin()` and no reflection at all. That
/// last pair is not a contrivance: the scene's twelve built-ins are registered exactly that way,
/// and `declare_from_reflection()` can say nothing about them.
///
/// `World::initialize()` registers two more of its own — the relationship components — so the
/// registry ends up at FOURTEEN, and the expectations below say fourteen. That number is read off a
/// real `World` rather than assumed, which is why the first case asserts it before anything else:
/// if the ECS registers a third built-in tomorrow, this suite says so instead of quietly computing
/// a percentage of the wrong denominator.
struct Registry {
    ecs::World world{allocator()};
    ecs::ComponentTypeId physics_owned = ecs::kInvalidComponent;
    ecs::ComponentTypeId parent_edge = ecs::kInvalidComponent;

    [[nodiscard]] bool build() noexcept {
        if (!world.initialize().has_value()) {
            return false;
        }
        const bool reflected =
            world.components()
                .register_reflected(component_type<0>("Health", replicated_attribute()))
                .has_value() &&
            world.components()
                .register_reflected(component_type<1>("Ammunition", replicated_attribute()))
                .has_value() &&
            world.components()
                .register_reflected(component_type<2>("Team", replicated_attribute()))
                .has_value() &&
            world.components()
                .register_reflected(component_type<3>("Score", replicated_attribute()))
                .has_value() &&
            world.components()
                .register_reflected(component_type<4>(
                    "Placement", persistence_attribute(reflect::PersistenceKind::RuntimeState)))
                .has_value() &&
            world.components()
                .register_reflected(component_type<5>(
                    "Inventory", persistence_attribute(reflect::PersistenceKind::PersistentState)))
                .has_value() &&
            world.components()
                .register_reflected(component_type<6>(
                    "Objective", persistence_attribute(reflect::PersistenceKind::Authoring)))
                .has_value() &&
            world.components()
                .register_reflected(component_type<7>(
                    "Flash", persistence_attribute(reflect::PersistenceKind::Derived)))
                .has_value() &&
            world.components()
                .register_reflected(component_type<8>(
                    "Trail", persistence_attribute(reflect::PersistenceKind::Derived)))
                .has_value() &&
            world.components()
                .register_reflected(component_type<9>(
                    "Bloom", persistence_attribute(reflect::PersistenceKind::Derived)))
                .has_value();
        if (!reflected) {
            return false;
        }
        auto owned = world.components().register_builtin("RigidBody", 16, 8);
        auto parent = world.components().register_builtin("Attachment", 8, 8);
        if (!owned.has_value() || !parent.has_value()) {
            return false;
        }
        physics_owned = *owned;
        parent_edge = *parent;
        return true;
    }
};

}  // namespace

CY_TEST_CASE("gameplay: arming derives what reflection says and counts what it could not") {
    Registry registry;
    CY_REQUIRE(registry.build());
    CY_REQUIRE_EQ(registry.world.components().size(), 14U);

    // Nothing has declared anything yet: this is the state M8.c found, and the number that says so
    // is zero.
    CY_REQUIRE_EQ(registry.world.firewall().guarded_count(), 0U);

    FirewallArmingReport report;
    CY_REQUIRE(arm_write_firewall(registry.world.components(), registry.world.firewall(),
                                  Span<const ManualAuthority>(), report)
                   .has_value());

    CY_CHECK_EQ(report.components_registered, 14U);
    CY_CHECK_EQ(report.derived_from_replication, 4U);
    // Placement (RuntimeState), Inventory (PersistentState) and Objective (Authoring) all map to an
    // authoritative simulation class through `determinism::class_of`.
    CY_CHECK_EQ(report.derived_from_persistence, 3U);
    CY_CHECK_EQ(report.guarded, 7U);
    // Three presentation components, the two built-ins registered here with no reflection behind
    // them, and the ECS's own two relationship components — which are exactly the case
    // `declare_from_reflection()`'s comment names as underivable.
    CY_CHECK_EQ(report.underived, 7U);
    CY_CHECK(report.armed);
    CY_CHECK_EQ(report.guarded_percent(), 50U);  // 7 of 14
}

CY_TEST_CASE("gameplay: what reflection cannot derive is declared by hand, with a reason") {
    Registry registry;
    CY_REQUIRE(registry.build());

    // `PhysicsOwned` is not derivable at all — nothing in reflection says "the physics step owns
    // this" — and a built-in registered by name has no `TypeInfo` to read. Both are real
    // categories, not contrivances.
    const ManualAuthority by_hand[] = {
        {registry.physics_owned, ecs::ComponentAuthority::PhysicsOwned,
         "the physics step owns this body's transform; nothing in reflection can say so"},
        {registry.parent_edge, ecs::ComponentAuthority::Authoritative,
         "a Parent edge is hashed, so re-parenting is an authoritative write"},
    };

    FirewallArmingReport report;
    CY_REQUIRE(arm_write_firewall(registry.world.components(), registry.world.firewall(),
                                  Span<const ManualAuthority>(by_hand, 2), report)
                   .has_value());
    CY_CHECK_EQ(report.declared_by_hand, 2U);
    CY_CHECK_EQ(report.guarded, 9U);
    // Five, not seven: the two declared by hand are `already_declared` as far as the derivation is
    // concerned, so they leave the underived count. That is the right answer and it is the reason
    // both numbers are reported rather than one — "underived" means "reflection could not say", and
    // a component somebody declared by hand is no longer in that state.
    CY_CHECK_EQ(report.underived, 5U);
    CY_CHECK_EQ(report.already_declared, 2U);
    CY_CHECK_EQ(report.guarded_percent(), 64U);  // 9 of 14
    CY_CHECK(registry.world.firewall().guards(registry.physics_owned));
    CY_CHECK(registry.world.firewall().guards(registry.parent_edge));
}

CY_TEST_CASE("gameplay: a by-hand declaration with no reason or no component is refused") {
    Registry registry;
    CY_REQUIRE(registry.build());
    FirewallArmingReport report;

    const ManualAuthority no_reason[] = {
        {registry.physics_owned, ecs::ComponentAuthority::PhysicsOwned, ""}};
    CY_CHECK_FALSE(arm_write_firewall(registry.world.components(), registry.world.firewall(),
                                      Span<const ManualAuthority>(no_reason, 1), report)
                       .has_value());

    // A typo naming a component that is not registered. Silently doing nothing here is a hole in
    // the firewall that nobody can see, which is the whole failure mode this task is about.
    const ManualAuthority typo[] = {
        {900, ecs::ComponentAuthority::Authoritative, "a component this registry does not have"}};
    CY_CHECK_FALSE(arm_write_firewall(registry.world.components(), registry.world.firewall(),
                                      Span<const ManualAuthority>(typo, 1), report)
                       .has_value());
}

CY_TEST_CASE("gameplay: the startup line puts guarded and underived side by side") {
    Registry registry;
    CY_REQUIRE(registry.build());
    FirewallArmingReport report;
    CY_REQUIRE(arm_write_firewall(registry.world.components(), registry.world.firewall(),
                                  Span<const ManualAuthority>(), report)
                   .has_value());

    char line[kArmingReportBuffer] = {};
    const usize written = format_arming_report(line, sizeof(line), report);
    CY_REQUIRE(written > usize{0});
    CY_CHECK(std::strstr(line, "armed=yes") != nullptr);
    CY_CHECK(std::strstr(line, "guarded=7/14") != nullptr);
    // The pair is the whole of the check. "armed: yes" beside "guarded: 0" is the state M8.c found,
    // and it has to be readable at a glance rather than inferred from a second line nobody prints.
    CY_CHECK(std::strstr(line, "underived=7") != nullptr);

    // A buffer too small produces nothing, which the caller must treat as a failure. A truncated
    // report would be a report that says the firewall guards 7 of 1 components.
    char tiny[16] = {};
    CY_CHECK_EQ(format_arming_report(tiny, sizeof(tiny), report), usize{0});
    CY_CHECK_EQ(format_arming_report(nullptr, kArmingReportBuffer, report), usize{0});
}

CY_TEST_CASE(
    "gameplay: a report over a bare world guards nothing, and says nothing rather than all") {
    // "we guarded everything there was" and "there was nothing" must not read alike. A percentage
    // that reported 100% for a world with no game components in it is how a startup line becomes
    // reassuring noise.
    //
    // A bare `World::initialize()` is not empty — it registers its own relationship components —
    // and neither of them derives anything, which is exactly the point: an engine that shipped with
    // this line would print `guarded=0/2 (0%)` and a reader would know at once that nothing had
    // declared itself.
    ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    FirewallArmingReport report;
    CY_REQUIRE(arm_write_firewall(world.components(), world.firewall(),
                                  Span<const ManualAuthority>(), report)
                   .has_value());
    CY_CHECK_EQ(report.components_registered, 2U);
    CY_CHECK_EQ(report.underived, 2U);
    CY_CHECK_EQ(report.guarded, 0U);
    CY_CHECK_EQ(report.guarded_percent(), 0U);

    char line[kArmingReportBuffer] = {};
    CY_REQUIRE(format_arming_report(line, sizeof(line), report) > usize{0});
    CY_CHECK(std::strstr(line, "guarded=0/2 (0%)") != nullptr);

    // The completely empty case, which cannot arise from `World::initialize()` but is what the
    // percentage's zero-denominator branch is for.
    FirewallArmingReport nothing;
    CY_CHECK_EQ(nothing.guarded_percent(), 0U);
}
