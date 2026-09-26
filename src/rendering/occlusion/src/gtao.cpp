#include <cy/rendering/occlusion/gtao.h>

#include <cy/core/math/scalar.h>
#include <cy/rendering/denoise/denoiser.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::rendering::occlusion {
namespace {

constexpr f32 kHalfPi = 1.57079632679490F;

/// gtao_common.slang's dither, the whole of the pass's noise.
[[nodiscard]] u32 dither(u32 x, u32 y) noexcept {
    constexpr u32 kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return kBayer[((y & 3U) * 4U) + (x & 3U)];
}

/// A shader's `normalize`, without the assertion `cy::normalize` makes: every caller here has
/// already excluded the zero vector, and the one that has not falls back explicitly.
[[nodiscard]] Vec3 unit(Vec3 value) noexcept {
    return value * (1.0F / std::sqrt(dot(value, value)));
}

struct Reconstruction {
    const GtaoConstants* constants = nullptr;
    const GtaoInputs* inputs = nullptr;

    /// gtao.slang's `viewPositionAt`: a pixel centre of the depth buffer, jitter undone.
    [[nodiscard]] Vec3 position(f32 pixel_x, f32 pixel_y, f32 depth) const noexcept {
        const f32* projection = constants->projection;
        const f32 u = (pixel_x + constants->jitter[0]) * constants->extent[2];
        const f32 v = (pixel_y + constants->jitter[1]) * constants->extent[3];
        const f32 ndc_x = (u * 2.0F) - 1.0F;
        const f32 ndc_y = 1.0F - (v * 2.0F);
        return Vec3{ndc_x * depth / projection[0], ndc_y * depth / projection[1], -depth};
    }

    [[nodiscard]] Vec3 to_view(Vec3 relative) const noexcept {
        const auto row = [&](u32 index) noexcept {
            return Vec3{constants->view_rows[index][0], constants->view_rows[index][1],
                        constants->view_rows[index][2]};
        };
        return Vec3{dot(row(0), relative), dot(row(1), relative), dot(row(2), relative)};
    }

    [[nodiscard]] Vec3 to_relative(Vec3 view) const noexcept {
        const auto row = [&](u32 index) noexcept {
            return Vec3{constants->view_rows[index][0], constants->view_rows[index][1],
                        constants->view_rows[index][2]};
        };
        return (row(0) * view.x) + (row(1) * view.y) + (row(2) * view.z);
    }

