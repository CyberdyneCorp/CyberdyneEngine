// The module registration levels, and the order they come up and go down in. Task 4.5.
//
// `engine-architecture`, "Deterministic startup and shutdown": subsystems initialise in a fixed
// order and tear down in exact reverse. The four module stages are this file's subject; the eleven
// stages around them are src/runtime/'s.
//
// Nothing here asserts on assertion behaviour, so every case holds in all four profiles. That is
// deliberate: CY_ASSERT is compiled out of Profile and Shipping, and an ordering claim that only
// held where assertions are live would be a claim about the developer's build.

#include <cy/core/config/module_registry.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using cy::config::ModuleLevel;
using cy::config::ModuleRegistration;
using cy::config::ModuleRegistry;

// The recorder every case registers against: modules append their own name, so the vector *is* the
// order in which the hooks actually ran, independently of the registry's own journal. Two
// independent records of the same order, and every case compares them.
struct Recorder {
    std::vector<std::string> registered;
    std::vector<std::string> unregistered;
    const char* fail_on = nullptr;
};

struct Hook {
    Recorder* recorder;
    const char* name;
};

cy::Status on_register(void* user) {
    Hook& hook = *static_cast<Hook*>(user);
    if (hook.recorder->fail_on != nullptr && std::strcmp(hook.recorder->fail_on, hook.name) == 0) {
        return cy::fail(cy::ErrorCode::Unavailable, "the fixture module refuses to register");
    }
    hook.recorder->registered.emplace_back(hook.name);
    return cy::ok();
}

void on_unregister(void* user) {
    Hook& hook = *static_cast<Hook*>(user);
    hook.recorder->unregistered.emplace_back(hook.name);
}

ModuleRegistration entry(const char* name, ModuleLevel level, Hook& hook) {
    ModuleRegistration registration;
    registration.name = name;
    registration.level = level;
    registration.on_register = &on_register;
    registration.on_unregister = &on_unregister;
    registration.user = &hook;
    return registration;
}

std::vector<std::string> names(std::span<const char* const> journal) {
    std::vector<std::string> result;
    result.reserve(journal.size());
    for (const char* name : journal) {
        result.emplace_back(name);
    }
    return result;
}

// One fixture used by most cases: four modules spread over the four levels, plus a second module at
// Core so that ordering *within* a level is exercised and not only ordering between them.
struct Fixture {
    Recorder recorder;
    Hook hooks[5] = {
        {&recorder, "zulu-core"},      {&recorder, "alpha-core"},    {&recorder, "mike-servers"},
        {&recorder, "november-scene"}, {&recorder, "sierra-editor"},
    };
    ModuleRegistry registry;

    Fixture() {
        // Added in an order that is neither the level order nor alphabetical, so a registry that
        // simply replayed insertion order would produce a different answer from the expected one.
        CY_REQUIRE(registry.add(entry("november-scene", ModuleLevel::Scene, hooks[3])));
        CY_REQUIRE(registry.add(entry("zulu-core", ModuleLevel::Core, hooks[0])));
        CY_REQUIRE(registry.add(entry("sierra-editor", ModuleLevel::Editor, hooks[4])));
        CY_REQUIRE(registry.add(entry("mike-servers", ModuleLevel::Servers, hooks[2])));
        CY_REQUIRE(registry.add(entry("alpha-core", ModuleLevel::Core, hooks[1])));
    }

    cy::Status start_all() {
        for (const ModuleLevel level :
             {ModuleLevel::Core, ModuleLevel::Servers, ModuleLevel::Scene, ModuleLevel::Editor}) {
            const cy::Status started = registry.start(level);
            if (!started) {
                return started;
            }
        }
        return cy::ok();
    }

    void stop_all() {
        for (const ModuleLevel level :
             {ModuleLevel::Editor, ModuleLevel::Scene, ModuleLevel::Servers, ModuleLevel::Core}) {
            registry.stop(level);
        }
    }
};

const std::vector<std::string> kExpectedOrder = {
    "alpha-core", "zulu-core", "mike-servers", "november-scene", "sierra-editor",
};

}  // namespace

CY_TEST_CASE("registration order is level, then name, whatever order modules were added in") {
    Fixture fixture;

    std::vector<std::string> ordered;
    for (const ModuleRegistration& registration : fixture.registry.modules()) {
        ordered.emplace_back(registration.name);
    }
    CY_CHECK(ordered == kExpectedOrder);
}

CY_TEST_CASE("the order is recorded, and shutdown is its exact reverse") {
    Fixture fixture;
    CY_REQUIRE(fixture.start_all());

    CY_CHECK(fixture.recorder.registered == kExpectedOrder);
    CY_CHECK(names(fixture.registry.start_journal()) == kExpectedOrder);

    fixture.stop_all();

    std::vector<std::string> reversed(kExpectedOrder.rbegin(), kExpectedOrder.rend());
    CY_CHECK(fixture.recorder.unregistered == reversed);
    CY_CHECK(names(fixture.registry.stop_journal()) == reversed);
    CY_CHECK(fixture.registry.journal_is_reversed());
    CY_CHECK_FALSE(fixture.registry.journal_overflowed());
}

