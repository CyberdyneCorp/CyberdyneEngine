// Node types as data: templates, composition, and the catalogue's honest state at M2. Task 3.1.4.

#include <cy/test/test.h>

#include <cy/scene/node_template.h>
#include <cy/scene/tree.h>

#include "fixtures.h"

using cy::scene::test::Fixture;

CY_TEST_CASE("a project template is a data declaration, registered at startup") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto health =
        fixture.world.components().register_reflected(cy::scene::test::health_type());
    const auto loudness =
        fixture.world.components().register_reflected(cy::scene::test::loudness_type());
    CY_REQUIRE(health.has_value());
    CY_REQUIRE(loudness.has_value());

    // No class, no factory, no engine change: a name, a component list and their defaults.
    static constexpr cy::scene::test::Health kDefaultHealth{250};
    const cy::scene::TemplateComponent components[] = {
        {"cy::scene::test::Health", &kDefaultHealth, sizeof(kDefaultHealth)},
        {"cy::scene::test::Loudness", nullptr, 0},
    };
    cy::scene::NodeTemplateDesc desc;
    desc.name = "NoisyUnit";
    desc.components = cy::Span<const cy::scene::TemplateComponent>(components, 2);
    CY_REQUIRE(fixture.tree.templates().add(fixture.world, desc).has_value());

    const cy::Name name = cy::Name::intern("NoisyUnit");
    CY_CHECK(fixture.tree.templates().status(name).instantiable);

    const auto node = fixture.tree.create_node(cy::Name::intern("Unit"), fixture.tree.root(), name);
    CY_REQUIRE(node.has_value());
    CY_CHECK(node->has(*health));
    CY_CHECK(node->has(*loudness));
    // The declared default is what the instance starts from.
    CY_CHECK_EQ(node->get_as<cy::scene::test::Health>(*health)->value, 250);
    // A component with no declared default starts from its own zero value.
    CY_CHECK_EQ(node->get_as<cy::scene::test::Loudness>(*loudness)->decibels, 0.0F);

    // And it is still a node: the template adds components, it does not replace the base ones.
    CY_CHECK(node->has(fixture.tree.components().local_transform));
    CY_CHECK(node->name() == cy::Name::intern("Unit"));

    // Registering the same name twice is a project configuration error, not a merge.
    CY_CHECK_FALSE(fixture.tree.templates().add(fixture.world, desc).has_value());
}

CY_TEST_CASE("composition over inheritance: two components on one entity, no new type") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto health =
        fixture.world.components().register_reflected(cy::scene::test::health_type());
    const auto loudness =
        fixture.world.components().register_reflected(cy::scene::test::loudness_type());
    CY_REQUIRE(loudness.has_value());

    // "A designer wants a light that also emits sound": both components on one entity, no class.
    cy::scene::Node node =
        cy::scene::test::make_child(fixture.tree, fixture.tree.root(), "LoudLight");
    CY_REQUIRE(node.add(*health).has_value());
    CY_REQUIRE(node.add(*loudness).has_value());
    CY_CHECK(node.has(*health));
    CY_CHECK(node.has(*loudness));
}

CY_TEST_CASE("the shipped catalogue is declared, and reports which entries this world can build") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    // Every template the specification lists is declared, as data.
    CY_CHECK_EQ(cy::scene::builtin_templates().size(), 23U);
    CY_CHECK_EQ(fixture.tree.templates().size(), 23U);

    // Spatial grouping names only components this module owns, so it is live at M2 and is what a
    // bare node is.
    const cy::scene::NodeTemplateStatus spatial =
        fixture.tree.templates().status(cy::Name::intern(cy::scene::kSpatialTemplate));
    CY_CHECK(spatial.instantiable);

    // The rest name components their own milestones register. They are declared and report
    // themselves as not instantiable rather than pretending — the honest state, and the one
    // `NodeTemplateRegistry::bindings_of` refuses on.
    const cy::scene::NodeTemplateStatus mesh =
        fixture.tree.templates().status(cy::Name::intern(cy::scene::kMeshRendererTemplate));
    CY_CHECK_FALSE(mesh.instantiable);
    CY_CHECK_EQ(mesh.missing_count, 1U);
    CY_CHECK_FALSE(fixture.tree
                       .create_node(cy::Name::intern("Mesh"), fixture.tree.root(),
                                    cy::Name::intern(cy::scene::kMeshRendererTemplate))
                       .has_value());

    cy::Array<cy::scene::NodeTemplateStatus> all(cy::scene::test::allocator());
    CY_REQUIRE(fixture.tree.templates().statuses(all).has_value());
    CY_CHECK_EQ(all.size(), 23U);
    cy::u32 live = 0;
    for (const cy::scene::NodeTemplateStatus& status : all) {
        live += status.instantiable ? 1U : 0U;
    }
    CY_CHECK_EQ(live, 1U);
}

CY_TEST_CASE("a UI document attaches at one node and its elements are not entities") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    // "Individual UI elements SHALL NOT be node templates": the catalogue holds one UI entry, the
    // host, and nothing that names a button, a label or a panel.
    cy::u32 ui_templates = 0;
    for (const cy::scene::NodeTemplateDesc& desc : cy::scene::builtin_templates()) {
        for (const cy::scene::TemplateComponent& component : desc.components) {
            const std::string_view name(component.component_name);
            ui_templates += name.starts_with("cy::ui::") ? 1U : 0U;
        }
    }
    CY_CHECK_EQ(ui_templates, 1U);
    CY_CHECK(fixture.tree.templates().contains(cy::Name::intern(cy::scene::kUiHostTemplate)));
}
