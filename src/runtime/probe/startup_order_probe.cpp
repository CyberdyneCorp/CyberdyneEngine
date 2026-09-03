// The startup-order probe. Task 3.4.4.
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
// Headless, with no window and no trace: this measures the sequence, not what the stages do.

#include <cy/platform/headless_display_server.h>
#include <cy/platform/sdl3_platform.h>
#include <cy/runtime/runtime.h>

#include <cstdio>
#include <span>

namespace {

void print_journal(const char* label, std::span<const cy::StartupStage> stages) {
    std::fputs(label, stdout);
    for (const cy::StartupStage stage : stages) {
        std::fprintf(stdout, " %s", cy::startup_stage_name(stage));
    }
    std::fputc('\n', stdout);
}

}  // namespace

int main(int argument_count, char** arguments) {
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

    cy::RuntimeConfig config;
    config.platform = &platform;
    config.display = &display;
    config.create_window = false;
    config.install_crash_handler = false;

    cy::Runtime runtime;
    if (const cy::Status started = runtime.startup(config); !started) {
        std::fprintf(stderr, "runtime: %s\n", started.error().message);
        return 1;
    }
    runtime.shutdown();

    print_journal("startup ", runtime.startup_order());
    print_journal("shutdown", runtime.shutdown_order());

    display.shutdown();
    platform.shutdown();
    return 0;
}