CY_TEST_CASE("the same modules added in a different order start in the same order") {
    // The whole claim, stated as an experiment: build the registry twice from two different
    // insertion orders and compare the journals. A registry that leaked insertion order into the
    // result — or a pointer value, or a hash — fails here.
    Recorder first_recorder;
    Recorder second_recorder;
    Hook first[3] = {{&first_recorder, "aa"}, {&first_recorder, "bb"}, {&first_recorder, "cc"}};
    Hook second[3] = {{&second_recorder, "aa"}, {&second_recorder, "bb"}, {&second_recorder, "cc"}};

    ModuleRegistry forward;
    CY_REQUIRE(forward.add(entry("aa", ModuleLevel::Core, first[0])));
    CY_REQUIRE(forward.add(entry("bb", ModuleLevel::Core, first[1])));
    CY_REQUIRE(forward.add(entry("cc", ModuleLevel::Core, first[2])));

    ModuleRegistry backward;
    CY_REQUIRE(backward.add(entry("cc", ModuleLevel::Core, second[2])));
    CY_REQUIRE(backward.add(entry("bb", ModuleLevel::Core, second[1])));
    CY_REQUIRE(backward.add(entry("aa", ModuleLevel::Core, second[0])));

    CY_REQUIRE(forward.start(ModuleLevel::Core));
    CY_REQUIRE(backward.start(ModuleLevel::Core));
    CY_CHECK(first_recorder.registered == second_recorder.registered);
    CY_CHECK(names(forward.start_journal()) == names(backward.start_journal()));
}

CY_TEST_CASE("a level cannot start before the levels below it, or stop before those above") {
    Fixture fixture;

    const cy::Status early = fixture.registry.start(ModuleLevel::Scene);
    CY_CHECK_FALSE(early);
    CY_CHECK_EQ(early.error().code, cy::ErrorCode::Unavailable);
    CY_CHECK(fixture.recorder.registered.empty());

    CY_REQUIRE(fixture.start_all());

    // Core is not free to go while Scene and Editor are still running.
    fixture.registry.stop(ModuleLevel::Core);
    CY_CHECK(fixture.registry.level_started(ModuleLevel::Core));
    CY_CHECK(fixture.recorder.unregistered.empty());

    fixture.stop_all();
    CY_CHECK_FALSE(fixture.registry.level_started(ModuleLevel::Core));
}

CY_TEST_CASE("a module that fails to register leaves its level exactly as it found it") {
    // `engine-architecture`, "Failure during startup unwinds cleanly", at the level of one stage:
    // the modules this call already registered are unregistered in reverse and nothing stays half
    // built. The runtime's own unwind, over the eleven stages, is src/runtime/'s test.
    Fixture fixture;
    fixture.recorder.fail_on = "zulu-core";  // the second of the two Core modules

    const cy::Status started = fixture.registry.start(ModuleLevel::Core);
    CY_CHECK_FALSE(started);
    CY_CHECK_EQ(started.error().code, cy::ErrorCode::Unavailable);
    CY_CHECK_FALSE(fixture.registry.level_started(ModuleLevel::Core));

    CY_CHECK(fixture.recorder.registered == std::vector<std::string>{"alpha-core"});
    CY_CHECK(fixture.recorder.unregistered == std::vector<std::string>{"alpha-core"});
    CY_CHECK(fixture.registry.journal_is_reversed());
}

CY_TEST_CASE("a module is refused twice, unnamed, or added while a level is running") {
    Recorder recorder;
    Hook hook{&recorder, "only"};
    ModuleRegistry registry;

    CY_REQUIRE(registry.add(entry("only", ModuleLevel::Core, hook)));

    const cy::Status duplicate = registry.add(entry("only", ModuleLevel::Servers, hook));
    CY_CHECK_FALSE(duplicate);
    CY_CHECK_EQ(duplicate.error().code, cy::ErrorCode::AlreadyExists);

    ModuleRegistration unnamed;
    unnamed.level = ModuleLevel::Core;
    CY_CHECK_EQ(registry.add(unnamed).error().code, cy::ErrorCode::InvalidArgument);

    CY_REQUIRE(registry.start(ModuleLevel::Core));
    Hook late{&recorder, "late"};
    const cy::Status running = registry.add(entry("late", ModuleLevel::Servers, late));
    CY_CHECK_FALSE(running);
    CY_CHECK_EQ(running.error().code, cy::ErrorCode::Unavailable);
    registry.stop(ModuleLevel::Core);
}

CY_TEST_CASE("a descriptor without hooks is still ordered and still journaled") {
    // The project graph declares modules before any code is bound to them: add_project_modules()
    // adds descriptors, and bind() attaches behaviour afterwards. A module with no hooks is a
    // module the runtime still initialises in its place — "registration SHALL be separable from
    // start" is the same idea one phase earlier.
    ModuleRegistry registry;
    ModuleRegistration descriptor;
    descriptor.name = "graph-only";
    descriptor.level = ModuleLevel::Servers;
    CY_REQUIRE(registry.add(descriptor));

    Recorder recorder;
    Hook hook{&recorder, "graph-only"};
    CY_REQUIRE(registry.bind("graph-only", &on_register, &on_unregister, &hook));
    CY_CHECK_FALSE(registry.bind("not-in-the-graph", &on_register, &on_unregister, &hook));

    CY_REQUIRE(registry.start(ModuleLevel::Core));
    CY_REQUIRE(registry.start(ModuleLevel::Servers));
    CY_CHECK(recorder.registered == std::vector<std::string>{"graph-only"});
    CY_CHECK(names(registry.start_journal()) == std::vector<std::string>{"graph-only"});
}

CY_TEST_CASE("level names round-trip through their manifest spelling") {
    for (const ModuleLevel level :
         {ModuleLevel::Core, ModuleLevel::Servers, ModuleLevel::Scene, ModuleLevel::Editor}) {
        const auto parsed =
            cy::config::module_level_from_name(cy::config::module_level_name(level));
        CY_REQUIRE(parsed);
        CY_CHECK(parsed.value() == level);
    }
    CY_CHECK_FALSE(cy::config::module_level_from_name("core"));  // the manifest spells it "Core"
    CY_CHECK_FALSE(cy::config::module_level_from_name(nullptr));
}
