// The source registry, and the seam engine-generated source arrives through. Tasks 3.1 and 3.8.
//
// THE CASE THIS FILE EXISTS FOR is the last one: a generated module and an authored module are
// indistinguishable to everything downstream. M7's material compiler calls `add_generated` once and
// then compiles that module name exactly as a hand-authored one is compiled — no second toolchain,
// no separate cache, no backend-specific source.

#include <cy/backends/shader/source.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/scope.h>
#include <cy/test/test.h>

#include <string_view>
#include <utility>

using cy::usize;
using namespace cy::shader;

namespace {

cy::assets::VirtualPath path_of(const char* raw) {
    auto path = cy::assets::VirtualPath::normalise(raw);
    CY_REQUIRE(path.has_value());
    return path.value();
}

/// A memory mount is the whole fixture. The registry cannot tell it from a directory mount, which
/// is the point of resolving through the namespace rather than through the operating system.
struct Fixture {
    cy::assets::VirtualFileSystem files;
    cy::assets::MemoryMount* memory = nullptr;

    Fixture() {
        auto mount = cy::make_unique<cy::assets::MemoryMount>(cy::current_allocator(), "memory");
        CY_REQUIRE(mount.has_value());
        memory = mount.value().get();
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), 0).has_value());
    }

    void write(const char* raw, const char* text) const {
        CY_REQUIRE(
            memory->add(path_of(raw), text, std::char_traits<char>::length(text)).has_value());
    }
};

std::string_view text_of(const SourceUnit& unit) {
    return {unit.text.data(), unit.text.size()};
}

}  // namespace

CY_TEST_CASE("a module name maps onto a path under the root, and back") {
    Fixture fixture;
    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(registry.start(fixture.files, path_of("shaders")).has_value());

    auto path = registry.path_for(cy::Name::intern("cy.brdf"));
    CY_REQUIRE(path.has_value());
    CY_CHECK(path->view() == "shaders/cy/brdf.slang");

    auto module_name = registry.module_for(*path);
    CY_REQUIRE(module_name.has_value());
    CY_CHECK(*module_name == cy::Name::intern("cy.brdf"));

    // A file under the root that is not a module — a README, an editor's swap file — is not one.
    CY_CHECK_FALSE(registry.module_for(path_of("shaders/README.md")).has_value());
    CY_CHECK_FALSE(registry.module_for(path_of("elsewhere/cy/brdf.slang")).has_value());
}

CY_TEST_CASE("an authored module is read once and cached, and reload re-reads it") {
    Fixture fixture;
    fixture.write("shaders/cy/brdf.slang", "module brdf; public float one() { return 1.0; }");

    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(registry.start(fixture.files, path_of("shaders")).has_value());

    auto unit = registry.load(cy::Name::intern("cy.brdf"));
    CY_REQUIRE(unit.has_value());
    CY_CHECK(unit->origin == SourceOrigin::Authored);
    CY_CHECK(text_of(*unit).find("return 1.0") != std::string_view::npos);
    CY_CHECK_FALSE(unit->hash.is_zero());
    CY_CHECK_EQ(registry.size(), usize{1});

    fixture.write("shaders/cy/brdf.slang", "module brdf; public float one() { return 2.0; }");
    // `load` returns the resident copy; `reload` is what the hot-reload path calls.
    auto stale = registry.load(cy::Name::intern("cy.brdf"));
    CY_REQUIRE(stale.has_value());
    CY_CHECK(text_of(*stale).find("return 1.0") != std::string_view::npos);

    auto fresh = registry.reload(cy::Name::intern("cy.brdf"));
    CY_REQUIRE(fresh.has_value());
    CY_CHECK(text_of(*fresh).find("return 2.0") != std::string_view::npos);
    CY_CHECK(fresh->hash != unit->hash);
    CY_CHECK_EQ(registry.size(), usize{1});
}

CY_TEST_CASE("a module that is not there is a reported failure, not an empty unit") {
    Fixture fixture;
    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(registry.start(fixture.files, path_of("shaders")).has_value());
    CY_CHECK_FALSE(registry.load(cy::Name::intern("cy.missing")).has_value());
    CY_CHECK_FALSE(registry.contains(cy::Name::intern("cy.missing")));
}

