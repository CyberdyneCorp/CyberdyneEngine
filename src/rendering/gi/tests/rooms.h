// SPDX-License-Identifier: MIT
#pragma once
// The lit room the classification, media, diagnostics and debug-view suites share: support.h's
// room as a field, its cards through the composed system's own ingestion, and its light — with,
// optionally, the divider as solid geometry the field can see.

#include <cy/core/math/matrix.h>
#include <cy/rendering/gi/system.h>
#include <cy/test/test.h>

#include "support.h"

#include <vector>

namespace gi_rooms {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;

/// The divider's two solid halves either side of the doorway, half a metre thick.
inline constexpr f32 kDividerHalfThickness = 0.25F;

struct LitRoom {
    gi_support::RoomField room{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    gi_support::BoxField half_wall{Vec3{1.5F, gi_support::kRoomY + 0.5F, kDividerHalfThickness}};
    std::vector<cy::rendering::gi::Surfel> surfels;
    std::vector<cy::rendering::gi::GiLight> lights = gi_support::room_lights();
    cy::rendering::gi::IlluminationSystem system;
    bool divider = false;

    explicit LitRoom(bool with_divider = false,
                     cy::rendering::gi::IlluminationSettings settings = gi_support::room_settings())
        : surfels(gi_support::room_surfels(1.0F, with_divider)), divider(with_divider) {
        CY_REQUIRE(system.configure(settings).has_value());
        CY_REQUIRE(system.field().place(1, room.asset(), cy::Mat4::identity()).has_value());
        if (with_divider) {
            CY_REQUIRE(system.field()
                           .place(2, half_wall.asset(),
                                  cy::Mat4::from_translation(Vec3{-2.5F, 0.0F, 0.0F}))
                           .has_value());
            CY_REQUIRE(
                system.field()
                    .place(3, half_wall.asset(), cy::Mat4::from_translation(Vec3{2.5F, 0.0F, 0.0F}))
                    .has_value());
        }
        CY_REQUIRE(
            system.ingest_cell(1, bounds(), {surfels.data(), surfels.size()}, 0).has_value());
        system.surfaces().set_lookup_radius(1.2F);
    }

    [[nodiscard]] static cy::Aabb bounds() noexcept {
        return cy::Aabb::from_center_extents(
            Vec3{0.0F, 0.0F, 0.0F},
            Vec3{gi_support::kRoomX + 1.0F, gi_support::kRoomY + 1.0F, gi_support::kRoomZ + 1.0F});
    }

    [[nodiscard]] cy::rendering::gi::FrameContext context(u64 frame) const noexcept {
        cy::rendering::gi::FrameContext ctx;
        ctx.camera = Vec3{0.0F, 0.0F, 0.0F};
        ctx.lights = {lights.data(), lights.size()};
        ctx.frame = frame;
        ctx.measured_gi_ms = 1.0F;
        return ctx;
    }

    /// Converged mode from frame `first`, which every golden and every reference comparison here
    /// starts from. Returns the next frame number.
    u64 converge(u64 first = 1, u32 frames = 12) {
        (void)system.advance_to_convergence(context(first), 0.99F, frames);
        return first + frames;
    }
};

}  // namespace gi_rooms
