// `cy_sample_ship` — M11.d's closing artefact. Section 8.
//
// ================================================================================================
// WHAT IT IS
// ================================================================================================
//
// ONE PROJECT, BUILT, COOKED, PACKAGED, INSTALLED AND LAUNCHED FROM A SINGLE RECIPE — `just
// run-ship`. This program is the last of those five: it reads what the installation says the card
// is, BY LOGICAL NAME through the manifest in force, composes it, opens a window through
// `cy::DisplayServer`, and PRESENTS it. `samples/` held fifteen entries before this one and not one
// of them was a packaged project.
//
// ================================================================================================
// WHAT IT IS NOT, AND THIS IS LOAD-BEARING
// ================================================================================================
//
// IT IS NOT A GAME. `m11b:the-game-exists` is a red criterion — `ls -d samples/*game*` matches
// nothing — and a packaging proof does not close it and must not be read as closing it. The card
// says so on the screen, README.md says so at length, and this comment says so to the next person
// who greps for "ship" looking for the missing game. It is not here.
//
// ================================================================================================
// THE ACTS
// ================================================================================================
//
//   1. OPEN      the installation, and VERIFY it. "Playable" for a test is not that a program
//                started: it is that every byte the manifest promised is present and digests
//                correctly, which is what `Installation::verify` answers.
//   2. PROVENANCE read out of the installed manifest and reported — the build identity, the
//                project, the revision, the platform, the profile, the toolchain fingerprint and
//                the content version. Task 8.4: the launch reproduces FROM the package.
//   3. READ      the card by logical name. Nothing is read off a path in the source tree; change
//                `project/card/ship.cycard`, re-run the recipe, and the window says something else
//                without a recompile.
//   4. DRAW      through a display server chosen at run time, on a swapchain, presented.
//   5. REPORT    what ran, what did not, and WHICH DEVICE ANSWERED — on the card's own face and on
//                standard output. A leg that could not run reports NOT EVALUATED, which is never a
//                pass, and asking for a leg that could not run is a non-zero exit.

#include <cy/build/artefact_store.h>
#include <cy/build/patch.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/platform/platform.h>
#include <cy/platform/sdl3_platform.h>
#ifdef CY_SHIP_HAS_NATIVE_PLATFORM
#    include <cy/platform/linux_platform.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "card.h"
#include "golden.h"
#include "present.h"

