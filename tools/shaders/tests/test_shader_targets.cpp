// SPDX-License-Identifier: MIT
// THE ENGINE'S OWN SHADERS, COMPILED FOR EVERY TARGET THIS BUILD EMITS. M11.d, which closed
// `m11c:every-shader-reaches-every-target`.
//
// `just build-shaders --strict src samples` asks this of the whole tree and is a ledger criterion;
// this suite asks it of `src/rendering/` on every `just test-smoke`, so the question is answered by
// the build that breaks it rather than by the next ledger run. It exists because of the two defects
// that run found:
//
//   * FOUR VERTEX STAGES THAT ONE TARGET REFUSED. `fullscreenVertex`, `cyParticleVertex`,
//     `cyStripVertex` and `vgVisHwVertex` took `SV_VulkanVertexID` (and the last one
//     `SV_VulkanInstanceID`), which DXC rejects for every vertex shader model. They take the
//     portable `SV_VertexID` and `SV_InstanceID` now; see `cy/fullscreen.slang` for what that costs
//     on each target and why every draw feeding them starts at zero.
//   * ONE MODULE THAT NO TARGET COMPILED. `cy/particle.slang` named `cyFrame`, which
//     `cy/frame.slang` turned into a macro when its view block became a parameter block — and a
//     macro does not cross an `import`. The checked-in `particle_spirv.h` hid it: it was compiled
//     before the change, so the renderer kept drawing while its source compiled for nothing.
//
// SMOKE, for `shader_slang`'s reason: this loads the Slang compiler and DXC and compiles about
// thirty-five entry points three times each, which costs seconds rather than milliseconds.
//
// THE ROOTS ARE RELATIVE TO THE SOURCE TREE, because that is how the tool reads them and how the
// recipe runs it; the case moves there first. HOW MANY TARGETS is the build's configuration and not
// the tool's probe: a build configured with `CY_SHADER_DXIL` must produce three artefacts an entry
// point, so one whose DXC did not load fails here rather than comparing two targets and passing.

#include <cy/core/memory/system_allocator.h>
#include <cy/shaders/targets.h>
#include <cy/test/test.h>

#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>

using namespace cy;

CY_TEST_CASE("every entry point under src/rendering compiles for every target this build emits") {
    std::error_code moved;
    std::filesystem::current_path(CY_SOURCE_DIR, moved);
    CY_REQUIRE_FALSE(moved);

    const std::string_view roots[] = {"src/rendering"};
    shadertool::Options options;
    options.roots = Span<const std::string_view>(roots, 1);

    auto report =
        shadertool::build_shader_set(system_allocator(MemoryDomain::Assets), options, stdout);
    CY_REQUIRE(report.has_value());
    // A floor, so a run that found nothing — a moved directory, an emptied glob — cannot pass.
    CY_CHECK_GE(report->entry_points, 30U);
    CY_CHECK_GT(report->comparisons, 0U);
    // Every entry point produced an artefact for every target this build is configured to emit. A
    // build whose DXC did not load would otherwise compare two targets and pass.
    CY_CHECK_EQ(report->artefacts, report->entry_points * CY_SHADER_TARGETS_EXPECTED);
    CY_CHECK_EQ(report->failures, 0U);
    CY_CHECK_EQ(report->disagreements, 0U);
    CY_CHECK_EQ(report->target_refusals, 0U);
}
