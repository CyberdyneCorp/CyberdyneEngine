// Hot reload across the ABI: the sequence, the generations, and the four ways it is refused.
// Task 2.7, and `native-abi`'s "Hot reload" requirement including its amendment.
//
// --- WHAT THESE CASES ARE FOR -------------------------------------------------------------------
//
// M4's spike answered the question "does state survive?" with Swift. It does, but not in place: the
// dangerous outcome — the one that looks perfect until the layout changes and then reports somebody
// else's fields with no diagnostic — is the DEFAULT outcome of the obvious implementation. So these
// cases hold the loader to the sequence that was measured to work, over C modules whose layout
// genuinely differs between the two schemas:
//
//   * state survives a reload across a layout change, by name, with a migration and a defaulted
//     new field;
//   * a reload the new module cannot accept is REFUSED and the previous generation stays live —
//     three separate ways, each with its own report;
//   * the retired generation's registration is still there afterwards, because its instances are
//     destroyed through its vtable and no image is ever unloaded.
//
// Observation goes through the module's own `serialize`, never through the struct: reading the
// instance as a C++ type would be the exact mistake under test, and it would pass.

#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/test/test.h>

#include <cstring>

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

// The manifest every case in this file loads with. Hot reload is declared, because the loader
// refuses to reload a module that did not ask for it.
constexpr const char* kManifestText =
    "name = \"counter\"\n"
    "entry_symbol = \"cy_module_entry\"\n"
    "min_abi_major = 1\n"
    "min_abi_minor = 0\n"
    "hot_reload = true\n";

struct Fixture {
    char manifest_text[256] = {};
    cy::abi::ModuleManifest manifest;
    cy::ecs::World world{allocator()};
    cy::abi::World binding{allocator(), world};
    cy::abi::Host host{allocator()};
    cy::abi::BehaviourRuntime runtime{allocator(), host};

    Fixture() {
        std::strncpy(manifest_text, kManifestText, sizeof(manifest_text) - 1);
        cy::Expected<cy::abi::ModuleManifest, cy::Error> parsed =
            cy::abi::parse_module_manifest(manifest_text);
        CY_REQUIRE(parsed.has_value());
        manifest = parsed.value();
        CY_REQUIRE(world.initialize().has_value());
        host.bind_world(&binding);
    }
};

// Read one named value out of the module's own blob. The format is the module's
// (src/abi/tests/module/behaviour.c): magic, schema, count, then length-prefixed keys with an i64.
//
// Reading state this way rather than through the struct is the point. A test that cast the instance
// to a C++ `Counter` would be doing exactly what the spike proved silently wrong, and it would
// agree with itself while the engine was corrupting the object.
bool blob_value(const cy::Array<cy::u8>& blob, const char* key, cy::i64& out) noexcept {
    if (blob.size() < 12) {
        return false;
    }
    cy::u32 count = 0;
    std::memcpy(&count, blob.data() + 8, sizeof(count));
    cy::usize offset = 12;
    for (cy::u32 index = 0; index < count; ++index) {
        cy::u32 length = 0;
        std::memcpy(&length, blob.data() + offset, sizeof(length));
        offset += sizeof(length);
        const bool matches =
            std::strlen(key) == length && std::memcmp(blob.data() + offset, key, length) == 0;
        offset += length;
        cy::i64 value = 0;
        std::memcpy(&value, blob.data() + offset, sizeof(value));
        offset += sizeof(value);
        if (matches) {
            out = value;
            return true;
        }
    }
    return false;
}

// Serialize one live instance THROUGH ITS OWN GENERATION'S VTABLE, which is the only correct way to
// read it and the same call the loader makes.
cy::Array<cy::u8> snapshot(const cy::abi::BehaviourRuntime& runtime, cy::u32 slot) noexcept {
    cy::Array<cy::u8> blob(allocator());
    const cy::abi::BehaviourInstance* live = runtime.instance(slot);
    if (live == nullptr || live->instance == nullptr) {
        return blob;
    }
    const CyBehaviourVTable& vtable = live->record->vtable;
    const cy::u32 required = vtable.serialize(live->instance, nullptr, 0, vtable.user_data);
    CY_REQUIRE(blob.resize(required).has_value());
    (void)vtable.serialize(live->instance, blob.data(), required, vtable.user_data);
    return blob;
}

