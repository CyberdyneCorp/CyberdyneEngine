#pragma once
// GPU-driven culling: the records a compute pass reads and writes, and the reference that says what
// it must compute. M6 task 8.5.
//
// `rendering-culling-and-lod` — "GPU-driven culling": "Where the device supports compute and
// indirect drawing, the renderer SHALL support GPU-driven culling: instance bounds are uploaded
// once, culling runs as a compute pass producing compacted draw arguments, and drawing uses
// indirect commands." And: "A CPU path SHALL remain for devices lacking the capability and for
// cases needing CPU visibility results (audio occlusion, gameplay queries)."
//
// ================================================================================================
// WHY THIS IS AT LAYER 2, UNDERNEATH THE DEVICE
// ================================================================================================
//
// A compute pass needs a device, and layer 2 may not name one. What is here is everything a compute
// pass needs and a device does not decide: the LAYOUT of the view constants it reads, the layout of
// the draw arguments and counters it writes, and — the reason the file exists rather than being
// three structs — a CPU implementation of exactly the algorithm the shader runs.
//
// That reference is not a fallback bolted on for old hardware, though it serves as one. It is what
// makes the GPU path TESTABLE: `cpu_reference_cull` runs headless in a unit test, over synthetic
// instances, in every profile and on every machine, and the shader is then checked against it by
// comparing outputs rather than by looking at a frame. A renderer whose culling can only be
// verified by looking at a picture is a renderer whose culling is verified by nobody.
//
// It is also the specification's own requirement in two other places: the CPU path must remain "for
// cases needing CPU visibility results", and `rendering-architecture` requires a headless build to
// run the frame. One implementation answers all three.
//
// ================================================================================================
// THE INPUT IS THE GPU SCENE, AND NOTHING ELSE
// ================================================================================================
//
// `virtual-geometry`: "Instance culling SHALL read the GPU scene, so virtual geometry does not
// traverse ECS entities or maintain its own instance list." So the input below is
// `Span<const render::GpuInstance>` — the flat, contiguous array `gpu_scene.h` publishes — plus two
// side tables indexed by the same slot. Nothing here can tell what produced a record, which is the
// property `gpu_scene.h` calls requirement 2 and which this file must not quietly break by adding a
// producer field to its inputs.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT HERE
// ================================================================================================
//
// Cluster-granular culling for virtual geometry. `rendering-culling-and-lod` requires it — "after
// instance culling, hierarchy traversal and per-cluster tests select the geometry actually
// rasterised" — and points at `virtual-geometry`, which is M7. `kInstanceVirtualGeometry` already
// exists in `gpu_scene.h` and this cull routes such an instance into a SEPARATE output list rather
// than emitting an indexed draw for it, so the day cluster traversal lands it consumes a list that
// is already being produced. `GpuCullCounters::virtual_geometry` reads that list's length, and a
// build with no virtual geometry sees it read zero.
//
// The hierarchical depth buffer is `hzb.h`, in this module, because occlusion is a per-instance
// test like the others and the two are run by one dispatch.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/gpu_scene.h>
#include <cy/servers/render/mesh.h>
#include <cy/servers/render/types.h>

#include <cstddef>

