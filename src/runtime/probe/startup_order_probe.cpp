// The startup-order probe. Tasks 3.4.4 and 4.5.
//
// It starts a runtime, shuts it down, and prints the two journals:
//
//   startup  platform core modules-core display servers ... boot
//   shutdown boot ... display modules-core core platform
//
// A test runs it a hundred times and compares the output. The comparison is across *processes*, not
// across iterations of one loop, because that is what makes the claim worth making: a fresh address
// space each run, so an order that depended on an allocation address, a hash seed or a static
// initialiser would differ between runs rather than being reproduced identically by all of them.
//
// WITH `--modules` it also populates a cy::config::ModuleRegistry and prints two further lines:
//
//   modules-started  <name>...
//   modules-stopped  <name>...
//
// which is task 4.5's claim — the order the four module stages bring the project graph's modules up
// and take them down in. The extra lines are behind a flag so that the default output is byte for
// byte what tests/smoke/test_startup_order.cpp already compares; one probe, two claims, and neither
// test has to know about the other.
//
// The synthetic modules are added in an order that is neither their level order nor alphabetical,
// so a registry that replayed insertion order would print something different from what it prints.
// The project graph's own modules are added first, from the generated table, which is what ties the
// printed order to the manifest rather than to this file.
//
// Headless, with no window and no trace: this measures the sequence, not what the stages do.

#include <cy/core/config/module_registry.h>
#include <cy/platform/headless_display_server.h>
#include <cy/platform/sdl3_platform.h>
#include <cy/runtime/runtime.h>

#include <cstdio>
#include <cstring>
#include <span>

namespace {

void print_journal(const char* label, std::span<const cy::StartupStage> stages) {
    std::fputs(label, stdout);
    for (const cy::StartupStage stage : stages) {
        std::fprintf(stdout, " %s", cy::startup_stage_name(stage));
    }
    std::fputc('\n', stdout);
}

void print_names(const char* label, std::span<const char* const> names) {
    std::fputs(label, stdout);
    for (const char* name : names) {
        std::fprintf(stdout, " %s", name);
    }
    std::fputc('\n', stdout);
}

cy::Status count_registration(void* user) {
    ++*static_cast<int*>(user);
    return cy::ok();
}

void count_unregistration(void* user) {
    --*static_cast<int*>(user);
}

// Five synthetic modules over the four levels, two of them at Core so that ordering *within* a
// level is exercised as well as ordering between them. Deliberately not in level order and not in
// alphabetical order.
struct Synthetic {
    const char* name;
    cy::config::ModuleLevel level;
};

constexpr Synthetic kSynthetic[] = {
    {"probe-november-scene", cy::config::ModuleLevel::Scene},
    {"probe-zulu-core", cy::config::ModuleLevel::Core},
    {"probe-sierra-editor", cy::config::ModuleLevel::Editor},
    {"probe-mike-servers", cy::config::ModuleLevel::Servers},
    {"probe-alpha-core", cy::config::ModuleLevel::Core},
};

int populate(cy::config::ModuleRegistry& registry, int& live) {
    if (const cy::Status added = registry.add_project_modules(); !added) {
        std::fprintf(stderr, "modules: %s\n", added.error().message);
        return 1;
    }
    for (const cy::config::ModuleRegistration& registration : registry.modules()) {
        if (const cy::Status bound =
                registry.bind(registration.name, &count_registration, &count_unregistration, &live);
            !bound) {
            std::fprintf(stderr, "modules: %s\n", bound.error().message);
            return 1;
        }
    }
    for (const Synthetic& synthetic : kSynthetic) {
        cy::config::ModuleRegistration registration;
        registration.name = synthetic.name;
        registration.level = synthetic.level;
        registration.on_register = &count_registration;
        registration.on_unregister = &count_unregistration;
        registration.user = &live;
        if (const cy::Status added = registry.add(registration); !added) {
            std::fprintf(stderr, "modules: %s\n", added.error().message);
            return 1;
        }
    }
    return 0;
}

}  // namespace

int main(int argument_count, char** arguments) {
    bool with_modules = false;
    for (int index = 1; index < argument_count; ++index) {
        if (std::strcmp(arguments[index], "--modules") == 0) {
            with_modules = true;
        }
    }

    cy::Sdl3Platform platform;
    if (const cy::Status started = platform.initialise(argument_count, arguments); !started) {
        std::fprintf(stderr, "platform: %s\n", started.error().message);
        return 1;
    }

    cy::HeadlessDisplayServer display;
    if (const cy::Status started = display.initialise(); !started) {
        std::fprintf(stderr, "display: %s\n", started.error().message);
        return 1;
    }

    cy::config::ModuleRegistry registry;
    int live = 0;
    if (with_modules && populate(registry, live) != 0) {
        return 1;
    }

    cy::RuntimeConfig config;
    config.platform = &platform;
    config.display = &display;
    config.create_window = false;
    config.install_crash_handler = false;
    config.modules = with_modules ? &registry : nullptr;

    cy::Runtime runtime;
    if (const cy::Status started = runtime.startup(config); !started) {
        std::fprintf(stderr, "runtime: %s\n", started.error().message);
        return 1;
    }
    runtime.shutdown();

    print_journal("startup ", runtime.startup_order());
    print_journal("shutdown", runtime.shutdown_order());
    if (with_modules) {
        print_names("modules-started ", registry.start_journal());
        print_names("modules-stopped ", registry.stop_journal());
        // The two claims a reader would otherwise have to take on trust, as one word each: every
        // module that registered has unregistered, and the second journal is the first reversed.
        std::fprintf(stdout, "modules-balanced %s\n", live == 0 ? "yes" : "no");
        std::fprintf(stdout, "modules-reversed %s\n",
                     registry.journal_is_reversed() ? "yes" : "no");
    }

    display.shutdown();
    platform.shutdown();
    return 0;
}