cy::i64 field(const cy::abi::BehaviourRuntime& runtime, cy::u32 slot, const char* key) noexcept {
    const cy::Array<cy::u8> blob = snapshot(runtime, slot);
    cy::i64 value = -1;
    return blob_value(blob, key, value) ? value : -1;
}

}  // namespace

CY_TEST_CASE("a module loads, registers its type, and its behaviours run") {
    Fixture fixture;
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_ABI_TEST_MODULE_V1).has_value());
    CY_CHECK_EQ(fixture.runtime.generation(), 0U);

    // Registration happened at the Scene init level, through the interface table.
    CY_REQUIRE(fixture.host.find_behaviour("Counter") != nullptr);
    CY_CHECK_EQ(fixture.host.find_behaviour("Counter")->vtable.schema_version, 1U);

    cy::Expected<cy::u32, cy::Error> slot = fixture.runtime.create("Counter", 1);
    CY_REQUIRE(slot.has_value());
    for (int tick = 0; tick < 5; ++tick) {
        fixture.runtime.fixed_update(1.0F / 60.0F);
    }
    CY_CHECK_EQ(field(fixture.runtime, slot.value(), "ticks"), 5);
    CY_CHECK_EQ(field(fixture.runtime, slot.value(), "health"), 90);
    CY_CHECK_EQ(field(fixture.runtime, slot.value(), "ammo"), 17);
}

CY_TEST_CASE("state survives a reload across a layout change, by name") {
    // `native-abi`'s "Behaviour reload preserves state". The two images do not agree about the
    // instance's layout: schema 1 has `ammo` where schema 2 has `mana` and `shield`. Every value
    // below would be wrong — and wrong without a diagnostic — under in-place preservation.
    Fixture fixture;
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_ABI_TEST_MODULE_V1).has_value());

    cy::u32 slots[3] = {0, 0, 0};
    for (cy::u32 index = 0; index < 3; ++index) {
        cy::Expected<cy::u32, cy::Error> created = fixture.runtime.create("Counter", index + 1);
        CY_REQUIRE(created.has_value());
        slots[index] = created.value();
    }
    for (int tick = 0; tick < 5; ++tick) {
        fixture.runtime.fixed_update(1.0F / 60.0F);
    }

    cy::Expected<cy::abi::ReloadReport, cy::Error> report =
        fixture.runtime.reload(CY_ABI_TEST_MODULE_V2);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().failure, cy::abi::ReloadFailure::None);
    CY_CHECK_EQ(report.value().generation, 1U);
    CY_CHECK_EQ(report.value().instances, 3U);
    CY_CHECK_GT(report.value().bytes_serialized, 0U);

    for (const cy::u32 slot : slots) {
        // Carried across unchanged.
        CY_CHECK_EQ(field(fixture.runtime, slot, "health"), 90);
        CY_CHECK_EQ(field(fixture.runtime, slot, "ticks"), 5);
        // Migrated: schema 1's `ammo` of 17 becomes schema 2's `mana` of 8.
        CY_CHECK_EQ(field(fixture.runtime, slot, "mana"), 8);
        // New in schema 2, and therefore the module's default rather than a reinterpreted byte.
        CY_CHECK_EQ(field(fixture.runtime, slot, "shield"), 10);
    }

    // The new code runs from the next tick.
    fixture.runtime.fixed_update(1.0F / 60.0F);
    CY_CHECK_EQ(field(fixture.runtime, slots[0], "ticks"), 6);

    // THE RETIRED GENERATION IS STILL THERE. Its record was not dropped and its image was not
    // unloaded — which is what makes destroying a generation-0 instance through a generation-0
    // vtable safe rather than lucky. Two registrations of one name, one per generation.
    CY_CHECK_EQ(fixture.host.behaviours.size(), 2U);
    CY_CHECK_EQ(fixture.host.behaviours[0]->generation, 0U);
    CY_CHECK_EQ(fixture.host.behaviours[1]->generation, 1U);
    CY_CHECK_EQ(fixture.runtime.images(), 2U);
    // And a lookup by name reaches the CURRENT generation only, so no stale entry is reachable.
    CY_CHECK_EQ(fixture.host.find_behaviour("Counter")->generation, 1U);
}

