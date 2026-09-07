// samples/06-open-world — M6's closing artefact. Section 9.
//
// A world larger than memory, traversed continuously at speed, saved, quit, reloaded, and resumed —
// and then a content change cooked, packaged and shipped as a patch. `openworld.py` is the driver
// that puts those five acts in order and `just run-open-world` is the recipe; this program is the
// game, and it is the half that has to be real.
//
// WHAT IT READS IS AN INSTALLATION, NOT A SOURCE TREE. `--install <dir>` is a directory `cy_build`
// installed a package into: content-addressed chunks and the manifest in force. The world's
// description is read out of it by its LOGICAL NAME, so the program cannot tell whether the bytes
// it got arrived in the original build or in a patch — which is the only way to demonstrate that a
// patch ships content rather than that a file was overwritten.
//
//   --act traverse    stream the route, page the surfaces, record what the player changed, save
//   --act resume      load the save, reactivate exactly the cells it names, and check them
//   --act report      parse the content and print what the world would be; no world is built
//
// EVERY LINE IT PRINTS IS A FUNCTION OF THE CONTENT AND THE ROUTE, except the four timing figures,
// which are named as measurements where they appear. tests/smoke runs this through the driver and
// the driver reads the printed form, so a figure that varied with an allocation address would turn
// the milestone gate into a flake — the rule samples/02-headless-sim states and every sample since
// has kept.

#include <cy/build/patch.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>

#include "content.h"
#include "run.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

using namespace cy;
using namespace cy::sample::openworld;

constexpr const char* kTag = "06-open-world";

struct Options {
    const char* install = "";
    const char* chunk = "world/city.cells";
    const char* save = "";
    const char* act = "traverse";
    RouteOptions route;
    bool help = false;
};

void print_usage() {
    std::fputs(
        "samples/06-open-world — a multi-kilometre world streamed, paged, saved and patched.\n"
        "\n"
        "  --act <traverse|resume|report>  what to do            (default traverse)\n"
        "  --install <dir>                 the installation to play (required)\n"
        "  --chunk <name>                  the world's logical name (default world/city.cells)\n"
        "  --save <dir>                    the save archive's directory\n"
        "  --ticks <n>                     route ticks at 60 Hz  (default 6000)\n"
        "  --radius <m>                    the traveller's streaming radius (default 320)\n"
        "  --workers <n>                   page production workers (default 2)\n"
        "  --abort-at <tick>               tear the session down mid-flight at this tick\n"
        "  --hitch-ms <ms>                 the per-tick ceiling  (default 8)\n"
        "  --help                          this text\n",
        stderr);
}

[[nodiscard]] bool parse_options(int argument_count, char** arguments, Options& options) {
    for (int index = 1; index < argument_count; ++index) {
        const std::string_view argument{arguments[index]};
        const bool has_value = index + 1 < argument_count;

        if (argument == "--help") {
            print_usage();
            options.help = true;
            return true;
        }
        if (argument == "--act" && has_value) {
            options.act = arguments[++index];
        } else if (argument == "--install" && has_value) {
            options.install = arguments[++index];
        } else if (argument == "--chunk" && has_value) {
            options.chunk = arguments[++index];
        } else if (argument == "--save" && has_value) {
            options.save = arguments[++index];
        } else if (argument == "--ticks" && has_value) {
            options.route.ticks = static_cast<u32>(std::strtoul(arguments[++index], nullptr, 10));
        } else if (argument == "--radius" && has_value) {
            options.route.radius = std::strtof(arguments[++index], nullptr);
        } else if (argument == "--workers" && has_value) {
            options.route.production_workers =
                static_cast<u32>(std::strtoul(arguments[++index], nullptr, 10));
        } else if (argument == "--abort-at" && has_value) {
            options.route.abort_at =
                static_cast<u32>(std::strtoul(arguments[++index], nullptr, 10));
        } else if (argument == "--hitch-ms" && has_value) {
            options.route.hitch_threshold_ms = std::strtod(arguments[++index], nullptr);
        } else {
            std::fprintf(stderr, "%s: unrecognised argument '%s'\n\n", kTag, arguments[index]);
            print_usage();
            return false;
        }
    }
    if (options.route.ticks == 0) {
        std::fprintf(stderr, "%s: --ticks must be at least 1\n", kTag);
        return false;
    }
    return true;
}

[[nodiscard]] Allocator& world_allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// Read the world's description out of the installation, BY LOGICAL NAME.
///
/// `build::Installation::read` resolves the name through the manifest in force and fetches the
/// chunk by its digest, so this program is reading what the installed build says the world is.
/// After a patch the same name resolves to a different digest and this function returns different
/// bytes, with nothing here to change.
[[nodiscard]] Status read_world(const Options& options, Array<u8>& out) {
    build::Installation installation;
    if (Status opened = installation.open(options.install); !opened) {
        return opened;
    }
    const Expected<std::string, Error> build = installation.current_build();
    if (!build) {
        return make_unexpected(build.error());
    }
    std::printf("  installation  %s  build %s\n", options.install, build->c_str());
    return installation.read(options.chunk, out);
}

