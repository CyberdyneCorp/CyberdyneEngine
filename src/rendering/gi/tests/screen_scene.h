// SPDX-License-Identifier: MIT
#pragma once
// One scene the per-pixel resolve runs over, the host half: its quads, cards, field, light, the
// composed system, one camera's surfaces, the resolve and the denoiser, a frame of each per
// `step`. `integration.render_gi_screen` measures it; `render.gi_gpu_resolve` runs the device
// beside it and compares. Issue #35, stage 4.

#include <cy/test/test.h>

#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/gi/screen_resolve.h>
#include <cy/rendering/gi/system.h>

#include "screen_support.h"
#include "support.h"

#include <memory>
#include <vector>

namespace gi_support {

using cy::f32;
using cy::f64;
using cy::i32;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::Vec3;

inline constexpr u32 kScreenWidth = 40;
inline constexpr u32 kScreenHeight = 24;

[[nodiscard]] inline f32 luminance(Vec3 c) noexcept {
    return (0.2126F * c.x) + (0.7152F * c.y) + (0.0722F * c.z);
}

/// A scene the resolve runs over: its quads, its cards, its field, a light, the composed system,
/// one camera's surfaces, the resolve and the denoiser — one frame of each per `step`.
struct ScreenScene {
    std::vector<AxisQuad> quads;
    std::vector<cy::rendering::gi::Surfel> surfels;
    std::vector<cy::rendering::gi::GiLight> lights = room_lights();
    RoomField room{Vec3{kRoomX, kRoomY, kRoomZ}};
    std::vector<std::unique_ptr<BoxField>> solids;
    std::vector<Vec3> solid_centres;
    cy::rendering::gi::IlluminationSystem system;
    ScreenView view;
    cy::rendering::gi::ScreenResolveSettings settings;
    cy::rendering::gi::ScreenResolve resolve;
    cy::rendering::denoise::Denoiser denoiser;
    std::vector<i32> source;
    std::vector<f32> confidence;
    std::vector<f32> previous_depth;
    cy::Mat4 previous_clip = cy::Mat4::identity();
    std::vector<cy::Aabb> invalidated;
    /// The resolve grid's divisor: one resolves every pixel, two a quarter of them and upsamples.
    u32 divisor = 1;
    /// Whether a frame denoises and upsamples the specular signal as well as the diffuse one. A
    /// case that measures only the diffuse image turns it off rather than pay for a signal nobody
    /// reads: the denoiser keeps each signal's history apart, so the diffuse frame is the same
    /// either way.
    bool specular_enabled = true;
    cy::rendering::gi::ScreenGrid grid;
    /// The denoised signals on the grid, and upsampled to the frame's pixels.
    cy::Span<const Vec3> grid_diffuse;
    cy::Span<const Vec3> grid_specular;
    std::vector<Vec3> diffuse_frame;
    std::vector<Vec3> specular_frame;
    cy::Span<const Vec3> diffuse;
    cy::Span<const Vec3> specular;

    /// Solid slabs in the field beside the hollow room: a divider, or a 10 cm wall.
    void add_solid(Vec3 centre, Vec3 half_extents, u32 samples) {
        solids.push_back(std::make_unique<BoxField>(half_extents, 0.5F, samples));
        solid_centres.push_back(centre);
    }

    /// `host_resolve` false skips the host resolve's and denoiser's buffers: a device suite that
    /// times the device alone at 1080p needs the view and the caches, not the oracle's frame.
    void build(Vec3 eye, Vec3 target, u32 width = kScreenWidth, u32 height = kScreenHeight,
               bool ray_tracing = false, bool host_resolve = true) {
        cy::rendering::gi::IlluminationSettings configured = room_settings();
        configured.ray_tracing.device_supports_ray_tracing = ray_tracing;
        // The device frame's bounce: each card gathers its own side of the room through the field.
        configured.surface_bounce = cy::rendering::gi::SurfaceBounce::Cards;
        configured.card_gather.max_distance_metres = configured.max_ray_distance_metres;

        CY_REQUIRE(system.configure(configured).has_value());
        CY_REQUIRE(system.field().place(1, room.asset(), cy::Mat4::identity()).has_value());
        for (usize index = 0; index < solids.size(); ++index) {
            CY_REQUIRE(system.field()
                           .place(10 + index, solids[index]->asset(),
                                  cy::Mat4::from_translation(solid_centres[index]))
                           .has_value());
        }
        const cy::Aabb bounds = cy::Aabb::from_center_extents(
            Vec3{0.0F, 0.0F, 0.0F}, Vec3{kRoomX + 1.0F, kRoomY + 1.0F, kRoomZ + 1.0F});
        CY_REQUIRE(
            system.scene().ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value());
        CY_REQUIRE(system.surfaces().allocate_from(system.scene(), bounds).has_value());
        system.surfaces().set_lookup_radius(0.6F);
        view = render_view(quads, eye, target, width, height);
        if (host_resolve) {
            resize_grid();
        }
        settings.max_ray_distance_metres = configured.max_ray_distance_metres;
    }

    /// Size the host resolve and denoiser for the grid at `divisor`. A new divisor is a new
    /// history: the denoiser starts over, as a frame whose grid changed must.
    void resize_grid() {
        CY_REQUIRE(grid.build(view.surfaces(), divisor).has_value());
        CY_REQUIRE(resolve.resize(grid.width(), grid.height()).has_value());
        CY_REQUIRE(denoiser.resize(grid.width(), grid.height()).has_value());
        source.assign(static_cast<usize>(grid.width()) * grid.height(), -1);
        confidence.assign(source.size(), 0.0F);
        previous_depth.clear();
        diffuse_frame.assign(view.depth.size(), Vec3{0.0F, 0.0F, 0.0F});
        specular_frame.assign(view.depth.size(), Vec3{0.0F, 0.0F, 0.0F});
    }

