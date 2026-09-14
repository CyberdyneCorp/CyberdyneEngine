// A plugin: identity, versions, trust tiers, resolution and the lockfile.
// `project-and-plugins`, M11.b section 2.
//
// THE CLAIM UNDER TEST is that the four requirements M10's audit found unimplemented — Plugins,
// Plugin lifecycle, Plugin resolution and lockfile, Trust tiers for extensions — are answered by
// code that can refuse. The lifecycle half is `integration.plugins`; everything here is the half
// that costs no world.
//
// Each case names the scenario it is about, because these requirements are mostly scenarios: "a
// renamed plugin still resolves", "an incompatible engine version is not loaded", "two plugins
// require incompatible versions of a third", "a data mod loads freely", "native code requires
// trust".

#include <cy/core/memory/system_allocator.h>
#include <cy/core/plugins/host.h>
#include <cy/core/plugins/plugin.h>
#include <cy/core/plugins/resolve.h>
#include <cy/test/test.h>

#include <string>
#include <string_view>

namespace {

using cy::Array;
using cy::Span;
using cy::u32;
using cy::u64;
using namespace cy::plugins;

[[nodiscard]] cy::Allocator& test_allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Assets);
}

[[nodiscard]] PluginManifest manifest_of(std::string_view text) {
    PluginManifest manifest(test_allocator());
    CY_REQUIRE(read_plugin_manifest(text, manifest));
    return manifest;
}

// --- versions and constraints -------------------------------------------------------------------

CY_TEST_CASE("a version parses and orders") {
    const cy::Expected<Version, cy::Error> parsed = parse_version("1.4.2");
    CY_REQUIRE(parsed);
    CY_CHECK_EQ(parsed->major, 1U);
    CY_CHECK_EQ(parsed->minor, 4U);
    CY_CHECK_EQ(parsed->patch, 2U);
    CY_CHECK(Version{1, 4, 2} < Version{1, 5, 0});
    CY_CHECK(Version{1, 9, 9} < Version{2, 0, 0});
    // A missing component is zero, so `1` and `1.0.0` are one version rather than two.
    const cy::Expected<Version, cy::Error> terse = parse_version("1");
    CY_REQUIRE(terse);
    CY_CHECK(*terse == Version{1, 0, 0});
    CY_CHECK(!parse_version("1.2.3.4"));
    CY_CHECK(!parse_version("1.x"));
}

CY_TEST_CASE("the three constraint kinds accept what they say they accept") {
    // "exact, minimum compatible, or bounded range" — and no fourth.
    const cy::Expected<VersionConstraint, cy::Error> exact = parse_constraint("=1.4.0");
    CY_REQUIRE(exact);
    CY_CHECK(exact->satisfied_by(Version{1, 4, 0}));
    CY_CHECK(!exact->satisfied_by(Version{1, 4, 1}));

    const cy::Expected<VersionConstraint, cy::Error> compatible = parse_constraint("^1.4.0");
    CY_REQUIRE(compatible);
    CY_CHECK(compatible->satisfied_by(Version{1, 4, 0}));
    CY_CHECK(compatible->satisfied_by(Version{1, 9, 2}));
    CY_CHECK(!compatible->satisfied_by(Version{1, 3, 9}));
    // THE HALF A BARE MINIMUM WOULD GET WRONG: 2.0.0 is not compatible with ^1.4.0.
    CY_CHECK(!compatible->satisfied_by(Version{2, 0, 0}));

    const cy::Expected<VersionConstraint, cy::Error> range = parse_constraint("[1.2, 2.0)");
    CY_REQUIRE(range);
    CY_CHECK(range->satisfied_by(Version{1, 2, 0}));
    CY_CHECK(range->satisfied_by(Version{1, 9, 9}));
    CY_CHECK(!range->satisfied_by(Version{2, 0, 0}));
    CY_CHECK(!parse_constraint("[2.0, 1.0)"));
}

// --- identity ------------------------------------------------------------------------------------

