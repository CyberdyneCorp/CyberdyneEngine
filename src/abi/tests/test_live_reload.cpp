// Reloading a module WHILE THE LOOP IS RUNNING. M5 task 1.1.
//
// --- WHAT MAKES THIS SUITE DIFFERENT FROM test_reload.cpp ---------------------------------------
//
// test_reload.cpp proves the SEQUENCE: state survives a layout change, an incompatible reload is
// refused, a retired generation stays callable. Every one of its cases stops the world, calls
// `reload()` by hand with a path it was compiled with, and starts again. Nothing in it is a live
// session, and M4 closed with nothing that was.
//
// These cases are the live session. The loop below never stops: it ticks, it polls, it applies at
// the boundary, and the image underneath it changes while it does. The assertions are the three
// things that make that trustworthy —
//
//   * the swap happened DURING the loop, at a tick the test can name;
//   * the instance's state crossed it, and the new code continued from the value the old code left,
//     rather than from a default or from a reinterpreted byte;
//   * a boundary that is not a boundary — the world iterating — defers instead of corrupting.
//
// The staging directory is populated by copying the two test modules, because what the watcher
// looks for is `<stem>_g<N><extension>` and CMake writes the two images under names of its own. For
// a C module a copy is a legitimate new image; for a Swift one it would not be, and
// cy/abi/live_reload.h says why at length. `bindings/swift/tests/test_swift_reload.cpp` is where
// that case is exercised against images the Swift build produced with distinct module names.

#include <cy/abi/errors.h>
#include <cy/abi/host.h>
#include <cy/abi/live_reload.h>
#include <cy/abi/module.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/test/test.h>

#include <cstring>

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Scripting);
}

constexpr const char* kManifestText =
    "name = \"counter\"\n"
    "entry_symbol = \"cy_module_entry\"\n"
    "min_abi_major = 1\n"
    "min_abi_minor = 0\n"
    "hot_reload = true\n";

constexpr const char* kStem = "cy_live_module";
constexpr const char* kExtension = ".so";

// The staging directory this suite owns. One per case, named after the case, so that two cases
// cannot see each other's generations — a shared directory would make the order the cases run in
// part of the result.
/// Append a NUL-terminated string, keeping the buffer a C string at every step. Character by
/// character rather than with `memcpy`, which is what src/backends/shader/src/cache.cpp settled on
/// and for the same reason: an unterminated `memcpy` into a buffer that is used as a string is a
/// real finding, and suppressing it would be suppressing it everywhere this file copies.
cy::usize append(char* out, cy::usize offset, const char* text) noexcept {
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        out[offset++] = *cursor;
    }
    out[offset] = '\0';
    return offset;
}

struct Staging {
    char path[512] = {};

    explicit Staging(const char* name) noexcept {
        const cy::usize root_length = std::strlen(CY_ABI_TEST_STAGE_DIR);
        const cy::usize name_length = std::strlen(name);
        CY_REQUIRE(root_length + 1 + name_length + 1 <= sizeof(path));
        cy::usize offset = append(path, 0, CY_ABI_TEST_STAGE_DIR);
        path[offset++] = '/';
        path[offset] = '\0';
        (void)append(path, offset, name);
        // A previous run's generations would be read as new ones, so the directory starts empty.
        (void)cy::assets::fs::remove_directory_recursive(path);
        CY_REQUIRE(cy::assets::fs::create_directories(path).has_value());
    }

    /// Publish `source` as generation `generation`. This is what a build writing a new image does.
    void publish(cy::u32 generation, const char* source) const noexcept {
        char target[512] = {};
        const int written = format(target, sizeof(target), generation);
        CY_REQUIRE(written > 0);
        CY_REQUIRE(cy::assets::fs::copy_file(source, target).has_value());
    }

