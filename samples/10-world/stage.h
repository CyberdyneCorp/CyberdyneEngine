#pragma once
// The stage: a graphics device, the ENGINE'S OWN ASSEMBLED FRAME, four draws and the PNG that comes
// back. M10, samples/10-world; the frame is M11.c task 3.1.
//
// ================================================================================================
// WHAT IS DRAWN, AND BY WHAT
// ================================================================================================
//
// Five runs per frame through ONE graphics pipeline of this file's own, recorded into the OPAQUE
// stage of `cy::rendering::assembly::FrameAssembly`'s frame, in this order:
//
//   the sky      a dome of triangles around the eye. A compute pass shades its vertices on the
//                device from the current sun and cloud state before this draw reads them.
//   the terrain  `terrain::mesh_tile()`'s output for every level-0 tile. Positions and normals are
//                uploaded once; a compute pass samples four packed field images through
//                `cy/field.slang` and writes its device-resident colour stream.
//   the water    `water::OceanSurface::build()`'s camera-relative patch, regenerated every frame
//                from the spectral model weather's wind drives. A ping-pong compute pass evolves
//                visual foam and writes the water colours.
//   the foliage  one procedural proxy per plant: a three-sided trunk and an eight-sided crown, with
//                the crown displaced by `foliage::evaluate_response()`'s answer for a vertex at the
//                top of the canopy.
//
// ================================================================================================
// WHAT THE FRAME IS, AFTER M11.c — AND THE SENTENCE THIS REPLACES
// ================================================================================================
//
// Until M11.c this file said "this is NOT `rendering::pipeline`'s forward frame", and it was true:
// the picture was two passes this file declared into a render graph, with no post chain, no
// exposure and no tone mapping anywhere in it. M11.c's ledger names that as the defect — "the world
// picture never passes through tone mapping or anti-aliasing at all" — and `Stage::create_frame` is
// what closes it.
//
// WHAT IS THE ENGINE'S NOW. `FrameAssembly` decides the frame's feature set from the post chain,
// advances the temporal framework's jitter (PINNED, so the still is reproducible), asks the shadow
// cache for the sun's pages, updates the sky table, and declares the frame's stages into the graph
// in the specification's order. `cy::rendering-pipeline`'s tonemapping resolve — `cy/fullscreen.
// slang`'s own — turns the linear HDR scene colour into the 8-bit image that is written out, at the
// exposure `samples/10-world/frame.cypost` commits. The stage list the frame ran is published
// beside the still by `write_manifest`.
//
// ================================================================================================
// WHAT IT STILL DOES NOT CLAIM, AND THE LIST IS LONG ON PURPOSE
// ================================================================================================
//
// **The geometry is this file's and not the renderer's.** The world is not in a mesh table, its
// vertices are not the render server's three streams, and its shading is still per-vertex colour.
// The terrain, sky, and water colours are now produced by device passes. It is drawn INSIDE the
// engine's frame through `FrameSinks::passes`,
// which is the seam `ForwardFrame` documents — "ForwardFrame knows the frame STRUCTURE and the
// caller knows how to draw" — and not through the pipeline layer's own draw path.
//
// **There is no anti-aliasing in this program.** The engine's temporal resolve is recorded by
// `cy::rendering-pipeline`'s `FrameRecorder`, but this program draws its own geometry through its
// own sinks, with no jitter, no velocity output and no depth prepass, so nothing here could feed
// it. Turning `temporal_antialiasing` on here would put a stage in the manifest that no pass of
// this frame ran, which is precisely the dishonesty `cy/rendering/assembly/capture_manifest.h`
// exists to detect. The chain this frame runs is the three unconditional stages and the manifest
// says three.
//
// **No visibility buffer, no virtual geometry, no material system.** No terrain material page is
// bound, no water surface is published into the GPU scene, and no grass blade is expanded on the
// device. The sample directly binds packed field images rather than the GPU scene's bindless table.
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
#include <cy/rendering/assembly/capture_manifest.h>

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
    /// RHI validation errors seen since the device was created. Any non-zero number is a defect
    /// and the artefact prints it rather than burying it in a log.
    u32 validation_errors = 0;
    u32 terrain_dispatches = 0;
    u32 cloud_dispatches = 0;
    u32 foam_dispatches = 0;
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

    // --- WHAT THE ASSEMBLED FRAME DID. M11.c task 3.1. -----------------------------------------
    //
    // Read off `AssemblyReport` rather than off this file's own intentions, which is the whole
    // point of the change: before M11.c this program linked no part of `src/rendering/` but the
    // graph and the sky, so there was no frame to report on and every number about the picture was
    // this file's own claim.

    /// Passes the assembled frame declared, and post stages it ran.
    u32 frame_passes = 0;
    u32 post_stages = 0;
    /// The manifest the frame emitted, as it would be published beside the still. Empty until a
    /// frame has executed.
    cy::rendering::assembly::CaptureManifest manifest;
    bool manifest_valid = false;
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
    [[nodiscard]] const char* backend_name() const noexcept;
    [[nodiscard]] const char* device_name() const noexcept;

    /// Upload everything that never changes: the terrain's positions and normals, and the sky
    /// dome's index list. Once, not per frame.
    [[nodiscard]] Status stage_world(const World& world) noexcept;

    /// Draw one frame of `world` from `eye` looking at `target`, writing the image to `png_path`
    /// when that is not null.
    [[nodiscard]] Status shoot(const World& world, const WorldVec3d& eye, const WorldVec3d& target,
                               const char* png_path, StageReport& out) noexcept;
    /// Compare the staged terrain compute result with `World`'s CPU reference after a warmup
    /// dispatch. Refuses missing vertices and any channel outside the declared tolerance.
    [[nodiscard]] Status verify_terrain_agreement(const World& world, f32& worst_error,
                                                  u32& compared) const noexcept;
    [[nodiscard]] Status verify_cloud_agreement(const World& world, f32& worst_error,
                                                u32& compared) const noexcept;
    [[nodiscard]] Status verify_foam_output(u32& active_cells) const noexcept;

    /// Write the frame's own stage list beside a still. `rendering-post-processing`: "a published
    /// capture SHALL be accompanied by that stage list, and where a caption states that a stage
    /// ran, the statement SHALL be checkable against it". Refuses a report with no manifest in it,
    /// which is a frame that never executed.
    [[nodiscard]] static Status write_manifest(const StageReport& report,
                                               const char* path) noexcept;

    /// The exposure this shot is graded at, in stops, as `samples/10-world/frame.cypost` committed
    /// it. M11.c task 3.3: the grade is content, not a constant in a sample's `main`.
    [[nodiscard]] f32 exposure_stops() const noexcept { return exposure_stops_; }
    /// Read the committed grade. Called before `open`; a missing file is an error rather than a
    /// silent default, because a shot tuned against a file nobody read is an untuned shot.
    [[nodiscard]] Status read_grade(const char* path) noexcept;

    /// The workers the per-frame plant proxies are written on — the world's own job system. Null,
    /// the default, writes them on the calling thread; the streams hold the same bits either way.
    ///
    /// LENT TO THE FRAME ASSEMBLY TOO, once the stage is open. Its sky view table and ambient
    /// irradiance are rebuilt every frame this camera and this sun move, which in a day compressed
    /// into sixty-four frames is every frame, and on one thread that was over half of
    /// `stage_submit_ms`: measured on the RTX 5060 host, 4.7 ms of a 7.5 ms band at a daytime
    /// sun, 3.0 ms of it the irradiance alone. Same bits either way, for the same reason.
    void set_jobs(jobs::JobSystem* jobs) noexcept;

    void close() noexcept;

