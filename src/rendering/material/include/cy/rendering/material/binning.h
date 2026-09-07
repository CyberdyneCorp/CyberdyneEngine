#pragma once
// Material classification and binning. M7 task 6.2.
//
// `material-compiler` — "Material classification and binning": "Under deferred material
// evaluation, visible pixels SHALL be classified by material program and evaluated in bins, so that
// the number of shading dispatches scales with the number of distinct programs rather than with the
// number of objects or material instances. Classification SHALL be computed on the GPU, and bin
// dispatches SHALL be indirect. Pixels whose program is identical but whose instances differ SHALL
// share a bin, since instance data is indexed rather than bound."
//
// ================================================================================================
// THIS IS THE CPU REFERENCE, AND IT SAYS SO
// ================================================================================================
//
// What is here is the algorithm and the buffers a compute shader produces: a count per program, a
// prefix sum, a compacted pixel list, and the indirect dispatch arguments. It is the same shape
// `rendering-culling-and-lod` gave `cpu_reference_cull` at M6 — and it exists for the same reason,
// which M7's own task 5.1 states: the dispatch is "checked against `cpu_reference_cull` by
// comparing buffers". A GPU classification pass that has nothing to be compared against is a pass
// whose correctness is a screenshot.
//
// NO DEVICE IS TOUCHED HERE. The dispatch itself belongs to the frame, which is not this module's;
// what this module owes it is the definition and a reference to check it against.
//
// THE POINT OF THE STRUCTURE IS WHAT IT DOES NOT CONTAIN: a material INSTANCE. A bin is keyed by
// program alone, because instance data is reached by indexing the GPU material table with the
// instance's identifier — so ten thousand instances of one program are one bin, and a view with a
// thousand instances drawn from twelve programs dispatches twelve times.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::material {

/// One bin: a contiguous run of the compacted pixel list, and the indirect arguments that shade it.
struct MaterialBin {
    /// The material program's index in the GPU material table's program list.
    u32 program = 0;
    /// Where this bin's pixels begin in `MaterialBinning::pixels()`.
    u32 first = 0;
    u32 count = 0;
    /// `ceil(count / group_size)`. Zero for a program that is not visible — which is the whole
    /// point of an indirect dispatch: the bin is still there and costs nothing.
    u32 groups = 0;
};

/// Classification over one view's pixels.
class MaterialBinning {
public:
    explicit MaterialBinning(Allocator& allocator) noexcept;

    MaterialBinning(const MaterialBinning&) = delete;
    MaterialBinning& operator=(const MaterialBinning&) = delete;

    /// Classify. `program_per_pixel` holds one program index per visible pixel, and
    /// `kNoProgram` for a pixel no material covers. Stable: a pixel's position within its bin is
    /// its position in the input, so two runs over one frame produce identical buffers.
    [[nodiscard]] Status classify(Span<const u32> program_per_pixel, u32 program_count,
                                  u32 group_size) noexcept;

    [[nodiscard]] Span<const MaterialBin> bins() const noexcept { return bins_.span(); }
    [[nodiscard]] Span<const u32> pixels() const noexcept { return pixels_.span(); }
    /// Bins with work. What "material evaluation SHALL dispatch twelve bins" is measured against.
    [[nodiscard]] u32 dispatch_count() const noexcept;
    [[nodiscard]] u32 classified_pixels() const noexcept { return classified_; }

private:
    Array<MaterialBin> bins_;
    Array<u32> pixels_;
    u32 classified_ = 0;
};

/// A pixel no material covers.
inline constexpr u32 kNoProgram = 0xFFFFFFFFU;

}  // namespace cy::rendering::material
