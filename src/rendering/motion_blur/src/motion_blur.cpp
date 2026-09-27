// SPDX-License-Identifier: MIT
#include <cy/rendering/motion_blur/motion_blur.h>

#include <cy/core/math/scalar.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/post/effects.h>

#include <cmath>

namespace cy::rendering::motion_blur {
namespace {

/// The same interleaved gradient noise `contact_shadows` offsets its taps by: a fraction in [0, 1)
/// fixed in screen space, so a still frame blurs identically every frame.
[[nodiscard]] f32 noise(u32 x, u32 y) noexcept {
    const f32 px = static_cast<f32>(x) + 0.5F;
    const f32 py = static_cast<f32>(y) + 0.5F;
    const f32 inner = (px * 0.06711056F) + (py * 0.00583715F);
    const f32 scaled = 52.9829189F * (inner - std::floor(inner));
    return scaled - std::floor(scaled);
}

[[nodiscard]] usize texel(const MotionBlurInputs& inputs, u32 x, u32 y) noexcept {
    return (static_cast<usize>(y) * inputs.width) + x;
}

/// Reversed-Z depth to positive view depth in metres. Zero, the cleared far plane, is the far
/// plane's distance — farther than every surface, which is what the ordering below wants of it.
[[nodiscard]] f32 view_depth(const MotionBlurConstants& constants, f32 depth) noexcept {
    return constants.depth[1] / (depth + constants.depth[0]);
}

[[nodiscard]] f32 row_dot(const f32 row[4], const Vec4& v) noexcept {
    return (row[0] * v.x) + (row[1] * v.y) + (row[2] * v.z) + (row[3] * v.w);
}

}  // namespace

Vec2 camera_motion_pixels(const MotionBlurConstants& constants, u32 x, u32 y, f32 depth) noexcept {
    const f32 u = (static_cast<f32>(x) + 0.5F) * constants.extent[2];
    const f32 v = (static_cast<f32>(y) + 0.5F) * constants.extent[3];
    const Vec4 clip{(u * 2.0F) - 1.0F, 1.0F - (v * 2.0F), depth, 1.0F};
    const Vec4 previous{row_dot(constants.current_to_previous[0], clip),
                        row_dot(constants.current_to_previous[1], clip),
                        row_dot(constants.current_to_previous[2], clip),
                        row_dot(constants.current_to_previous[3], clip)};
    const f32 previous_x = previous.x / previous.w;
    const f32 previous_y = previous.y / previous.w;
    return Vec2{(previous_x - clip.x) * 0.5F * constants.extent[0],
                (clip.y - previous_y) * 0.5F * constants.extent[1]};
}

namespace {

[[nodiscard]] f32 magnitude(Vec2 v) noexcept {
    return std::sqrt((v.x * v.x) + (v.y * v.y));
}

[[nodiscard]] f32 squared(Vec2 v) noexcept {
    return (v.x * v.x) + (v.y * v.y);
}

/// McGuire's cone: how much of a streak of radius `radius` reaches `distance` from its centre.
[[nodiscard]] f32 cone(f32 distance, f32 radius) noexcept {
    return math::saturate(1.0F - (distance / radius));
}

/// McGuire's cylinder: a streak's footprint, with a softened edge so a tap on the boundary does not
/// flicker between in and out.
[[nodiscard]] f32 cylinder(f32 distance, f32 radius) noexcept {
    const f32 edge0 = 0.95F * radius;
    const f32 edge1 = 1.05F * radius;
    const f32 t = math::saturate((distance - edge0) / (edge1 - edge0));
    return 1.0F - (t * t * (3.0F - (2.0F * t)));
}

/// Whether sample depth `b` is in front of `a`, softened over the soft depth: 1 nearer, 0 farther
/// by more than the extent.
[[nodiscard]] f32 soft_depth_compare(f32 a, f32 b, f32 extent) noexcept {
    return math::saturate(1.0F - ((b - a) / extent));
}

/// `cyMotionBlurTileMax` at one tile: its longest blur vector, the first found on a tie, walking
/// rows then columns as the dispatch does.
[[nodiscard]] Vec2 tile_maximum(const MotionBlurInputs& inputs,
                                const MotionBlurConstants& constants, u32 tx, u32 ty) noexcept;

/// `cyMotionBlurNeighbourMax` at one tile: the longest of the 3x3 tile maxima around it.
[[nodiscard]] Vec2 neighbour_maximum(const MotionBlurConstants& constants, Span<const Vec2> tiles,
                                     u32 tx, u32 ty) noexcept {
    const auto across = static_cast<i32>(constants.control[2]);
    const auto down = static_cast<i32>(constants.control[3]);
    Vec2 longest{0.0F, 0.0F};
    for (i32 dy = -1; dy <= 1; ++dy) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const i32 nx = static_cast<i32>(tx) + dx;
            const i32 ny = static_cast<i32>(ty) + dy;
            if (nx < 0 || ny < 0 || nx >= across || ny >= down) {
                continue;
            }
            const Vec2 candidate = tiles[(static_cast<usize>(ny) * static_cast<usize>(across)) +
                                         static_cast<usize>(nx)];
            if (squared(candidate) > squared(longest)) {
                longest = candidate;
            }
        }
    }
    return longest;
}

}  // namespace

