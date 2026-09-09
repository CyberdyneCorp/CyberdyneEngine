// The renderer's components are reflected, and what that makes possible. M8.b task 11.3.
//
// M8.a's artefact report: "the two entities it authored are drawn as unit boxes because the mesh a
// primitive references reaches no renderer". Four things had to be true for that sentence to stop
// being true, and each is a case below:
//
//   1. the three components have a `reflect::TypeId` — before this they were `register_builtin`
//      names, invisible to every lookup by type;
//   2. `MeshRenderer` carries an ASSET reference and not only a runtime handle, because a handle is
//      a slot index and a file cannot hold one;
//   3. the engine's authoring schema presents that reference under the name and in the value kind
//      the editor's own mesh binding looks for, so a `.cyworld`'s `MeshRenderer` resolves here;
//   4. the node templates that name those components are INSTANTIABLE in a world that registered
//      them — five of them named a component that never existed.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/registry.h>
#include <cy/rendering/scene/components.h>
#include <cy/rendering/scene/node_templates.h>
#include <cy/scene/node_template.h>
#include <cy/scene/serialization/authoring_schema.h>
#include <cy/scene/tree.h>
#include <cy/test/test.h>

#include <string_view>

using cy::u32;
using namespace cy::rendering;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// A world with the scene layer's components and the renderer's, which is the pair every case here
/// needs: the templates are the scene's and the components they name are the renderer's.
struct Fixture {
    Fixture() noexcept : world(allocator()), tree(world) {
        ok = world.initialize().has_value() && tree.initialize().has_value();
        if (!ok) {
            return;
        }
        auto registered = RenderComponents::register_all(world);
        ok = registered.has_value();
        if (ok) {
            render = *registered;
        }
        // THE ORDER EVERY HOST ACTUALLY USES: the tree first, because the renderer extracts from
        // one, then the renderer's components. A template binds at registration, so the catalogue
        // resolved its renderer components before they existed — this is the call that gives them
        // their component and their defaults and rebinds.
        ok = ok && declare_render_templates(tree).has_value();
    }

    cy::ecs::World world;
    cy::scene::SceneTree tree;
    RenderComponents render;
    bool ok = false;
};

/// The status of one shipped template in this world.
[[nodiscard]] cy::scene::NodeTemplateStatus status_of(const Fixture& fixture,
                                                      const char* name) noexcept {
    return fixture.tree.templates().status(cy::Name::intern(name));
}

}  // namespace

CY_TEST_CASE("the renderer's three components have a reflected type") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok);

    // Registration is what puts them in the process-wide registry — see components.cpp for why it
    // happens there and not in a start-up path.
    const cy::reflect::TypeRegistry& registry = cy::reflect::default_registry();
    const cy::reflect::TypeInfo* mesh = registry.find(kMeshRendererComponentName);
    const cy::reflect::TypeInfo* light = registry.find(kLightSourceComponentName);
    const cy::reflect::TypeInfo* camera = registry.find(kCameraComponentName);
    CY_REQUIRE(mesh != nullptr);
    CY_REQUIRE(light != nullptr);
    CY_REQUIRE(camera != nullptr);
    CY_CHECK(mesh->id.valid());
    CY_CHECK_EQ(mesh->size, static_cast<u32>(sizeof(MeshRenderer)));

    // And the world registered them THROUGH that description rather than by name and size, which is
    // what makes a component's fields reachable from its id.
    const cy::ecs::ComponentInfo* info =
        fixture.world.components().find(kMeshRendererComponentName);
    CY_REQUIRE(info != nullptr);
    CY_CHECK_EQ(info->id, fixture.render.mesh_renderer);
}

CY_TEST_CASE("a mesh renderer names an asset, and separately a handle") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok);
    const cy::reflect::TypeInfo* mesh =
        cy::reflect::default_registry().find(kMeshRendererComponentName);
    CY_REQUIRE(mesh != nullptr);

    // The asset reference is reflected, as two lanes, because reflection describes fixed-width
    // scalars and an asset id is 128 bits.
    bool has_high = false;
    bool has_low = false;
    bool has_handle = false;
    for (u32 index = 0; index < mesh->field_count; ++index) {
        const std::string_view name = mesh->fields[index].name;
        has_high = has_high || name == "mesh.high";
        has_low = has_low || name == "mesh.low";
        has_handle = has_handle || name == "mesh_handle";
    }
    CY_CHECK(has_high);
    CY_CHECK(has_low);
    // THE HANDLE IS NOT REFLECTED and must not become so: "handles are runtime-only and are never
    // serialized", and a schema carrying one would invite a file to hold a slot index.
    CY_CHECK_FALSE(has_handle);

    // The two spellings of an asset id are the same 128 bits.
    const cy::AssetId id{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    const AssetRef reference = AssetRef::from_asset_id(id);
    CY_CHECK(reference.to_asset_id() == id);
    CY_CHECK_FALSE(reference.is_nil());
    CY_CHECK(AssetRef{}.is_nil());
}

