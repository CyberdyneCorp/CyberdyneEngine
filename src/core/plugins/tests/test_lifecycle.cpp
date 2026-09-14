// The plugin lifecycle: eight phases, containment, and unload safety.
// `project-and-plugins`, M11.b tasks 2.1 and 2.2.
//
// THE CLAIM UNDER TEST is that the phases are separable and that a failure in one is contained.
// Both are scenarios rather than prose:
//
//   * "WHEN a plugin contributes component types THEN it SHALL register them during the register
//     phase, before any world is created" — so `register` has to be observable separately from
//     `start`, and a plugin that registered in `start` has to be distinguishable from one that did
//     not;
//   * "WHEN a plugin fails to initialise THEN it SHALL be disabled with a diagnostic and the engine
//     SHALL continue if it is not required";
//   * "WHEN a plugin is asked to unload while entities carry its components THEN the unload SHALL
//     be refused, naming the types and instance counts";
//   * "WHEN a plugin unloads successfully THEN every type, importer, extension, and callback it
//     registered SHALL be withdrawn".
//
// It is an integration suite rather than a unit one because a case here brings a SET of plugins up
// through four phases each and takes them back down, which is a sequence rather than a call.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/plugins/host.h>
#include <cy/test/test.h>

#include <string>
#include <string_view>
#include <utility>

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

/// A plugin implementation that records the phases it was taken through, and can be told to fail at
/// one of them. The journal is what makes "separable phases" a measurement.
struct Recorder {
    std::string journal;
    Phase fail_at = Phase::Unload;
    bool should_fail = false;
    /// Whether a world existed when `register` ran. Written by the harness below.
    bool world_existed_at_register = false;
    const bool* world = nullptr;

    static cy::Status note(void* user, Phase phase) noexcept {
        auto* self = static_cast<Recorder*>(user);
        self->journal += phase_name(phase);
        self->journal += ' ';
        if (phase == Phase::Register && self->world != nullptr) {
            self->world_existed_at_register = *self->world;
        }
        if (self->should_fail && self->fail_at == phase) {
            return cy::fail(cy::ErrorCode::Internal, "this plugin was told to fail");
        }
        return cy::ok();
    }

    static void note_void(void* user, Phase phase) noexcept { (void)note(user, phase); }
};

[[nodiscard]] PluginRuntime runtime_for(Recorder& recorder) noexcept {
    PluginRuntime runtime;
    runtime.user = &recorder;
    runtime.load = [](void* user) { return Recorder::note(user, Phase::Load); };
    runtime.initialise = [](void* user) { return Recorder::note(user, Phase::Initialise); };
    runtime.register_types = [](void* user) { return Recorder::note(user, Phase::Register); };
    runtime.start = [](void* user) { return Recorder::note(user, Phase::Start); };
    runtime.stop = [](void* user) { Recorder::note_void(user, Phase::Stop); };
    runtime.unregister_types = [](void* user) { Recorder::note_void(user, Phase::Unregister); };
    runtime.shutdown = [](void* user) { Recorder::note_void(user, Phase::Shutdown); };
    runtime.unload = [](void* user) { Recorder::note_void(user, Phase::Unload); };
    return runtime;
}

constexpr std::string_view kPhysics =
    "cyplugin 1\n"
    "id studio.physics\n"
    "name \"A Physics Backend\"\n"
    "version 1.0.0\n"
    "engine-api 1.0.0 2.0.0\n"
    "tier TrustedNative\n"
    "contains modules\n"
    "module studio_physics\n"
    "type StudioBody\n"
    "type StudioJoint\n"
    "extends physics-backend 1 studio\n";

constexpr std::string_view kTerrain =
    "cyplugin 1\n"
    "id studio.terrain\n"
    "name \"Terrain Tools\"\n"
    "version 1.0.0\n"
    "engine-api 1.0.0 2.0.0\n"
    "tier DataOnly\n"
    "contains content editor-extensions\n"
    "type TerrainStroke\n"
    "extends editor-panel 1 terrain\n"
    "depends studio.physics ^1.0.0\n";

[[nodiscard]] Array<PluginId> order_of(std::initializer_list<PluginId> plugins) {
    Array<PluginId> order(test_allocator());
    for (const PluginId plugin : plugins) {
        CY_REQUIRE(order.push_back(plugin));
    }
    return order;
}

/// A host with the standard points declared and both plugins added.
struct Fixture {
    PluginManifest physics = manifest_of(kPhysics);
    PluginManifest terrain = manifest_of(kTerrain);
    Recorder physics_recorder;
    Recorder terrain_recorder;
    PluginHost host{test_allocator()};
    bool world_exists = false;

