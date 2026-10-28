// SPDX-License-Identifier: MIT
// The level's GI. See illumination.h.

#include "illumination.h"

#include <cy/core/memory/array.h>

#include <array>
#include <chrono>
#include <numbers>
#include <utility>

namespace sample::tps {
namespace {

namespace gi = cy::rendering::gi;
using cy::f32;
using cy::u32;
using cy::Vec3;

/// The unit cube's twelve triangles, wound outward: what every box is a transform of.
struct UnitCube {
    Vec3 positions[8]{};
    u32 indices[36]{};

    constexpr UnitCube() noexcept {
        for (u32 corner = 0; corner < 8U; ++corner) {
            positions[corner] =
                Vec3{(corner & 1U) != 0U ? 0.5F : -0.5F, (corner & 2U) != 0U ? 0.5F : -0.5F,
                     (corner & 4U) != 0U ? 0.5F : -0.5F};
        }
        constexpr u32 kFaces[36] = {0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                                    2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5};
        for (u32 index = 0; index < 36U; ++index) {
            indices[index] = kFaces[index];
        }
    }
};
constexpr UnitCube kCube{};

}  // namespace

TpsIllumination::TpsIllumination(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}

cy::Status TpsIllumination::build(const std::vector<GiBox>& boxes, Vec3 sun_travel,
                                  f32 sun_lux) noexcept {
    gi::IlluminationSettings settings;
    settings.field.levels = 2;
    settings.field.resolution = 64;
    settings.field.base_extent_metres = 20.0F;
    settings.probes.levels = 2;
    settings.probes.base_spacing_metres = 1.5F;
    settings.probes.half_extent_probes = 6;
    settings.rays_per_probe = 8;
    settings.max_ray_distance_metres = 32.0F;
    settings.surface_bounce = gi::SurfaceBounce::Cards;
    settings.card_gather.rays = 4;
    settings.card_gather.max_distance_metres = 32.0F;
    // The valley's sky: a clear dome of a few thousand nits over the sun.
    settings.sky.intensity = 3200.0F;
    if (cy::Status configured = system_.configure(settings); !configured) {
        return configured;
    }
    gi::MeshFieldSettings field;
    field.dimension = 16;
    field.margin_metres = 0.25F;
    if (cy::Status made = cube_field_.build({kCube.positions, 8}, {kCube.indices, 36}, field);
        !made) {
        return made;
    }
    cy::Array<gi::Surfel> cards(*allocator_);
    cy::Aabb bounds = cy::Aabb::empty();
    for (u32 index = 0; index < boxes.size(); ++index) {
        const GiBox& box = boxes[index];
        const cy::Mat4 transform =
            cy::Mat4::from_translation(box.centre) * cy::Mat4::from_scale(box.half * 2.0F);
        if (cy::Status placed = system_.field().place(index + 1U, cube_field_.asset(), transform);
            !placed) {
            return placed;
        }
        gi::MeshCardSettings carded;
        carded.spacing_metres = box.card_spacing;
        carded.albedo = box.albedo;
        carded.instance_id = index + 1U;
        carded.material_id = index + 1U;
        if (cy::Status made = gi::append_mesh_cards({kCube.positions, 8}, {kCube.indices, 36},
                                                    transform, carded, cards);
            !made) {
            return made;
        }
        bounds = cy::Aabb::from_min_max(cy::cwise_min(bounds.min, box.centre - box.half),
                                        cy::cwise_max(bounds.max, box.centre + box.half));
    }
    const Vec3 margin{1.0F, 1.0F, 1.0F};
    if (cy::Status ingested = system_.ingest_cell(
            1, cy::Aabb::from_min_max(bounds.min - margin, bounds.max + margin), cards.span(), 0);
        !ingested) {
        return ingested;
    }
    system_.surfaces().set_lookup_radius(0.75F);

    gi::GiLight sun;
    sun.directional = true;
    sun.direction = cy::normalize(sun_travel);
    sun.colour = Vec3{1.0F, 0.95F, 0.86F};
    sun.intensity = sun_lux / std::numbers::pi_v<f32>;
    sun.id = 1;
    lights_.assign(1, sun);

    // A quarter of the engine's default probe and card updates a frame, as the RTS level does: the
    // courtyard is about as large, and those updates are most of the GI's host cost.
    for (const auto& [lever, values] :
         {std::pair{gi::GiLever::ProbeUpdates, std::array<f32, 3>{64.0F, 32.0F, 16.0F}},
          std::pair{gi::GiLever::SurfaceCacheRate, std::array<f32, 3>{128.0F, 64.0F, 32.0F}}}) {
        gi::GiLeverSchedule ladder = gi::default_schedule(lever);
        ladder.position_count = 3;
        for (u32 position = 0; position < 3U; ++position) {
            ladder.value[position] = values[position];
            ladder.relative_cost[position] = 1.0F / static_cast<f32>(1U << position);
        }
        if (cy::Status declared = system_.budget().declare(lever, ladder); !declared) {
            return declared;
        }
    }
    built_ = true;
    return cy::ok();
}

cy::Status TpsIllumination::update(Vec3 focus, cy::u64 frame) noexcept {
    if (!built_) {
        return cy::ok();
    }
    const auto started = std::chrono::steady_clock::now();
    gi::FrameContext context;
    context.camera = focus;
    context.lights = {lights_.data(), lights_.size()};
    context.frame = frame;
    (void)system_.update(context);
    frame_ = frame;
    update_ms_ =
        std::chrono::duration<cy::f64, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return cy::ok();
}

}  // namespace sample::tps