private:
    struct Device;

    [[nodiscard]] Status create_pipeline() noexcept;
    [[nodiscard]] Status create_visual_pipelines() noexcept;
    /// Build the assembled frame: the assembly, the pipeline layer and the image the resolve
    /// writes. M11.c task 3.1.
    [[nodiscard]] Status create_frame() noexcept;
    [[nodiscard]] Status write_png(const char* path) noexcept;
    /// Refill geometry and CPU-authored foliage streams. Device passes replace the terrain, sky,
    /// and water colour ranges before the opaque draw consumes them.
    [[nodiscard]] Status build_dynamic(const World& world, const WorldVec3d& eye,
                                       StageReport& out) noexcept;
    [[nodiscard]] Status upload_dynamic(const World& world, f32& field_origin_x,
                                        f32& field_origin_z) noexcept;

    Allocator* allocator_;
    Device* device_ = nullptr;
    f32 exposure_stops_ = 0.0F;
    f32 grade_contrast_ = 1.0F;
    f32 grade_saturation_ = 1.0F;
    u32 width_ = 0;
    u32 height_ = 0;
    bool available_ = false;

    /// The static half: terrain geometry, uploaded once.
    u32 terrain_vertices_ = 0;
    u32 terrain_indices_ = 0;
    u32 dynamic_capacity_ = 0;
    u32 dynamic_index_capacity_ = 0;
    u32 sky_vertices_ = 0;
    u32 water_first_vertex_ = 0;
    u32 water_vertices_ = 0;
    u32 visual_frame_index_ = 0;
    u64 field_image_bytes_[4] = {};

    /// The dynamic half's sky, stars and sea, rebuilt every frame and copied to the front of the
    /// dynamic buffers. Held as members so the arrays keep their capacity between frames. The
    /// plant proxies after them are written straight into the mapped buffers; see `build_dynamic`.
    Array<Vertex> dynamic_vertices_;
    Array<Vec3> dynamic_colours_;
    Array<u32> dynamic_indices_;
    /// This frame's chosen plants, as indices into `World::plants()`, in the order they are drawn.
    Array<u32> drawn_plants_;
    jobs::JobSystem* jobs_ = nullptr;
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
