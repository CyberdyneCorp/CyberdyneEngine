// SPDX-License-Identifier: MIT
// The host half of the device surface cache, headless: the field's change journal, the card
// lookup a device can index, the shadow map, and the shading seam `system.h` chooses CPU or GPU
// through. Issue #35, stages 1 and 2.
//
// `render.gi_gpu` holds every dispatch to these functions. This file holds these functions to the
// host subsystems they stand beside — the snapshot to `SurfaceCache::radiance_at`, the shadow map
// to the field's own occlusion, the journal to the scroll report — so the chain is host subsystem →
// oracle → dispatch, and only the last link needs a device.

#include <cy/test/test.h>

#include <cy/core/math/matrix.h>
#include <cy/rendering/gi/card_lighting.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi/tracing.h>

#include "support.h"

#include <cmath>
#include <vector>

namespace {

using cy::f32;
using cy::Span;
using cy::u32;
using cy::u64;
using cy::Vec3;
using cy::rendering::gi::CardSnapshot;
using cy::rendering::gi::ClipmapSettings;
using cy::rendering::gi::DistanceField;
using cy::rendering::gi::FieldBrickChange;
using cy::rendering::gi::GiLight;
using cy::rendering::gi::ShadowMap;
using cy::rendering::gi::ShadowMapOccluder;
using cy::rendering::gi::ShadowMapSettings;
using cy::rendering::gi::SoftwareTracer;
using cy::rendering::gi::SurfaceCache;
using cy::rendering::gi::SurfacePage;
using cy::rendering::gi::SurfaceShadingBackend;
using cy::rendering::gi::SurfaceUpdateContext;
using cy::rendering::gi::Surfel;

/// A backend that shades nothing and records what it was handed: the seam, observed.
class RecordingBackend final : public SurfaceShadingBackend {
public:
    std::vector<u32> submitted;
    u64 frame = 0;
    u32 submissions = 0;
    u32 retirements = 0;

    [[nodiscard]] u32 submit(Span<const SurfacePage> pages, Span<const u32> selected,
                             const SurfaceUpdateContext& context) noexcept override {
        (void)pages;
        submitted.assign(selected.begin(), selected.end());
        frame = context.frame;
        submissions += 1;
        return static_cast<u32>(selected.size());
    }

    u32 retire(Span<SurfacePage> pages) noexcept override {
        u32 written = 0;
        for (const u32 handle : submitted) {
            pages[handle].direct = Vec3{1.0F, 2.0F, 3.0F};
            pages[handle].valid = true;
            pages[handle].error = 0.0F;
            pages[handle].last_update_frame = frame;
            written += 1;
        }
        submitted.clear();
        retirements += written == 0 ? 0U : 1U;
        return written;
    }
};

}  // namespace

CY_TEST_CASE("the field's change journal is the bricks a scroll solved") {
    const gi_support::BoxField box(Vec3{1.0F, 1.0F, 1.0F});
    DistanceField field;
    ClipmapSettings settings;
    settings.levels = 2;
    settings.resolution = 32;
    settings.base_extent_metres = 8.0F;
    CY_REQUIRE(field.configure(settings).has_value());
    CY_REQUIRE(field.place(1, box.asset(), cy::Mat4::identity()).has_value());

    const u64 before = field.generation();
    const auto first = field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK_EQ(field.generation(), before + 1U);
    CY_CHECK_EQ(field.last_changes().size(), first.bricks_solved);

    // A still scroll: a generation, and nothing in it.
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK_EQ(field.last_changes().size(), 0U);

    // A step along x: the journal is the newly exposed band and nothing else, and every entry that
    // carries a slot names samples the pool actually holds.
    const auto moved = field.scroll_to(Vec3{2.0F, 0.0F, 0.0F});
    CY_CHECK_EQ(field.last_changes().size(), moved.bricks_solved);
    for (const FieldBrickChange& change : field.last_changes()) {
        CY_CHECK(change.slot == cy::rendering::gi::kEmptyBrick ||
                 change.slot < field.brick_slot_count());
    }

    // `visit_bricks` is the whole field: every allocated and every empty brick, once.
    u32 visited = 0;
    u32 stored = 0;
    field.visit_bricks([&](const FieldBrickChange& change) {
        visited += 1;
        stored += change.slot == cy::rendering::gi::kEmptyBrick ? 0U : 1U;
    });
    CY_CHECK_EQ(visited, field.diagnostics().allocated_bricks + field.diagnostics().empty_bricks);
    CY_CHECK_EQ(stored, field.diagnostics().allocated_bricks);
}