constexpr std::string_view kTerrainTools =
    "cyplugin 1\n"
    "id studio.terrain\n"
    "name \"Terrain Tools\"\n"
    "version 1.4.0\n"
    "engine-api 1.0.0 2.0.0\n"
    "tier DataOnly\n"
    "contains content schemas\n";

CY_TEST_CASE("renaming a plugin does not change what a project resolves") {
    // "WHEN a plugin's display name changes THEN projects referencing it SHALL continue to resolve
    // it by identifier."
    const PluginManifest before = manifest_of(kTerrainTools);
    std::string renamed(kTerrainTools);
    const std::string::size_type at = renamed.find("Terrain Tools");
    CY_REQUIRE(at != std::string::npos);
    renamed.replace(at, std::string_view("Terrain Tools").size(), "Landscape Studio");
    const PluginManifest after = manifest_of(renamed);

    CY_CHECK(!(before.display_name == after.display_name));
    CY_CHECK(before.id == after.id);

    const PluginManifest* available[] = {&after};
    const PluginId requested[] = {before.id};
    const cy::Expected<Resolution, cy::Error> resolution =
        resolve(Span<const PluginManifest* const>(available, 1), Span<const PluginId>(requested, 1),
                test_allocator());
    CY_REQUIRE(resolution);
    CY_CHECK(resolution->resolved());
    CY_CHECK_EQ(resolution->order().size(), 1U);
}

CY_TEST_CASE("a manifest with no identity is refused") {
    PluginManifest manifest(test_allocator());
    CY_CHECK(
        !read_plugin_manifest("cyplugin 1\nversion 1.0.0\nengine-api 1.0.0 2.0.0\n", manifest));
}

// --- trust tiers ---------------------------------------------------------------------------------

CY_TEST_CASE("a data mod loads freely and native code requires a trust decision") {
    // The two scenarios of "Trust tiers for extensions", side by side.
    const PluginManifest data = manifest_of(kTerrainTools);
    CY_CHECK_EQ(data.tier, TrustTier::DataOnly);
    CY_CHECK_EQ(required_tier_for(data.contents), TrustTier::DataOnly);

    PluginHost host(test_allocator());
    PluginRuntime nothing;
    CY_CHECK(host.add(data, nothing, false, /*trusted=*/false));

    const PluginManifest native = manifest_of(
        "cyplugin 1\n"
        "id studio.physics\n"
        "name \"A Physics Backend\"\n"
        "version 1.0.0\n"
        "engine-api 1.0.0 2.0.0\n"
        "tier TrustedNative\n"
        "contains modules platform-binaries\n");
    CY_CHECK_EQ(required_tier_for(native.contents), TrustTier::TrustedNative);
    // Untrusted: refused by name. There is no default that grants it, which is what makes the
    // decision explicit rather than a setting somebody forgot.
    CY_CHECK(!host.add(native, nothing, false, /*trusted=*/false));
    CY_CHECK(host.add(native, nothing, false, /*trusted=*/true));
}

CY_TEST_CASE("a plugin may not declare a tier lower than its contents require") {
    // Refused AT THE MANIFEST rather than at load, because at load the only honest answer left is
    // to refuse it and nobody knows why it was packaged that way.
    PluginManifest manifest(test_allocator());
    CY_CHECK(
        !read_plugin_manifest("cyplugin 1\n"
                              "id studio.sneaky\n"
                              "version 1.0.0\n"
                              "engine-api 1.0.0 2.0.0\n"
                              "tier DataOnly\n"
                              "contains content platform-binaries\n",
                              manifest));
}

// --- engine API range
// -----------------------------------------------------------------------------

CY_TEST_CASE("a plugin declaring an engine range that excludes this engine is not loaded") {
    // "WHEN a plugin declares an engine API range that excludes the current engine THEN it SHALL be
    // reported as incompatible and not loaded."
    const PluginManifest manifest = manifest_of(
        "cyplugin 1\n"
        "id studio.future\n"
        "version 1.0.0\n"
        "engine-api 3.0.0 4.0.0\n"
        "tier DataOnly\n"
        "contains content\n");
    PluginHost host(test_allocator());
    host.set_engine_api(Version{2, 1, 0});
    PluginRuntime nothing;
    CY_CHECK(!host.add(manifest, nothing, false, true));
    CY_CHECK(host.find(manifest.id) == nullptr);

    host.set_engine_api(Version{3, 2, 0});
    CY_CHECK(host.add(manifest, nothing, false, true));
}