    [[nodiscard]] int format(char* out, cy::usize capacity, cy::u32 generation) const noexcept {
        // "<path>/<stem>_g<N><extension>", with a single-digit N: no case here reaches ten.
        CY_REQUIRE(generation < 10U);
        const cy::usize path_length = std::strlen(path);
        const cy::usize stem_length = std::strlen(kStem);
        const cy::usize extension_length = std::strlen(kExtension);
        if (path_length + 1 + stem_length + 3 + extension_length + 1 > capacity) {
            return -1;
        }
        cy::usize offset = append(out, 0, path);
        out[offset++] = '/';
        out[offset] = '\0';
        offset = append(out, offset, kStem);
        out[offset++] = '_';
        out[offset++] = 'g';
        out[offset++] = static_cast<char>('0' + static_cast<char>(generation));
        out[offset] = '\0';
        (void)append(out, offset, kExtension);
        return 1;
    }
};

struct Fixture {
    char manifest_text[256] = {};
    cy::abi::ModuleManifest manifest;
    cy::ecs::World world{allocator()};
    cy::abi::World binding{allocator(), world};
    cy::abi::Host host{allocator()};
    cy::abi::BehaviourRuntime runtime{allocator(), host};
    cy::abi::LiveReload live{runtime};

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

// Read one named value out of the module's own blob, through the vtable of the generation that
// created the instance. Identical in intent to test_reload.cpp's: reading the instance as a C++
// struct is the mistake the whole reload model exists to prevent, and it would agree with itself.
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

cy::i64 field(const cy::abi::BehaviourRuntime& runtime, cy::u32 slot, const char* key) noexcept {
    cy::Array<cy::u8> blob(allocator());
    const cy::abi::BehaviourInstance* live = runtime.instance(slot);
    if (live == nullptr || live->instance == nullptr) {
        return -1;
    }
    const CyBehaviourVTable& vtable = live->record->vtable;
    const cy::u32 required = vtable.serialize(live->instance, nullptr, 0, vtable.user_data);
    CY_REQUIRE(blob.resize(required).has_value());
    (void)vtable.serialize(live->instance, blob.data(), required, vtable.user_data);
    cy::i64 value = -1;
    return blob_value(blob, key, value) ? value : -1;
}

}  // namespace

CY_TEST_CASE("a module reloads while the loop is running, and the loop never stops") {
    // THE CASE M5 CANNOT PROCEED WITHOUT. Nothing here calls reload(); the loop polls and applies,
    // and the reload happens because a file appeared.
    const Staging staging("running");
    staging.publish(0, CY_ABI_TEST_MODULE_V1);

    Fixture fixture;
    char generation_zero[512] = {};
    CY_REQUIRE(staging.format(generation_zero, sizeof(generation_zero), 0) > 0);
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, generation_zero).has_value());
    CY_REQUIRE(
        fixture.live.watch(cy::abi::LiveReloadSource{staging.path, kStem, kExtension}).has_value());

    cy::Expected<cy::u32, cy::Error> slot = fixture.runtime.create("Counter", 1);
    CY_REQUIRE(slot.has_value());

    cy::u32 applied_at = 0;
    cy::i64 ticks_before_swap = -1;
    for (cy::u32 tick = 1; tick <= 40; ++tick) {
        // The loop's own work, unchanged by anything below it.
        fixture.runtime.fixed_update(1.0F / 60.0F);

        // A build lands mid-session. From here the loop is expected to notice on its own.
        if (tick == 10) {
            ticks_before_swap = field(fixture.runtime, slot.value(), "ticks");
            staging.publish(1, CY_ABI_TEST_MODULE_V2);
        }

        (void)fixture.live.poll();
        cy::Expected<cy::abi::LiveReloadStatus, cy::Error> status =
            fixture.live.apply_at_frame_boundary(fixture.world);
        CY_REQUIRE(status.has_value());
        if (status.value().outcome == cy::abi::LiveReloadOutcome::Applied) {
            CY_CHECK_EQ(applied_at, 0U);  // exactly once, not once per frame
            applied_at = tick;
            CY_CHECK_EQ(status.value().report.failure, cy::abi::ReloadFailure::None);
            CY_CHECK_EQ(status.value().report.instances, 1U);
            CY_CHECK_EQ(status.value().generation, 1U);
        }
    }

    // It happened, it happened while the loop was running, and it happened after the settle poll
    // rather than on the same frame the file appeared.
    CY_CHECK_GT(applied_at, 10U);
    CY_CHECK_LT(applied_at, 15U);
    CY_CHECK_EQ(ticks_before_swap, 10);

    // STATE CROSSED THE SWAP AND THE NEW CODE CONTINUED FROM IT. `ticks` is 40 because generation 1
    // resumed at 10 rather than at zero; `mana` is schema 1's `ammo` migrated by name; `shield` is
    // new in schema 2 and therefore the module's default rather than a reinterpreted byte.
    CY_CHECK_EQ(field(fixture.runtime, slot.value(), "ticks"), 40);
    CY_CHECK_EQ(field(fixture.runtime, slot.value(), "mana"), 8);
    CY_CHECK_EQ(field(fixture.runtime, slot.value(), "shield"), 10);

    // The session's own record of what it did.
    CY_CHECK_EQ(fixture.live.status().applied, 1U);
    CY_CHECK_EQ(fixture.live.status().refused, 0U);
    CY_CHECK_EQ(fixture.live.status().deferred, 0U);
    CY_CHECK_EQ(fixture.live.status().polls, 40U);
    CY_CHECK_EQ(fixture.runtime.generation(), 1U);
    CY_CHECK_EQ(fixture.runtime.images(), 2U);
}

