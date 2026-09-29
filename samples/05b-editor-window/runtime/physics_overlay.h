// SPDX-License-Identifier: MIT
#pragma once
// Physics debug layers and joint gizmos, drawn into the frame the engine publishes. Issue #29.
//
// ================================================================================================
// THE ENGINE DRAWS THEM, FROM ITS OWN PHYSICS WORLD
// ================================================================================================
//
// `editor-viewport-and-gizmos` forbids a second renderer in the editor, and `physics` already has
// the debug visualisation: `PhysicsServer::debug_draw` writes colliders, contacts, constraints,
// sleep state, velocities, centres of mass and broad-phase bounds into a `DebugDrawSink` the caller
// implements. This file is that caller for the editor's viewport: `FrameDebugSink` projects every
// primitive through the view the frame was rendered with and rasterises it into the same pixels
// the gizmo is composited into (overlay.h says why in software). The editor only asks, with the
// `physics_overlays` bits of its gizmo intent.
//
// Authored joints are drawn through the same sink and `cy::physics::debug_draw_constraint`, the
// function the Jolt backend draws a simulated constraint with, so a joint looks the same selected
// in the editor and running in play.
//
// Like the gizmo, the overlay is not depth-tested against the scene: a collider behind a wall is
// drawn over it, which is what a debug view of colliders is for.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/debug.h>
#include <cy/servers/physics/server.h>
#include <cy/servers/render/model.h>

#include "overlay.h"

namespace cy::sample::editor_window {

/// The colour a physics primitive is drawn in, as 0xRRGGBB from the editor's dark palette: static
/// bodies in secondary grey, kinematic in the active blue, awake dynamic bodies in the live green,
/// sleeping ones dimmed, contacts in the error red and joint limits in the attention gold.
[[nodiscard]] u32 physics_colour(physics::DebugColor color) noexcept;

/// A `DebugDrawSink` that draws into one frame.
///
/// Positions are world space; `eye` is the camera position the frame was rendered from, because
/// `render::View` is camera-relative. A segment with an end behind the camera is skipped rather
/// than clipped: this is a diagnostic, and a line that folded through the camera plane would be
/// worse than a missing one.
class FrameDebugSink final : public physics::DebugDrawSink {
public:
    FrameDebugSink(const Canvas& canvas, const render::View& view, Vec3 eye) noexcept
        : canvas_(canvas), view_(view), eye_(eye) {}

    void line(Vec3 from, Vec3 to, physics::DebugColor color) noexcept override;
    void contact(Vec3 position, Vec3 normal, f32 penetration) noexcept override;

    /// How many segments reached the frame, for a test and for the runtime's report.
    [[nodiscard]] u32 drawn() const noexcept { return drawn_; }

private:
    Canvas canvas_;
    const render::View& view_;
    Vec3 eye_;
    u32 drawn_ = 0;
};

/// Draw the physics layers `flags` asks for (the bits of `cy::physics::DebugDrawFlags`) from one
/// physics world. Zero draws nothing and asks the server for nothing.
[[nodiscard]] Status draw_physics_overlays(FrameDebugSink& sink,
                                           const physics::PhysicsServer& server,
                                           physics::WorldHandle world, u32 flags) noexcept;

/// Draw the joint authored on the node `identity` names, between where its two bodies are authored:
/// both anchors, the line between them, and the kind's limits about anchor A. Answers whether the
/// node carries a joint that could be drawn.
bool draw_authored_joint(FrameDebugSink& sink, const scene::serialization::World& world,
                         u64 identity) noexcept;

}  // namespace cy::sample::editor_window
