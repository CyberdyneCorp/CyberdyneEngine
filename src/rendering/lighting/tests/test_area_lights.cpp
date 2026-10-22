// Area lights as lights of the frame, at unit cost: the record a rect, disc, sphere or tube
// becomes, the unit its intensity is in, its reach, and the static light's placeholder. Nothing
// here builds an `LtcTable`; `test_area_shading.cpp` holds the shading to its emitter's surface.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/core/math/quat.h>
#include <cy/rendering/lighting/area_shading.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/lighting/units.h>

#include <cmath>
#include <string_view>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::render::LightDescription;
using cy::render::LightKind;
using cy::rendering::GpuLight;
using cy::rendering::LightBuildParameters;
using cy::rendering::PhotometricUnit;

/// An area light at `position`, its forward axis turned to `forward` about the vertical.
LightDescription area(LightKind kind, f32 width, f32 height, Vec3 position) noexcept {
    LightDescription light;
    light.kind = kind;
    light.transform.translation = position;
    light.source_width = width;
    light.source_height = height;
    light.intensity = 500.0F;
    light.range = 12.0F;
    return light;
}

GpuLight record_of(const LightDescription& light) noexcept {
    LightBuildParameters parameters;
    parameters.unit = cy::rendering::default_unit_for(light.kind);
    return cy::rendering::build_gpu_light(light, parameters);
}

}  // namespace

CY_TEST_CASE("area lights: a record carries its kind, its two half-extents and its axes") {
    LightDescription rect = area(LightKind::Rect, 2.0F, 1.0F, Vec3{1.0F, 3.0F, -2.0F});
    rect.transform.rotation = cy::Quat::from_axis_angle(Vec3{0.0F, 1.0F, 0.0F}, 0.6F);
    rect.two_sided = true;
    const GpuLight record = record_of(rect);
    CY_CHECK_EQ(record.kind & cy::rendering::kGpuLightKindMask, cy::rendering::kGpuLightRect);
    CY_CHECK((record.kind & cy::rendering::kGpuLightTwoSided) != 0U);
    CY_CHECK(cy::rendering::gpu_light_is_area(record.kind));
    CY_CHECK_EQ(record.spot_scale, 1.0F);
    CY_CHECK_EQ(record.spot_bias, 0.5F);
    CY_CHECK_EQ(record.range, 12.0F);
    const Vec3 forward = cy::normalize(rect.transform.forward());
    CY_CHECK_NEAR(record.direction[0], forward.x, 1e-6F);
    CY_CHECK_NEAR(record.direction[2], forward.z, 1e-6F);
    const Vec3 tangent = cy::rendering::decode_area_tangent(record.shadow_slot);
    CY_CHECK_LT(cy::distance(tangent, cy::normalize(rect.transform.right())), 1e-4F);

    // A disc's and a sphere's two extents are the radius; a tube's are its half length and its
    // radius, and its one direction is its AXIS — the right axis, not the forward one.
    const GpuLight disc = record_of(area(LightKind::Disc, 0.8F, 0.0F, Vec3{}));
    CY_CHECK_EQ(disc.spot_scale, 0.4F);
    CY_CHECK_EQ(disc.spot_bias, 0.4F);
    const GpuLight sphere = record_of(area(LightKind::Sphere, 0.3F, 0.0F, Vec3{}));
    CY_CHECK_EQ(sphere.kind, cy::rendering::kGpuLightSphere);
    CY_CHECK_EQ(sphere.spot_scale, 0.15F);
    LightDescription tube = area(LightKind::Tube, 1.2F, 0.1F, Vec3{});
    tube.two_sided = true;  // meaningless for a tube, and not carried
    const GpuLight tube_record = record_of(tube);
    CY_CHECK_EQ(tube_record.kind, cy::rendering::kGpuLightTube);
    CY_CHECK_EQ(tube_record.spot_scale, 0.6F);
    CY_CHECK_EQ(tube_record.spot_bias, 0.05F);
    CY_CHECK_NEAR(tube_record.direction[0], 1.0F, 1e-6F);

    // And a punctual light is what it always was: nothing above its kind's byte, its slot free.
    LightDescription point = area(LightKind::Point, 0.0F, 0.0F, Vec3{});
    const GpuLight point_record = record_of(point);
    CY_CHECK_EQ(point_record.kind, cy::rendering::kGpuLightPoint);
    CY_CHECK_EQ(point_record.shadow_slot, cy::rendering::kNoShadowSlot);
    CY_CHECK_FALSE(cy::rendering::gpu_light_is_area(point_record.kind));
}