    /// gtao.slang's `searchHorizon`. `lattice` is the slice's direction in whole pixels.
    [[nodiscard]] f32 horizon(i32 pixel_x, i32 pixel_y, Vec3 position_view, Vec3 view_vector,
                              i32 lattice_x, i32 lattice_y, f32 radius_pixels, f32 step_noise,
                              f32 low) const noexcept {
        const auto steps = static_cast<u32>(constants->control[1]);
        const auto width = static_cast<i32>(constants->extent[0]);
        const auto height = static_cast<i32>(constants->extent[1]);
        const f32 lattice_length =
            std::sqrt(static_cast<f32>((lattice_x * lattice_x) + (lattice_y * lattice_y)));
        f32 result = low;
        for (u32 index = 0; index < steps; ++index) {
            const f32 t = (static_cast<f32>(index) + step_noise) / static_cast<f32>(steps);
            const i32 multiple = std::max(
                static_cast<i32>(std::floor((t * t * radius_pixels / lattice_length) + 0.5F)), 1);
            const i32 tap_x = pixel_x + (lattice_x * multiple);
            const i32 tap_y = pixel_y + (lattice_y * multiple);
            if (tap_x < 0 || tap_y < 0 || tap_x >= width || tap_y >= height) {
                continue;
            }
            const f32 tap_depth =
                inputs
                    ->depth[(static_cast<usize>(tap_y) * inputs->width) + static_cast<u32>(tap_x)];
            if (tap_depth <= 0.0F) {
                continue;
            }
            const Vec3 tap =
                position(static_cast<f32>(tap_x) + 0.5F, static_cast<f32>(tap_y) + 0.5F,
                         view_depth(constants->projection, tap_depth));
            const Vec3 delta = tap - position_view;
            const f32 separation = std::sqrt(dot(delta, delta));
            if (separation <= 0.0F) {
                continue;
            }
            const f32 cosine = (dot(delta, view_vector) / separation) - constants->control[3];
            const f32 weight =
                math::saturate((separation * constants->radius[1]) + constants->radius[2]);
            result = std::max(result, low + ((cosine - low) * weight));
        }
        return result;
    }
};

/// gtao_common.slang's `kCyGtaoDirections`.
constexpr i32 kDirections[8][2] = {{1, 0}, {2, 1},  {1, 1},  {1, 2},
                                   {0, 1}, {-1, 2}, {-1, 1}, {-2, 1}};

[[nodiscard]] f32 arc_integral(f32 h, f32 n, f32 cos_n, f32 sin_n) noexcept {
    return (cos_n + (2.0F * h * sin_n) - std::cos((2.0F * h) - n)) * 0.25F;
}

struct SliceSums {
    f32 visible = 0.0F;
    f32 reference = 0.0F;
    Vec3 bent{0.0F, 0.0F, 0.0F};
};

/// One slice of gtao.slang's loop, accumulated into `sums`.
void integrate_slice(const Reconstruction& scene, u32 x, u32 y, Vec3 position, Vec3 view_vector,
                     Vec3 normal, const i32 lattice[2], f32 radius_pixels, f32 step_noise,
                     SliceSums& sums) noexcept {
    const f32 lattice_length =
        std::sqrt(static_cast<f32>((lattice[0] * lattice[0]) + (lattice[1] * lattice[1])));
    const Vec2 direction{static_cast<f32>(lattice[0]) / lattice_length,
                         static_cast<f32>(lattice[1]) / lattice_length};
    const Vec3 plane_direction{direction.x, -direction.y, 0.0F};
    const Vec3 orthogonal = plane_direction - (view_vector * dot(plane_direction, view_vector));
    const Vec3 axis = unit(cross(plane_direction, view_vector));
    const Vec3 projected = normal - (axis * dot(normal, axis));
    const f32 projected_length = std::sqrt(dot(projected, projected));
    if (projected_length <= 1.0e-4F) {
        return;
    }
    const f32 cos_n = math::saturate(dot(projected, view_vector) / projected_length);
    const f32 n = (dot(orthogonal, projected) < 0.0F ? -1.0F : 1.0F) * std::acos(cos_n);
    const f32 sin_n = std::sin(n);

    const f32 low_positive = std::cos(n + kHalfPi);
    const f32 low_negative = std::cos(n - kHalfPi);
    const auto pixel_x = static_cast<i32>(x);
    const auto pixel_y = static_cast<i32>(y);
    const f32 cos_positive = scene.horizon(pixel_x, pixel_y, position, view_vector, lattice[0],
                                           lattice[1], radius_pixels, step_noise, low_positive);
    const f32 cos_negative = scene.horizon(pixel_x, pixel_y, position, view_vector, -lattice[0],
                                           -lattice[1], radius_pixels, step_noise, low_negative);
    const f32 h_positive =
        n + math::clamp(std::acos(math::clamp(cos_positive, -1.0F, 1.0F)) - n, -kHalfPi, kHalfPi);
    const f32 h_negative =
        n + math::clamp(-std::acos(math::clamp(cos_negative, -1.0F, 1.0F)) - n, -kHalfPi, kHalfPi);

    const f32 arc =
        arc_integral(h_negative, n, cos_n, sin_n) + arc_integral(h_positive, n, cos_n, sin_n);
    sums.visible += projected_length * arc;
    sums.reference += projected_length * (cos_n + (n * sin_n));

    const f32 middle = (h_negative + h_positive) * 0.5F;
    const Vec3 across = unit(orthogonal);
    sums.bent = sums.bent + (((view_vector * std::cos(middle)) + (across * std::sin(middle))) *
                             (projected_length * arc));
}

[[nodiscard]] Vec4 gtao_pixel(const Reconstruction& scene, u32 x, u32 y) noexcept {
    const GtaoConstants& constants = *scene.constants;
    const GtaoInputs& inputs = *scene.inputs;
    const usize index = (static_cast<usize>(y) * inputs.width) + x;
    const f32 depth = inputs.depth[index];
    if (depth <= 0.0F) {
        return Vec4{0.0F, 0.0F, 0.0F, 1.0F};
    }
    const Vec2 centre{static_cast<f32>(x) + 0.5F, static_cast<f32>(y) + 0.5F};
    const f32 distance = view_depth(constants.projection, depth);
    const Vec3 position = scene.position(centre.x, centre.y, distance);
    const Vec3 view_vector = unit(Vec3{-position.x, -position.y, -position.z});
    const Vec3 relative_normal = decode_octahedral(inputs.normals[index]);
    const Vec3 normal = unit(scene.to_view(relative_normal));

    const f32 radius_pixels = std::min(
        constants.radius[0] * constants.projection[1] * 0.5F * constants.extent[1] / distance,
        constants.control[2]);
    if (radius_pixels < 1.0F) {
        return Vec4{relative_normal.x, relative_normal.y, relative_normal.z, 1.0F};
    }
    const auto slices = static_cast<u32>(constants.control[0]);
    const u32 spacing = 8U / slices;
    const u32 rotation = dither(x, y) % spacing;
    const f32 step_noise = (static_cast<f32>(dither(y, x)) + 0.5F) / 16.0F;

    SliceSums sums;
    for (u32 slice = 0; slice < slices; ++slice) {
        integrate_slice(scene, x, y, position, view_vector, normal,
                        kDirections[rotation + (slice * spacing)], radius_pixels, step_noise, sums);
    }
    f32 visibility = sums.reference > 0.0F ? math::saturate(sums.visible / sums.reference) : 1.0F;
    visibility = std::pow(visibility, constants.radius[3]);
    const f32 bent_length = std::sqrt(dot(sums.bent, sums.bent));
    const Vec3 bent = bent_length > 1.0e-6F ? scene.to_relative(sums.bent * (1.0F / bent_length))
                                            : relative_normal;
    return Vec4{bent.x, bent.y, bent.z, visibility};
}

}  // namespace

