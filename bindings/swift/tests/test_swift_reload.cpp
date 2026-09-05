// A real Swift game module, loaded and hot-reloaded by the engine's own loader.
// Tasks 3.2, 3.5, 3.7.
//
// --- WHAT THIS ASSERTS THAT NOTHING ELSE DOES
// -----------------------------------------------------
//
// `integration.abi_reload` holds the loader to the reload sequence over C modules. This suite holds
// SWIFT to it, which is a different claim in three ways, and every one of them is a thing the
// hot-reload spike measured going wrong:
//
//   1. A SWIFT OBJECT'S STATE, not a C struct's. `@Export`ed properties are Swift stored properties
//      behind property wrappers, and one of them is a `String` — an ARC-managed heap allocation
//      whose metadata word points into the image that created it. The spike read one of those as a
//      `Double` and got 3.5e18 with no trap. Carrying it across a reload BY NAME is the only thing
//      that works, and this is where that is checked with a real Swift runtime in the process.
//
//   2. A DIFFERENT SWIFT `-module-name` PER GENERATION. `bindings/swift/tools/cy_swift_module.py`
//      builds `CyGame_g0`, `CyGame_g1` and `CyGame_g2`, and `cy/abi/module.h` says why: name-based
//      type lookup is process-global and first-registration-wins, so two resident images called the
//      same thing make the new one find the old one's metadata. The loader cannot enforce it; this
//      suite is downstream of a build that does.
//
//   3. THE SWIFT SIDE OF THE SCHEMA CHECK. Generation 2 is generation 0's source again — schema 1 —
//      so reloading into it with a schema-2 blob is a DOWNGRADE. `CyberdyneKit`'s deserialize thunk
//      answers CY_RESULT_SCHEMA_TOO_NEW, and the loader must keep generation 1 live. That is
//      `native-abi`'s "Incompatible reload" reached through Swift rather than through C.
//
// STATE IS READ THROUGH THE MODULE'S OWN `serialize`, never by casting the instance. Casting is
// precisely the mistake under test: it agrees with itself while the object is being misread.

#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/base/diagnostic_sink.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

constexpr const char* kManifestText =
    "name = \"swift_reload_fixture\"\n"
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

// --- Reading CyberdyneKit's blob
// ------------------------------------------------------------------
//
// The format is Serialization.swift's, which is the hot-reload spike's with a kind per entry:
//
//     magic 'CYST' u32 | schema u32 | count u32 | count x (keyLength u32, key, kind u32, payload)
//
// Only the two kinds this fixture uses are decoded — i64 and string. A kind the reader does not
// know ENDS the scan rather than being skipped, because its payload width is unknown; that is the
// same rule the Swift reader follows and the reason the kind is in the blob at all.
struct BlobCursor {
    const cy::u8* data = nullptr;
    cy::usize size = 0;
    cy::usize offset = 0;

    [[nodiscard]] bool word(cy::u32& out) noexcept {
        if (offset + sizeof(out) > size) {
            return false;
        }
        std::memcpy(&out, data + offset, sizeof(out));
        offset += sizeof(out);
        return true;
    }

    [[nodiscard]] bool doubleword(cy::i64& out) noexcept {
        if (offset + sizeof(out) > size) {
            return false;
        }
        std::memcpy(&out, data + offset, sizeof(out));
        offset += sizeof(out);
        return true;
    }
};

constexpr cy::u32 kBlobMagic = 0x54535943u;  // 'CYST'
constexpr cy::u32 kKindI64 = 2u;
constexpr cy::u32 kKindString = 9u;

// The state of one instance, as its own module reports it.
struct CounterState {
    bool valid = false;
    cy::u32 schema = 0;
    cy::i64 health = 0;
    cy::i64 ammo = 0;
    cy::i64 mana = 0;
    cy::i64 shield = 0;
    std::string label;
    bool has_ammo = false;
    bool has_mana = false;
    bool has_shield = false;
};