// --- resolution
// -------------------------------------------------------------------------------------

namespace {

/// Three plugins: `app` depends on `physics` and on `terrain`, and `terrain` depends on `physics`.
struct Trio {
    PluginManifest physics;
    PluginManifest terrain;
    PluginManifest app;
};

[[nodiscard]] Trio trio(std::string_view app_physics_constraint) {
    std::string app;
    app += "cyplugin 1\nid studio.app\nversion 1.0.0\nengine-api 1.0.0 2.0.0\ntier DataOnly\n";
    app += "contains content\n";
    app += "depends studio.terrain ^1.0.0\n";
    app += "depends studio.physics ";
    app += std::string(app_physics_constraint);
    app += "\n";
    return Trio{
        manifest_of("cyplugin 1\nid studio.physics\nversion 1.4.0\nengine-api 1.0.0 2.0.0\n"
                    "tier DataOnly\ncontains content\n"),
        manifest_of("cyplugin 1\nid studio.terrain\nversion 1.0.0\nengine-api 1.0.0 2.0.0\n"
                    "tier DataOnly\ncontains content\ndepends studio.physics ^1.0.0\n"),
        manifest_of(app),
    };
}

}  // namespace

CY_TEST_CASE("resolution answers a deterministic load order with dependencies first") {
    // "Load order SHALL follow the resolved dependency graph and SHALL be deterministic."
    const Trio set = trio("^1.4.0");
    const PluginManifest* forwards[] = {&set.physics, &set.terrain, &set.app};
    const PluginManifest* backwards[] = {&set.app, &set.terrain, &set.physics};
    const PluginId requested[] = {set.app.id};

    const cy::Expected<Resolution, cy::Error> first =
        resolve(Span<const PluginManifest* const>(forwards, 3), Span<const PluginId>(requested, 1),
                test_allocator());
    CY_REQUIRE(first);
    CY_REQUIRE(first->resolved());
    CY_REQUIRE_EQ(first->order().size(), 3U);
    CY_CHECK_EQ(first->order()[0], set.physics.id);
    CY_CHECK_EQ(first->order()[1], set.terrain.id);
    CY_CHECK_EQ(first->order()[2], set.app.id);

    // The same set in a different order resolves to the SAME order, which is the half of
    // "deterministic" a topological sort alone does not give.
    const cy::Expected<Resolution, cy::Error> second =
        resolve(Span<const PluginManifest* const>(backwards, 3), Span<const PluginId>(requested, 1),
                test_allocator());
    CY_REQUIRE(second);
    CY_REQUIRE(second->resolved());
    CY_REQUIRE_EQ(second->order().size(), 3U);
    for (cy::usize index = 0; index < 3; ++index) {
        CY_CHECK_EQ(second->order()[index], first->order()[index]);
    }
}

CY_TEST_CASE("the load order is the same whatever order the project asked in") {
    // THE HALF A TOPOLOGICAL SORT ALONE DOES NOT GIVE, and the case that catches its absence. Two
    // plugins with no dependency between them have two valid topological orders; which one a walk
    // produces is decided by the order it met them in, which here is the order the PROJECT listed
    // them. A project that reordered two lines in its manifest would otherwise start its plugins
    // in a different order, and "SHALL be deterministic" would mean "deterministic per manifest".
    const PluginManifest zeta = manifest_of(
        "cyplugin 1\nid studio.zeta\nversion 1.0.0\nengine-api 1.0.0 2.0.0\ntier DataOnly\n"
        "contains content\n");
    const PluginManifest alpha = manifest_of(
        "cyplugin 1\nid studio.alpha\nversion 1.0.0\nengine-api 1.0.0 2.0.0\ntier DataOnly\n"
        "contains content\n");
    const PluginManifest* available[] = {&zeta, &alpha};

    const PluginId zeta_first[] = {zeta.id, alpha.id};
    const PluginId alpha_first[] = {alpha.id, zeta.id};
    const cy::Expected<Resolution, cy::Error> first =
        resolve(Span<const PluginManifest* const>(available, 2),
                Span<const PluginId>(zeta_first, 2), test_allocator());
    const cy::Expected<Resolution, cy::Error> second =
        resolve(Span<const PluginManifest* const>(available, 2),
                Span<const PluginId>(alpha_first, 2), test_allocator());
    CY_REQUIRE(first);
    CY_REQUIRE(second);
    CY_REQUIRE_EQ(first->order().size(), 2U);
    CY_REQUIRE_EQ(second->order().size(), 2U);
    CY_CHECK_EQ(first->order()[0], alpha.id);
    CY_CHECK_EQ(first->order()[1], zeta.id);
    CY_CHECK_EQ(second->order()[0], first->order()[0]);
    CY_CHECK_EQ(second->order()[1], first->order()[1]);
}

