// SPDX-License-Identifier: MIT
#include <cy/rendering/gi/media.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering::gi {
namespace {

constexpr Vec3 kAxes[6] = {
    Vec3{1.0F, 0.0F, 0.0F},  Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F},
    Vec3{0.0F, -1.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F},  Vec3{0.0F, 0.0F, -1.0F},
};

}  // namespace

TransmissionSupport transmission_support(TransmissionClass value) noexcept {
    switch (value) {
        case TransmissionClass::Opaque:
        case TransmissionClass::Masked:
            return TransmissionSupport::Full;
        case TransmissionClass::ThinTransmission:
            return TransmissionSupport::Approximate;
        case TransmissionClass::VolumetricMedia:
            return TransmissionSupport::LowFrequency;
        case TransmissionClass::Refraction:
        case TransmissionClass::Caustics:
        case TransmissionClass::Count:
            break;
    }
    return TransmissionSupport::Deferred;
}

const char* transmission_class_name(TransmissionClass value) noexcept {
    switch (value) {
        case TransmissionClass::Opaque:
            return "Opaque";
        case TransmissionClass::Masked:
            return "Masked";
        case TransmissionClass::ThinTransmission:
            return "ThinTransmission";
        case TransmissionClass::VolumetricMedia:
            return "VolumetricMedia";
        case TransmissionClass::Refraction:
            return "Refraction";
        case TransmissionClass::Caustics:
            return "Caustics";
        case TransmissionClass::Count:
            break;
    }
    return "Unknown";
}

const char* transmission_support_name(TransmissionSupport value) noexcept {
    switch (value) {
        case TransmissionSupport::Full:
            return "Full";
        case TransmissionSupport::Approximate:
            return "Approximate";
        case TransmissionSupport::LowFrequency:
            return "LowFrequency";
        case TransmissionSupport::Deferred:
            return "Deferred";
    }
    return "Unknown";
}

Vec3 media_indirect_radiance(const RadianceCache& cache, Vec3 position, Vec3 fallback,
                             f32& coverage) noexcept {
    Vec3 total{0.0F, 0.0F, 0.0F};
    f32 confidence = 0.0F;
    for (const Vec3 axis : kAxes) {
        const RadianceSample answer = cache.sample(position, axis, 0);
        if (answer.confidence > 0.0F) {
            total = total + (answer.radiance * answer.confidence);
            confidence += answer.confidence;
        }
    }
    coverage = confidence / 6.0F;
    const Vec3 cached = confidence > 0.0F ? total / confidence : Vec3{0.0F, 0.0F, 0.0F};
    return (cached * coverage) + (fallback * (1.0F - coverage));
}

Status build_media_indirect(const RadianceCache& cache, const Aabb& bounds, f32 spacing,
                            Vec3 fallback, MediaIndirectGrid& out, u32 max_points) noexcept {
    if (bounds.is_empty() || !(spacing > 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "build_media_indirect: the bounds are empty or the spacing is not positive");
    }
    u32 dimensions[3] = {};
    const Vec3 size = bounds.size();
    u64 points = 1;
    for (u32 axis = 0; axis < 3; ++axis) {
        dimensions[axis] = static_cast<u32>(std::floor(size[axis] / spacing)) + 1U;
        points *= dimensions[axis];
    }
    if (points > max_points) {
        return fail(ErrorCode::InvalidArgument,
                    "build_media_indirect: the grid has more points than the caller allows");
    }
    out.origin = bounds.min;
    out.spacing = spacing;
    for (u32 axis = 0; axis < 3; ++axis) {
        out.dimensions[axis] = dimensions[axis];
    }
    if (Status sized = out.radiance.resize(static_cast<usize>(points)); !sized) {
        return sized;
    }
    if (Status sized = out.coverage.resize(static_cast<usize>(points)); !sized) {
        return sized;
    }
    usize index = 0;
    for (u32 z = 0; z < dimensions[2]; ++z) {
        for (u32 y = 0; y < dimensions[1]; ++y) {
            for (u32 x = 0; x < dimensions[0]; ++x) {
                const Vec3 point =
                    bounds.min +
                    (Vec3{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)} * spacing);
                f32 covered = 0.0F;
                out.radiance[index] = media_indirect_radiance(cache, point, fallback, covered);
                out.coverage[index] = covered;
                index += 1;
            }
        }
    }
    return ok();
}

}  // namespace cy::rendering::gi