CY_TEST_CASE(
    "a reload is deferred while the world is iterating, and applied at the next boundary") {
    // The half that makes "at a frame boundary" mean something. A reload destroys and recreates
    // every instance; under an iterating query that is a use-after-free the ECS cannot see, because
    // the ECS is not what moved.
    const Staging staging("iterating");
    staging.publish(0, CY_ABI_TEST_MODULE_V1);

    Fixture fixture;
    char generation_zero[512] = {};
    CY_REQUIRE(staging.format(generation_zero, sizeof(generation_zero), 0) > 0);
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, generation_zero).has_value());
    CY_REQUIRE(fixture.runtime.create("Counter", 1).has_value());

    char generation_one[512] = {};
    CY_REQUIRE(staging.format(generation_one, sizeof(generation_one), 1) > 0);
    staging.publish(1, CY_ABI_TEST_MODULE_V2);
    CY_REQUIRE(fixture.live.request(generation_one).has_value());
    CY_CHECK(fixture.live.pending());

    {
        const cy::ecs::World::IterationGuard guard(fixture.world);
        cy::Expected<cy::abi::LiveReloadStatus, cy::Error> status =
            fixture.live.apply_at_frame_boundary(fixture.world);
        CY_REQUIRE(status.has_value());
        CY_CHECK_EQ(status.value().outcome, cy::abi::LiveReloadOutcome::Deferred);
        // Nothing was attempted: the request survives and the generation did not move.
        CY_CHECK(fixture.live.pending());
        CY_CHECK_EQ(fixture.runtime.generation(), 0U);
    }

    cy::Expected<cy::abi::LiveReloadStatus, cy::Error> status =
        fixture.live.apply_at_frame_boundary(fixture.world);
    CY_REQUIRE(status.has_value());
    CY_CHECK_EQ(status.value().outcome, cy::abi::LiveReloadOutcome::Applied);
    CY_CHECK_EQ(fixture.runtime.generation(), 1U);
    CY_CHECK_EQ(fixture.live.status().deferred, 1U);
    CY_CHECK_FALSE(fixture.live.pending());
}