CY_TEST_CASE("the card snapshot answers a lookup with the card the surface cache would") {
    const std::vector<Surfel> surfels = gi_support::room_surfels(1.0F);
    SurfaceCache cache;
    for (const Surfel& surfel : surfels) {
        CY_REQUIRE(cache.allocate(surfel).has_value());
    }
    const std::vector<GiLight> lights = gi_support::room_lights();
    SurfaceUpdateContext context;
    context.lights = {lights.data(), lights.size()};
    context.frame = 3;
    (void)cache.update_all(context);

    CardSnapshot snapshot;
    CY_REQUIRE(snapshot.capture(cache, 3).has_value());

    // Every card's own position, nudged off it and with its normal tilted: the lookups a traced hit
    // makes, including the edges where two walls meet and two cards compete.
    u32 queries = 0;
    u32 disagreements = 0;
    u32 found = 0;
    for (const Surfel& surfel : surfels) {
        for (const f32 nudge : {0.0F, 0.2F, 0.45F}) {
            const Vec3 offset{nudge, -nudge * 0.5F, nudge * 0.3F};
            const Vec3 point = surfel.position + offset;
            const Vec3 normal =
                cy::normalized_or(surfel.normal + Vec3{0.3F, 0.1F, -0.2F}, surfel.normal);
            Vec3 expected{};
            Vec3 measured{};
            u32 age = 0;
            const bool a = cache.radiance_at(point, normal, expected, age);
            const bool b = snapshot.radiance_at(point, normal, measured, age);
            queries += 1;
            found += a ? 1U : 0U;
            if (a != b || cy::length(expected - measured) > 1.0e-6F) {
                disagreements += 1;
            }
        }
    }
    CY_CHECK_GT(found, queries / 2U);
    CY_CHECK_EQ(disagreements, 0U);

    // An invalidated card answers nothing through either.
    const cy::Aabb corner =
        cy::Aabb::from_min_max(Vec3{-4.5F, -2.5F, -4.5F}, Vec3{-3.5F, -1.5F, -3.5F});
    CY_REQUIRE(cache.invalidate(corner) > 0U);
    CY_REQUIRE(snapshot.capture(cache, 4).has_value());
    Vec3 radiance{};
    u32 age = 0;
    CY_CHECK_FALSE(
        snapshot.radiance_at(Vec3{-4.0F, -2.0F, -4.0F}, Vec3{0.0F, 1.0F, 0.0F}, radiance, age));
}

CY_TEST_CASE("the shadow map agrees with the field about what the sun cannot reach") {
    const gi_support::BoxField slab(Vec3{6.0F, 0.5F, 6.0F}, 1.0F, 17);
    const gi_support::BoxField pillar(Vec3{0.5F, 1.5F, 0.5F}, 1.0F, 9);
    DistanceField field;
    ClipmapSettings settings;
    settings.levels = 1;
    settings.resolution = 64;
    settings.base_extent_metres = 16.0F;
    CY_REQUIRE(field.configure(settings).has_value());
    CY_REQUIRE(field.place(1, slab.asset(), cy::Mat4::from_translation(Vec3{0.0F, -0.5F, 0.0F}))
                   .has_value());
    CY_REQUIRE(field.place(2, pillar.asset(), cy::Mat4::from_translation(Vec3{0.0F, 1.5F, 0.0F}))
                   .has_value());
    (void)field.scroll_to(Vec3{0.0F, 1.0F, 0.0F});

    const Vec3 sun = cy::normalized_or(Vec3{0.45F, -1.0F, 0.35F}, Vec3{0.0F, -1.0F, 0.0F});
    ShadowMapSettings map_settings;
    map_settings.direction = sun;
    map_settings.centre = Vec3{0.0F, 1.0F, 0.0F};
    map_settings.half_extent_metres = 7.0F;
    map_settings.depth_range_metres = 24.0F;
    map_settings.resolution = 128;
    ShadowMap map;
    CY_REQUIRE(map.configure(map_settings).has_value());
    map.capture(field);

    SoftwareTracer software;
    software.bind(field, nullptr);
    const ShadowMapOccluder occluder(&map, &software);

    // The floor around the pillar, where its shadow falls and where it does not.
    u32 points = 0;
    u32 agree = 0;
    u32 shadowed = 0;
    for (int iz = -12; iz <= 12; ++iz) {
        for (int ix = -12; ix <= 12; ++ix) {
            const Vec3 p{static_cast<f32>(ix) * 0.25F, 0.01F, static_cast<f32>(iz) * 0.25F};
            if (std::abs(p.x) < 0.6F && std::abs(p.z) < 0.6F) {
                continue;
            }
            const Vec3 target = p - (sun * 1000.0F);
            const bool by_map = occluder.occluded(p, target);
            const bool by_field = software.occluded(p, target);
            points += 1;
            agree += by_map == by_field ? 1U : 0U;
            shadowed += by_field ? 1U : 0U;
        }
    }
    // The map and the field are two resolutions of one scene, so they disagree only along the
    // shadow's edge: a texel's width of it.
    CY_CHECK_GT(shadowed, 8U);
    CY_CHECK_GE(agree * 100U, points * 95U);

    // A segment that does not point back along the map's light is the fallback's to answer, and
    // the map does not claim it: the lamp's shadow ray through the pillar is the field's.
    const Vec3 lamp{2.0F, 1.0F, 0.0F};
    const Vec3 behind{-2.0F, 1.0F, 0.0F};
    CY_CHECK(software.occluded(behind, lamp));
    CY_CHECK(occluder.occluded(behind, lamp));
    const ShadowMapOccluder unbacked(&map, nullptr);
    CY_CHECK_FALSE(unbacked.occluded(behind, lamp));
}