CY_TEST_CASE("a reload whose schema predates the saved state is refused, and the old code lives") {
    // `native-abi`'s "Incompatible reload". The module going back to schema 1 cannot read a schema
    // 2 blob and says so; the loader must keep the previous generation and report, not restore into
    // a shape nobody wrote.
    Fixture fixture;
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_ABI_TEST_MODULE_V1).has_value());
    cy::Expected<cy::u32, cy::Error> slot = fixture.runtime.create("Counter", 1);
    CY_REQUIRE(slot.has_value());
    fixture.runtime.fixed_update(1.0F / 60.0F);
    CY_REQUIRE(fixture.runtime.reload(CY_ABI_TEST_MODULE_V2).has_value());

    cy::Expected<cy::abi::ReloadReport, cy::Error> report =
        fixture.runtime.reload(CY_ABI_TEST_MODULE_V1);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().failure, cy::abi::ReloadFailure::SchemaTooNew);
    CY_CHECK(std::strcmp(report.value().detail, "Counter") == 0);

    // The generation did not advance, the instance is still the one that was live, and it still
    // answers correctly — the refusal cost nothing.
    CY_CHECK_EQ(fixture.runtime.generation(), 1U);
    CY_CHECK_EQ(fixture.runtime.live_instances(), 1U);
    CY_CHECK_EQ(field(fixture.runtime, slot.value(), "mana"), 8);
    fixture.runtime.fixed_update(1.0F / 60.0F);
    CY_CHECK_EQ(field(fixture.runtime, slot.value(), "ticks"), 2);
}

CY_TEST_CASE("a reload that drops a live type is refused") {
    // The other half of "Incompatible reload": the new module simply does not have the type any
    // more. There is nowhere for the instance to go, so the reload does not happen.
    Fixture fixture;
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_ABI_TEST_MODULE_V1).has_value());
    CY_REQUIRE(fixture.runtime.create("Counter", 1).has_value());

    cy::Expected<cy::abi::ReloadReport, cy::Error> report =
        fixture.runtime.reload(CY_ABI_TEST_MODULE_RENAMED);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().failure, cy::abi::ReloadFailure::TypeNotRegistered);
    CY_CHECK_EQ(fixture.runtime.generation(), 0U);
    CY_CHECK_EQ(fixture.runtime.live_instances(), 1U);
    // The abandoned generation left nothing behind: the registration it made is gone, not merely
    // invisible.
    CY_CHECK_EQ(fixture.host.behaviours.size(), 1U);
}

CY_TEST_CASE("an entry point that returns false is reported, not fatal") {
    // `native-abi`'s "Entry point returns false" — on the first load and on a reload. In both cases
    // the engine keeps running, which is the whole requirement.
    {
        Fixture fixture;
        cy::Expected<cy::abi::ReloadReport, cy::Error> loaded =
            fixture.runtime.load(fixture.manifest, CY_ABI_TEST_MODULE_REFUSE);
        CY_CHECK_FALSE(loaded.has_value());
        CY_CHECK_EQ(fixture.runtime.images(), 0U);
    }
    {
        Fixture fixture;
        CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_ABI_TEST_MODULE_V1).has_value());
        CY_REQUIRE(fixture.runtime.create("Counter", 1).has_value());
        cy::Expected<cy::abi::ReloadReport, cy::Error> report =
            fixture.runtime.reload(CY_ABI_TEST_MODULE_REFUSE);
        CY_REQUIRE(report.has_value());
        CY_CHECK_EQ(report.value().failure, cy::abi::ReloadFailure::EntryRefused);
        CY_CHECK_EQ(fixture.runtime.generation(), 0U);
        CY_CHECK_EQ(fixture.runtime.live_instances(), 1U);
    }
}

