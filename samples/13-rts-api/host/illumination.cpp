// SPDX-License-Identifier: MIT
// illumination.cpp — the level's GI. See illumination.h.

#include "illumination.h"

#include <cy/core/math/scalar.h>
#include <cy/core/memory/array.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <numbers>
#include <utility>

namespace sample::rts {
namespace {

namespace gi = cy::rendering::gi;
using cy::f32;
using cy::u32;
using cy::Vec3;

/// The level as boxes. The barracks stands behind the spawn point game/Commander.swift builds
/// workers at (8, 0, 24); the beacon beside it; the crate where level.h puts it.
///
/// THE BEACON IS LARGER THAN A PROMOTED EMITTER MAY BE (12 m² of glass against
/// `EmitterSettings::promotion_max_area_m2`'s 4), so its light reaches its surroundings through the
/// surface cache's gather — the light the frame's GI composites. A promoted emitter's light would
/// be direct light, which the forward frame's own lights do not carry yet.
constexpr FieldBox kBoxes[] = {
    {"ground", Vec3{16.0F, -0.25F, 16.0F}, Vec3{16.0F, 0.25F, 16.0F}, Vec3{0.46F, 0.42F, 0.34F},
     Vec3{0.0F, 0.0F, 0.0F}, 1.0F, false},
    {"barracks", Vec3{8.0F, 1.5F, 27.0F}, Vec3{3.0F, 1.5F, 2.0F}, Vec3{0.66F, 0.30F, 0.20F},
     Vec3{0.0F, 0.0F, 0.0F}, 0.5F, false},
    {kBeaconMaterial, Vec3{12.5F, 1.0F, 26.0F}, Vec3{0.6F, 1.0F, 0.6F}, Vec3{0.9F, 0.9F, 0.85F},
     Vec3{40000.0F, 20000.0F, 6000.0F}, 0.5F, false},
    {"crate", Vec3{28.0F, 0.5F, 6.0F}, Vec3{0.5F, 0.5F, 0.5F}, Vec3{0.55F, 0.40F, 0.24F},
     Vec3{0.0F, 0.0F, 0.0F}, 0.5F, true},
};

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

[[nodiscard]] cy::Mat4 box_transform(Vec3 centre, Vec3 half) noexcept {
    return cy::Mat4::from_translation(centre) * cy::Mat4::from_scale(half * 2.0F);
}

/// The GI's object id and card instance for the crate, past the static boxes' instances.
constexpr cy::u64 kCrateObject = 4;
constexpr u32 kCrateInstance = 4;

}  // namespace

cy::Span<const FieldBox> field_boxes() noexcept {
    return {kBoxes, sizeof(kBoxes) / sizeof(kBoxes[0])};
}

RtsIllumination::RtsIllumination(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}

RtsIllumination::~RtsIllumination() = default;

cy::Status RtsIllumination::build() noexcept {
    gi::IlluminationSettings settings;
    settings.field.levels = 2;
    settings.field.resolution = 64;
    settings.field.base_extent_metres = 16.0F;
    settings.probes.levels = 2;
    settings.probes.base_spacing_metres = 1.5F;
    settings.probes.half_extent_probes = 5;
    settings.rays_per_probe = 8;
    settings.max_ray_distance_metres = 32.0F;
    settings.surface_bounce = gi::SurfaceBounce::Cards;
    settings.card_gather.rays = 4;
    settings.card_gather.max_distance_metres = 32.0F;
    // An overcast dome of a few thousand nits over the 22 000 lux sun: the GI's sky, which
    // replaces the frame's ambient wherever the GI is composited.
    settings.sky.intensity = 2500.0F;
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
    // THE STATIC BOXES: placed in the field, carded in the world, and ingested as one cell — the
    // classification sees the beacon's glass there.
    cy::Array<gi::Surfel> cards(*allocator_);
    cy::Aabb bounds = cy::Aabb::empty();
    const cy::Span<const FieldBox> boxes = field_boxes();
    for (u32 index = 0; index < boxes.size(); ++index) {
        const FieldBox& box = boxes[index];
        if (box.movable) {
            continue;
        }
        const cy::Mat4 transform = box_transform(box.centre, box.half);
        if (cy::Status placed = system_.field().place(index + 1U, cube_field_.asset(), transform);
            !placed) {
            return placed;
        }
        gi::MeshCardSettings carded;
        carded.spacing_metres = box.card_spacing;
        carded.albedo = box.albedo;
        carded.emission = box.emission;
        carded.instance_id = index + 1U;
        carded.material_id = index + 1U;
        if (index == kGroundBox) {
            // The ground's top alone: nothing reaches its underside or its edges.
            carded.clip_to_region = true;
            carded.region = cy::Aabb::from_min_max(
                box.centre + Vec3{-box.half.x, box.half.y - 0.01F, -box.half.z},
                box.centre + box.half + Vec3{0.0F, 0.01F, 0.0F});
        }
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

    // THE CRATE MOVES: an object, its cards in its own space, placed by its transform.
    const FieldBox& crate = boxes[kCrateBox];
    cy::Array<gi::Surfel> crate_cards(*allocator_);
    gi::MeshCardSettings carded;
    carded.spacing_metres = crate.card_spacing;
    carded.albedo = crate.albedo;
    carded.instance_id = kCrateInstance;
    carded.material_id = kCrateBox + 1U;
    if (cy::Status made =
            gi::append_mesh_cards({kCube.positions, 8}, {kCube.indices, 36},
                                  cy::Mat4::from_scale(crate.half * 2.0F), carded, crate_cards);
        !made) {
        return made;
    }
    const gi::AssetDistanceField asset = cube_field_.asset();
    gi::GiObjectDesc object;
    object.id = kCrateObject;
    object.mobility = gi::ObjectMobility::Movable;
    object.local_bounds = cy::Aabb::from_center_extents(Vec3{0.0F, 0.0F, 0.0F}, crate.half);
    object.transform = cy::Mat4::from_translation(crate.centre);
    object.surfels = crate_cards.span();
    object.field = &asset;
    if (cy::Status added = system_.add_object(object, 0); !added) {
        return added;
    }

    // THE SUN, IN THE BAKE'S UNITS: lux over pi, as gi/bake.h hands its path tracer every light.
    gi::GiLight sun;
    sun.directional = true;
    sun.direction = cy::normalize(kSunTravel);
    sun.colour = Vec3{1.0F, 0.96F, 0.88F};
    sun.intensity = kSunLux / std::numbers::pi_v<f32>;
    sun.id = 1;
    lights_.assign(1, sun);

    // THE LEVEL'S OWN LADDERS, sized to it: a quarter of the engine's default probe updates and
    // card updates a frame keep a 32 m field up to date, and those are most of the GI's host cost
    // (at the defaults 22 ms a frame here). The base's volume asks for its probes first (twice
    // the update rate), so what a game looks at settles first.
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

cy::Status RtsIllumination::bind(cy::abi::Host& host, CyEntity crate) noexcept {
    if (!built_) {
        return cy::fail(cy::ErrorCode::InvalidArgument, "the level's GI is not built");
    }
    adapter_ = std::make_unique<cy::game_backend::IlluminationAdapter>(*allocator_, system_);
    const cy::Span<const FieldBox> boxes = field_boxes();
    for (u32 index = 0; index < boxes.size(); ++index) {
        if (cy::Status added = adapter_->add_material(boxes[index].material, index + 1U); !added) {
            return added;
        }
    }
    if (crate != CY_ENTITY_NULL) {
        if (cy::Status bound = adapter_->bind_object(crate, kCrateObject, kCrateInstance); !bound) {
            return bound;
        }
    }
    cy::game_backend::bind_illumination(host, adapter_.get());
    return cy::ok();
}

void RtsIllumination::unbind(cy::abi::Host& host) noexcept {
    cy::game_backend::bind_illumination(host, nullptr);
    adapter_.reset();
}

void RtsIllumination::begin_frame(cy::u64 frame) noexcept {
    if (adapter_ != nullptr) {
        adapter_->begin_frame(frame);
    }
}

cy::Status RtsIllumination::update(Vec3 focus, Vec3 crate, cy::u64 frame) noexcept {
    if (!built_) {
        return cy::ok();
    }
    const auto started = std::chrono::steady_clock::now();
    if (cy::Status moved =
            system_.move_object(kCrateObject, cy::Mat4::from_translation(crate), frame);
        !moved) {
        return moved;
    }
    gi::FrameContext context;
    context.camera = focus;
    context.lights = {lights_.data(), lights_.size()};
    context.frame = frame;
    // No measured cost: a wall-clock figure would make the light the game reads differ from run
    // to run, and the sample's report is the same twice over. The level's ladders (`build`) are
    // what holds the cost.
    (void)system_.update(context);
    frame_ = frame;
    update_ms_ =
        std::chrono::duration<cy::f64, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return cy::ok();
}

}  // namespace sample::rts