namespace {

using namespace cy;
using namespace cy::sample::ship;

constexpr const char* kTag = "11-ship";

struct Options {
    std::string install;
    std::string chunk = "card/ship.card";
    std::string shot;
    std::string coverage;
    PlatformChoice platform = PlatformChoice::Auto;
    u32 frames = 120;
    bool require_draw = false;
    bool validation = true;
};

void print_usage() {
    std::fputs(
        "samples/11-ship — the packaged project, launched from its installation.\n"
        "\n"
        "  --install <dir>      the installation `cy_build install` produced (required)\n"
        "  --chunk <name>       the card's logical name (default card/ship.card)\n"
        "  --platform <p>       sdl3 | native | headless | auto (default auto)\n"
        "  --frames <n>         how many frames to present (default 120)\n"
        "  --shot <path.png>    write the PRESENTED frame, read back off the device\n"
        "  --coverage <path>    write the coverage table this run measured\n"
        "  --require-draw       exit non-zero if nothing was presented\n"
        "  --no-validation      do not turn the backend's validation layers on\n"
        "\n"
        "Exit: 0 drew clean, 1 asked to draw and did not, 2 bad arguments,\n"
        "      3 drew and tripped backend validation (a gap, not a fallen-over run)\n",
        stderr);
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const bool has_value = index + 1 < argc;
        if (argument == "--install" && has_value) {
            options.install = argv[++index];
        } else if (argument == "--chunk" && has_value) {
            options.chunk = argv[++index];
        } else if (argument == "--shot" && has_value) {
            options.shot = argv[++index];
        } else if (argument == "--coverage" && has_value) {
            options.coverage = argv[++index];
        } else if (argument == "--frames" && has_value) {
            options.frames = static_cast<u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (argument == "--require-draw") {
            options.require_draw = true;
        } else if (argument == "--no-validation") {
            options.validation = false;
        } else if (argument == "--platform" && has_value) {
            const std::string_view value{argv[++index]};
            if (value == "sdl3") {
                options.platform = PlatformChoice::Sdl3;
            } else if (value == "native") {
                options.platform = PlatformChoice::Native;
            } else if (value == "headless") {
                options.platform = PlatformChoice::Headless;
            } else if (value == "auto") {
                options.platform = PlatformChoice::Auto;
            } else {
                std::fprintf(stderr, "%s: '%.*s' is not a display server this binary knows\n", kTag,
                             static_cast<int>(value.size()), value.data());
                return false;
            }
        } else {
            std::fprintf(stderr, "%s: unrecognised argument '%s'\n\n", kTag, argv[index]);
            print_usage();
            return false;
        }
    }
    return true;
}

/// THE COVERAGE THIS ARTEFACT HAS, STATED BY THE ARTEFACT. Task 8.3.
///
/// M10's artefact put its 122 ms on its own face rather than in a document beside it, and this is
/// the same act for a different fact: this package was built on ONE operating system with ONE GPU
/// vendor and no Metal and no D3D12, and a reader who is shown a window with a frame in it will
/// otherwise assume more than that.
///
/// THE VERDICTS ARE ABOUT THE ARTEFACT, NOT ABOUT THIS RUN, and BUILT is deliberately not RAN. The
/// card is composed before a single frame exists, so a row that said RAN would be a claim made
/// before its evidence. What this run actually did is the two lines `present.cpp` writes once it
/// knows the device, and the frame count in the table below.
[[nodiscard]] std::vector<CoverageLine> static_coverage() {
    std::vector<CoverageLine> lines;
    lines.push_back({"LINUX / SDL3", "BUILT", "THIS HOST"});
#ifdef CY_SHIP_HAS_NATIVE_PLATFORM
    lines.push_back({"LINUX / NATIVE X11", "BUILT", "PLATFORM/LINUX-NATIVE, NO SDL BENEATH IT"});
#else
    lines.push_back({"LINUX / NATIVE X11", "ABSENT", "NOT BUILT: NO PLATFORM/LINUX-NATIVE TARGET"});
#endif
    lines.push_back({"MACOS / METAL", "NOT EVALUATED", "NO APPLE TOOLCHAIN ON THIS HOST"});
    lines.push_back(
        {"WINDOWS / D3D12", "NOT EVALUATED", "LINUX HOST - MOVED TO A RUNG OF ITS OWN"});
    lines.push_back({"GPU VENDORS", "1", "ONE VENDOR, ONE DRIVER, ONE OS"});
    return lines;
}

void print_coverage(std::FILE* out, const std::vector<CoverageLine>& lines,
                    const PresentReport& report, const std::string& build_id) {
    std::fprintf(out, "coverage of samples/11-ship\n");
    std::fprintf(out, "  build              %s\n", build_id.c_str());
    for (const CoverageLine& line : lines) {
        std::fprintf(out, "  %-24s %-14s %s\n", line.label.c_str(), line.verdict.c_str(),
                     line.detail.c_str());
    }
    std::fprintf(out, "  display server     %s\n",
                 report.display_server.empty() ? "(none)" : report.display_server.c_str());
    std::fprintf(out, "  rhi backend        %s\n",
                 report.backend.empty() ? "(none)" : report.backend.c_str());
    std::fprintf(out, "  device             %s\n",
                 report.device_name.empty() ? "(none)" : report.device_name.c_str());
    std::fprintf(out, "  device class       %s\n",
                 report.device_class.empty() ? "(none)" : report.device_class.c_str());
    std::fprintf(out, "  swapchain          %s %ux%u\n",
                 report.swapchain_format.empty() ? "(none)" : report.swapchain_format.c_str(),
                 report.swapchain_width, report.swapchain_height);
    std::fprintf(out, "  frames presented   %u\n", report.frames_presented);
    std::fprintf(out,
                 "  frame plan         %u submit(s), %u pass(es), %u derived barrier(s), "
                 "plan 0x%016llx\n",
                 report.submits, report.passes_recorded, report.barriers,
                 static_cast<unsigned long long>(report.plan_hash));
    std::fprintf(out, "  validation errors  %u\n", report.validation_errors);
    if (!report.not_evaluated.empty()) {
        std::fprintf(out, "  NOT EVALUATED      %s\n", report.not_evaluated.c_str());
    }
}

[[nodiscard]] int fail(const char* what, const Error& error) {
    std::fprintf(stderr, "%s: %s: %s (%s)\n", kTag, what, error.message,
                 error_code_name(error.code));
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }
    if (options.install.empty()) {
        std::fprintf(stderr, "%s: --install <dir> is required\n\n", kTag);
        print_usage();
        return 2;
    }

