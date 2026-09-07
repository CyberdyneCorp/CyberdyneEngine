// The authoring schema the editor reads, built from the engine's own type registry. M6 task 2.4.
//
// The claim under test is not "this function returns a struct". It is that **the component types an
// editor shows are the engine's**, and that the file a project carries is the one this build would
// write. So the last test regenerates the committed manifest and compares it byte for byte — the
// pattern `just generate-check` already uses for reflection headers, and the only thing that keeps
// a project's schema from quietly diverging from the engine that has to load its worlds.

#include <cy/core/memory/array.h>
#include <cy/core/reflect/registry.h>
#include <cy/scene/serialization/authoring_schema.h>
#include <cy/test/test.h>
#include <cy_reflect_generated_scene.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using cy::Array;
using cy::u32;
using cy::usize;
using cy::reflect::TypeRegistry;
using namespace cy::scene::serialization;

/// A registry holding exactly the scene module's reflected components.
///
/// Its own registry rather than the process-wide one: `default_registry()` accumulates whatever a
/// test run happened to register before this file, and a manifest built from that would differ
/// between running the suite alone and running it after another.
struct SceneRegistry {
    TypeRegistry registry;

    SceneRegistry() { CY_REQUIRE(cy::reflect::register_scene_types(registry)); }
};

[[nodiscard]] std::string manifest_of(const TypeRegistry& registry) {
    AuthoringSchema schema;
    CY_REQUIRE(build_authoring_schema(registry, schema));
    Array<char> text;
    CY_REQUIRE(write_authoring_schema(schema, text));
    return {text.data(), text.size()};
}

/// The `Transform` type's block of the manifest, so a test can assert on it without matching a
/// whole file.
[[nodiscard]] std::string block_of(const std::string& manifest, std::string_view type_line) {
    const std::string::size_type start = manifest.find(type_line);
    if (start == std::string::npos) {
        return {};
    }
    const std::string::size_type next = manifest.find("\ntype ", start + 1);
    return manifest.substr(start, next == std::string::npos ? std::string::npos : next - start + 1);
}

}  // namespace

CY_TEST_CASE("the engine's local transform reaches the editor as a Transform a gizmo can bind to") {
    // The whole of task 2.4's requirement, as one assertion. `TransformBinding::of_schema` looks
    // for a component named `Transform` carrying `translation`, `rotation` and `scale`; before M6
    // the editor's schema was empty and the binding found nothing.
    SceneRegistry scene;
    const std::string manifest = manifest_of(scene.registry);
    const std::string transform = block_of(manifest, "type 5 runtime \"Transform\"");

    CY_REQUIRE(!transform.empty());
    CY_CHECK(transform.find("vec3 \"translation\"") != std::string::npos);
    CY_CHECK(transform.find("quat \"rotation\"") != std::string::npos);
    CY_CHECK(transform.find("vec3 \"scale\"") != std::string::npos);
}

CY_TEST_CASE(
    "ten reflected float lanes become three values, and each keeps its first lane's identity") {
    // Reflection describes `LocalTransform` as `value.rotation.x` ... `value.scale.z`: ten f32
    // fields with identifiers 1 to 10. An inspector generated from that shows ten spin boxes and a
    // gizmo has nothing to move. Grouping is what makes it three, and the identifier of each group
    // is its FIRST lane's — which is what keeps an override or a history entry addressing it.
    SceneRegistry scene;
    const std::string manifest = manifest_of(scene.registry);
    const std::string transform = block_of(manifest, "type 5 runtime \"Transform\"");

    CY_CHECK(transform.find("field 1 quat \"rotation\"") != std::string::npos);
    CY_CHECK(transform.find("field 5 vec3 \"translation\"") != std::string::npos);
    CY_CHECK(transform.find("field 8 vec3 \"scale\"") != std::string::npos);

    usize fields = 0;
    for (std::string::size_type at = transform.find("field "); at != std::string::npos;
         at = transform.find("field ", at + 1)) {
        ++fields;
    }
    CY_CHECK(fields == 3);
}

CY_TEST_CASE("the one alias is declared, enumerable, and applies to nothing else") {
    // A table nobody can enumerate is a table that silently grows, so the rule is readable from
    // here rather than only observable through its output.
    CY_CHECK(authoring_name_of("cy::scene::LocalTransform") == "Transform");
    CY_CHECK(authoring_name_of("cy::scene::WorldTransform") == "WorldTransform");
    CY_CHECK(authoring_name_of("cy::demo::Health") == "Health");
    CY_CHECK(authoring_name_of("Unqualified") == "Unqualified");
}

CY_TEST_CASE("a manifest is the same bytes however the registry was filled") {
    // A registry iterates its hash table. Two processes that registered the same types in a
    // different order would produce manifests that differ only in line order — and the regeneration
    // gate below would then fail on somebody else's machine for no reason at all.
    SceneRegistry first;
    SceneRegistry second;
    CY_CHECK(manifest_of(first.registry) == manifest_of(second.registry));

    // Ascending by identifier, which is the ordering that makes that true.
    const std::string manifest = manifest_of(first.registry);
    u32 previous = 0;
    for (std::string::size_type at = manifest.find("type "); at != std::string::npos;
         at = manifest.find("\ntype ", at + 1)) {
        const std::string::size_type digits = manifest.find_first_of("0123456789", at);
        const u32 id = static_cast<u32>(std::strtoul(manifest.c_str() + digits, nullptr, 10));
        CY_CHECK(id > previous);
        previous = id;
    }
    CY_CHECK(previous > 0);
}

CY_TEST_CASE("a derived field is absent, because a person cannot author one") {
    // `WorldTransform` is computed from `LocalTransform` and the parent chain by one system at one
    // point in the frame. An editor that offered it for editing would offer a value the next frame
    // overwrites, with no diagnostic.
    SceneRegistry scene;
    const std::string manifest = manifest_of(scene.registry);
    const std::string world = block_of(manifest, "type 9 runtime \"WorldTransform\"");
    CY_REQUIRE(!world.empty());
    CY_CHECK(world.find("field ") == std::string::npos);
}

CY_TEST_CASE("the committed manifest is the one this build writes") {
    // THE GATE. `samples/05b-editor-window/project/types.cytypes` is what the editor reads when it
    // opens that project, and it is committed so that a checkout without a build has one. This test
    // regenerates it and compares — the same shape as `just generate-check`, and the only thing
    // that stops a project's schema drifting away from the engine that has to load its worlds.
    //
    // WHEN THIS FAILS, the remedy is to write the generated text over the committed file and read
    // the diff: it is the editor's view of the engine's components, changing.
    SceneRegistry scene;
    const std::string generated = manifest_of(scene.registry);

    // Read through the standard library's stream types rather than `fopen`/`fread`: the analyser
    // cannot prove a hand-rolled read loop leaves the stream in a defined state after a short read,
    // and it is right to say so.
    std::ifstream file(CY_AUTHORING_SCHEMA_GOLDEN, std::ios::binary);
    CY_REQUIRE(file.is_open());
    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string committed = contents.str();

    CY_CHECK(committed == generated);
}