namespace cy::render::culling {

// --- The per-mesh draw table --------------------------------------------------------------------

/// Where one level of one mesh lives in the shared index and vertex buffers.
///
/// The cull emits a draw from this and from nothing else, which is what makes the dispatch a pure
/// function of two buffers: an instance's `lod_chain` selects a run of these, its coverage selects
/// one of them, and the record IS the draw's arguments.
///
/// 32 bytes, no padding, every field a `u32`, because a compute shader reads it as a structured
/// buffer of words.
struct alignas(16) GpuMeshLod {
    u32 index_count = 0;
    u32 first_index = 0;
    /// Added to every index before the vertex buffer is addressed. Signed on the API and unsigned
    /// here for the same reason the record is all words: a shader adds it and a negative base is a
    /// case this engine's mesh allocator does not produce.
    u32 vertex_offset = 0;
    u32 material = 0;
    /// The coverage below which the NEXT level is used, matching `render::MeshLod`'s authored
    /// threshold. Descending across a chain.
    f32 screen_coverage_threshold = 0.0F;
    /// The bounding sphere's radius in the mesh's own space, so coverage can be computed without a
    /// second table.
    f32 radius = 0.0F;
    u32 reserved0 = 0;
    u32 reserved1 = 0;
};

static_assert(sizeof(GpuMeshLod) == 32, "GpuMeshLod is a 32-byte shader-visible record");
static_assert(offsetof(GpuMeshLod, index_count) == 0);
static_assert(offsetof(GpuMeshLod, screen_coverage_threshold) == 16);

/// One instance's chain: a run of `GpuMeshLod` records, addressed by `GpuInstance::lod_chain`.
struct GpuLodChain {
    u32 first = 0;
    u32 count = 0;
};

// --- Visibility ranges and HLOD, in the form a dispatch can read --------------------------------

/// `rendering-culling-and-lod` — "Visibility ranges and HLOD", as one 16-byte record per instance.
///
/// The CPU form is `cy::rendering::VisibilityRange` in src/rendering/culling/, which carries a
/// `FadeMode` enum and is read by the CPU cull. This is the same information laid out for a shader:
/// a mode is a small integer and a parent is a slot index, because a compute thread resolving a
/// hierarchy walks slots.
struct alignas(16) GpuVisibilityRange {
    f32 begin = 0.0F;
    /// Zero means no upper bound, so a zeroed record is "always visible" — which is what an
    /// instance that declares nothing gets, and it costs no initialisation.
    f32 end = 0.0F;
    f32 fade_margin = 0.0F;
    /// Bits 0-1 are the fade mode (0 none, 1 self, 2 dependents); the rest are the visibility
    /// parent's slot PLUS ONE, so that zero means "no parent".
    ///
    /// The bias is not a micro-optimisation, it is what makes a zeroed record correct. Storing the
    /// slot directly would make a default-constructed range say "my parent is slot 0", and slot 0
    /// is a real instance — so every instance that declared no hierarchy would be suppressed by
    /// whatever happened to be published first. Encoding it as a sentinel value instead would need
    /// every producer to remember to write it.
    u32 mode_and_parent = 0;
};

static_assert(sizeof(GpuVisibilityRange) == 16, "GpuVisibilityRange is one shader-visible word4");

inline constexpr u32 kNoVisibilityParent = ~0U;

enum class GpuFadeMode : u32 { None = 0, Self = 1, Dependents = 2 };

[[nodiscard]] constexpr u32 pack_visibility_parent(GpuFadeMode mode, u32 parent_slot) noexcept {
    const u32 biased = parent_slot == kNoVisibilityParent ? 0U : parent_slot + 1U;
    return static_cast<u32>(mode) | (biased << 2U);
}
[[nodiscard]] constexpr GpuFadeMode fade_mode_of(u32 packed) noexcept {
    return static_cast<GpuFadeMode>(packed & 3U);
}
[[nodiscard]] constexpr u32 visibility_parent_of(u32 packed) noexcept {
    const u32 biased = packed >> 2U;
    return biased == 0U ? kNoVisibilityParent : biased - 1U;
}

// --- The view the dispatch is run for -----------------------------------------------------------

/// Which tests a dispatch performs. A bit rather than a separate shader, because the alternative is
/// eight permutations of one compute program.
enum GpuCullFlagBits : u32 {
    /// Reject against the hierarchical depth buffer. Off for the first pass of a two-pass scheme
    /// and off entirely on the frame after a camera cut.
    kGpuCullOcclusion = 1U << 0U,
    /// Apply visibility ranges and resolve the HLOD hierarchy.
    kGpuCullVisibilityRanges = 1U << 1U,
    /// This is a shadow view: LOD takes the shadow bias and the caster test replaces the frustum
    /// test's second half.
    kGpuCullShadowCasters = 1U << 2U,
    /// Emit only instances that moved, for a motion-vector or shadow-invalidation pass.
    kGpuCullMovedOnly = 1U << 3U,
};

/// The constant block a culling dispatch reads. One per view per frame.
///
/// 176 bytes. The frustum is six `float4` planes in `Frustum::PlaneIndex` order, which is the order
/// `cy::Frustum` fixes and which the shader must not re-derive.
struct alignas(16) GpuCullView {
    /// `(normal.xyz, distance)`, in the order Left, Right, Bottom, Top, Near, Far.
    f32 planes[Frustum::kCount][4] = {};