[[nodiscard]] int fail(const char* what, const Error& error) {
    std::fprintf(stderr, "%s: %s: %s\n", kTag, what, error.message);
    return 1;
}

/// The traversal act: stream the route, page the surfaces, and save.
[[nodiscard]] int act_traverse(const Options& options, const WorldContent& content) {
    Session session(world_allocator(), content);
    if (Status started = session.start(options.route); !started) {
        return fail("the world could not be started", started.error());
    }
    std::printf("  traversing\n");
    Telemetry telemetry;
    if (Status traversed = session.traverse(options.route, telemetry); !traversed) {
        return fail("the route could not be traversed", traversed.error());
    }
    print_telemetry(content, telemetry);

    if (options.route.abort_at != 0) {
        // TORN DOWN IN MID-FLIGHT, WHICH IS THE POINT OF THIS SWITCH. Cells are preparing, pages
        // are in production on two worker threads, and the session is destroyed underneath both.
        std::printf("\n  torn down at tick %u with cells preparing and pages in production\n",
                    options.route.abort_at);
        return 0;
    }

    if (options.save[0] != '\0') {
        const Expected<u32, Error> generation = session.checkpoint(options.save, telemetry);
        if (!generation) {
            return fail("the save could not be committed", generation.error());
        }
        std::printf("\n  saved         generation %u into %s\n", *generation, options.save);
    }

    // FAILURES, RATHER THAN FIGURES NOBODY READS. Each of these is one of M6's exit criteria and
    // the run is the evidence, so a run that violated one must not exit zero.
    int problems = 0;
    if (telemetry.ticks_over_budget != 0) {
        std::fprintf(stderr, "%s: %u ticks exceeded the modelled activation budget\n", kTag,
                     telemetry.ticks_over_budget);
        ++problems;
    }
    if (telemetry.hitches != 0) {
        std::fprintf(stderr, "%s: %u ticks cost more than %.1f ms of CPU time\n", kTag,
                     telemetry.hitches, options.route.hitch_threshold_ms);
        ++problems;
    }
    if (telemetry.missing_samples != 0) {
        std::fprintf(stderr, "%s: %llu samples resolved to nothing; the mip tail is not resident\n",
                     kTag, static_cast<unsigned long long>(telemetry.missing_samples));
        ++problems;
    }
    if (telemetry.cells_evicted == 0 || telemetry.cells_activated == 0) {
        std::fprintf(stderr, "%s: nothing streamed; the world is not larger than the budget\n",
                     kTag);
        ++problems;
    }
    return problems == 0 ? 0 : 1;
}

/// The resume act: load the save, reactivate the cells it names, and check them.
[[nodiscard]] int act_resume(const Options& options, const WorldContent& content) {
    if (options.save[0] == '\0') {
        std::fprintf(stderr, "%s: --act resume needs --save <dir>\n", kTag);
        return 2;
    }
    Session session(world_allocator(), content);
    if (Status started = session.start(options.route); !started) {
        return fail("the world could not be started", started.error());
    }
    ResumeReport report;
    if (Status resumed = session.resume(options.save, report); !resumed) {
        return fail("the save could not be resumed", resumed.error());
    }
    print_resume(report);
    if (report.mismatches != 0 || report.regions == 0) {
        std::fprintf(stderr, "%s: the world did not resume in the state that was saved\n", kTag);
        return 1;
    }
    return 0;
}

[[nodiscard]] int act_report(const WorldContent& content) {
    Telemetry empty;
    print_telemetry(content, empty);
    return 0;
}

}  // namespace

int main(int argument_count, char** arguments) {
    Options options;
    if (!parse_options(argument_count, arguments, options)) {
        return 2;
    }
    if (options.help) {
        return 0;
    }
    if (options.install[0] == '\0') {
        std::fprintf(stderr, "%s: --install <dir> is required\n\n", kTag);
        print_usage();
        return 2;
    }

    std::printf("%s\n", kTag);
    Array<u8> bytes(world_allocator());
    if (Status read = read_world(options, bytes); !read) {
        return fail("the world's description could not be read from the installation",
                    read.error());
    }
    WorldContent content(world_allocator());
    if (Status parsed = content.parse(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        !parsed) {
        return fail("the world's description could not be parsed", parsed.error());
    }

    const std::string_view act{options.act};
    if (act == "traverse") {
        return act_traverse(options, content);
    }
    if (act == "resume") {
        return act_resume(options, content);
    }
    if (act == "report") {
        return act_report(content);
    }
    std::fprintf(stderr, "%s: '%s' is not an act\n\n", kTag, options.act);
    print_usage();
    return 2;
}
