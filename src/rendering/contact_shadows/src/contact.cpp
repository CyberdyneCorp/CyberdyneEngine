// SPDX-License-Identifier: MIT
#include <cy/rendering/contact_shadows/contact.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering::contact_shadows {
namespace {

/// contact_shadows.slang's `cyContactNoise`: interleaved gradient noise at the pixel centre, the
/// fraction of a step every tap is offset by. Fixed in screen space.
[[nodiscard]] f32 noise(u32 x, u32 y) noexcept {
    const f32 px = static_cast<f32>(x) + 0.5F;
    const f32 py = static_cast<f32>(y) + 0.5F;
    const f32 inner = (px * 0.06711056F) + (py * 0.00583715F);
    const f32 scaled = 52.9829189F * (inner - std::floor(inner));
    return scaled - std::floor(scaled);
}

[[nodiscard]] f32 view_depth(const f32 projection[4], f32 depth) noexcept {
    return projection[3] / (depth + projection[2]);
}

/// A shader's `normalize`, without the assertion `cy::normalize` makes.
[[nodiscard]] Vec3 unit(Vec3 value) noexcept {
    return value * (1.0F / std::sqrt(dot(value, value)));
}

[[nodiscard]] Vec3 to_view(const ContactShadowConstants& constants, Vec3 relative) noexcept {
    const auto row = [&](u32 index) noexcept {
        return Vec3{constants.view_rows[index][0], constants.view_rows[index][1],
                    constants.view_rows[index][2]};
    };
    return Vec3{dot(row(0), relative), dot(row(1), relative), dot(row(2), relative)};
}

[[nodiscard]] f32 depth_at(const ContactShadowInputs& inputs, u32 x, u32 y) noexcept {
    return inputs.depth[(static_cast<usize>(y) * inputs.width) + x];
}

}  // namespace

Vec3 decode_octahedral(Vec2 encoded) noexcept {
    const f32 cx = (encoded.x * 2.0F) - 1.0F;
    const f32 cy = (encoded.y * 2.0F) - 1.0F;
    Vec3 normal{cx, cy, 1.0F - std::abs(cx) - std::abs(cy)};
    const f32 fold = math::saturate(-normal.z);
    normal.x += normal.x >= 0.0F ? -fold : fold;
    normal.y += normal.y >= 0.0F ? -fold : fold;
    return unit(normal);
}

Vec2 encode_octahedral(Vec3 normal) noexcept {
    const f32 sum = std::abs(normal.x) + std::abs(normal.y) + std::abs(normal.z);
    const Vec3 projected = normal * (1.0F / sum);
    Vec2 encoded{projected.x, projected.y};
    if (projected.z < 0.0F) {
        encoded.x = (1.0F - std::abs(projected.y)) * (projected.x >= 0.0F ? 1.0F : -1.0F);
        encoded.y = (1.0F - std::abs(projected.x)) * (projected.y >= 0.0F ? 1.0F : -1.0F);
    }
    return Vec2{(encoded.x * 0.5F) + 0.5F, (encoded.y * 0.5F) + 0.5F};
}

ContactShadowSettings apply_refinement(const ContactShadowSettings& settings,
                                       f32 refinement) noexcept {
    ContactShadowSettings applied = settings;
    applied.max_distance = settings.max_distance * math::saturate(refinement);
    return applied;
}

Expected<ContactShadowConstants, Error> make_contact_constants(
    const ContactShadowSettings& settings, const ContactShadowView& view) noexcept {
    if (view.width == 0 || view.height == 0) {
        return fail(ErrorCode::InvalidArgument, "contact shadows: the view has no pixels");
    }
    if (settings.steps == 0 || !(settings.length > 0.0F) || !(settings.thickness > 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "contact shadows: the step count, the length and the thickness must be "
                    "positive");
    }
    ContactShadowConstants constants;
    for (u32 row = 0; row < 3; ++row) {
        constants.view_rows[row][0] = view.relative_to_view.columns[0][row];
        constants.view_rows[row][1] = view.relative_to_view.columns[1][row];
        constants.view_rows[row][2] = view.relative_to_view.columns[2][row];
    }
    constants.projection[0] = view.projection.columns[0].x;
    constants.projection[1] = view.projection.columns[1].y;
    constants.projection[2] = view.projection.columns[2].z;
    constants.projection[3] = view.projection.columns[3].z;
    constants.extent[0] = static_cast<f32>(view.width);
    constants.extent[1] = static_cast<f32>(view.height);
    constants.extent[2] = 1.0F / static_cast<f32>(view.width);
    constants.extent[3] = 1.0F / static_cast<f32>(view.height);
    const Vec3 light = unit(to_view(constants, unit(view.to_light)));
    constants.light[0] = light.x;
    constants.light[1] = light.y;
    constants.light[2] = light.z;
    constants.light[3] = settings.length;
    constants.control[0] = static_cast<f32>(settings.steps);
    constants.control[1] = settings.thickness;
    constants.control[2] = settings.max_distance;
    constants.control[3] = settings.normal_offset_pixels;
    // GtaoConstants' convention: the jittered image shows the unjittered point at (x - jx, y + jy).
    constants.jitter[0] = -view.jitter.x;
    constants.jitter[1] = view.jitter.y;
    return constants;
}