    f32 camera_position[3] = {0.0F, 0.0F, 0.0F};
    /// `1 / tan(fov_y / 2)`, precomputed: coverage is `radius * this / depth` and a shader should
    /// not run a tangent per instance.
    f32 inverse_tan_half_fov = 1.0F;

    /// Normalised. Distances are measured along it, so an instance at the edge of a wide frame is
    /// not judged further away than the same instance at the centre.
    f32 camera_forward[3] = {0.0F, 0.0F, -1.0F};
    /// Zero means unlimited. The per-instance limit is applied as well and the smaller wins.
    f32 max_distance = 0.0F;

    /// The direction light travels, for a shadow view's sweep test.
    f32 light_direction[3] = {0.0F, -1.0F, 0.0F};
    /// How far a caster's bounds are swept along `light_direction` when testing whether it can
    /// reach the camera frustum. The cascade's own extent is the right number: further than that
    /// and the shadow falls outside the map anyway.
    ///
    /// ZERO DISABLES THE TIGHTER TEST, and that is a correctness switch rather than a quality one.
    /// "WHERE a light's shadow is rendered for multiple camera views in one frame, the tighter
    /// culling SHALL be disabled for that light so one shadow map is valid for all of them."
    f32 sweep_distance = 0.0F;

    /// The CAMERA frustum the shadows will be seen in, in the same plane order as `planes`. Read
    /// only when `kGpuCullShadowCasters` is set.
    ///
    /// TWO PLANE SETS, AND THE SECOND IS NOT REDUNDANT. For a shadow view `planes` holds the SHADOW
    /// projection's frustum — the cascade's, or the cube face's — and this holds the camera's. The
    /// requirement needs both: a caster is rejected when it is outside the light's volume, and
    /// rejected again when, though inside it, "they cannot cast into the camera frustum, by testing
    /// against the convex volume swept between the light and the camera frustum". One frustum
    /// cannot answer both questions, and an implementation that used the shadow frustum for the
    /// sweep test would be asking whether a caster can cast onto itself.
    f32 camera_planes[Frustum::kCount][4] = {};

    u32 layer_mask = kAllLayers;
    u32 flags = kGpuCullVisibilityRanges;
    /// One past the highest slot the dispatch covers: `GpuScene::high_water()`.
    u32 instance_count = 0;
    /// Positive keeps more detail. The global bias and the view's own, already summed, because a
    /// shader adding two numbers per instance is two numbers it did not need.
    f32 lod_bias = 0.0F;