CY_TEST_CASE("the load order is the same whatever order a manifest declared its dependencies in") {
    // The same claim one level down: two manifests describing the same graph with their `depends`
    // lines swapped resolve to one order.
    const PluginManifest zeta = manifest_of(
        "cyplugin 1\nid studio.zeta\nversion 1.0.0\nengine-api 1.0.0 2.0.0\ntier DataOnly\n"
        "contains content\n");
    const PluginManifest alpha = manifest_of(
        "cyplugin 1\nid studio.alpha\nversion 1.0.0\nengine-api 1.0.0 2.0.0\ntier DataOnly\n"
        "contains content\n");
    const PluginManifest zeta_first = manifest_of(
        "cyplugin 1\nid studio.app\nversion 1.0.0\nengine-api 1.0.0 2.0.0\ntier DataOnly\n"
        "contains content\ndepends studio.zeta ^1.0.0\ndepends studio.alpha ^1.0.0\n");
    const PluginManifest alpha_first = manifest_of(
        "cyplugin 1\nid studio.app\nversion 1.0.0\nengine-api 1.0.0 2.0.0\ntier DataOnly\n"
        "contains content\ndepends studio.alpha ^1.0.0\ndepends studio.zeta ^1.0.0\n");

    const PluginManifest* declared_zeta_first[] = {&zeta, &alpha, &zeta_first};
    const PluginManifest* declared_alpha_first[] = {&zeta, &alpha, &alpha_first};
    const PluginId requested[] = {zeta_first.id};
    const cy::Expected<Resolution, cy::Error> first =
        resolve(Span<const PluginManifest* const>(declared_zeta_first, 3),
                Span<const PluginId>(requested, 1), test_allocator());
    const cy::Expected<Resolution, cy::Error> second =
        resolve(Span<const PluginManifest* const>(declared_alpha_first, 3),
                Span<const PluginId>(requested, 1), test_allocator());
    CY_REQUIRE(first);
    CY_REQUIRE(second);
    CY_REQUIRE_EQ(first->order().size(), 3U);
    CY_REQUIRE_EQ(second->order().size(), 3U);
    CY_CHECK_EQ(first->order()[0], alpha.id);
    CY_CHECK_EQ(first->order()[1], zeta.id);
    CY_CHECK_EQ(second->order()[0], first->order()[0]);
    CY_CHECK_EQ(second->order()[1], first->order()[1]);
}

