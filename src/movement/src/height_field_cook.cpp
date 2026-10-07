// SPDX-License-Identifier: MIT
// The cook of a height field: the module's second float boundary (convert_mesh.cpp is the first).
// It runs at cook or load, before tick 0, and converts each height once with the IEEE-exact
// `detmath::from_f32_cooked` (design §7.1).

#include <cy/core/detmath/convert.h>
#include <cy/movement/height_field.h>

namespace cy::movement {

Status FixedHeightField::cook(Span<const f32> heights, u32 width, u32 depth, FixedVec2 origin,
                              int cell_shift) noexcept {
    if (heights.size() != usize{width} * depth) {
        return fail(ErrorCode::InvalidArgument,
                    "movement: a height field's heights do not match its width and depth");
    }
    Array<Fixed16> cooked(samples_.allocator());
    if (Status reserved = cooked.reserve(heights.size()); !reserved) {
        return reserved;
    }
    for (const f32 height : heights) {
        // Written as the range a sample holds, so a NaN — which compares false with everything —
        // is refused with it.
        const bool holds = height > -32768.0F && height < 32767.0F;
        if (!holds) {
            return fail(ErrorCode::OutOfRange,
                        "movement: a terrain height lies outside the ±32 768 m a cooked sample "
                        "holds");
        }
        if (Status pushed = cooked.push_back(Fixed16::narrow(detmath::from_f32_cooked(height)));
            !pushed) {
            return pushed;
        }
    }
    return adopt(cooked.span(), width, depth, origin, cell_shift);
}

}  // namespace cy::movement