CY_TEST_CASE("a library that is not there is a failed load, not a crash") {
    Fixture fixture;
    CY_CHECK_FALSE(
        fixture.runtime.load(fixture.manifest, "/nonexistent/libnothing.so").has_value());
    CY_CHECK_EQ(fixture.runtime.images(), 0U);

    // And a reload that cannot open its image changes nothing at all.
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_ABI_TEST_MODULE_V1).has_value());
    CY_REQUIRE(fixture.runtime.create("Counter", 1).has_value());
    cy::Expected<cy::abi::ReloadReport, cy::Error> report =
        fixture.runtime.reload("/nonexistent/libnothing.so");
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().failure, cy::abi::ReloadFailure::ImageDidNotOpen);
    CY_CHECK_EQ(fixture.runtime.generation(), 0U);
    CY_CHECK_EQ(fixture.runtime.live_instances(), 1U);
}

CY_TEST_CASE("a module that did not declare hot reload is not reloaded") {
    // Reloading a module that never said it could be is how state that has no serializer gets
    // silently dropped. The declaration is opt-in and the loader honours it.
    Fixture fixture;
    fixture.manifest.hot_reload = false;
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_ABI_TEST_MODULE_V1).has_value());
    cy::Expected<cy::abi::ReloadReport, cy::Error> report =
        fixture.runtime.reload(CY_ABI_TEST_MODULE_V2);
    CY_REQUIRE_FALSE(report.has_value());
    CY_CHECK_EQ(report.error().code, cy::ErrorCode::PermissionDenied);
}

CY_TEST_CASE("reload time and address-space cost stay flat as generations accumulate") {
    // The spike measured reload wall time flat across 40 generations and 58-85 kB of address space
    // per image, never reclaimed. What this case can assert cheaply is the structural half of that
    // claim: every reload adds exactly one image and one generation, and nothing accumulates per
    // instance — so the cost is linear in reloads and independent of how long the session has run.
    Fixture fixture;
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_ABI_TEST_MODULE_V1).has_value());
    CY_REQUIRE(fixture.runtime.create("Counter", 1).has_value());

    // Alternating between the two schemas is a migration in both directions... except that going
    // back is refused, which is the previous case. So the repeated reload here is v2 onto v2: a
    // module rebuilt without a schema change, which is what the overwhelming majority of edits are.
    CY_REQUIRE(fixture.runtime.reload(CY_ABI_TEST_MODULE_V2).has_value());
    for (int cycle = 0; cycle < 8; ++cycle) {
        cy::Expected<cy::abi::ReloadReport, cy::Error> report =
            fixture.runtime.reload(CY_ABI_TEST_MODULE_V2);
        CY_REQUIRE(report.has_value());
        CY_REQUIRE_EQ(report.value().failure, cy::abi::ReloadFailure::None);
        CY_CHECK_EQ(report.value().instances, 1U);
    }
    CY_CHECK_EQ(fixture.runtime.generation(), 9U);
    CY_CHECK_EQ(fixture.runtime.images(), 10U);
    // One registration per generation and no more: the record set grows with reloads, not with
    // instances or with ticks.
    CY_CHECK_EQ(fixture.host.behaviours.size(), 10U);
    // And the state is still the state, nine generations later.
    CY_CHECK_EQ(field(fixture.runtime, 0, "mana"), 8);
    CY_CHECK_EQ(field(fixture.runtime, 0, "shield"), 10);
}