void assign(CounterState& state, const std::string& key, cy::i64 value) noexcept {
    if (key == "health") {
        state.health = value;
    } else if (key == "ammo") {
        state.ammo = value;
        state.has_ammo = true;
    } else if (key == "mana") {
        state.mana = value;
        state.has_mana = true;
    } else if (key == "shield") {
        state.shield = value;
        state.has_shield = true;
    }
}

CounterState parse_blob(const cy::Array<cy::u8>& blob) noexcept {
    CounterState state;
    BlobCursor cursor{blob.data(), blob.size(), 0};
    cy::u32 magic = 0;
    cy::u32 count = 0;
    if (!cursor.word(magic) || !cursor.word(state.schema) || !cursor.word(count) ||
        magic != kBlobMagic) {
        return state;
    }
    for (cy::u32 index = 0; index < count; ++index) {
        cy::u32 key_length = 0;
        if (!cursor.word(key_length) || cursor.offset + key_length > cursor.size) {
            return state;
        }
        const std::string key(reinterpret_cast<const char*>(cursor.data + cursor.offset),
                              key_length);
        cursor.offset += key_length;
        cy::u32 kind = 0;
        if (!cursor.word(kind)) {
            return state;
        }
        if (kind == kKindI64) {
            cy::i64 value = 0;
            if (!cursor.doubleword(value)) {
                return state;
            }
            assign(state, key, value);
        } else if (kind == kKindString) {
            cy::u32 length = 0;
            if (!cursor.word(length) || cursor.offset + length > cursor.size) {
                return state;
            }
            if (key == "label") {
                state.label.assign(reinterpret_cast<const char*>(cursor.data + cursor.offset),
                                   length);
            }
            cursor.offset += length;
        } else {
            return state;
        }
    }
    state.valid = true;
    return state;
}

// Ask the instance's OWN generation to serialize itself, then read the blob. Two calls, because the
// size query is how `native-abi` says a host sizes a blob: a null buffer reports the requirement.
CounterState read_state(cy::abi::BehaviourRuntime& runtime, cy::u32 slot) noexcept {
    const cy::abi::BehaviourInstance* live = runtime.instance(slot);
    if (live == nullptr || live->record == nullptr || live->record->vtable.serialize == nullptr) {
        return {};
    }
    const cy::u32 required =
        live->record->vtable.serialize(live->instance, nullptr, 0, live->record->vtable.user_data);
    cy::Array<cy::u8> blob(allocator());
    CY_REQUIRE(blob.resize(required).has_value());
    const cy::u32 written = live->record->vtable.serialize(live->instance, blob.data(), required,
                                                           live->record->vtable.user_data);
    CY_REQUIRE(written == required);
    return parse_blob(blob);
}

}  // namespace

// REGRESSION: the registration name must outlive the registration.
//
// The first version of `Behaviours.register` passed the name through `withCString`, whose pointer
// is valid only inside the closure. `register_behaviour` returned a handle, nothing reported a
// failure, and `find_behaviour` then compared against freed memory and answered null — no crash, no
// diagnostic, the behaviour simply absent. `CyberdyneKit/CStrings.swift` is the fix and this case
// is what would have caught it.
CY_TEST_CASE("a Swift module loads and registers its behaviour") {
    Fixture fixture;
    const cy::Expected<cy::abi::ReloadReport, cy::Error> loaded =
        fixture.runtime.load(fixture.manifest, CY_SWIFT_MODULE_G0);
    CY_REQUIRE(loaded.has_value());
    CY_CHECK(loaded.value().failure == cy::abi::ReloadFailure::None);
    CY_CHECK(fixture.runtime.generation() == 0);

    // Registered by name, from Swift, through `register_behaviour` — the same entry a C module
    // uses.
    CY_REQUIRE(fixture.host.find_behaviour("SwiftCounter") != nullptr);
    CY_CHECK(fixture.host.find_behaviour("NotARealBehaviour") == nullptr);
}