f32 shutter_angle_for(f32 shutter_seconds, f32 frame_seconds) noexcept {
    if (!(frame_seconds > 0.0F) || !(shutter_seconds > 0.0F)) {
        return 0.0F;
    }
    return math::clamp(360.0F * shutter_seconds / frame_seconds, 0.0F, 360.0F);
}

MotionBlurSettings settings_for_camera(MotionBlurSettings settings, f32 shutter_seconds,
                                       f32 frame_seconds) noexcept {
    settings.shutter_angle_degrees = shutter_angle_for(shutter_seconds, frame_seconds);
    return settings;
}

u32 tile_count(u32 extent, u32 tile) noexcept {
    return tile == 0 ? 0U : (extent + tile - 1U) / tile;
}

bool blurs(const MotionBlurConstants& constants) noexcept {
    return constants.scales[0] > 0.0F || constants.scales[1] > 0.0F;
}

Expected<MotionBlurConstants, Error> make_motion_blur_constants(
    const MotionBlurSettings& settings, const MotionBlurView& view) noexcept {
    if (view.width == 0 || view.height == 0) {
        return fail(ErrorCode::InvalidArgument, "motion blur: the view has no pixels");
    }
    if (settings.max_radius_pixels == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "motion blur: the longest radius is the tile edge and cannot be zero");
    }
    if (settings.samples == 0 || (settings.samples & 1U) == 0U) {
        return fail(ErrorCode::InvalidArgument,
                    "motion blur: the gather needs an odd sample count, symmetric about the pixel");
    }
    if (settings.camera_scale < 0.0F || settings.object_scale < 0.0F ||
        !(settings.soft_depth_metres > 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "motion blur: the scales cannot be negative and the soft depth must be "
                    "positive");
    }
    Expected<Mat4, Error> inverse_clip = inverse(view.relative_to_clip);
    if (!inverse_clip.has_value()) {
        return fail(ErrorCode::InvalidArgument,
                    "motion blur: the camera-relative to clip transform is not invertible");
    }

    MotionBlurConstants constants;
    const Mat4 current_to_previous = view.previous_relative_to_clip * *inverse_clip;
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            constants.current_to_previous[row][column] = current_to_previous.columns[column][row];
        }
    }
    constants.extent[0] = static_cast<f32>(view.width);
    constants.extent[1] = static_cast<f32>(view.height);
    constants.extent[2] = 1.0F / static_cast<f32>(view.width);
    constants.extent[3] = 1.0F / static_cast<f32>(view.height);
    // THE SHUTTER, THROUGH THE POST MODULE'S OWN ARITHMETIC. `motion_blur_length` is the length a
    // one-pixel motion blurs to at this angle — the open fraction — and halving it is the radius.
    const f32 open = motion_blur_length(1.0F, settings.shutter_angle_degrees);
    constants.scales[0] = open * settings.camera_scale * 0.5F;
    constants.scales[1] = open * settings.object_scale * 0.5F;
    constants.scales[2] = static_cast<f32>(settings.max_radius_pixels);
    constants.scales[3] = settings.soft_depth_metres;
    constants.depth[0] = view.projection.columns[2].z;
    constants.depth[1] = view.projection.columns[3].z;
    constants.depth[2] = settings.camera_scale != settings.object_scale ? 1.0F : 0.0F;
    constants.control[0] = settings.samples;
    constants.control[1] = settings.max_radius_pixels;
    constants.control[2] = tile_count(view.width, settings.max_radius_pixels);
    constants.control[3] = tile_count(view.height, settings.max_radius_pixels);
    return constants;
}