CY_TEST_CASE("generated source is published, attributed, and indistinguishable downstream") {
    Fixture fixture;
    fixture.write("shaders/cy/brdf.slang", "module brdf;");

    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(registry.start(fixture.files, path_of("shaders")).has_value());

    // THIS IS M7'S ENTRY POINT. The material compiler lowers a material to Slang and calls this
    // once; everything after it is the ordinary path.
    auto generated = registry.add_generated(cy::Name::intern("material.4f3a"),
                                            cy::Name::intern("material-compiler"),
                                            "import cy.brdf; float4 shade() { return 1; }");
    CY_REQUIRE(generated.has_value());
    CY_CHECK(generated->origin == SourceOrigin::Generated);
    CY_CHECK(generated->generator == cy::Name::intern("material-compiler"));
    // A generated module has no file, deliberately: a generator cannot smuggle a private on-disk
    // cache in beside the engine's.
    CY_CHECK(generated->path.empty());
    CY_CHECK_FALSE(generated->hash.is_zero());

    // An anonymous generated module is one whose bad diagnostic nobody can attribute.
    CY_CHECK_FALSE(
        registry.add_generated(cy::Name::intern("material.anon"), cy::Name{}, "x").has_value());
    CY_CHECK_FALSE(registry.add_generated(cy::Name{}, cy::Name::intern("gen"), "x").has_value());

    // Re-publishing replaces rather than accumulating.
    auto again = registry.add_generated(cy::Name::intern("material.4f3a"),
                                        cy::Name::intern("material-compiler"),
                                        "import cy.brdf; float4 shade() { return 2; }");
    CY_REQUIRE(again.has_value());
    CY_CHECK(again->hash != generated->hash);
    CY_CHECK_EQ(registry.size(), usize{1});

    // And it has no file to reload, which is refused by name rather than silently reading whatever
    // sits at the path its name maps to.
    CY_CHECK_FALSE(registry.reload(cy::Name::intern("material.4f3a")).has_value());
}

CY_TEST_CASE("the resolver resolves an authored module and a generated one the same way") {
    Fixture fixture;
    fixture.write("shaders/cy/brdf.slang", "module brdf; public float one() { return 1.0; }");

    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(registry.start(fixture.files, path_of("shaders")).has_value());
    CY_REQUIRE(registry
                   .add_generated(cy::Name::intern("material.4f3a"),
                                  cy::Name::intern("material-compiler"), "import cy.brdf;")
                   .has_value());

    const SourceResolver resolver = registry.resolver();

    // The compiler front end sees one interface and cannot tell which kind it got. That is what
    // "no separate path" means when it is executable rather than aspirational.
    SourceUnit authored;
    CY_REQUIRE(resolver("cy.brdf", authored));
    CY_CHECK(authored.origin == SourceOrigin::Authored);

    SourceUnit generated;
    CY_REQUIRE(resolver("material.4f3a", generated));
    CY_CHECK(generated.origin == SourceOrigin::Generated);

    SourceUnit missing;
    CY_CHECK_FALSE(resolver("cy.nothing", missing));
    CY_CHECK_FALSE(resolver("", missing));

    // AND A MODULE THAT HAS NOT BEEN READ YET STILL RESOLVES. The resolver interns the name rather
    // than looking it up: an authored module nobody has loaded has never had its name interned, and
    // a lookup would refuse the first `import` of every module in the project.
    fixture.write("shaders/cy/lighting.slang", "module lighting;");
    SourceUnit unread;
    CY_CHECK(resolver("cy.lighting", unread));
    CY_CHECK(unread.origin == SourceOrigin::Authored);
}

CY_TEST_CASE("a registry with no filesystem still serves generated modules") {
    // Which is what a cook step driven entirely by the material compiler looks like.
    SourceRegistry registry(cy::current_allocator());
    CY_REQUIRE(
        registry.add_generated(cy::Name::intern("material.4f3a"), cy::Name::intern("gen"), "x")
            .has_value());
    SourceUnit unit;
    CY_CHECK(registry.find(cy::Name::intern("material.4f3a"), unit));
    CY_CHECK_FALSE(registry.load(cy::Name::intern("cy.brdf")).has_value());
    CY_CHECK(registry.remove(cy::Name::intern("material.4f3a")).has_value());
    CY_CHECK_EQ(registry.size(), usize{0});
}