CY_TEST_CASE("a refused reload leaves the session live and is not retried every frame") {
    // A build that cannot be loaded is the ordinary case in a live session — somebody saved a file
    // that does not compile into a module that does not run. The loop must survive it, and it must
    // not spend the rest of the session attempting the same image once per frame.
    const Staging staging("refused");
    staging.publish(0, CY_ABI_TEST_MODULE_V1);

    Fixture fixture;
    char generation_zero[512] = {};
    CY_REQUIRE(staging.format(generation_zero, sizeof(generation_zero), 0) > 0);
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, generation_zero).has_value());
    CY_REQUIRE(
        fixture.live.watch(cy::abi::LiveReloadSource{staging.path, kStem, kExtension}).has_value());
    cy::Expected<cy::u32, cy::Error> slot = fixture.runtime.create("Counter", 1);
    CY_REQUIRE(slot.has_value());

    // Generation 1 renames the type, so the live instance has nowhere to go.
    staging.publish(1, CY_ABI_TEST_MODULE_RENAMED);

    for (cy::u32 tick = 1; tick <= 20; ++tick) {
        fixture.runtime.fixed_update(1.0F / 60.0F);
        (void)fixture.live.poll();
        cy::Expected<cy::abi::LiveReloadStatus, cy::Error> status =
            fixture.live.apply_at_frame_boundary(fixture.world);
        CY_REQUIRE(status.has_value());
    }

    // ONCE. Not nineteen times.
    CY_CHECK_EQ(fixture.live.status().refused, 1U);
    CY_CHECK_EQ(fixture.live.status().applied, 0U);
    CY_CHECK_EQ(fixture.live.status().report.failure, cy::abi::ReloadFailure::TypeNotRegistered);

    // And the session is untouched: generation 0 is still live and still ticking its instance.
    CY_CHECK_EQ(fixture.runtime.generation(), 0U);
    CY_CHECK_EQ(fixture.runtime.live_instances(), 1U);
    CY_CHECK_EQ(field(fixture.runtime, slot.value(), "ticks"), 20);
}

CY_TEST_CASE("a watcher ignores everything in the staging directory that is not a generation") {
    // The scan is strict on purpose: a debug-information file, an editor's backup or a
    // half-renamed image must not be handed to dlopen. Each of these is a real name a build
    // directory grows.
    const Staging staging("noise");
    staging.publish(0, CY_ABI_TEST_MODULE_V1);

    Fixture fixture;
    char generation_zero[512] = {};
    CY_REQUIRE(staging.format(generation_zero, sizeof(generation_zero), 0) > 0);
    CY_REQUIRE(fixture.runtime.load(fixture.manifest, generation_zero).has_value());
    CY_REQUIRE(
        fixture.live.watch(cy::abi::LiveReloadSource{staging.path, kStem, kExtension}).has_value());

    for (const char* name : {"cy_live_module_g1.so.bak", "cy_live_module_gx.so",
                             "cy_live_module_g.so", "cy_live_module_g1.debug", "other_g9.so"}) {
        char target[512] = {};
        const cy::usize path_length = std::strlen(staging.path);
        const cy::usize name_length = std::strlen(name);
        CY_REQUIRE(path_length + 1 + name_length + 1 <= sizeof(target));
        cy::usize offset = append(target, 0, staging.path);
        target[offset++] = '/';
        target[offset] = '\0';
        (void)append(target, offset, name);
        CY_REQUIRE(cy::assets::fs::copy_file(CY_ABI_TEST_MODULE_V2, target).has_value());
    }

    for (cy::u32 tick = 0; tick < 4; ++tick) {
        CY_CHECK_EQ(fixture.live.poll(), cy::abi::LiveReloadOutcome::Idle);
        cy::Expected<cy::abi::LiveReloadStatus, cy::Error> status =
            fixture.live.apply_at_frame_boundary(fixture.world);
        CY_REQUIRE(status.has_value());
        CY_CHECK_EQ(status.value().outcome, cy::abi::LiveReloadOutcome::Idle);
    }
    CY_CHECK_EQ(fixture.runtime.generation(), 0U);
}

CY_TEST_CASE("a watcher refuses a source it cannot use, rather than reporting silence") {
    Fixture fixture;
    CY_CHECK_FALSE(
        fixture.live.watch(cy::abi::LiveReloadSource{"", kStem, kExtension}).has_value());
    CY_CHECK_FALSE(
        fixture.live.watch(cy::abi::LiveReloadSource{CY_ABI_TEST_STAGE_DIR, "", kExtension})
            .has_value());
    // A directory that does not exist is the failure that otherwise looks exactly like a working
    // watcher: every poll reports "nothing changed", forever.
    CY_CHECK_FALSE(
        fixture.live.watch(cy::abi::LiveReloadSource{"/nonexistent/stage", kStem, kExtension})
            .has_value());
    CY_CHECK_FALSE(fixture.live.request("").has_value());
    CY_CHECK_FALSE(fixture.live.pending());
}