CY_TEST_CASE("the authoring schema presents the mesh reference as the editor's own binding") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok);

    cy::scene::serialization::AuthoringSchema schema(allocator());
    CY_REQUIRE(
        cy::scene::serialization::build_authoring_schema(cy::reflect::default_registry(), schema)
            .has_value());

    // `cy_editor_services::primitives::MeshBinding` looks for a type named `MeshRenderer` with a
    // field named `mesh`, and writes a `Text` value into it. All three are asserted, because all
    // three are the contract: a schema carrying the type but not the field would make the editor
    // declare a SECOND `MeshRenderer` of its own and the authored mesh would go back to reaching
    // nothing.
    const cy::scene::serialization::AuthoringType* found = nullptr;
    for (const cy::scene::serialization::AuthoringType& type : schema.types()) {
        if (type.name == "MeshRenderer") {
            found = &type;
        }
    }
    CY_REQUIRE(found != nullptr);

    const cy::scene::serialization::AuthoringField* mesh = nullptr;
    for (const cy::scene::serialization::AuthoringField& field : found->fields) {
        if (field.name == "mesh") {
            mesh = &field;
        }
    }
    CY_REQUIRE(mesh != nullptr);
    CY_CHECK_EQ(mesh->kind, cy::scene::serialization::AuthoringKind::Text);
    // One field, not two: the pair of lanes is grouped, and the group keeps the identifier of the
    // first lane so an override naming it still names the same bytes.
    u32 lanes = 0;
    for (const cy::scene::serialization::AuthoringField& field : found->fields) {
        lanes += (field.name == "mesh" || field.name == "high" || field.name == "low") ? 1U : 0U;
    }
    CY_CHECK_EQ(lanes, 1U);
}

CY_TEST_CASE("the shipped templates that name the renderer's components are instantiable") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok);

    // Before task 11.3 every one of these named `cy::render::…` — the render SERVER's namespace —
    // and reported itself not instantiable in a world that had registered all three.
    CY_CHECK(status_of(fixture, cy::scene::kMeshRendererTemplate).instantiable);
    CY_CHECK(status_of(fixture, cy::scene::kCameraTemplate).instantiable);
    CY_CHECK(status_of(fixture, cy::scene::kDirectionalLightTemplate).instantiable);
    CY_CHECK(status_of(fixture, cy::scene::kPointLightTemplate).instantiable);
    CY_CHECK(status_of(fixture, cy::scene::kSpotLightTemplate).instantiable);

    // An area light is not a kind this engine has, and the template says so rather than defaulting
    // to a number `render::LightKind` does not define.
    const cy::scene::NodeTemplateStatus area = status_of(fixture, cy::scene::kAreaLightTemplate);
    CY_CHECK_FALSE(area.instantiable);
    CY_CHECK_EQ(area.missing_count, 1U);

    // And a created node really carries the component, which is the whole point: an authored mesh
    // node with no `MeshRenderer` on it is what M8.a photographed.
    const cy::Expected<cy::scene::Node, cy::Error> node =
        fixture.tree.create_node(cy::Name::intern("Mesh"), fixture.tree.root(),
                                 cy::Name::intern(cy::scene::kMeshRendererTemplate));
    CY_REQUIRE(node.has_value());
    const auto* renderer =
        fixture.world.get<MeshRenderer>(node->entity(), fixture.render.mesh_renderer);
    CY_REQUIRE(renderer != nullptr);
    CY_CHECK(renderer->mesh.is_nil());
    // Visible, casting and receiving: the component's own defaults, which a zeroed component is
    // not. `render::kInstanceVisible` is bit zero and snapshot.h says so in capitals — "ZERO IS
    // 'NOT VISIBLE, CASTS NOTHING'".
    CY_CHECK(renderer->visible);
    CY_CHECK(renderer->casts_shadow);
}

CY_TEST_CASE("the four light templates are four defaults over one component") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok);

    struct Case {
        const char* template_name;
        cy::render::LightKind kind;
    };
    const Case cases[] = {
        {cy::scene::kDirectionalLightTemplate, cy::render::LightKind::Directional},
        {cy::scene::kPointLightTemplate, cy::render::LightKind::Point},
        {cy::scene::kSpotLightTemplate, cy::render::LightKind::Spot},
    };
    for (const Case& one : cases) {
        const cy::Expected<cy::scene::Node, cy::Error> node =
            fixture.tree.create_node(cy::Name::intern(one.template_name), fixture.tree.root(),
                                     cy::Name::intern(one.template_name));
        CY_REQUIRE(node.has_value());
        const auto* light =
            fixture.world.get<LightSource>(node->entity(), fixture.render.light_source);
        CY_REQUIRE(light != nullptr);
        CY_CHECK_EQ(light->kind, one.kind);
        // AND THE REST OF THE COMPONENT IS ITS OWN DEFAULT, NOT ZERO. A zeroed `LightSource` is a
        // disabled light of zero intensity — a node that exists and lights nothing — which is what
        // the catalogue alone can produce and what `declare_render_templates` exists to prevent.
        CY_CHECK(light->enabled);
        CY_CHECK_GT(light->intensity, 0.0F);
    }
}