CY_TEST_CASE("a shading backend installed on the system shades what the scheduler selects") {
    cy::rendering::gi::IlluminationSystem system;
    CY_REQUIRE(system.configure(gi_support::room_settings()).has_value());
    for (const Surfel& surfel : gi_support::room_surfels(1.0F)) {
        CY_REQUIRE(system.surfaces().allocate(surfel).has_value());
    }
    RecordingBackend backend;
    system.set_surface_shading(&backend);

    cy::rendering::gi::FrameContext context;
    const std::vector<GiLight> lights = gi_support::room_lights();
    context.lights = {lights.data(), lights.size()};
    context.frame = 1;
    const auto first = system.update(context);
    CY_CHECK_EQ(backend.submissions, 1U);
    CY_CHECK_GT(first.surfaces.pages_updated, 0U);
    CY_CHECK_EQ(first.surfaces.pages_updated, static_cast<u32>(backend.submitted.size()));
    // Nothing is valid yet: the host shaded nothing, and the backend has not been retired.
    CY_CHECK_EQ(system.surfaces().diagnostics().valid_pages, 0U);

    // The selection handed over is the one the scheduler makes, in its order.
    cy::Array<u32> expected;
    cy::rendering::gi::SurfaceUpdateReport report;
    SurfaceUpdateContext selection;
    selection.frame = 1;
    selection.budget = first.surfaces.pages_updated;
    // `select` over a page array that has not moved since: the same answer the update computed.
    std::vector<SurfacePage> pages(system.surfaces().pages().begin(),
                                   system.surfaces().pages().end());
    CY_REQUIRE(
        SurfaceCache::select({pages.data(), pages.size()}, selection, false, expected, report)
            .has_value());
    CY_REQUIRE_EQ(expected.size(), backend.submitted.size());
    for (cy::usize index = 0; index < expected.size(); ++index) {
        CY_CHECK_EQ(expected[index], backend.submitted[index]);
    }

    // The next update retires the submission before it selects, so those pages are valid now and
    // carry the backend's answer, and the selection moves on to pages that are not.
    const std::vector<u32> retired = backend.submitted;
    context.frame = 2;
    (void)system.update(context);
    CY_CHECK_EQ(backend.retirements, 1U);
    CY_CHECK_EQ(system.surfaces().diagnostics().valid_pages, static_cast<u32>(retired.size()));
    for (const u32 handle : retired) {
        CY_CHECK_EQ(system.surfaces().page(handle).direct.y, 2.0F);
    }

    // Null restores the host shading, which shades in place.
    system.set_surface_shading(nullptr);
    context.frame = 3;
    const auto host = system.update(context);
    CY_CHECK_GT(host.surfaces.pages_updated, 0U);
    CY_CHECK_EQ(backend.submissions, 2U);
}