    /// Fraction of a threshold coverage must fall below before a level is coarsened. Zero disables
    /// hysteresis, which is what a deterministic test wants.
    f32 lod_hysteresis = 0.0F;
    /// Width of the cross-fade band as a fraction of the threshold.
    f32 lod_cross_fade_band = 0.0F;
    /// Orthographic views size coverage from their height instead of their field of view. Zero
    /// selects the perspective formula.
    f32 ortho_height = 0.0F;
    f32 reserved = 0.0F;
};

static_assert(sizeof(GpuCullView) == 272,
              "GpuCullView is a 272-byte shader-visible constant block");
static_assert(offsetof(GpuCullView, camera_position) == 96);
static_assert(offsetof(GpuCullView, camera_planes) == 144);
static_assert(offsetof(GpuCullView, layer_mask) == 240);

/// Fill the plane array and the derived scalars from an engine frustum. One function, because the
/// plane ORDER is a contract with the shader and a caller writing the array by hand would get it
/// right until somebody reordered `Frustum::PlaneIndex`.
void write_frustum(GpuCullView& view, const Frustum& frustum) noexcept;

/// The same for `camera_planes`, which a shadow view fills and a camera view leaves alone.
void write_camera_frustum(GpuCullView& view, const Frustum& frustum) noexcept;

/// Set `inverse_tan_half_fov` from a vertical field of view in radians.
void write_field_of_view(GpuCullView& view, f32 fov_y_radians) noexcept;

// --- What the dispatch writes -------------------------------------------------------------------

/// The indirect draw arguments, in the layout every graphics API's indexed indirect draw expects:
/// five 32-bit words, in this order, at a 20-byte stride.
///
/// It is exactly five words and carries nothing else, and that is the whole point. An engine that
/// appends its own field here discovers on the first indirect draw that the API reads a fixed
/// stride, and the fix is a second buffer — which is `GpuDrawPayload` below, present from the
/// start.
struct GpuDrawIndexedIndirect {
    u32 index_count = 0;
    u32 instance_count = 0;
    u32 first_index = 0;
    u32 vertex_offset = 0;
    u32 first_instance = 0;
};

static_assert(sizeof(GpuDrawIndexedIndirect) == 20,
              "the indirect argument struct is fixed by the graphics APIs, not by this engine");

/// What the draw at the same index is about. Parallel to the argument array, read by the vertex and
/// fragment stages through `first_instance`.
struct alignas(16) GpuDrawPayload {
    /// The GPU scene slot this draw came from.
    u32 instance_slot = 0;
    u32 material = 0;
    u32 lod_level = 0;
    /// The level being faded towards, or `kNoLodFade`.
    u32 lod_fade_to = 0;
    /// 0 at the start of the cross-fade band and 1 at its end. The dither threshold the fragment
    /// shader compares against.
    f32 lod_fade = 0.0F;
    /// The visibility-range alpha, which an HLOD cross-fade drives. 1 for an instance that declares
    /// no range.
    f32 alpha = 1.0F;
    /// Distance along the view direction. What the sort key quantises.
    f32 view_depth = 0.0F;
    f32 coverage = 0.0F;
};

static_assert(sizeof(GpuDrawPayload) == 32, "GpuDrawPayload is a 32-byte shader-visible record");

inline constexpr u32 kNoLodFade = ~0U;

/// `rendering-culling-and-lod` — "Culling diagnostics", as the words a dispatch atomically
/// increments. The CPU reads them back a frame later, which is the documented latency the
/// specification's "the GPU result SHALL be read back with one frame of latency, documented as
/// such" refers to.
struct alignas(16) GpuCullCounters {
    u32 tested = 0;
    u32 rejected_by_layer = 0;
    u32 rejected_by_frustum = 0;
    u32 rejected_by_occlusion = 0;
    u32 rejected_by_range = 0;
    u32 visible = 0;
    /// Draws emitted. Equal to `visible` today; they part company when one instance emits a draw
    /// per section.
    u32 draws = 0;
    /// Instances routed to cluster traversal instead of an indexed draw. Reads zero until
    /// `virtual-geometry` lands at M7.
    u32 virtual_geometry = 0;
    /// Survivors at each level. Index 7 accumulates every level past 6, which is a bucket rather
    /// than a lost number.
    u32 lod_histogram[8] = {};
};

static_assert(sizeof(GpuCullCounters) == 64, "GpuCullCounters is one shader-visible cache line");

/// The buffers one view's dispatch fills.
///
/// Sized once for the scene's high water mark and reused: the point of GPU-driven culling is that
/// the CPU does no per-instance work, and an output that reallocated per frame would put the
/// allocator back on the frame path.
class GpuCullOutput {
public:
    explicit GpuCullOutput(Allocator& allocator) noexcept;

    GpuCullOutput(const GpuCullOutput&) = delete;
    GpuCullOutput& operator=(const GpuCullOutput&) = delete;

    /// Make room for `capacity` draws. Idempotent and cheap when the capacity has not shrunk.
    [[nodiscard]] Status reserve(u32 capacity) noexcept;

    /// Drop the draws and zero the counters, keeping the memory.
    void clear() noexcept;

    [[nodiscard]] Span<const GpuDrawIndexedIndirect> commands() const noexcept;
    [[nodiscard]] Span<const GpuDrawPayload> payloads() const noexcept;
    /// Slots of instances that are virtual geometry, for M7's cluster traversal.
    [[nodiscard]] Span<const u32> virtual_geometry() const noexcept;
    [[nodiscard]] const GpuCullCounters& counters() const noexcept { return counters_; }