CY_TEST_CASE("unsatisfiable constraints fail naming both requirements") {
    // "WHEN two plugins require incompatible versions of a third THEN resolution SHALL fail naming
    // the conflict rather than choosing arbitrarily."
    const Trio set = trio("=2.0.0");
    const PluginManifest* available[] = {&set.physics, &set.terrain, &set.app};
    const PluginId requested[] = {set.app.id};
    const cy::Expected<Resolution, cy::Error> resolution =
        resolve(Span<const PluginManifest* const>(available, 3), Span<const PluginId>(requested, 1),
                test_allocator());
    CY_REQUIRE(resolution);
    CY_CHECK(!resolution->resolved());

    const ResolutionConflict& conflict = resolution->conflict();
    CY_CHECK_EQ(conflict.subject, set.physics.id);
    CY_CHECK(conflict.has_available);
    CY_CHECK(conflict.available == Version{1, 4, 0});
    // BOTH requirements, not the one that happened to be checked last: naming one explains half of
    // a conflict, and the half it leaves out is the one somebody has to go and find.
    CY_CHECK(conflict.has_second);
    const bool app_named =
        conflict.first_requirer == set.app.id || conflict.second_requirer == set.app.id;
    const bool terrain_named =
        conflict.first_requirer == set.terrain.id || conflict.second_requirer == set.terrain.id;
    CY_CHECK(app_named);
    CY_CHECK(terrain_named);
}

CY_TEST_CASE("a dependency cycle is an error rather than an order") {
    const PluginManifest first = manifest_of(
        "cyplugin 1\nid studio.a\nversion 1.0.0\nengine-api 1.0.0 2.0.0\ntier DataOnly\n"
        "contains content\ndepends studio.b ^1.0.0\n");
    const PluginManifest second = manifest_of(
        "cyplugin 1\nid studio.b\nversion 1.0.0\nengine-api 1.0.0 2.0.0\ntier DataOnly\n"
        "contains content\ndepends studio.a ^1.0.0\n");
    const PluginManifest* available[] = {&first, &second};
    const PluginId requested[] = {first.id};
    CY_CHECK(!resolve(Span<const PluginManifest* const>(available, 2),
                      Span<const PluginId>(requested, 1), test_allocator()));
}

CY_TEST_CASE("a requested plugin that is not available is an error rather than an empty set") {
    const PluginId requested[] = {cy::Name::intern("studio.absent")};
    CY_CHECK(!resolve(Span<const PluginManifest* const>(), Span<const PluginId>(requested, 1),
                      test_allocator()));
}

// --- the lockfile
// -------------------------------------------------------------------------------------

CY_TEST_CASE("a lockfile round-trips and is ordered by identity rather than by insertion") {
    // "so that a build is reproducible and a change of dependency is a reviewable diff" — a diff is
    // only reviewable if the file is a function of the set rather than of the walk order.
    Lockfile first(test_allocator());
    CY_REQUIRE(first.record(LockedPlugin{cy::Name::intern("studio.terrain"), {1, 0, 0}, 0xAAAA}));
    CY_REQUIRE(first.record(LockedPlugin{cy::Name::intern("studio.app"), {1, 0, 0}, 0xBBBB}));
    CY_REQUIRE(first.record(LockedPlugin{cy::Name::intern("studio.physics"), {1, 4, 0}, 0xCCCC}));

    Lockfile second(test_allocator());
    CY_REQUIRE(second.record(LockedPlugin{cy::Name::intern("studio.physics"), {1, 4, 0}, 0xCCCC}));
    CY_REQUIRE(second.record(LockedPlugin{cy::Name::intern("studio.app"), {1, 0, 0}, 0xBBBB}));
    CY_REQUIRE(second.record(LockedPlugin{cy::Name::intern("studio.terrain"), {1, 0, 0}, 0xAAAA}));

    Array<char> first_text(test_allocator());
    Array<char> second_text(test_allocator());
    CY_REQUIRE(write_lockfile(first, first_text));
    CY_REQUIRE(write_lockfile(second, second_text));
    CY_CHECK_EQ(std::string_view(first_text.data(), first_text.size()),
                std::string_view(second_text.data(), second_text.size()));

    Lockfile read_back(test_allocator());
    CY_REQUIRE(read_lockfile(std::string_view(first_text.data(), first_text.size()), read_back));
    CY_REQUIRE_EQ(read_back.entries().size(), 3U);
    const LockedPlugin* physics = read_back.find(cy::Name::intern("studio.physics"));
    CY_REQUIRE(physics != nullptr);
    CY_CHECK(physics->version == Version{1, 4, 0});
    CY_CHECK_EQ(physics->content_hash, 0xCCCCU);
}