    [[nodiscard]] cy::rendering::gi::ScreenResolveSources sources(u64 frame) const noexcept {
        cy::rendering::gi::ScreenResolveSources out;
        out.cache = &system.radiance();
        out.field = &system.field();
        out.software = &system.software_tracer();
        out.hardware = &system.hardware_tracer();
        out.sky = system.sky_term();
        out.frame = frame;
        out.cache_frame = frame;
        return out;
    }

    /// One frame: the caches, the resolve, the history and both signals through the denoiser.
    void step(u64 frame) {
        cy::rendering::gi::FrameContext context;
        context.camera = view.camera;
        context.lights = {lights.data(), lights.size()};
        context.frame = frame;
        context.measured_gi_ms = 1.0F;
        (void)system.update(context);
        CY_REQUIRE(grid.build(view.surfaces(), divisor).has_value());
        const cy::rendering::gi::ScreenSurfaces surfaces = grid.surfaces();
        CY_REQUIRE(resolve.produce(surfaces, settings, sources(frame)).has_value());
        cy::rendering::gi::ScreenHistoryInputs history;
        history.current_world_to_clip = view.world_to_clip;
        history.previous_world_to_clip = previous_clip;
        history.previous_depth = {previous_depth.data(), previous_depth.size()};
        history.invalidated = {invalidated.data(), invalidated.size()};
        CY_REQUIRE(cy::rendering::gi::screen_history(surfaces, history,
                                                     {source.data(), source.size()},
                                                     {confidence.data(), confidence.size()})
                       .has_value());
        cy::rendering::denoise::HistoryGuidance guidance;
        guidance.source = {source.data(), source.size()};
        guidance.confidence = {confidence.data(), confidence.size()};
        auto filtered =
            denoiser.denoise(cy::rendering::denoise::SignalKind::IndirectDiffuse,
                             resolve.noisy(cy::rendering::denoise::SignalKind::IndirectDiffuse),
                             cy::rendering::gi::screen_guidance(surfaces), guidance);
        CY_REQUIRE(filtered.has_value());
        grid_diffuse = filtered.value();
        CY_REQUIRE(cy::rendering::gi::screen_upsample(view.surfaces(), surfaces, divisor,
                                                      grid_diffuse,
                                                      {diffuse_frame.data(), diffuse_frame.size()})
                       .has_value());
        diffuse = {diffuse_frame.data(), diffuse_frame.size()};
        if (specular_enabled) {
            step_specular(surfaces, guidance);
        }
        previous_depth.assign(surfaces.depth.begin(), surfaces.depth.end());
        previous_clip = view.world_to_clip;
        invalidated.clear();
    }

    void step_specular(const cy::rendering::gi::ScreenSurfaces& surfaces,
                       const cy::rendering::denoise::HistoryGuidance& guidance) {
        auto reflected =
            denoiser.denoise(cy::rendering::denoise::SignalKind::IndirectSpecular,
                             resolve.noisy(cy::rendering::denoise::SignalKind::IndirectSpecular),
                             cy::rendering::gi::screen_guidance(surfaces), guidance);
        CY_REQUIRE(reflected.has_value());
        grid_specular = reflected.value();
        CY_REQUIRE(
            cy::rendering::gi::screen_upsample(view.surfaces(), surfaces, divisor, grid_specular,
                                               {specular_frame.data(), specular_frame.size()})
                .has_value());
        specular = {specular_frame.data(), specular_frame.size()};
    }

    void run(u64 first, u32 frames) {
        for (u32 index = 0; index < frames; ++index) {
            step(first + index);
        }
    }

    [[nodiscard]] std::vector<Vec3> copy_diffuse() const {
        return {diffuse.begin(), diffuse.end()};
    }
};

/// The GI test room: the host suites' room with its divider, the divider in the field as two thin
/// slabs either side of the doorway, and a camera in the lit half looking across it.
inline void make_room(ScreenScene& scene, u32 width = kScreenWidth, u32 height = kScreenHeight,
                      bool ray_tracing = false, bool host_resolve = true) {
    scene.quads = room_quads(kRoomX, kRoomY, kRoomZ, true);
    scene.surfels = surfels_from_quads(scene.quads, 1.0F);
    const f32 half_span = (kRoomX - 1.0F) * 0.5F;
    scene.add_solid(Vec3{-1.0F - half_span, 0.0F, 0.0F}, Vec3{half_span, kRoomY, 0.05F}, 33);
    scene.add_solid(Vec3{1.0F + half_span, 0.0F, 0.0F}, Vec3{half_span, kRoomY, 0.05F}, 33);
    scene.build(Vec3{2.5F, 0.4F, -3.4F}, Vec3{-1.5F, -0.9F, 0.2F}, width, height, ray_tracing,
                host_resolve);
}

/// Mean relative difference between two images over the pixels with a surface.
[[nodiscard]] inline f32 image_difference(const std::vector<Vec3>& a, const std::vector<Vec3>& b,
                                          const std::vector<f32>& depth) {
    f64 total = 0.0;
    f64 scale = 0.0;
    for (usize index = 0; index < a.size(); ++index) {
        if (depth[index] <= 0.0F) {
            continue;
        }
        total += static_cast<f64>(cy::length(a[index] - b[index]));
        scale += static_cast<f64>(cy::length(b[index]));
    }
    return scale > 0.0 ? static_cast<f32>(total / scale) : 0.0F;
}

}  // namespace gi_support
