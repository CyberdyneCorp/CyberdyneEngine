// Illumination's consumer of the cloud shadow field.

#include <cy/rendering/lighting/cloud_shadow.h>

#include <cy/core/math/math.h>
#include <cy/rendering/sky/cloud_shadows.h>

namespace cy::rendering {

f32 cloud_shadow_at(const environment::FieldStore& store, const world::WorldVec3d& at) noexcept {
    // THE FUNCTION EVERY CONSUMER CALLS. Not `FieldStore::sample_at` with a residency this module
    // chose: a consumer that picked its own level would read a different shadow from the one
    // terrain read at the same point whenever the regional level had not streamed in, and the two
    // surfaces would disagree along a line nobody could find.
    return math::saturate(sky::CloudShadowField::sample(store, at));
}

f32 sunlight_under_clouds(const GpuLight& sun, const environment::FieldStore& store,
                          const world::WorldVec3d& at) noexcept {
    if (sun.kind != kGpuLightDirectional) {
        return sun.intensity;
    }
    return sun.intensity * cloud_shadow_at(store, at);
}

CloudShadowIllumination apply_cloud_shadows(Span<GpuLight> lights,
                                            const environment::FieldStore& store,
                                            const world::WorldVec3d& at) noexcept {
    CloudShadowIllumination report;
    // ONE SAMPLE FOR THE WHOLE LIST. The field's cells are over a hundred metres wide and every
    // directional light in a frame is shadowed by the same deck at the same point; sampling per
    // light would cost a substrate lookup per light to return the same number.
    report.transmittance = cloud_shadow_at(store, at);
    for (GpuLight& light : lights) {
        if (light.kind != kGpuLightDirectional) {
            ++report.punctual_lights;
            continue;
        }
        light.intensity *= report.transmittance;
        ++report.directional_lights;
    }
    return report;
}

}  // namespace cy::rendering