    // --- Act 1: the installation, verified ------------------------------------------------------
    build::Installation installation;
    if (const Status opened = installation.open(options.install); !opened) {
        return fail("the installation could not be opened", opened.error());
    }
    const Expected<std::string, Error> build_id = installation.current_build();
    if (!build_id) {
        return fail("the installation has no build in force", build_id.error());
    }
    if (const Status verified = installation.verify(); !verified) {
        return fail("the installation does not verify", verified.error());
    }
    std::printf("%s: installation  %s\n", kTag, options.install.c_str());
    std::printf("%s: build         %s  (verified: every chunk present and digesting)\n", kTag,
                build_id->c_str());

    // --- Act 2: provenance, out of the package rather than out of this process ------------------
    const Expected<build::PackageSet, Error> package = installation.current_package();
    if (!package) {
        return fail("the installed manifest could not be read", package.error());
    }
    const build::Provenance& provenance = package->provenance;
    std::printf("%s: provenance    project=%s revision=%s platform=%s profile=%s\n", kTag,
                provenance.project.c_str(), provenance.revision.c_str(),
                provenance.platform.c_str(), provenance.profile.c_str());
    std::printf("%s:               toolchain=%s content-version=%u\n", kTag,
                provenance.toolchain.c_str(), provenance.content_version);

    // --- Act 3: the card, by logical name -------------------------------------------------------
    Array<u8> bytes(system_allocator(MemoryDomain::Assets));
    if (const Status read = installation.read(options.chunk, bytes); !read) {
        return fail("the card could not be read out of the installation", read.error());
    }
    Card card;
    if (const Status parsed =
            card.parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        !parsed) {
        return fail("the cooked card did not parse", parsed.error());
    }
    std::printf("%s: card          %s %ux%u, %zu directives, %llu bytes out of the package\n", kTag,
                card.id().c_str(), card.width(), card.height(), card.directive_count(),
                static_cast<unsigned long long>(bytes.size()));

    Expected<Image, Error> image = card.compose();
    if (!image) {
        return fail("the card did not compose", image.error());
    }
    const std::vector<CoverageLine> coverage = static_coverage();
    card.draw_footer(*image, coverage);

    // --- Act 4: the window ----------------------------------------------------------------------
    // THE PLATFORM, CHOSEN THE SAME WAY THE DISPLAY SERVER IS. `cy::Platform` is the process's own
    // abstraction — arguments, environment, standard streams, the three user directories, exit —
    // and `platform/linux-native/` implements it as well as the display server. A run on the native
    // leg therefore touches no SDL at all, which is what makes the leg worth having.
    Sdl3Platform sdl3_platform;
#ifdef CY_SHIP_HAS_NATIVE_PLATFORM
    LinuxPlatform native_platform;
    const bool use_native = options.platform == PlatformChoice::Native;
    Platform& platform = use_native ? static_cast<Platform&>(native_platform) : sdl3_platform;
    const Status started =
        use_native ? native_platform.initialise(argc, argv) : sdl3_platform.initialise(argc, argv);
#else
    Platform& platform = sdl3_platform;
    const Status started = sdl3_platform.initialise(argc, argv);
#endif
    if (!started) {
        return fail("the platform did not initialise", started.error());
    }
    std::printf("%s: platform      %.*s\n", kTag, static_cast<int>(platform.name().size()),
                platform.name().data());
    // The display servers this binary was BUILT with, printed before one is chosen. A leg that is
    // absent is absent at link time, and saying so here is cheaper for a reader than discovering it
    // from a refusal further down.
    std::printf("%s: display legs ", kTag);
    for (const PlatformChoice choice : available_platforms()) {
        std::printf(" %s", platform_choice_name(choice));
    }
    std::printf("\n");