CY_TEST_CASE("area lights: the tangent survives its sixteen bits a component") {
    const Vec3 directions[] = {Vec3{1.0F, 0.0F, 0.0F},    Vec3{0.0F, 0.0F, -1.0F},
                               Vec3{0.3F, -0.8F, -0.52F}, Vec3{-0.7F, 0.1F, 0.7F},
                               Vec3{0.0F, -1.0F, 0.0F},   Vec3{-0.2F, 0.2F, -0.96F}};
    for (const Vec3 direction : directions) {
        const Vec3 unit = cy::normalize(direction);
        const Vec3 decoded =
            cy::rendering::decode_area_tangent(cy::rendering::encode_area_tangent(unit));
        CY_CHECK_LT(cy::distance(decoded, unit), 1e-4F);
    }
}

CY_TEST_CASE("area lights: nits are the unit, and a lumen spreads over the emitting area") {
    CY_CHECK_EQ(cy::rendering::default_unit_for(LightKind::Rect), PhotometricUnit::Nit);
    CY_CHECK_EQ(cy::rendering::default_unit_for(LightKind::Tube), PhotometricUnit::Nit);

    LightDescription rect = area(LightKind::Rect, 2.0F, 0.5F, Vec3{});
    CY_CHECK_NEAR(cy::rendering::area_light_emitting_area(rect), 1.0F, 1e-6F);
    rect.two_sided = true;
    CY_CHECK_NEAR(cy::rendering::area_light_emitting_area(rect), 2.0F, 1e-6F);
    const LightDescription disc = area(LightKind::Disc, 2.0F, 0.0F, Vec3{});
    CY_CHECK_NEAR(cy::rendering::area_light_emitting_area(disc), cy::math::kPi, 1e-5F);
    const LightDescription sphere = area(LightKind::Sphere, 2.0F, 0.0F, Vec3{});
    CY_CHECK_NEAR(cy::rendering::area_light_emitting_area(sphere), 4.0F * cy::math::kPi, 1e-5F);
    // A tube's side alone: its ends do not emit.
    const LightDescription tube = area(LightKind::Tube, 1.0F, 0.2F, Vec3{});
    CY_CHECK_NEAR(cy::rendering::area_light_emitting_area(tube), 2.0F * cy::math::kPi * 0.1F,
                  1e-5F);

    // A thousand lumens off a one-square-metre panel is 1000 / pi nits: the Lambertian flux
    // spreads over pi steradians of projected solid angle, and a two-sided panel halves it.
    CY_CHECK_NEAR(cy::rendering::to_shading_intensity(LightKind::Rect, PhotometricUnit::Lumen,
                                                      1000.0F, 0.0F, 1.0F),
                  1000.0F / cy::math::kPi, 1e-2F);
    CY_CHECK_NEAR(cy::rendering::to_shading_intensity(LightKind::Rect, PhotometricUnit::Lumen,
                                                      1000.0F, 0.0F, 2.0F),
                  500.0F / cy::math::kPi, 1e-2F);
    // Nits pass through.
    CY_CHECK_EQ(cy::rendering::to_shading_intensity(LightKind::Disc, PhotometricUnit::Nit, 42.0F,
                                                    0.0F, 3.0F),
                42.0F);
    CY_CHECK(cy::rendering::intensity_is_plausible(LightKind::Rect, 2000.0F));
    CY_CHECK_FALSE(cy::rendering::intensity_is_plausible(LightKind::Rect, 1.0e9F));
}

