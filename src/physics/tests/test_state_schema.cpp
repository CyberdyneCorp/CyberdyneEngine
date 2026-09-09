// Physics' components in the state hash. M8.a task 4.1.
//
// M2's carried-forward debt, and the reason `src/rendering/scene/state_schema.h` exists beside its
// components: **a component registered by name is invisible to the state hash unless something
// declares a schema for it.** Registering eight physics components and declaring nothing would mean
// a body's mass, its damping and its collider's dimensions could all differ between two peers while
// the hash said the worlds agreed.
//
// The claim these cases make is not "a schema exists". It is the two-sided one:
//
//   * a change an author makes CHANGES the fold — mass, and a collider's half extents;
//   * a change the backend makes DOES NOT — the body handle, which is a slot number two backends
//     legitimately disagree about while simulating identically.
//
// The fold is done here with `StateHashTree` and `hash_field`, which are layer 0 and are the same
// two the runtime's `hash_world` walk uses per component. The walk itself is layer 5 and out of
// reach from a suite at layer 4, exactly as the renderer's equivalent file records.

#include <cy/core/determinism/classification.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/physics/state_schema.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::physics;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// A world with physics' components registered and their schema declared and frozen.
struct Fixture {
    Fixture() noexcept : world(allocator()), schema(allocator()) {}

    [[nodiscard]] bool start() noexcept {
        if (!world.initialize().has_value()) {
            return false;
        }
        const auto registered = PhysicsComponents::register_all(world);
        if (!registered) {
            return false;
        }
        components = *registered;
        if (!declare_physics_state(schema, components).has_value()) {
            return false;
        }
        schema.freeze();
        return true;
    }

    /// Fold one component value the way the runtime's walk folds it: every declared field whose
    /// classification participates in the hash, and no others.
    [[nodiscard]] u64 fold(cy::ecs::ComponentTypeId component, const void* value) const noexcept {
        const cy::determinism::SubjectSchema* subject =
            schema.find(cy::determinism::SchemaSubject{component});
        if (subject == nullptr) {
            return 0;
        }
        cy::determinism::StateHashTree tree(allocator());
        if (!tree.begin(cy::determinism::HashLevel::Component, component, subject->name)) {
            return 0;
        }
        for (const cy::determinism::StateField& field : schema.fields_of(*subject)) {
            if (cy::determinism::participation_of(field.classification).hashed) {
                cy::determinism::hash_field(tree, field, value);
            }
        }
        if (!tree.end()) {
            return 0;
        }
        return tree.root_hash();
    }

    cy::ecs::World world;
    cy::determinism::StateSchema schema;
    PhysicsComponents components;
};

}  // namespace

CY_TEST_CASE("declaring over an unregistered set is refused rather than reporting coverage") {
    cy::determinism::StateSchema schema(allocator());
    const PhysicsComponents none;
    const cy::Status declared = declare_physics_state(schema, none);
    CY_REQUIRE_FALSE(declared.has_value());
    CY_CHECK(declared.error().code == cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("all eight components are declared, and each names its fields") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_CHECK_EQ(fixture.schema.subject_count(), 8U);

    const cy::ecs::ComponentTypeId all[8] = {
        fixture.components.rigid_body,     fixture.components.static_body,
        fixture.components.kinematic_body, fixture.components.collider,
        fixture.components.trigger,        fixture.components.material,
        fixture.components.joint,          fixture.components.character_body};
    for (const cy::ecs::ComponentTypeId component : all) {
        const cy::determinism::SubjectSchema* subject =
            fixture.schema.find(cy::determinism::SchemaSubject{component});
        CY_REQUIRE(subject != nullptr);
        CY_CHECK_GT(subject->field_count, 0U);
    }

    // A static body is declared and folds NOTHING: its one field is a runtime handle. That is a
    // different fact from "not declared", and the distinction is what `hashed_field_count` is for.
    const cy::determinism::SubjectSchema* immovable =
        fixture.schema.find(cy::determinism::SchemaSubject{fixture.components.static_body});
    CY_REQUIRE(immovable != nullptr);
    CY_CHECK_EQ(immovable->field_count, 1U);
    CY_CHECK_EQ(immovable->hashed_field_count, 0U);

    // A rigid body folds twelve of its thirteen: everything but the handle.
    const cy::determinism::SubjectSchema* rigid =
        fixture.schema.find(cy::determinism::SchemaSubject{fixture.components.rigid_body});
    CY_REQUIRE(rigid != nullptr);
    CY_CHECK_EQ(rigid->field_count, 13U);
    CY_CHECK_EQ(rigid->hashed_field_count, 12U);
}

CY_TEST_CASE("a body's mass is folded and its runtime handle is not") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    RigidBody light;
    light.mass = 1.0F;
    const u64 base = fixture.fold(fixture.components.rigid_body, &light);
    CY_CHECK_NE(base, 0U);

    // A DIFFERENT MASS IS A DIFFERENT WORLD. Without this schema it would not have been.
    RigidBody heavy = light;
    heavy.mass = 2.0F;
    CY_CHECK_NE(fixture.fold(fixture.components.rigid_body, &heavy), base);

    RigidBody damped = light;
    damped.linear_damping = 0.5F;
    CY_CHECK_NE(fixture.fold(fixture.components.rigid_body, &damped), base);

    RigidBody locked = light;
    locked.locked_axes = kLockPlaneXY;
    CY_CHECK_NE(fixture.fold(fixture.components.rigid_body, &locked), base);

    // A DIFFERENT HANDLE IS THE SAME WORLD. Two peers running two backends allocate different slot
    // numbers for the same body and simulate identically; a hash that folded the handle would call
    // that a divergence on the first tick.
    RigidBody created = light;
    created.body = BodyHandle::from_slot(17, 3);
    CY_CHECK_EQ(fixture.fold(fixture.components.rigid_body, &created), base);
}

CY_TEST_CASE("a collider's dimensions are folded and its cached shape handle is not") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    Collider small;
    small.shape.type = ShapeType::Box;
    small.shape.half_extents = cy::Vec3{0.5F, 0.5F, 0.5F};
    const u64 base = fixture.fold(fixture.components.collider, &small);
    CY_CHECK_NE(base, 0U);

    Collider large = small;
    large.shape.half_extents = cy::Vec3{0.5F, 0.6F, 0.5F};
    CY_CHECK_NE(fixture.fold(fixture.components.collider, &large), base);

    Collider sphere = small;
    sphere.shape.type = ShapeType::Sphere;
    CY_CHECK_NE(fixture.fold(fixture.components.collider, &sphere), base);

    Collider filtered = small;
    filtered.filter.mask = 0x0000'0003U;
    CY_CHECK_NE(fixture.fold(fixture.components.collider, &filtered), base);

    Collider cached = small;
    cached.handle = ShapeHandle::from_slot(4, 2);
    CY_CHECK_EQ(fixture.fold(fixture.components.collider, &cached), base);
}
