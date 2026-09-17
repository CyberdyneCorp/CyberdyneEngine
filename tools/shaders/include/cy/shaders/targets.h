#pragma once
// The shader set, compiled for every target this build emits, and the agreement between them.
// M11.c task 1.7 — what `just build-shaders` runs.
//
// WHAT THIS IS FOR, AND WHAT WOULD MAKE IT WORTHLESS. `shader-system`'s pipeline step 4 is "SPIR-V
// retained (Vulkan), or translated (MSL for Metal, DXIL for D3D12)", and M11.d cannot start until
// the second and third exist. THE CLAIM IS NOT THAT A FILE APPEARS. A recipe that wrote three files
// and checked they were not empty would pass on a stub that copied the SPIR-V twice, and a
// criterion that cannot go red for the defect it names is not a criterion.
//
// THE CLAIM IS THAT THE SAME GRAPH, COMPILED TWICE, PRODUCES TWO ARTEFACTS THAT DECLARE THE SAME
// INTERFACE: the same entry point name, the same stage, the same workgroup size, and the same
// parameters in the same order with the same kinds. Where each target PUT those parameters is not
// compared and must not be — Metal gives buffers, textures and samplers three separate index spaces
// and D3D12 gives them four register classes, so requiring the indices to match would be requiring
// a falsehood. `cy::shader::interfaces_agree` draws that line and this walks the shader set across
// it.
//
// THE SET IS DISCOVERED AND NOT LISTED. Every `.slang` file under the roots is read; an entry point
// is a `[shader("stage")]` attribute, which is how Slang itself finds one. A hand-written list of
// shaders beside a directory of shaders is a list that goes stale on the day somebody adds a file,
// and this whole exercise is about a claim that must not be able to go stale quietly. A file that
// declares no entry point by attribute is reported as skipped, by name, with the reason — it is
// still compiled into the set as an importable module.

#include <cy/backends/shader/compiler.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>

#include <cstdio>
#include <string_view>

namespace cy::shadertool {

/// What a run looked at and what it found. Every number is printed, because a summary that reported
/// only failures could not be told from a run that found nothing to do.
struct Report {
    /// `.slang` files read into the module set.
    usize modules = 0;
    /// Entry points discovered by `[shader(...)]` attribute.
    usize entry_points = 0;
    /// Files with no entry-point attribute. Importable, not compiled on their own.
    usize modules_without_entry_points = 0;
    /// Artefacts produced: one per entry point per emitted target.
    usize artefacts = 0;
    /// Pairs of artefacts of one entry point compared against each other.
    usize comparisons = 0;
    /// Comparisons where the two declared different interfaces. A run with one of these fails.
    usize disagreements = 0;
    /// Entry points a target refused to compile. A run with one of these fails.
    usize failures = 0;

    [[nodiscard]] bool ok() const noexcept { return disagreements == 0 && failures == 0; }
};

struct Options {
    /// Directories under the working directory to walk, in order. `just build-shaders` passes the
    /// engine's and the samples' shader trees.
    Span<const std::string_view> roots;
    /// The targets to compile for. Empty means every target the front end reports it emits, which
    /// is what an honest default is: a machine without DXC compiles two rather than failing at
    /// three, and says which.
    Span<const shader::Target> targets;
    /// Where artefacts are written. Empty writes none — the agreement is measured in memory, and a
    /// run that only wants the measurement should not have to produce a directory of files.
    std::string_view out_dir;
    /// Print one line per artefact rather than one per entry point.
    bool verbose = false;
};

/// Print the target table: every target, whether this build emits it, and when it does not, why.
///
/// PROBED RATHER THAN DECLARED — it compiles a shader for each target and reports what happened.
/// A table built from CMake options would report a target the machine cannot produce.
[[nodiscard]] Status print_targets(Allocator& allocator, std::FILE* out) noexcept;

/// Compile the shader set and measure the agreement between targets.
[[nodiscard]] Expected<Report, Error> build_shader_set(Allocator& allocator, const Options& options,
                                                       std::FILE* out) noexcept;

}  // namespace cy::shadertool
