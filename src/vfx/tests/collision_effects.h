// SPDX-License-Identifier: MIT
#pragma once
// The colliding effects every collision suite runs: authored once, so the CPU suite, the device
// suite and the pictures disagree about nothing but the path. `integration.vfx`,
// `integration.vfx_compiler`, `render.vfx_gpu`.
//
// One effect, configured: particles spawned at a point (with a spread), integrated under gravity,
// and handed to one `vfx.collide.<shape>` node whose outputs are written back — position, velocity,
// and the contact flag the next step reads as `was_in_contact`. Everything a requirement scenario
// varies is a field of `DropScenario`, so each case states its scenario as data.

#include "effects.h"

#include <cy/core/math/vec.h>

namespace cy::vfx_test {

struct DropScenario {
    /// `plane`, `sphere`, `box`, `capsule`, `sdf` or `depth`.
    const char* shape = "plane";
    /// The shape's own inputs, as `ColliderShape` in ir.h packs them: plane `(normal, offset)`,
    /// sphere `(centre, radius)`, box `(centre, -)` and `(half extents, -)`, capsule `(start,
    /// radius)` and `(end, -)`, sdf `(step)`, depth `(thickness)`. Set on the node as values.
    f32 a[4] = {0.0F, 1.0F, 0.0F, 0.0F};
    f32 b[4] = {0.5F, 0.5F, 0.5F, 0.0F};
    f32 radius = 0.0F;
    f32 restitution = 0.5F;
    f32 friction = 0.0F;
    f32 stick = 0.0F;
    f32 rest_speed = 0.0F;
    Vec3 start{0.0F, 2.0F, 0.0F};
    Vec3 spread{0.0F, 0.0F, 0.0F};
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    Vec3 velocity_spread{0.0F, 0.0F, 0.0F};
    Vec3 gravity{0.0F, -9.81F, 0.0F};
    /// Particles asked for each step while the emitter is younger than `spawn_until` seconds.
    u32 spawn_per_step = 64;
    f32 spawn_until = 0.02F;
    u32 capacity = 64;
    /// Seconds a particle lives.
    f32 lifetime = 1000.0F;
    /// `kill_if(contact)`: the particle dies where it touches.
    bool kill_on_contact = false;
    /// `emit_event("impact", impact)` with the resolved position as the payload.
    bool raise_impacts = false;
    /// A second emitter, `sparks`, spawned one particle an `impact` event at its position.
    bool sparks = false;
    /// The integration is gated by the contact flag, so a stuck particle stays where it stuck.
    bool hold_when_stuck = false;
    /// Muting the collider: the graph keeps the node and the node does nothing.
    bool mute_collider = false;
    SimulationPath path = SimulationPath::GpuPreferred;
    /// Sprite presentation, for the pictures: size and a radiance.
    f32 size = 0.05F;
    Vec3 tint{1.0F, 0.5F, 0.15F};
    f32 emission = 6000.0F;
};

/// The `impact` channel's per-frame bound.
inline constexpr u32 kImpactEventsPerFrame = 1024;

[[nodiscard]] Expected<vfx::VfxSystemAsset, Error> build_drop(
    Allocator& allocator, const DropScenario& scenario) noexcept;

/// Cook `build_drop` with the shipping options.
[[nodiscard]] Expected<vfx::CompiledSystem, Error> cook_drop(Allocator& allocator,
                                                             const DropScenario& scenario,
                                                             graph::DiagnosticSink& sink,
                                                             vfx::CompileReport& report) noexcept;

}  // namespace cy::vfx_test
