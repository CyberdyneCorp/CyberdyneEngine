#pragma once
// The stage: a graphics device, one pipeline, four draws and the PNG that comes back.
// M10, samples/10-world.
//
// ================================================================================================
// WHAT IS DRAWN, AND BY WHAT
// ================================================================================================
//
// Four draws per frame through ONE graphics pipeline, in this order:
//
//   the sky      a dome of triangles around the eye, each vertex carrying the radiance
//                `rendering::sky::compose_sky()` answered for its direction — the engine's own
//                atmosphere, its own multiple-scattering table, its own volumetric cloud march and
//                its own stars, evaluated on the processor once per dome vertex.
//   the terrain  `terrain::mesh_tile()`'s output for every level-0 tile. Positions and normals are
//                uploaded ONCE; the colours are re-uploaded every frame, because they are the
//                environment substrate re-sampled and that is what makes a snowfall visible.
//   the water    `water::OceanSurface::build()`'s camera-relative patch, regenerated every frame
//                from the spectral model weather's wind drives.
//   the foliage  one procedural proxy per plant: a three-sided trunk and an eight-sided crown, with
//                the crown displaced by `foliage::evaluate_response()`'s answer for a vertex at the
//                top of the canopy.
//
// ================================================================================================
// WHAT IT DOES NOT CLAIM, AND THE LIST IS LONG ON PURPOSE
// ================================================================================================
//
// This is NOT `rendering::pipeline`'s forward frame, NOT the visibility buffer, NOT virtual
// geometry, and NOT the material system. No terrain material page is bound, no environment field is
// sampled through `environment::build_field_image()`'s GPU layout, no water surface is published
// into the GPU scene, and no grass blade is expanded on the device. Every one of those is a gap
// M10's own module READMEs record against their own rows, and an artefact that implied otherwise
// would be the false green this milestone's brief forbids.
//
// The foliage proxies in particular are THIS FILE'S geometry and not an asset: `foliage` stores 16
// bytes an instance and names a species, and what a species' mesh looks like is the project's. A
// cone and a prism are the least this artefact can draw that still shows a forest reacting to wind.
//
// ================================================================================================
// THE DEVICE IS OPTIONAL
// ================================================================================================
//
// `open()` reports an ABSENT DEVICE as success with `available()` false rather than as an error, so
// a machine with no graphics device says what it is missing and the program still runs the whole
// world headless and prints its report. That is the same arrangement samples/07-fidelity,
// samples/08-vertical-slice and samples/09b-animated-character use.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>

#include "world.h"

namespace cy::sample::world {

/// One vertex of the geometry stream: position and normal, in world-RELATIVE metres.
///
/// Relative to `World::centre()`, because the world is a kilometre and a half across and an f32
/// position is exact to well under a millimetre over that — while an absolute one a hundred
/// kilometres out would not be. It is the same argument `world::WorldPosition` makes for entities
/// and `terrain::TerrainMesh` makes for its own tile-local positions.
struct Vertex {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
};

static_assert(sizeof(Vertex) == 24, "binding 0's stride is declared as 24 in create_pipeline()");

/// What one frame cost and produced, measured rather than claimed.
struct StageReport {
    /// Vulkan validation errors seen since the device was created. Any non-zero number is a defect
    /// and the artefact prints it rather than burying it in a log.
    u32 validation_errors = 0;
    u32 sky_triangles = 0;
    u32 terrain_triangles = 0;
    u32 water_triangles = 0;
    u32 foliage_triangles = 0;
    u32 plants_drawn = 0;
    u32 stars_drawn = 0;
    /// Milliseconds spent turning the world's state into vertex buffers, on the processor. Part of
    /// the frame-budget curve, and separated from the world's own producers because it is this
    /// artefact's cost and not the engine's.
    f64 build_ms = 0.0;
    /// Milliseconds spent inside `execute()` and the wait for the device.
    f64 submit_ms = 0.0;
};

/// The device, the pipeline, the buffers and the picture.
class Stage {
public:
    explicit Stage(Allocator& allocator) noexcept;
    ~Stage();

    Stage(const Stage&) = delete;
    Stage& operator=(const Stage&) = delete;

    [[nodiscard]] Status open(u32 width, u32 height) noexcept;
    [[nodiscard]] bool available() const noexcept { return available_; }
    /// Why no device answered, for the message a machine without one prints.
    [[nodiscard]] const char* absence() const noexcept;

    /// Upload everything that never changes: the terrain's positions and normals, and the sky
    /// dome's index list. Once, not per frame.
    [[nodiscard]] Status stage_world(const World& world) noexcept;

    /// Draw one frame of `world` from `eye` looking at `target`, writing the image to `png_path`
    /// when that is not null.
    [[nodiscard]] Status shoot(const World& world, const WorldVec3d& eye, const WorldVec3d& target,
                               const char* png_path, StageReport& out) noexcept;

    void close() noexcept;

private:
    struct Device;

    [[nodiscard]] Status create_pipeline() noexcept;
    [[nodiscard]] Status write_png(const char* path) noexcept;
    /// Refill the per-frame streams: the sky dome's vertices, the water patch and the foliage
    /// proxies. Everything here is CPU work over what `World::advance()` produced.
    [[nodiscard]] Status build_dynamic(const World& world, const WorldVec3d& eye,
                                       StageReport& out) noexcept;
    [[nodiscard]] Status upload_dynamic() noexcept;

    Allocator* allocator_;
    Device* device_ = nullptr;
    u32 width_ = 0;
    u32 height_ = 0;
    bool available_ = false;

    /// The static half: terrain geometry, uploaded once.
    u32 terrain_vertices_ = 0;
    u32 terrain_indices_ = 0;

    /// The dynamic half, rebuilt every frame. Held as members so the arrays keep their capacity
    /// between frames and the per-frame cost is a memcpy rather than an allocation.
    Array<Vertex> dynamic_vertices_;
    Array<Vec3> dynamic_colours_;
    Array<u32> dynamic_indices_;
    Array<Vec3> terrain_colours_;
    /// Where each draw's run begins in the dynamic buffers.
    u32 sky_first_index_ = 0;
    u32 sky_index_count_ = 0;
    u32 water_first_index_ = 0;
    u32 water_index_count_ = 0;
    u32 foliage_first_index_ = 0;
    u32 foliage_index_count_ = 0;
    u32 star_first_index_ = 0;
    u32 star_index_count_ = 0;

    Array<u32> pixels_;
};

}  // namespace cy::sample::world
