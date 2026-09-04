// The renderer's components, declared to the state hash. Task 4.1.2, and M2's debt 1.2 kept closed.
//
// M2's gate recorded that the state hash covered 4 of 17 subjects, because thirteen components were
// registered by name and declared to nothing. The renderer's three are registered by name for the
// same reason (see components.h), so the claim this file makes is that they were declared IN THE
// SAME CHANGE that introduced them — that M3 adds three subjects to the hash's coverage rather than
// three to its debt.
//
// The second claim is about WHAT is hashed, and it is the interesting one: a change a designer
// makes changes the hash, and a change the renderer makes does not.

#include <cy/core/determinism/state_schema.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/scene/state_schema.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

#include <string_view>

using cy::u32;
using cy::u64;
using namespace cy::rendering;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

struct Fixture {
    Fixture() noexcept : world(allocator()), tree(world), schema(allocator()) {}

    [[nodiscard]] bool start() noexcept {
        if (!world.initialize().has_value() || !tree.initialize().has_value()) {
            return false;
        }
        auto registered = RenderComponents::register_all(world);
        if (!registered) {
            return false;
        }
        components = *registered;
        return declare_render_state(schema, components).has_value();
    }

    cy::ecs::World world;
    cy::scene::SceneTree tree;
    cy::determinism::StateSchema schema;
    RenderComponents components;
};

}  // namespace

CY_TEST_CASE("all three components are declared, so none is silently outside the hash") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    CY_REQUIRE(fixture.schema.find(
                   cy::determinism::SchemaSubject{fixture.components.mesh_renderer}) != nullptr);
    CY_REQUIRE(fixture.schema.find(
                   cy::determinism::SchemaSubject{fixture.components.light_source}) != nullptr);
    CY_REQUIRE(fixture.schema.find(cy::determinism::SchemaSubject{fixture.components.camera}) !=
               nullptr);
}

CY_TEST_CASE("a handle is declared and not hashed, because its value is allocation order") {
    // The one classification worth a case of its own. A `MeshHandle` is a slot index and a
    // generation the render server assigns as assets load: two runs that load the same assets in a
    // different order give the same mesh different handles, and a hash over one would report a
    // divergence between two identical worlds. Declared `Derived` — present in the schema, absent
    // from the hash — which is a different fact from "not declared" and is reported separately.
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    const cy::determinism::SubjectSchema* mesh =
        fixture.schema.find(cy::determinism::SchemaSubject{fixture.components.mesh_renderer});
    CY_REQUIRE(mesh != nullptr);
    CY_CHECK_EQ(mesh->field_count, 15U);
    // Eight of the fifteen are the handles and the bounds (`Derived`) and one is the importance
    // (`Presentation`); the remaining six are what a designer sets.
    CY_CHECK_EQ(mesh->hashed_field_count, 6U);
}

CY_TEST_CASE("a camera is declared and contributes nothing, which is not the same as undeclared") {
    // `simulation-and-determinism` names "camera" in its own list of presentation state. Hashing it
    // would make two clients watching one match from different angles diverge by construction.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::determinism::SubjectSchema* camera =
        fixture.schema.find(cy::determinism::SchemaSubject{fixture.components.camera});
    CY_REQUIRE(camera != nullptr);
    CY_CHECK_GT(camera->field_count, 0U);
    CY_CHECK_EQ(camera->hashed_field_count, 0U);
}

CY_TEST_CASE("a light is authoritative state, every field of it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::determinism::SubjectSchema* light =
        fixture.schema.find(cy::determinism::SchemaSubject{fixture.components.light_source});
    CY_REQUIRE(light != nullptr);
    CY_CHECK_EQ(light->field_count, light->hashed_field_count);
    CY_CHECK_EQ(light->field_count, 11U);
}

CY_TEST_CASE("declaring over an unregistered set is refused rather than reporting coverage") {
    cy::determinism::StateSchema schema(allocator());
    RenderComponents none;
    const cy::Status declared = declare_render_state(schema, none);
    CY_REQUIRE_FALSE(declared.has_value());
    CY_CHECK(declared.error().code == cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("what a designer changes is hashed and what the renderer computes is not") {
    // The field-level statement of the claim above, read off the schema. The walk that folds these
    // fields into a number is `cy::runtime::hash_world`, which is layer 5 and cannot be reached
    // from here — so what this asserts is the DECLARATION, which is the thing this module owns and
    // the thing M2's debt was about. The end-to-end claim ("a divergence in visibility changes the
    // hash") belongs beside the other modules' in tests/integration/.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const cy::determinism::SubjectSchema* mesh =
        fixture.schema.find(cy::determinism::SchemaSubject{fixture.components.mesh_renderer});
    CY_REQUIRE(mesh != nullptr);

    struct Expected {
        const char* name;
        cy::determinism::SimulationClass classification;
    };
    const Expected expected[] = {
        {"mesh", cy::determinism::SimulationClass::Derived},
        {"material", cy::determinism::SimulationClass::Derived},
        {"importance", cy::determinism::SimulationClass::Presentation},
        {"visible", cy::determinism::SimulationClass::Authoritative},
        {"layer_mask", cy::determinism::SimulationClass::Authoritative},
        {"lod_bias", cy::determinism::SimulationClass::Authoritative},
        {"casts_shadow", cy::determinism::SimulationClass::Authoritative},
    };
    for (const Expected& want : expected) {
        bool found = false;
        for (const cy::determinism::StateField& field : fixture.schema.fields_of(*mesh)) {
            if (std::string_view(field.name) != std::string_view(want.name)) {
                continue;
            }
            found = true;
            CY_CHECK(field.classification == want.classification);
        }
        CY_CHECK(found);
    }

    // And the firewall on the field itself, which is the other half of the same statement: the
    // value is written with a presentation witness because an authoritative one does not compile.
    cy::ecs::ComponentTypeId components[1] = {fixture.components.mesh_renderer};
    const auto entity =
        fixture.world.create(cy::Span<const cy::ecs::ComponentTypeId>(components, 1));
    CY_REQUIRE(entity.has_value());
    auto* renderer = fixture.world.get_mut<MeshRenderer>(*entity, fixture.components.mesh_renderer);
    CY_REQUIRE(renderer != nullptr);
    renderer->importance.write(cy::determinism::PresentationContext{}, 42.0F);
    CY_CHECK_NEAR(renderer->importance.read(cy::determinism::PresentationContext{}), 42.0F, 1e-6F);
}