    /// Append one draw. Fails with `OutOfRange` past the reserved capacity rather than growing,
    /// because on the GPU this buffer cannot grow and a reference that could would hide the day the
    /// reservation became too small.
    [[nodiscard]] Status emit(const GpuDrawIndexedIndirect& command,
                              const GpuDrawPayload& payload) noexcept;
    [[nodiscard]] Status emit_virtual_geometry(u32 instance_slot) noexcept;

    [[nodiscard]] GpuCullCounters& counters() noexcept { return counters_; }

private:
    Array<GpuDrawIndexedIndirect> commands_;
    Array<GpuDrawPayload> payloads_;
    Array<u32> virtual_geometry_;
    GpuCullCounters counters_{};
    u32 capacity_ = 0;
};

// --- The reference implementation ---------------------------------------------------------------

/// Everything a dispatch reads besides the view.
///
/// Every span is indexed by GPU scene slot except `mesh_lods`, which the chains index into. An
/// empty `ranges` span means no instance declares a visibility range, which is the common case and
/// costs nothing to express.
struct GpuCullScene {
    Span<const GpuInstance> instances;
    Span<const GpuLodChain> chains;
    Span<const GpuMeshLod> mesh_lods;
    Span<const GpuVisibilityRange> ranges;
    /// The level each slot was drawn at last frame, read and written, for LOD hysteresis. Empty
    /// disables hysteresis whatever the view says — which is what a deterministic test wants and
    /// what a first frame has.
    Span<u32> previous_levels;
};

/// Something that can answer "are these bounds hidden". `hzb.h` implements it; a null pointer in
/// `GpuCullOptions` means occlusion culling is off however the view's flag is set.
class OcclusionTester {
public:
    virtual ~OcclusionTester() = default;

    /// True when the sphere is certainly behind what has already been drawn. MUST be conservative:
    /// a false positive here is geometry missing from the frame.
    [[nodiscard]] virtual bool occluded(Vec3 centre, f32 radius) const noexcept = 0;

protected:
    OcclusionTester() = default;
    OcclusionTester(const OcclusionTester&) = default;
    OcclusionTester& operator=(const OcclusionTester&) = default;
};

struct GpuCullOptions {
    /// Null disables the occlusion test whatever `kGpuCullOcclusion` says.
    const OcclusionTester* occlusion = nullptr;
};

/// Run one view's cull on the CPU, computing exactly what the compute pass must.
///
/// THE ORDER OF THE TESTS IS THE REQUIREMENT. `rendering-culling-and-lod`: "Culling SHALL reject
/// instances by, in order: layer mask against the view's mask, then conservative
/// frustum-versus-AABB using precomputed plane sign masks, then optional per-instance distance
/// limits." A layer test is one AND over a word already loaded; a frustum test is six dot products.
/// Reversing them would be correct and would cost six times as much on the instances a view does
/// not draw.
///
/// Deterministic: draws are emitted in ASCENDING SLOT ORDER, so two runs over one scene produce one
/// buffer. A GPU dispatch's atomics do not guarantee that, which is why the sort key exists — but
/// the reference must be reproducible or it cannot be a test's expected value.
///
/// `output` is cleared first, so a caller that reuses one object across views cannot accumulate two
/// views into one buffer.
[[nodiscard]] Status cpu_reference_cull(const GpuCullScene& scene, const GpuCullView& view,
                                        const GpuCullOptions& options,
                                        GpuCullOutput& output) noexcept;

/// The fraction of the viewport height a bounding sphere subtends, as the dispatch computes it.
///
/// A non-positive depth answers 1: an object the camera is inside covers the screen, and answering
/// 0 there would drop it to its coarsest level exactly when it fills the frame.
[[nodiscard]] f32 screen_coverage(f32 radius, f32 view_depth, f32 inverse_tan_half_fov) noexcept;

/// How visible one instance is at a distance, ignoring its hierarchy: 0 fully faded out, 1 fully
/// in.
[[nodiscard]] f32 visibility_range_alpha(const GpuVisibilityRange& range, f32 distance) noexcept;

}  // namespace cy::render::culling