Vec2 blur_vector_at(const MotionBlurInputs& inputs, const MotionBlurConstants& constants, u32 x,
                    u32 y) noexcept {
    const usize at = texel(inputs, x, y);
    const f32 depth = inputs.depth[at];
    // A PIXEL NO SURFACE COVERED IS THE SKY, and the prepass wrote it no motion because it drew
    // nothing there. The sky is at the far plane and moves with the camera alone, so its motion is
    // the far plane's reprojection — which is the camera's rotation, and nothing at all for a
    // camera that only translates under an infinite far plane.
    const Vec2 motion = depth > 0.0F ? Vec2{inputs.velocity[at].x * constants.extent[0],
                                            inputs.velocity[at].y * constants.extent[1]}
                                     : camera_motion_pixels(constants, x, y, 0.0F);
    Vec2 blur{motion.x * constants.scales[0], motion.y * constants.scales[0]};
    // The camera's share is separated only where there is a surface: the sky's motion is all of it
    // the camera's already.
    if (constants.depth[2] > 0.5F && depth > 0.0F) {
        const Vec2 camera = camera_motion_pixels(constants, x, y, depth);
        blur =
            Vec2{(camera.x * constants.scales[0]) + ((motion.x - camera.x) * constants.scales[1]),
                 (camera.y * constants.scales[0]) + ((motion.y - camera.y) * constants.scales[1])};
    }
    const f32 length = magnitude(blur);
    if (length > constants.scales[2]) {
        const f32 shrink = constants.scales[2] / length;
        blur = Vec2{blur.x * shrink, blur.y * shrink};
    }
    return blur;
}

namespace {

Vec2 tile_maximum(const MotionBlurInputs& inputs, const MotionBlurConstants& constants, u32 tx,
                  u32 ty) noexcept {
    const u32 edge = constants.control[1];
    const u32 last_y = math::min((ty + 1U) * edge, inputs.height);
    const u32 last_x = math::min((tx + 1U) * edge, inputs.width);
    Vec2 longest{0.0F, 0.0F};
    for (u32 y = ty * edge; y < last_y; ++y) {
        for (u32 x = tx * edge; x < last_x; ++x) {
            const Vec2 blur = blur_vector_at(inputs, constants, x, y);
            if (squared(blur) > squared(longest)) {
                longest = blur;
            }
        }
    }
    return longest;
}

}  // namespace

Status motion_blur_tiles_reference(const MotionBlurInputs& inputs,
                                   const MotionBlurConstants& constants, Span<Vec2> tiles,
                                   Span<Vec2> neighbours) noexcept {
    const u32 across = constants.control[2];
    const u32 down = constants.control[3];
    const usize count = static_cast<usize>(across) * down;
    if (tiles.size() != count || neighbours.size() != count) {
        return fail(ErrorCode::InvalidArgument, "motion blur: the tile spans are not the grid");
    }
    for (u32 ty = 0; ty < down; ++ty) {
        for (u32 tx = 0; tx < across; ++tx) {
            tiles[(static_cast<usize>(ty) * across) + tx] = tile_maximum(inputs, constants, tx, ty);
        }
    }
    for (u32 ty = 0; ty < down; ++ty) {
        for (u32 tx = 0; tx < across; ++tx) {
            neighbours[(static_cast<usize>(ty) * across) + tx] =
                neighbour_maximum(constants, tiles, tx, ty);
        }
    }
    return ok();
}