CY_TEST_CASE("a Swift behaviour's state survives a reload across a layout change") {
    Fixture fixture;
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_SWIFT_MODULE_G0).has_value());

    const cy::Expected<cy::u32, cy::Error> slot =
        fixture.runtime.create("SwiftCounter", cy::abi::to_abi(cy::ecs::Entity{}));
    CY_REQUIRE(slot.has_value());

    // Ten fixed ticks of Swift code: `health` counts down from its exported default.
    for (int tick = 0; tick < 10; ++tick) {
        fixture.runtime.fixed_update(1.0F / 60.0F);
    }

    const CounterState before = read_state(fixture.runtime, slot.value());
    CY_REQUIRE(before.valid);
    CY_CHECK(before.schema == 1);
    CY_CHECK(before.health == 85);  // 95 - 10
    CY_CHECK(before.has_ammo);
    CY_CHECK(before.ammo == 34);
    CY_CHECK(before.label == "player");

    // THE RELOAD. A different file and a different Swift module name, which the build guarantees.
    const cy::Expected<cy::abi::ReloadReport, cy::Error> reloaded =
        fixture.runtime.reload(CY_SWIFT_MODULE_G1);
    CY_REQUIRE(reloaded.has_value());
    CY_CHECK(reloaded.value().failure == cy::abi::ReloadFailure::None);
    CY_CHECK(reloaded.value().instances == 1);
    CY_CHECK(fixture.runtime.generation() == 1);
    // The old image is RETIRED, not unloaded. Two images resident is the whole model.
    CY_CHECK(fixture.runtime.images() == 2);

    const CounterState after = read_state(fixture.runtime, slot.value());
    CY_REQUIRE(after.valid);
    CY_CHECK(after.schema == 2);
    CY_CHECK(after.health == 85);  // carried by name
    CY_CHECK(after.has_mana);
    CY_CHECK(after.mana == 17);  // migrated: ammo / 2, in onMigrate
    CY_CHECK(after.has_shield);
    CY_CHECK(after.shield == 10);       // a field schema 1 did not have keeps its default
    CY_CHECK(after.label == "player");  // a Swift String, ARC-managed, intact
    CY_CHECK(!after.has_ammo);          // the old key is gone, not reinterpreted

    // And the new generation's code is what runs now.
    fixture.runtime.fixed_update(1.0F / 60.0F);
    const CounterState ticked = read_state(fixture.runtime, slot.value());
    CY_REQUIRE(ticked.valid);
    CY_CHECK(ticked.health == 84);
}

CY_TEST_CASE(
    "a Swift module whose schema predates the blob is refused and the old one stays live") {
    Fixture fixture;
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_SWIFT_MODULE_G0).has_value());
    const cy::Expected<cy::u32, cy::Error> slot =
        fixture.runtime.create("SwiftCounter", cy::abi::to_abi(cy::ecs::Entity{}));
    CY_REQUIRE(slot.has_value());
    CY_REQUIRE(fixture.runtime.reload(CY_SWIFT_MODULE_G1).has_value());
    fixture.runtime.fixed_update(1.0F / 60.0F);

    // Generation 2 is generation 0's source: schema 1, reading a schema-2 blob. `CyberdyneKit`'s
    // deserialize answers CY_RESULT_SCHEMA_TOO_NEW and the loader keeps generation 1.
    const cy::Expected<cy::abi::ReloadReport, cy::Error> refused =
        fixture.runtime.reload(CY_SWIFT_MODULE_G2);
    CY_REQUIRE(refused.has_value());
    CY_CHECK(refused.value().failure == cy::abi::ReloadFailure::SchemaTooNew);
    CY_CHECK(fixture.runtime.generation() == 1);

    // Still live, still generation 1's shape, still ticking.
    const CounterState kept = read_state(fixture.runtime, slot.value());
    CY_REQUIRE(kept.valid);
    CY_CHECK(kept.schema == 2);
    CY_CHECK(kept.has_shield);
    CY_CHECK(kept.label == "player");
}