CY_TEST_CASE(
    "area lights: with no range, the reach is where its far field falls to the threshold") {
    LightDescription panel = area(LightKind::Rect, 0.5F, 0.5F, Vec3{0.0F, 0.0F, 0.0F});
    panel.range = 0.0F;
    panel.intensity = 2000.0F;  // nits
    const GpuLight record = record_of(panel);
    const f32 reach = cy::rendering::light_bounding_radius(panel);
    CY_CHECK_EQ(record.range, reach);
    // L A / d^2 at the reach is the threshold — on the axis, where the far field is that.
    const f32 expected = std::sqrt(2000.0F * 0.25F / cy::rendering::kAreaLightReachIlluminance);
    CY_CHECK_NEAR_REL(reach, expected, 1e-5F);
    // An authored range wins.
    panel.range = 5.0F;
    CY_CHECK_EQ(cy::rendering::light_bounding_radius(panel), 5.0F);
    CY_CHECK_EQ(record_of(panel).range, 5.0F);
}

CY_TEST_CASE("area lights: the window ends a light at its reach and leaves the middle whole") {
    const GpuLight record = record_of(area(LightKind::Sphere, 0.2F, 0.0F, Vec3{0.0F, 0.0F, 0.0F}));
    CY_CHECK_NEAR(cy::rendering::area_light_window(record, Vec3{0.5F, 0.0F, 0.0F}), 1.0F, 1e-5F);
    CY_CHECK_EQ(cy::rendering::area_light_window(record, Vec3{12.0F, 0.0F, 0.0F}), 0.0F);
    CY_CHECK_EQ(cy::rendering::area_light_window(record, Vec3{20.0F, 0.0F, 0.0F}), 0.0F);
    const f32 near_edge = cy::rendering::area_light_window(record, Vec3{11.0F, 0.0F, 0.0F});
    CY_CHECK_GT(near_edge, 0.0F);
    CY_CHECK_LT(near_edge, 0.5F);
}

CY_TEST_CASE("area lights: a one-sided panel lights its front and nothing behind it") {
    LightDescription panel = area(LightKind::Rect, 1.0F, 1.0F, Vec3{0.0F, 2.0F, 0.0F});
    // Forward is -Z; pitch it to face straight down.
    panel.transform.rotation =
        cy::Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, -0.5F * cy::math::kPi);
    const GpuLight record = record_of(panel);
    const Vec3 below =
        cy::rendering::light_irradiance(record, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 above =
        cy::rendering::light_irradiance(record, Vec3{0.0F, 4.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F});
    CY_CHECK_GT(below.x, 0.0F);
    CY_CHECK_EQ(above.x, 0.0F);
    panel.two_sided = true;
    const Vec3 both = cy::rendering::light_irradiance(record_of(panel), Vec3{0.0F, 4.0F, 0.0F},
                                                      Vec3{0.0F, -1.0F, 0.0F});
    CY_CHECK_NEAR_REL(both.x, below.x, 1e-4F);
}

CY_TEST_CASE("mobility: a static light keeps its place as a record no loop shades") {
    const GpuLight placeholder = cy::rendering::baked_light_placeholder();
    CY_CHECK_EQ(placeholder.kind, cy::rendering::kGpuLightBaked);
    CY_CHECK_EQ(placeholder.intensity, 0.0F);
    CY_CHECK_EQ(placeholder.layer_mask, 0U);
    CY_CHECK_EQ(cy::rendering::light_irradiance(placeholder, Vec3{}, Vec3{0.0F, 1.0F, 0.0F}).x,
                0.0F);
    CY_CHECK_EQ(cy::render::light_mobility_name(cy::render::LightMobility::Static),
                std::string_view("Static"));
    CY_CHECK_EQ(LightDescription{}.mobility, cy::render::LightMobility::Movable);
}
