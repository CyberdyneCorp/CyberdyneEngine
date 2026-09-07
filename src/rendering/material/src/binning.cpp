// Material classification and binning, as the CPU reference. M7 task 6.2. See binning.h.

#include <cy/rendering/material/binning.h>

namespace cy::rendering::material {

MaterialBinning::MaterialBinning(Allocator& allocator) noexcept
    : bins_(allocator), pixels_(allocator) {}

Status MaterialBinning::classify(Span<const u32> program_per_pixel, u32 program_count,
                                 u32 group_size) noexcept {
    if (group_size == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a dispatch group cannot be empty", 0});
    }
    bins_.clear();
    pixels_.clear();
    classified_ = 0;

    // Pass one: count. This is the histogram a compute shader builds with atomics into groupshared
    // memory; the reference does it serially and the two must agree bin for bin.
    if (Status sized = bins_.resize(program_count); !sized) {
        return sized;
    }
    for (u32 program = 0; program < program_count; ++program) {
        bins_[program] = MaterialBin{program, 0, 0, 0};
    }
    for (const u32 program : program_per_pixel) {
        if (program == kNoProgram) {
            continue;
        }
        if (program >= program_count) {
            return make_unexpected(Error{ErrorCode::OutOfRange,
                                         "a pixel names a material program that is not in the "
                                         "table",
                                         0});
        }
        ++bins_[program].count;
        ++classified_;
    }

    // Pass two: the prefix sum that turns counts into offsets.
    u32 running = 0;
    for (u32 program = 0; program < program_count; ++program) {
        bins_[program].first = running;
        running += bins_[program].count;
        bins_[program].groups = (bins_[program].count + group_size - 1U) / group_size;
    }

    // Pass three: the scatter. `cursor` is the per-bin write position the shader keeps as an
    // atomic; walking the input in order is what makes the compacted list stable.
    if (Status sized = pixels_.resize(classified_); !sized) {
        return sized;
    }
    Array<u32> cursor(pixels_.allocator());
    if (Status sized = cursor.resize(program_count); !sized) {
        return sized;
    }
    for (u32 program = 0; program < program_count; ++program) {
        cursor[program] = bins_[program].first;
    }
    for (usize index = 0; index < program_per_pixel.size(); ++index) {
        const u32 program = program_per_pixel[index];
        if (program == kNoProgram) {
            continue;
        }
        pixels_[cursor[program]++] = static_cast<u32>(index);
    }
    return ok();
}

u32 MaterialBinning::dispatch_count() const noexcept {
    u32 total = 0;
    for (const MaterialBin& bin : bins_) {
        total += bin.groups > 0 ? 1U : 0U;
    }
    return total;
}

}  // namespace cy::rendering::material