CY_TEST_CASE("many reloads leave every generation's instances valid") {
    Fixture fixture;
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_SWIFT_MODULE_G0).has_value());
    const cy::Expected<cy::u32, cy::Error> first =
        fixture.runtime.create("SwiftCounter", cy::abi::to_abi(cy::ecs::Entity{}));
    CY_REQUIRE(first.has_value());
    fixture.runtime.fixed_update(1.0F / 60.0F);

    CY_REQUIRE(fixture.runtime.reload(CY_SWIFT_MODULE_G1).has_value());
    const cy::Expected<cy::u32, cy::Error> second =
        fixture.runtime.create("SwiftCounter", cy::abi::to_abi(cy::ecs::Entity{}));
    CY_REQUIRE(second.has_value());
    CY_CHECK(fixture.runtime.live_instances() == 2);

    // Both instances belong to generation 1 now — the first was recreated by the reload — and both
    // tick. The claim underneath is the model's: an image is never unloaded, so nothing dangles.
    for (int tick = 0; tick < 5; ++tick) {
        fixture.runtime.fixed_update(1.0F / 60.0F);
    }
    const CounterState carried = read_state(fixture.runtime, first.value());
    const CounterState fresh = read_state(fixture.runtime, second.value());
    CY_REQUIRE(carried.valid);
    CY_REQUIRE(fresh.valid);
    CY_CHECK(carried.health == 89);  // 95 - 1 before the reload, - 5 after
    CY_CHECK(fresh.health == 90);    // 95 - 5
    CY_CHECK(carried.schema == 2);
    CY_CHECK(fresh.schema == 2);
}

// REGRESSION. `Log.info` from a behaviour arrived in the engine's log as an ERROR.
//
// `CyberdyneKit`'s `Severity` shipped with six enumerators numbered 0-5 — trace, debug, info,
// warning, error, fatal — and the comment above it claimed they were `cy::DiagnosticSeverity`'s
// values. They are not: that enum has three, Info 0, Warning 1, Error 2. So `.info` went over the
// ABI as 2, and `abi_log` in src/abi/src/interface.cpp turned it into `DiagnosticSeverity::Error`.
//
// NOTHING FAILED. The message arrived, with the right text, under the wrong severity: the reload
// suite printed `[error] abi: SwiftCounter restored ammo, health, label` on a green run, four
// times, and no assertion in this file or in the Swift package could see it — the Swift side
// asserts what it SENT, and the number it sent was the number it meant. Only a reader on the
// engine's side of the boundary can catch a mistranslation, which is why this case installs a sink
// rather than checking an enum's raw value.
//
// It also makes the clamp visible: `abi_log` maps everything above Error to Error, so an
// out-of-range severity is not an error but a relabelling, and a test that only looked for a crash
// would find nothing to look at.
CY_TEST_CASE("a behaviour's Log.info reaches the engine as Info, not as Error") {
    struct Captured {
        int info = 0;
        int warning = 0;
        int error = 0;
        std::string first_error;
    };
    static Captured captured;
    captured = Captured{};

    const cy::DiagnosticSink previous = cy::set_diagnostic_sink(
        [](cy::DiagnosticSeverity severity, const char* category, const char* message,
           void* /*user*/) {
            if (std::strcmp(category, "abi") != 0) {
                return;
            }
            switch (severity) {
                case cy::DiagnosticSeverity::Info:
                    captured.info += 1;
                    break;
                case cy::DiagnosticSeverity::Warning:
                    captured.warning += 1;
                    break;
                case cy::DiagnosticSeverity::Error:
                    captured.error += 1;
                    if (captured.first_error.empty()) {
                        captured.first_error = (message != nullptr) ? message : "";
                    }
                    break;
            }
        },
        nullptr);

    {
        Fixture fixture;
        CY_REQUIRE(fixture.runtime.load(fixture.manifest, CY_SWIFT_MODULE_G0).has_value());
        CY_REQUIRE(
            fixture.runtime.create("SwiftCounter", cy::abi::to_abi(cy::ecs::Entity{})).has_value());
        // Generation 1's `onAfterReload` calls `Log.info` exactly once per restored instance.
        CY_REQUIRE(fixture.runtime.reload(CY_SWIFT_MODULE_G1).has_value());
    }

    (void)cy::set_diagnostic_sink(previous, nullptr);

    CY_CHECK(captured.info == 1);
    // The whole point: a clean reload of a working module emits NO error. Before the fix this was
    // 1, and the message was the informational one.
    CY_CHECK(captured.error == 0);
    CY_CHECK(captured.first_error.empty());
}