    void arm() {
        CY_REQUIRE(ExtensionRegistry::declare_standard_points(host.extensions()));
        host.set_engine_api(Version{1, 2, 0});
        CY_REQUIRE(host.set_platform("linux-x86_64"));
        physics_recorder.world = &world_exists;
        terrain_recorder.world = &world_exists;
        CY_REQUIRE(host.add(physics, runtime_for(physics_recorder), true, /*trusted=*/true));
        CY_REQUIRE(host.add(terrain, runtime_for(terrain_recorder), false, /*trusted=*/true));
    }
};

// --- the phases ---------------------------------------------------------------------------------

CY_TEST_CASE("the eight phases run in order and register precedes any world") {
    Fixture fixture;
    fixture.arm();
    const Array<PluginId> order = order_of({fixture.physics.id, fixture.terrain.id});

    CY_REQUIRE(fixture.host.bring_up(order.span()));
    CY_CHECK_EQ(fixture.physics_recorder.journal, std::string("load initialise register start "));
    CY_CHECK_EQ(fixture.terrain_recorder.journal, std::string("load initialise register start "));
    // "WHEN a plugin contributes component types THEN it SHALL register them during the register
    // phase, BEFORE ANY WORLD EXISTS." The world is created after bring-up, and the recorder saw
    // that it did not exist when its register callback ran.
    CY_CHECK(!fixture.physics_recorder.world_existed_at_register);
    fixture.world_exists = true;

    // The types the manifest declared are owned by the plugin that declared them, and they were
    // recorded by the HOST during register rather than by the plugin's own callback — a plugin that
    // forgot to call something would otherwise own nothing and unload freely.
    CY_CHECK_EQ(fixture.host.ownership().owner_of(cy::Name::intern("StudioBody")),
                fixture.physics.id);
    CY_CHECK_EQ(fixture.host.ownership().owner_of(cy::Name::intern("TerrainStroke")),
                fixture.terrain.id);
    CY_CHECK_EQ(fixture.host.extensions().bindings().size(), 2U);

    fixture.host.tear_down(order.span());
    // Reverse phases, in reverse plugin order: terrain stops before the physics backend it uses.
    CY_CHECK_EQ(fixture.physics_recorder.journal,
                std::string("load initialise register start stop unregister shutdown unload "));
    CY_CHECK_EQ(fixture.host.extensions().bindings().size(), 0U);
    CY_CHECK(fixture.host.ownership().owner_of(cy::Name::intern("StudioBody")).is_empty());
}

CY_TEST_CASE("a plugin that fails to initialise is disabled and the engine continues") {
    // "WHEN a plugin fails to initialise THEN it SHALL be disabled with a diagnostic and the engine
    // SHALL continue if it is not required." Terrain is the optional one here.
    Fixture fixture;
    fixture.terrain_recorder.should_fail = true;
    fixture.terrain_recorder.fail_at = Phase::Initialise;
    fixture.arm();
    const Array<PluginId> order = order_of({fixture.physics.id, fixture.terrain.id});

    CY_CHECK(fixture.host.bring_up(order.span()));  // the engine continued
    const HostedPlugin* terrain = fixture.host.find(fixture.terrain.id);
    CY_REQUIRE(terrain != nullptr);
    CY_CHECK(terrain->disabled);
    CY_CHECK(!terrain->running);
    const HostedPlugin* physics = fixture.host.find(fixture.physics.id);
    CY_REQUIRE(physics != nullptr);
    CY_CHECK(physics->running);

    CY_REQUIRE_EQ(fixture.host.failures().size(), 1U);
    CY_CHECK_EQ(fixture.host.failures()[0].plugin, fixture.terrain.id);
    CY_CHECK_EQ(fixture.host.failures()[0].phase, Phase::Initialise);
    CY_CHECK(!fixture.host.failures()[0].cascaded);
    // It never reached register, so nothing it would have registered is in the registry.
    CY_CHECK_EQ(fixture.host.extensions().bindings().size(), 1U);
    CY_CHECK(fixture.host.ownership().owner_of(cy::Name::intern("TerrainStroke")).is_empty());
}

CY_TEST_CASE("a required plugin that fails takes the bring-up with it") {
    Fixture fixture;
    fixture.physics_recorder.should_fail = true;
    fixture.physics_recorder.fail_at = Phase::Start;
    fixture.arm();
    const Array<PluginId> order = order_of({fixture.physics.id, fixture.terrain.id});

    CY_CHECK(!fixture.host.bring_up(order.span()));
    // And the dependant is disabled for a DIFFERENT reason, which the report distinguishes:
    // blaming terrain for the physics backend's defect sends somebody to the wrong file.
    const HostedPlugin* terrain = fixture.host.find(fixture.terrain.id);
    CY_REQUIRE(terrain != nullptr);
    CY_CHECK(terrain->disabled);
    CY_REQUIRE_EQ(fixture.host.failures().size(), 2U);
    CY_CHECK(!fixture.host.failures()[0].cascaded);
    CY_CHECK(fixture.host.failures()[1].cascaded);
    CY_CHECK_EQ(fixture.host.failures()[1].because_of, fixture.physics.id);
}

