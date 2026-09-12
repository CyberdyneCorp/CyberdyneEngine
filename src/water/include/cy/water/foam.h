#pragma once
// Foam: a persistent ADVECTED field, bounded in memory, that decays. M10 task 2.3.
//
// `water` — "Foam": "Foam SHALL be a persistent advected field rather than an instantaneous
// function of the surface: generated from wave curvature and breaking, shoreline interaction, river
// turbulence, waterfalls, and object interaction; advected by the flow or wave velocity; and
// decaying over a declared lifetime. Foam SHALL be bounded in memory and resolution, scoped to
// regions near streaming sources."
//
// ================================================================================================
// WHY PERSISTENT IS THE WHOLE REQUIREMENT
// ================================================================================================
//
// A foam term computed from the surface's own curvature disappears the moment the boat that made it
// moves on, because the curvature it was reading moved with the boat. The specification's scenario
// is exactly that: "its wake foam SHALL persist behind it, drift with the surface, and decay,
// rather than disappearing when the boat moves on." So foam is STATE, and the three things state
// needs are here: something writes into it (`deposit()`), something moves it (`advect()`), and
// something forgets it (the decay inside `advect()`).
//
// ================================================================================================
// BOUNDED, AND BOUNDED BY CONSTRUCTION
// ================================================================================================
//
// The field is a fixed grid of `resolution x resolution` cells of `cell_metres`, centred on a focus
// — the camera, or a streaming source. `bytes()` is a function of the resolution alone and of
// nothing else: not of how long the session has run, not of how many wakes have been cut across it,
// not of how large the world is. Moving the focus SHIFTS the grid and drops what falls off the
// edge, which is what "scoped to regions near streaming sources" means when it is implemented
// rather than asserted.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/world/coordinates.h>

namespace cy::water {

/// How much foam there is, how finely, and for how long.
struct FoamParams {
    /// Cells per side. The grid is square; `bytes()` is `resolution^2 * 4` and nothing else.
    u32 resolution = 128;
    /// Metres per cell. 128 cells at 0.5 m covers 64 m, which is a harbour or a boat's wake.
    f32 cell_metres = 0.5F;
    /// How long foam survives with nothing sustaining it, seconds. The decay is exponential with
    /// this as its time constant, so a cell is at 1/e after one lifetime and visually gone by
    /// three — which is what a declared lifetime means for a quantity that fades.
    f32 lifetime_seconds = 6.0F;
};

/// Where foam is coming from this frame, and how fast it is moving.
struct FoamDeposit {
    world::WorldVec3d position;
    /// Coverage added per second, in [0, 1] before clamping.
    f32 rate = 1.0F;
    /// The radius over which it is laid down, metres.
    f32 radius = 1.0F;
};

/// The persistent field.
class FoamField {
public:
    explicit FoamField(Allocator& allocator) noexcept;

    FoamField(const FoamField&) = delete;
    FoamField& operator=(const FoamField&) = delete;

    /// Allocate the grid. Refuses a zero resolution, a non-positive cell size or a non-positive
    /// lifetime. Re-configuring clears the field, because the cells would otherwise mean a
    /// different amount of world than the coverage in them was deposited over.
    [[nodiscard]] Status configure(const FoamParams& params) noexcept;

    /// Move the grid so it is centred on `focus`. Coverage is SHIFTED by whole cells and what falls
    /// off the edge is dropped; a sub-cell move does nothing, which is what keeps a moving camera
    /// from smearing the field one fraction of a cell per frame.
    [[nodiscard]] Status recentre(const world::WorldVec3d& focus) noexcept;

    /// Lay foam down. Positions outside the grid are ignored rather than refused: a source at the
    /// far end of a river is not an error, it is simply not near the focus.
    [[nodiscard]] Status deposit(const FoamDeposit& deposit, f32 seconds) noexcept;

    /// Advect by a velocity field and decay. `velocity_at` is the water's velocity at a position —
    /// the wave orbital velocity offshore and the river's flow in a channel — supplied as a
    /// callback so this module needs no reference to the system that answers it.
    using VelocitySource = Vec3 (*)(void* user, const world::WorldVec3d& at) noexcept;
    [[nodiscard]] Status advect(f32 seconds, VelocitySource velocity_at, void* user) noexcept;

    /// Coverage in [0, 1] at a position, bilinear. Zero outside the grid.
    [[nodiscard]] f32 sample(const world::WorldVec3d& at) const noexcept;

    [[nodiscard]] u64 bytes() const noexcept;
    [[nodiscard]] u32 resolution() const noexcept { return params_.resolution; }
    [[nodiscard]] const world::WorldVec3d& centre() const noexcept { return centre_; }
    /// Total coverage over the grid. The number a test watches rise behind a boat and fall after.
    [[nodiscard]] f32 total_coverage() const noexcept;

private:
    [[nodiscard]] f32* cell(i32 x, i32 z) noexcept;
    [[nodiscard]] const f32* cell(i32 x, i32 z) const noexcept;
    /// Grid coordinates of a world position, as continuous values. Out of range is legal and the
    /// caller checks; returning a flag here would put the bound in two places.
    void grid_of(const world::WorldVec3d& at, f32& x, f32& z) const noexcept;

    Allocator* allocator_;
    FoamParams params_;
    Array<f32> coverage_;
    Array<f32> scratch_;
    world::WorldVec3d centre_;
    /// The grid's origin in whole cells, so `recentre()` shifts by an integer and the field does
    /// not creep by a fraction of a cell per frame.
    i64 origin_cell_x_ = 0;
    i64 origin_cell_z_ = 0;
    bool configured_ = false;
};

}  // namespace cy::water