    PresentOptions present_options;
    present_options.platform = options.platform;
    present_options.frames = options.frames;
    present_options.capture = !options.shot.empty();
    present_options.validation = options.validation;
    present_options.device_line_colour = card.colour_or("dim", Rgba{140, 150, 170, 255});

    const PresentReport report = present_card(platform, *image, present_options);

    // --- Act 5: the report ----------------------------------------------------------------------
    print_coverage(stdout, coverage, report, *build_id);
    if (!options.coverage.empty()) {
        if (std::FILE* out = std::fopen(options.coverage.c_str(), "w"); out != nullptr) {
            print_coverage(out, coverage, report, *build_id);
            (void)std::fclose(out);
            std::printf("%s: coverage      %s\n", kTag, options.coverage.c_str());
        }
    }

    if (!options.shot.empty()) {
        // THE SHOT IS THE PRESENTED FRAME WHERE THERE WAS ONE. Falling back to the composed image
        // is stated rather than silent: a picture of what was going to be drawn is a different
        // claim from a picture of what was drawn, and the two must not be confusable.
        const Image& source = report.photograph.pixels.empty() ? *image : report.photograph;
        render_test::Image out(system_allocator(MemoryDomain::Renderer));
        out.width = source.width;
        out.height = source.height;
        if (const Status sized =
                out.texels.resize(static_cast<usize>(source.width) * source.height);
            !sized) {
            return fail("the screenshot could not be sized", sized.error());
        }
        // `render_test::Image` packs a texel into a u32 with RED IN THE LOW BYTE, which is what
        // `Rgba8Unorm` means; `Image` here is the byte order a PNG row wants. The repack is four
        // shifts rather than a memcpy so that the two conventions never have to agree by accident.
        for (usize index = 0; index < out.texels.size(); ++index) {
            const u8* texel = source.pixels.data() + (index * 4U);
            out.texels[index] = static_cast<u32>(texel[0]) | (static_cast<u32>(texel[1]) << 8U) |
                                (static_cast<u32>(texel[2]) << 16U) |
                                (static_cast<u32>(texel[3]) << 24U);
        }
        if (const Status written = render_test::write_png(options.shot.c_str(), out); !written) {
            return fail("the screenshot could not be written", written.error());
        }
        std::printf("%s: shot          %s (%s)\n", kTag, options.shot.c_str(),
                    report.photograph.pixels.empty()
                        ? "COMPOSED, NOT PRESENTED — nothing was drawn on a device"
                        : "read back off the device after presentation");
    }

    if (options.require_draw && report.frames_presented == 0) {
        std::fprintf(
            stderr,
            "%s: --require-draw was given and nothing was presented: %s\n"
            "%s: NOT EVALUATED is never a pass.\n",
            kTag,
            report.not_evaluated.empty() ? "no reason was recorded" : report.not_evaluated.c_str(),
            kTag);
        return 1;
    }
    if (report.validation_errors != 0) {
        // EXIT 3, NOT 1, AND THE DIFFERENCE IS THE FINDING.
        //
        // `rhi-and-render-graph` is explicit that "a frame that renders but trips validation is not
        // a frame that works", so this cannot be a zero. But it is not a failure of THIS program
        // either: the frame drew, presented and photographed correctly, and what synchronisation
        // validation objects to is how the RENDER GRAPH spells the two barriers at the swapchain
        // boundary. See README.md, "The two hazards this artefact found". `ship.py` reads a 3 as a
        // GAP — named, counted and non-zero — rather than as a run that fell over.
        std::fprintf(stderr,
                     "%s: the frame tripped %u validation error(s). It drew and presented; see "
                     "samples/11-ship/README.md for what they are and whose they are.\n",
                     kTag, report.validation_errors);
        return 3;
    }
    return 0;
}