f32 view_depth(const f32 projection[4], f32 depth) noexcept {
    return projection[3] / (depth + projection[2]);
}

void write_occlusion_control(u32 slot, const AmbientOcclusionSettings& settings,
                             u32 out[4]) noexcept {
    out[0] = slot;
    out[1] = settings.apply_to_direct ? 1U : 0U;
    std::memcpy(&out[2], &settings.direct_strength, sizeof(f32));
    out[3] = 0;
}

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

Expected<GtaoConstants, Error> make_gtao_constants(const GtaoSettings& settings,
                                                   const GtaoView& view) noexcept {
    if (view.width == 0 || view.height == 0) {
        return fail(ErrorCode::InvalidArgument, "gtao: the view has no pixels");
    }
    const f32 radius = settings.shared.radius;
    if (!(radius > 0.0F) || settings.steps == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "gtao: the radius must be positive and the step count non-zero");
    }
    if (settings.slices == 0 || settings.slices > 8 || (8U % settings.slices) != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "gtao: the slices are an even share of the eight lattice directions, so 1, 2, "
                    "4 or 8");
    }
    GtaoConstants constants;
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
    // weight(d) = saturate(d * mul + add): 1 inside `radius - range`, 0 at `radius`.
    const f32 range = std::max(settings.falloff_fraction, 1.0e-3F) * radius;
    const f32 from = radius - range;
    constants.radius[0] = radius;
    constants.radius[1] = -1.0F / range;
    constants.radius[2] = (from / range) + 1.0F;
    constants.radius[3] = settings.shared.power;
    constants.control[0] = static_cast<f32>(settings.slices);
    constants.control[1] = static_cast<f32>(settings.steps);
    constants.control[2] = settings.max_radius_pixels;
    constants.control[3] = settings.horizon_bias;
    // The jittered image shows the unjittered point at (x - jx, y + jy): +y is up in the temporal
    // framework's convention and down in pixel rows.
    constants.jitter[0] = -view.jitter.x;
    constants.jitter[1] = view.jitter.y;
    return constants;
}

u32 filter_pass_count() noexcept {
    const denoise::SignalConfig config =
        denoise::default_config(denoise::SignalKind::AmbientOcclusion);
    return std::min(config.max_passes, denoise::quality_ladder()[0].max_passes);
}

GtaoFilterConstants make_filter_constants(const GtaoView& view, u32 step) noexcept {
    const denoise::SignalConfig config =
        denoise::default_config(denoise::SignalKind::AmbientOcclusion);
    GtaoFilterConstants constants;
    constants.projection[0] = view.projection.columns[0].x;
    constants.projection[1] = view.projection.columns[1].y;
    constants.projection[2] = view.projection.columns[2].z;
    constants.projection[3] = view.projection.columns[3].z;
    constants.sigma[0] = config.sigma_depth;
    constants.sigma[1] = config.sigma_normal;
    constants.sigma[2] = config.sigma_value;
    constants.control[0] = step;
    constants.control[1] = denoise::quality_ladder()[0].kernel_extent;
    constants.control[2] = view.width;
    constants.control[3] = view.height;
    return constants;
}

Vec4 gtao_reference_at(const GtaoInputs& inputs, const GtaoConstants& constants, u32 x,
                       u32 y) noexcept {
    Reconstruction scene;
    scene.constants = &constants;
    scene.inputs = &inputs;
    return gtao_pixel(scene, x, y);
}

Status gtao_reference(const GtaoInputs& inputs, const GtaoConstants& constants,
                      Span<Vec4> out) noexcept {
    const usize pixels = static_cast<usize>(inputs.width) * inputs.height;
    if (pixels == 0 || inputs.depth.size() != pixels || inputs.normals.size() != pixels ||
        out.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "gtao_reference: every buffer must cover the view exactly");
    }
    Reconstruction scene;
    scene.constants = &constants;
    scene.inputs = &inputs;
    for (u32 y = 0; y < inputs.height; ++y) {
        for (u32 x = 0; x < inputs.width; ++x) {
            out[(static_cast<usize>(y) * inputs.width) + x] = gtao_pixel(scene, x, y);
        }
    }
    return ok();
}

}  // namespace cy::rendering::occlusion