f32 contact_shadow_reference_at(const ContactShadowInputs& inputs,
                                const ContactShadowConstants& constants, u32 x, u32 y) noexcept {
    const f32 depth = depth_at(inputs, x, y);
    if (depth <= 0.0F) {
        return 1.0F;
    }
    const f32* projection = constants.projection;
    const f32 distance = view_depth(projection, depth);
    if (distance > constants.control[2]) {
        return 1.0F;
    }
    const Vec3 light{constants.light[0], constants.light[1], constants.light[2]};
    const Vec3 normal = unit(to_view(
        constants, decode_octahedral(inputs.normals[(static_cast<usize>(y) * inputs.width) + x])));
    if (dot(normal, light) <= 0.0F) {
        return 1.0F;
    }
    // contact_shadows.slang's `viewPositionAt`: the pixel centre, jitter undone.
    const f32 u = (static_cast<f32>(x) + 0.5F + constants.jitter[0]) * constants.extent[2];
    const f32 v = (static_cast<f32>(y) + 0.5F + constants.jitter[1]) * constants.extent[3];
    const Vec3 position{((u * 2.0F) - 1.0F) * distance / projection[0],
                        (1.0F - (v * 2.0F)) * distance / projection[1], -distance};
    const f32 footprint = 2.0F * distance / (projection[1] * constants.extent[1]);
    const Vec3 start = position + (normal * (constants.control[3] * footprint));

    const auto steps = static_cast<u32>(constants.control[0]);
    const f32 offset = noise(x, y);
    for (u32 step = 0; step < steps; ++step) {
        const f32 t = (static_cast<f32>(step) + offset) / static_cast<f32>(steps);
        const Vec3 tap = start + (light * (t * constants.light[3]));
        const f32 tap_depth = -tap.z;
        if (tap_depth <= 0.0F) {
            break;
        }
        const f32 ndc_x = projection[0] * tap.x / tap_depth;
        const f32 ndc_y = projection[1] * tap.y / tap_depth;
        const f32 screen_x = (((ndc_x * 0.5F) + 0.5F) * constants.extent[0]) - constants.jitter[0];
        const f32 screen_y = ((0.5F - (ndc_y * 0.5F)) * constants.extent[1]) - constants.jitter[1];
        if (screen_x < 0.0F || screen_y < 0.0F || screen_x >= constants.extent[0] ||
            screen_y >= constants.extent[1]) {
            break;
        }
        const f32 stored = depth_at(inputs, static_cast<u32>(screen_x), static_cast<u32>(screen_y));
        if (stored <= 0.0F) {
            continue;
        }
        const f32 behind = tap_depth - view_depth(projection, stored);
        if (behind > 0.0F && behind < constants.control[1]) {
            // Full shadow for an occluder in the first half of the trace, fading to none at its
            // end, so the term has no edge at `length`.
            return math::saturate((t - 0.5F) * 2.0F);
        }
    }
    return 1.0F;
}

Status contact_shadow_reference(const ContactShadowInputs& inputs,
                                const ContactShadowConstants& constants, Span<f32> out) noexcept {
    const usize pixels = static_cast<usize>(inputs.width) * inputs.height;
    if (inputs.width == 0 || inputs.height == 0 || inputs.depth.size() != pixels ||
        inputs.normals.size() != pixels || out.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "contact shadows: the inputs and the output must all cover the view");
    }
    for (u32 y = 0; y < inputs.height; ++y) {
        for (u32 x = 0; x < inputs.width; ++x) {
            out[(static_cast<usize>(y) * inputs.width) + x] =
                contact_shadow_reference_at(inputs, constants, x, y);
        }
    }
    return ok();
}

}  // namespace cy::rendering::contact_shadows