CY_TEST_CASE("a build uses the lockfile and a version that moved is refused") {
    // "Building SHALL use the lockfile; updating it SHALL be a deliberate action." There is no
    // call here that both checks and updates, which is what makes the update deliberate.
    const Trio set = trio("^1.4.0");
    const PluginManifest* available[] = {&set.physics, &set.terrain, &set.app};
    const PluginId requested[] = {set.app.id};
    const cy::Expected<Resolution, cy::Error> resolution =
        resolve(Span<const PluginManifest* const>(available, 3), Span<const PluginId>(requested, 1),
                test_allocator());
    CY_REQUIRE(resolution);
    CY_REQUIRE(resolution->resolved());

    Lockfile lock(test_allocator());
    for (const PluginId& plugin : resolution->order()) {
        Version version = set.app.version;
        if (plugin == set.physics.id) {
            version = set.physics.version;
        } else if (plugin == set.terrain.id) {
            version = set.terrain.version;
        }
        CY_REQUIRE(lock.record(LockedPlugin{plugin, version, 1}));
    }
    Array<char> diagnostic(test_allocator());
    CY_CHECK(resolve_against_lock(Span<const PluginManifest* const>(available, 3), *resolution,
                                  lock, diagnostic));

    // The lock says 1.3.0 and the available plugin is 1.4.0: refused, naming the plugin.
    CY_REQUIRE(lock.record(LockedPlugin{set.physics.id, Version{1, 3, 0}, 1}));
    Array<char> moved(test_allocator());
    CY_CHECK(!resolve_against_lock(Span<const PluginManifest* const>(available, 3), *resolution,
                                   lock, moved));
    CY_CHECK_EQ(std::string_view(moved.data(), moved.size()), std::string_view("studio.physics"));
}

// --- extension points
// ---------------------------------------------------------------------------------

CY_TEST_CASE("an extension point is versioned independently and a stale plugin is refused") {
    // "WHEN an extension interface changes incompatibly THEN its version SHALL increment, and
    // plugins targeting the previous version SHALL be reported as incompatible rather than loaded."
    ExtensionRegistry registry(test_allocator());
    CY_REQUIRE(registry.declare(ExtensionPoint{cy::Name::intern("editor-panel"), 2}));

    ExtensionRegistration current;
    current.point = cy::Name::intern("editor-panel");
    current.interface_version = 2;
    current.contribution = cy::Name::intern("terrain");
    CY_CHECK(registry.bind(cy::Name::intern("studio.terrain"), current));

    ExtensionRegistration stale = current;
    stale.interface_version = 1;
    CY_CHECK(!registry.bind(cy::Name::intern("studio.terrain"), stale));

    ExtensionRegistration unknown = current;
    unknown.point = cy::Name::intern("a-point-the-engine-does-not-offer");
    CY_CHECK(!registry.bind(cy::Name::intern("studio.terrain"), unknown));
}

CY_TEST_CASE("the standard extension points cover the requirement's own list") {
    ExtensionRegistry registry(test_allocator());
    CY_REQUIRE(ExtensionRegistry::declare_standard_points(registry));
    // The list the requirement names "at minimum". Spelled out here rather than counted, so that a
    // point deleted from the registry fails on its own name.
    for (const std::string_view point : {"physics-backend",
                                         "audio-backend",
                                         "network-transport",
                                         "asset-importer",
                                         "asset-processor",
                                         "render-feature",
                                         "material-node",
                                         "material-closure",
                                         "editor-panel",
                                         "property-editor",
                                         "gizmo",
                                         "viewport-tool",
                                         "editor-command",
                                         "build-step",
                                         "platform-target",
                                         "upscaler",
                                         "ray-tracing-backend",
                                         "virtual-texture-producer",
                                         "source-control",
                                         "streaming-producer",
                                         "residency-producer"}) {
        CY_CHECK(registry.point(cy::Name::intern(point)) != nullptr);
    }
    // And declaring one twice is refused: two versions of one point is the ambiguity the version
    // number exists to remove.
    CY_CHECK(!registry.declare(ExtensionPoint{cy::Name::intern("editor-panel"), 3}));
}

}  // namespace