Vec4 motion_blur_reference_at(const MotionBlurInputs& inputs, const MotionBlurConstants& constants,
                              Span<const Vec2> neighbours, u32 x, u32 y) noexcept {
    const Vec4 centre = inputs.color[texel(inputs, x, y)];
    const u32 edge = constants.control[1];
    const Vec2 dominant =
        neighbours[(static_cast<usize>(y / edge) * constants.control[2]) + (x / edge)];
    // NOTHING WITHIN REACH MOVES FAR ENOUGH TO BLUR, and the pixel is copied as it is — not
    // reconstructed as a weighted sum that happens to come out equal. That copy is what makes a
    // closed shutter, and every still region of a moving frame, exactly the frame without the
    // stage.
    if (squared(dominant) < 0.25F) {
        return centre;
    }
    const Vec2 own = blur_vector_at(inputs, constants, x, y);
    const f32 own_radius = math::max(magnitude(own), 0.5F);
    const f32 own_depth = view_depth(constants, inputs.depth[texel(inputs, x, y)]);
    const f32 soft = constants.scales[3];

    f32 weight = 1.0F / own_radius;
    Vec3 sum{centre.x * weight, centre.y * weight, centre.z * weight};
    const f32 jitter = noise(x, y) - 0.5F;
    const u32 samples = constants.control[0];
    const u32 middle = (samples - 1U) / 2U;
    for (u32 index = 0; index < samples; ++index) {
        if (index == middle) {
            continue;
        }
        const f32 t = -1.0F + (2.0F * (static_cast<f32>(index) + jitter + 1.0F) /
                               (static_cast<f32>(samples) + 1.0F));
        const Vec2 offset{dominant.x * t, dominant.y * t};
        const f32 fx = std::floor(static_cast<f32>(x) + 0.5F + offset.x);
        const f32 fy = std::floor(static_cast<f32>(y) + 0.5F + offset.y);
        const u32 sx = static_cast<u32>(math::clamp(fx, 0.0F, static_cast<f32>(inputs.width - 1U)));
        const u32 sy =
            static_cast<u32>(math::clamp(fy, 0.0F, static_cast<f32>(inputs.height - 1U)));
        const f32 distance = magnitude(offset);
        const f32 sample_depth = view_depth(constants, inputs.depth[texel(inputs, sx, sy)]);
        const f32 sample_radius =
            math::max(magnitude(blur_vector_at(inputs, constants, sx, sy)), 0.5F);
        // Nearer than the pixel: it covers the pixel as far as ITS streak reaches. Farther: only as
        // far as the PIXEL's own streak reveals it. Both about as near: two blurred surfaces over
        // one another, each seen where both streaks cover the tap.
        const f32 front = soft_depth_compare(own_depth, sample_depth, soft);
        const f32 back = soft_depth_compare(sample_depth, own_depth, soft);
        const f32 contribution =
            (front * cone(distance, sample_radius)) + (back * cone(distance, own_radius)) +
            (cylinder(distance, sample_radius) * cylinder(distance, own_radius) * 2.0F);
        const Vec4 colour = inputs.color[texel(inputs, sx, sy)];
        weight += contribution;
        sum = Vec3{sum.x + (colour.x * contribution), sum.y + (colour.y * contribution),
                   sum.z + (colour.z * contribution)};
    }
    return Vec4{sum.x / weight, sum.y / weight, sum.z / weight, centre.w};
}

Status motion_blur_reference(const MotionBlurInputs& inputs, const MotionBlurConstants& constants,
                             Span<Vec4> out) noexcept {
    const usize pixels = static_cast<usize>(inputs.width) * inputs.height;
    if (pixels == 0 || inputs.color.size() != pixels || inputs.velocity.size() != pixels ||
        inputs.depth.size() != pixels || out.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "motion blur: every input and the output must be the whole image");
    }
    if (constants.control[1] == 0 || constants.control[0] == 0) {
        return fail(ErrorCode::InvalidArgument, "motion blur: the constants were never made");
    }
    const usize tiles = static_cast<usize>(constants.control[2]) * constants.control[3];
    Array<Vec2> maxima;
    Array<Vec2> neighbours;
    if (Status sized = maxima.resize(tiles); !sized) {
        return sized;
    }
    if (Status sized = neighbours.resize(tiles); !sized) {
        return sized;
    }
    if (Status made =
            motion_blur_tiles_reference(inputs, constants, maxima.span(), neighbours.span());
        !made) {
        return made;
    }
    for (u32 y = 0; y < inputs.height; ++y) {
        for (u32 x = 0; x < inputs.width; ++x) {
            out[texel(inputs, x, y)] =
                motion_blur_reference_at(inputs, constants, neighbours.span(), x, y);
        }
    }
    return ok();
}

}  // namespace cy::rendering::motion_blur