CY_TEST_CASE("a half-brought-up plugin leaves no registration behind") {
    // Failing at START means register already ran, so the registrations exist and have to be taken
    // back. A plugin disabled with a live binding is a call into an image nothing is driving.
    Fixture fixture;
    fixture.terrain_recorder.should_fail = true;
    fixture.terrain_recorder.fail_at = Phase::Start;
    fixture.arm();
    const Array<PluginId> order = order_of({fixture.physics.id, fixture.terrain.id});

    CY_CHECK(fixture.host.bring_up(order.span()));
    CY_CHECK_EQ(fixture.terrain_recorder.journal, std::string("load initialise register start "));
    CY_CHECK_EQ(fixture.host.extensions().bindings().size(), 1U);
    CY_CHECK(fixture.host.ownership().owner_of(cy::Name::intern("TerrainStroke")).is_empty());
}

CY_TEST_CASE("a plugin for another platform is skipped rather than failed") {
    Fixture fixture;
    fixture.arm();
    PluginManifest windows_only = manifest_of(
        "cyplugin 1\n"
        "id studio.windows\n"
        "version 1.0.0\n"
        "engine-api 1.0.0 2.0.0\n"
        "tier DataOnly\n"
        "contains content\n"
        "platform windows-x86_64\n");
    Recorder recorder;
    CY_REQUIRE(fixture.host.add(windows_only, runtime_for(recorder), false, true));
    const Array<PluginId> order =
        order_of({fixture.physics.id, fixture.terrain.id, windows_only.id});

    CY_CHECK(fixture.host.bring_up(order.span()));
    CY_CHECK_EQ(recorder.journal, std::string(""));
    // Skipped, and NOT a failure: a plugin with no binary for this platform is not a defect on it,
    // and recording one would fill every cross-platform project's report with them.
    CY_CHECK_EQ(fixture.host.failures().size(), 0U);
}

// --- unload safety ------------------------------------------------------------------------------

namespace {

/// How many live instances of each type there are. The host cannot know this — `cy::ecs` is above
/// the layer this module sits at — so it is supplied, and this is the supplier.
u64 count_instances(void* user, cy::Name type) noexcept {
    const auto* live = static_cast<const std::pair<const char*, u64>*>(user);
    return type.text() == std::string_view(live->first) ? live->second : 0;
}

}  // namespace

CY_TEST_CASE("an unload with outstanding instances is refused naming the types and counts") {
    // "WHEN a plugin is asked to unload while entities carry its components THEN the unload SHALL
    // be refused, naming the types and instance counts."
    Fixture fixture;
    fixture.arm();
    const Array<PluginId> order = order_of({fixture.physics.id, fixture.terrain.id});
    CY_REQUIRE(fixture.host.bring_up(order.span()));

    std::pair<const char*, u64> live{"StudioBody", 53};
    Array<char> diagnostic(test_allocator());
    CY_CHECK(!fixture.host.ownership().may_unload(fixture.physics.id, count_instances, &live,
                                                  diagnostic));
    CY_CHECK_EQ(std::string_view(diagnostic.data(), diagnostic.size()),
                std::string_view("StudioBody x53"));

    // With none outstanding, the same call permits it — a refusal that refused everything would
    // pass this test's first half and be useless.
    std::pair<const char*, u64> none{"StudioBody", 0};
    Array<char> empty(test_allocator());
    CY_CHECK(
        fixture.host.ownership().may_unload(fixture.physics.id, count_instances, &none, empty));
    CY_CHECK_EQ(empty.size(), 0U);
}

CY_TEST_CASE("a host with no way to count instances refuses rather than assuming none") {
    Fixture fixture;
    fixture.arm();
    const Array<PluginId> order = order_of({fixture.physics.id, fixture.terrain.id});
    CY_REQUIRE(fixture.host.bring_up(order.span()));
    Array<char> diagnostic(test_allocator());
    CY_CHECK(
        !fixture.host.ownership().may_unload(fixture.physics.id, nullptr, nullptr, diagnostic));
}

CY_TEST_CASE("two plugins cannot own one type") {
    TypeOwnership ownership(test_allocator());
    CY_REQUIRE(ownership.record(cy::Name::intern("studio.a"), cy::Name::intern("Body")));
    CY_CHECK(ownership.record(cy::Name::intern("studio.a"), cy::Name::intern("Body")));
    CY_CHECK(!ownership.record(cy::Name::intern("studio.b"), cy::Name::intern("Body")));
}

}  // namespace
