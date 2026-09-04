#include <cy/rendering/culling/lod.h>

#include <cy/core/base/expected.h>
#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering {
namespace {

/// The threshold level `level` must be reached at, given the level the instance was at last frame.
///
/// THE BAND IS ASYMMETRIC AND IT IS AROUND THE LEVEL ALREADY HELD, which is the only arrangement
/// that actually stops oscillation:
///
///   * staying where it is       — the threshold is RELAXED, so coverage has to fall meaningfully
///                                 below it before the instance coarsens;
///   * refining to a finer level — the threshold is TIGHTENED, so coverage has to rise meaningfully
///                                 above it before the instance refines;
///   * coarsening past that      — the plain threshold, because the instance has already left the
///                                 band around the level it held.
///
/// A band applied to the coarser level instead would let an instance at the boundary alternate: it
/// would leave level 0 at 0.5 and re-enter it at 0.5, which is the defect the band exists to remove
/// and which the suite pins.
[[nodiscard]] f32 effective_threshold(f32 threshold, u32 level, u32 previous,
                                      f32 hysteresis) noexcept {
    if (previous == kInvalidLod) {
        return threshold;
    }
    if (level == previous) {
        return threshold * (1.0F - hysteresis);
    }
    if (level < previous) {
        return threshold * (1.0F + hysteresis);
    }
    return threshold;
}

}  // namespace

f32 screen_coverage(f32 radius, f32 view_depth, f32 fov_y_radians) noexcept {
    if (radius <= 0.0F) {
        return 0.0F;
    }
    if (view_depth <= 0.0F) {
        return 1.0F;
    }
    const f32 half_extent = std::tan(fov_y_radians * 0.5F);
    if (half_extent <= 0.0F) {
        return 1.0F;
    }
    return radius / (view_depth * half_extent);
}

f32 screen_coverage_orthographic(f32 radius, f32 ortho_height) noexcept {
    if (radius <= 0.0F || ortho_height <= 0.0F) {
        return 0.0F;
    }
    return (2.0F * radius) / ortho_height;
}

LodSelection select_lod(Span<const render::MeshLod> chain, f32 coverage, f32 instance_bias,
                        const LodSettings& settings, u32 previous) noexcept {
    LodSelection selection;
    if (chain.empty()) {
        return selection;
    }

    // The biases scale coverage rather than shifting it: a threshold is a ratio, and adding a
    // constant to a ratio means the same bias behaves differently at 0.5 and at 0.01. Positive bias
    // therefore multiplies coverage up, which keeps more detail — which is what the field says.
    const f32 bias = settings.global_bias + settings.view_bias + instance_bias;
    const f32 biased = coverage * std::exp2(bias);
    const f32 hysteresis = math::clamp(settings.hysteresis, 0.0F, 0.9F);

    const auto level_count = static_cast<u32>(chain.size());
    u32 chosen = level_count - 1;
    for (u32 level = 0; level < level_count; ++level) {
        const f32 threshold = effective_threshold(chain[level].screen_coverage_threshold, level,
                                                  previous, hysteresis);
        if (biased >= threshold) {
            chosen = level;
            break;
        }
    }
    selection.level = chosen;

    // The cross-fade band sits just above the NEXT level's threshold: an instance inside it is on
    // its way to being coarsened, so both levels are drawn and the dither resolves the blend.
    if (settings.cross_fade_band > 0.0F && chosen + 1 < level_count) {
        const f32 next_threshold = chain[chosen].screen_coverage_threshold;
        const f32 band = next_threshold * settings.cross_fade_band;
        if (band > 0.0F && biased < next_threshold + band) {
            selection.fade_to = chosen + 1;
            selection.fade = math::clamp((next_threshold + band - biased) / band, 0.0F, 1.0F);
        }
    }
    return selection;
}

f32 visibility_range_alpha(const VisibilityRange& range, f32 distance) noexcept {
    // `end` of zero means unbounded, so a zeroed range is "always visible" and needs no special
    // case at any call site.
    const f32 far_bound = range.end > 0.0F ? range.end : math::kInfinity;
    if (distance < range.begin || distance > far_bound) {
        if (range.fade_margin <= 0.0F) {
            return 0.0F;
        }
        // Outside the range but inside the margin: fading rather than gone.
        if (distance < range.begin && distance >= range.begin - range.fade_margin) {
            return math::clamp((distance - (range.begin - range.fade_margin)) / range.fade_margin,
                               0.0F, 1.0F);
        }
        if (far_bound != math::kInfinity && distance > far_bound &&
            distance <= far_bound + range.fade_margin) {
            return math::clamp(((far_bound + range.fade_margin) - distance) / range.fade_margin,
                               0.0F, 1.0F);
        }
        return 0.0F;
    }
    return 1.0F;
}

Status resolve_hlod(Span<const VisibilityRange> ranges, Span<const f32> distances,
                    Span<HlodResolution> out) noexcept {
    if (ranges.size() != distances.size() || ranges.size() != out.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "resolve_hlod: ranges, distances and out must be parallel");
    }
    const auto count = static_cast<u32>(ranges.size());

    // Pass one: each instance's own alpha, ignoring the hierarchy.
    for (u32 index = 0; index < count; ++index) {
        const f32 alpha = visibility_range_alpha(ranges[index], distances[index]);
        out[index].alpha = alpha;
        out[index].visible = alpha > 0.0F;
    }

    // Pass two: a visible ancestor replaces its descendants. Walking to the root per instance is
    // what makes nesting resolve to exactly one visible level per branch; the step counter is the
    // cycle guard, because a cycle in authored parent links would otherwise hang the frame.
    for (u32 index = 0; index < count; ++index) {
        u32 parent = ranges[index].parent;
        u32 steps = 0;
        while (parent != kInvalidVisibilityParent) {
            if (parent >= count || steps++ > count) {
                return fail(ErrorCode::InvalidArgument,
                            "resolve_hlod: visibility parent links contain a cycle or a bad index");
            }
            if (out[parent].visible) {
                // The parent has taken over. Under `Dependents` both are drawn while the parent is
                // still fading in, which is the HLOD swap; otherwise the child is simply replaced.
                if (ranges[parent].mode == FadeMode::Dependents && out[parent].alpha < 1.0F) {
                    out[index].alpha = 1.0F - out[parent].alpha;
                    out[index].visible = out[index].alpha > 0.0F;
                } else {
                    out[index].alpha = 0.0F;
                    out[index].visible = false;
                }
                break;
            }
            parent = ranges[parent].parent;
        }
    }
    return ok();
}

}  // namespace cy::rendering
