// SPDX-License-Identifier: MIT
// smoke.rts_api_fog — samples/13-rts-api drawn with blue's fog of war. ABI 1.10.
//
// The host runs as a separate process with its scripted player, drawing every frame on a device
// (`--render`), once with the fog (`--fog`) and once without it, the control. The Swift game gives
// every vision (game/Fog.swift); the host keeps the grid (host/fog.h); the renderer draws it
// (`fog_of_war::FogOfWarPass` at the frame's `FogOfWar` stage) and places the enemy camp by blue's
// sight. What it asserts:
//
//   the camp by blue's sight     at frame 150 the near raider is drawn, the tower — seen at the
//                                start, left behind at frame 90 — is drawn as its ghost, and the
//                                far raider is not drawn; once the ordered worker reaches its point
//                                the far raider is drawn too
//   the fog is in the frame      the fogged stills differ from the plain ones in a fifth of their
//                                pixels or more, and every one of those is darker
//   the cost                     the stage's device time under half a millisecond at 480x270, and
//                                the host's half — the grid read, the field faded, the camp placed
//                                — under a millisecond
//
// It leaves the stills (`rts-fog-*.png`, `rts-gi-*.png`) in the test's binary directory.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstdlib>
#include <string>

#include "golden.h"
#include "process.h"

namespace {

[[nodiscard]] std::string line_with(const std::string& output, const char* marker) {
    const std::string::size_type found = output.find(marker);
    if (found == std::string::npos) {
        return {};
    }
    const std::string::size_type end = output.find('\n', found);
    return output.substr(found, end == std::string::npos ? end : end - found);
}

[[nodiscard]] double number_after(const std::string& line, const char* key) {
    const std::string::size_type found = line.find(key);
    if (found == std::string::npos) {
        return -1e30;
    }
    return std::strtod(line.c_str() + found + std::char_traits<char>::length(key), nullptr);
}

/// The `still<n>=drawn/ghosts/hidden` triple after `key`, as text.
[[nodiscard]] std::string word_after(const std::string& line, const char* key) {
    const std::string::size_type found = line.find(key);
    if (found == std::string::npos) {
        return {};
    }
    const std::string::size_type start = found + std::char_traits<char>::length(key);
    const std::string::size_type end = line.find(' ', start);
    return line.substr(start, end == std::string::npos ? end : end - start);
}

struct Drawn {
    cy::test::smoke::ProcessResult process;
    std::string render;
    std::string draw;
    bool skipped = false;
};

[[nodiscard]] Drawn draw(bool fog) {
    Drawn run;
    run.process = cy::test::smoke::run(cy::test::smoke::quoted(CY_SAMPLE_RTS_API) + " --render " +
                                       cy::test::smoke::quoted(CY_TEST_BINARY_DIR) +
                                       (fog ? " --fog" : "") + " 2>&1");
    run.skipped = run.process.output.find("no requested graphics device on this machine") !=
                  std::string::npos;
    run.render = line_with(run.process.output, "rts render ");
    run.draw = line_with(run.process.output, "rts fog draw ");
    return run;
}

struct Darkening {
    cy::usize differing = 0;
    cy::usize brighter = 0;
};

/// How many pixels of `fogged` differ from `plain`, and how many of those are brighter.
[[nodiscard]] Darkening compare(const char* plain, const char* fogged) {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);
    cy::render_test::Image left(allocator);
    cy::render_test::Image right(allocator);
    const std::string directory = CY_TEST_BINARY_DIR;
    CY_REQUIRE(cy::render_test::read_png((directory + plain).c_str(), left).has_value());
    CY_REQUIRE(cy::render_test::read_png((directory + fogged).c_str(), right).has_value());
    CY_REQUIRE_EQ(left.texels.size(), right.texels.size());
    Darkening out;
    for (cy::usize index = 0; index < left.texels.size(); ++index) {
        const cy::u32 a = left.texels[index];
        const cy::u32 b = right.texels[index];
        if (a == b) {
            continue;
        }
        out.differing += 1;
        const auto sum = [](cy::u32 texel) {
            return (texel & 0xFFU) + ((texel >> 8U) & 0xFFU) + ((texel >> 16U) & 0xFFU);
        };
        // A pixel the fog brightened by more than rounding: the fog only takes light away.
        out.brighter += sum(b) > sum(a) + 3U ? 1U : 0U;
    }
    return out;
}

}  // namespace

CY_TEST_CASE(
    "samples/13-rts-api: blue's fog of war in the frame, the camp by its sight, the cost") {
    const Drawn fogged = draw(true);
    if (fogged.skipped) {
        std::fprintf(stderr, "no requested graphics device on this machine; skipped\n");
        return;
    }
    std::fprintf(stderr, "%s\n%s\n", fogged.render.c_str(), fogged.draw.c_str());
    CY_REQUIRE_EQ(fogged.process.exit_code, 0);
    CY_REQUIRE(!fogged.draw.empty());
    const Drawn plain = draw(false);
    CY_REQUIRE_EQ(plain.process.exit_code, 0);
    CY_REQUIRE(plain.draw.empty());
    CY_CHECK_EQ(number_after(fogged.render, "stills="), 4.0);
    CY_CHECK_EQ(number_after(fogged.render, "validation_errors="), 0.0);

    // THE CAMP BY BLUE'S SIGHT: drawn / ghost / hidden at frames 150 and 420.
    CY_CHECK_EQ(word_after(fogged.draw, "still0="), std::string("1/1/1"));
    CY_CHECK_EQ(word_after(fogged.draw, "still3="), std::string("2/1/0"));

    // THE FOG IS IN THE FRAME, and only ever darkens it.
    for (const char* frame : {"150", "420"}) {
        const std::string gi = std::string("/rts-gi-") + frame + ".png";
        const std::string fog = std::string("/rts-fog-") + frame + ".png";
        const Darkening seen = compare(gi.c_str(), fog.c_str());
        std::fprintf(stderr, "frame %s: %zu pixels differ, %zu brighter\n", frame, seen.differing,
                     seen.brighter);
        CY_CHECK_GT(seen.differing, static_cast<cy::usize>(480U * 270U / 5U));
        CY_CHECK_EQ(seen.brighter, static_cast<cy::usize>(0U));
    }

    // THE COST at 480x270: the stage on the device, and the host's half.
    CY_CHECK_GT(number_after(fogged.draw, "fog_timed="), 0.0);
    CY_CHECK_LT(number_after(fogged.draw, "fog_device_ms="), 0.5);
    CY_CHECK_GT(number_after(fogged.draw, "fog_device_ms="), 0.0);
    CY_CHECK_LT(number_after(fogged.draw, "fog_host_ms="), 1.0);
}
