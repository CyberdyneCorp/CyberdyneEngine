#pragma once
// THE FRAME, ASSEMBLED OUT OF THE RENDERER'S OWN PARTS. M8.b task 11.2.
//
// ================================================================================================
// THE DEFECT THIS MODULE EXISTS TO CLOSE, IN M7'S CLOSING GATE'S OWN WORDS
// ================================================================================================
//
// > Of the modules under `src/rendering/`, `cy_rendering_forward`, `cy_rendering_material`,
// > `cy_rendering_post`, `cy_rendering_shadows`, `cy_rendering_sky`, `cy_rendering_temporal`,
// > `cy_rendering_gpu_culling` and `cy_rendering_virtual_texturing` are linked by **nothing but
// > their own test binaries**. Nothing in the tree assembles a frame out of the renderer's own
// > parts.
//
// Eight suites, eight green ticks, and no frame. The material compiler filled a GPU material table
// nothing drew with; the post chain's fifteen stages were ordered in a test and run in no frame;
// the temporal framework advanced a jitter sequence nobody sampled with. `design.md` §3 says why
// M8.b is the first milestone that cannot avoid it: a vertical slice with a heads-up interface and
// 2D content renders INTO a frame, and there was no frame to render into.
//
// This module is that frame. It links all eight — and `culling/`, `lighting/`, `graph/` and
// `scene/` besides — and turns one view of one world into a compiled, executed render graph.
//
// ================================================================================================
// WHAT IT IS NOT
// ================================================================================================
//
// **It is not a renderer's policy.** It chooses no quality level, owns no configuration asset and
// reads no project settings — `cy::rendering-arbiter` is where a budget decision lives, and this
// module takes the answers as arguments. What it owns is the ORDER and the plumbing: which module's
// output is which module's input, once, in one place, so that the next consumer to arrive plugs
// into a frame instead of inventing a second one.
//
// **It does not own the shaders or the pipelines.** A pass's record callback is the CALLER's, as
// `ForwardFrame` already requires — "`ForwardFrame` knows the frame STRUCTURE and the caller knows
// how to draw". What the assembly adds is that the caller is now handed a SORTED DRAW LIST, a
// cluster assignment, a light buffer and a material table when its callback runs, instead of being
// handed nothing and expected to build all four itself. `FrameSinks` is that hand-off.
//
// **It is not a second scene.** The input is a `SpatialIndex` — the culling module's, published
// from the extract stage's snapshot — and a `render::GpuScene` slot per instance. Nothing here
// walks a node tree; design.md §4 forbids it and there would be no way to do it anyway.
//
// ================================================================================================
// THE ORDER, WHICH IS THE WHOLE OF THE DESIGN
// ================================================================================================
//
//    1. POST decides the frame's features.   `build_post_chain` is asked FIRST, because whether the
//       chain has a temporal stage is what decides whether the prepass writes motion vectors, and
//       `ForwardFrame::select_prepass_mode` derives that from the feature set rather than being
//       told. Asking post last would mean a chain that needed velocity in a frame that had already
//       decided not to produce it — which is the exact failure `PostChainRefusal` names.
//    2. TEMPORAL advances.                   One jitter for the frame, applied in one place. The
//       cull and the cluster assignment use the UNJITTERED matrices, because jitter is a sub-pixel
//       sample offset and a frustum that moved with it would cull differently every frame.
//    3. CULLING runs.                        On the CPU through `cull_view` + `select_lods`, or on
//       the device through `GpuCullPass` + `apply_gpu_cull` when a device that supports it is
//       attached. The two produce the same `CullResults`, which is why they are interchangeable
//       here and why the choice is one branch rather than two frames.
//    4. The DRAW LIST is built and sorted.   `build_draw_list` over the survivors, then
//       `sort_draw_list`, so a pass's callback receives draws in the order the sort key defines
//       rather than in cull order.
//    5. LIGHTS become records and CLUSTERS.  `build_gpu_light` per light, then `assign_clusters`
//       over the same set. One conversion, one assignment, one buffer.
//    6. SHADOWS request pages.               Every shadow-casting light asks its cache for the
//       pages this frame needs, so the budget is spent before the frame is declared rather than
//       discovered inside it.
//    7. The SKY table updates.               Rebuilt only when the sun has moved; the frame reads
//       `sky_irradiance` for its ambient term.
//    8. The FORWARD FRAME is declared.       Thirteen stages into the graph, with the callbacks the
//       caller supplied, plus the two device passes — GPU culling and virtual-texture feedback —
//       when a device is attached.
//    9. The GRAPH is compiled and executed.  `GraphExecutor`, one submit set, one fence.
//
// ================================================================================================
// WHAT IS DELIBERATELY ABSENT, AND WHY THAT IS NOT A HOLE
// ================================================================================================
//
// `gi/`, `denoise/`, `raytracing/` and `virtual_geometry/` are not here. They were never the gate's
// complaint: each is reached from a frame through a pass the caller records, and none of them is
// "linked by nothing but its own tests" — they link each other. Adding them to this module before a
// consumer needs them would be the same mistake in the other direction.
//
// `src/rendering/material/` IS linked and IS used — the assembly owns a `MaterialTable` and
// resolves a draw's material through it — and is NOT modified. It is M7's closed work; one
// extension would invalidate its cook keys.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/culling/cull.h>
#include <cy/rendering/culling/gpu_bridge.h>
#include <cy/rendering/culling/spatial.h>
#include <cy/rendering/forward/cluster.h>
#include <cy/rendering/forward/draw_list.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/gpu_culling/cull_pass.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/material/material.h>
#include <cy/rendering/post/chain.h>
#include <cy/rendering/shadows/cache.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/rendering/sky/sky_light.h>
#include <cy/rendering/temporal/framework.h>
#include <cy/rendering/virtual_texturing/frame.h>

namespace cy::rendering::assembly {

/// The renderer's configuration for one view, as the assembly needs it.
///
/// Everything here is a decision somebody else made — a project setting, a quality level, an
/// arbiter's allocation. The assembly reads them and never chooses one.
struct AssemblyDescription {
    u32 width = 0;
    u32 height = 0;
    /// Finite. `make_cluster_grid` requires it, for the reason cluster.h gives: the slice mapping
    /// is logarithmic in `far / near` and an infinite far plane has no last slice.
    f32 near_plane = 0.1F;
    f32 far_plane = 1000.0F;
    ClusterGridConfig clusters;
    PostChainConfig post;
    ShadowCacheConfig shadows;
    sky::SkyTableQuality sky = sky::SkyTableQuality::Medium;
    TemporalConfig temporal;
    /// How many material slots the table holds. The GPU material table is the renderer's, and a
    /// frame that could not resolve a draw's material would be a frame drawing with slot zero.
    u32 material_capacity = 64;
    /// How many draws one view may produce. Sizes nothing — the arrays grow — but it is what the
    /// device cull pass is created with, and a dispatch cannot grow mid-frame.
    u32 max_draws = 4096;
    u32 max_instances = 4096;
    /// The LOD tables the device dispatch is sized for. `AssemblyView::lod_chains` may be shorter
    /// and may not be longer.
    u32 max_lod_chains = 64;
    u32 max_mesh_lods = 256;
    rhi::Format color_format = rhi::Format::Rgba16Sfloat;
    rhi::Format depth_format = rhi::Format::D32Sfloat;
    /// The queue cluster assignment is declared on. The graph folds async compute onto graphics
    /// where the device has no second queue, from the same declaration.
    rhi::QueueKind cluster_queue = rhi::QueueKind::Graphics;
    /// Prefer the device cull dispatch when the attached device supports it. Off gives the CPU
    /// path, which is the reference the device path is compared against and the only path a
    /// headless build has.
    bool gpu_culling = true;
};

/// One view of one world, this frame.
struct AssemblyView {
    /// The cull's own view: frustum, camera pose, layers, LOD settings. UNJITTERED.
    CullView cull;
    /// The unjittered matrices, for the temporal framework and for the cluster assignment's
    /// view-space transform.
    Mat4 view = Mat4::identity();
    Mat4 projection = Mat4::identity();
    /// Vertical field of view, radians. The cluster assignment needs its tangent and the cull needs
    /// the angle; both come from here so they cannot disagree.
    f32 fov_y_radians = 1.0471975512F;
    /// The lights this view sees, as the render server describes them.
    Span<const render::LightDescription> lights;
    /// The mesh table's LOD chains and levels, in the form the cull dispatch reads.
    ///
    /// The CPU path takes them through `LodChainFn` and the device path through these two spans,
    /// and both are the caller's for the same reason: the chains live in the render server's mesh
    /// records and neither the culling module nor this one may hold a second copy. A device cull
    /// with EMPTY chains emits no draws at all — an instance whose `lod_chain` names nothing has no
    /// level to draw — which is a legitimate frame (a cull with nothing to draw) and is exactly
    /// what a caller that forgot them gets. `AssemblyReport::draws` is where that shows.
    Span<const render::culling::GpuLodChain> lod_chains;
    Span<const render::culling::GpuMeshLod> mesh_lods;
    /// Normalised, pointing FROM the surface TO the sun. What the sky table is rebuilt against.
    Vec3 sun_direction{0.0F, 1.0F, 0.0F};
    sky::Atmosphere atmosphere;
    /// The swapchain image the frame ends in, imported by the caller, or `kInvalidResource` to make
    /// the frame create its own target — which is what a headless run and an offscreen capture do.
    ResourceId output = kInvalidResource;
    /// Signalled to the temporal framework rather than inferred. A cinematic cut and a teleport
    /// both invalidate history and neither is a camera that moved fast.
    bool cut = false;
    /// The virtual texture's page table as the residency system left it this frame, or null when
    /// the assembly has no virtual texture attached. Uploaded once per frame, because the sampling
    /// shader reads the buffer and not this.
    const vt::PageTable* page_table = nullptr;
};

/// What the caller supplies for the parts the assembly does not own: how a mesh becomes draws, and
/// how a pass records.
struct FrameSinks {
    /// Where a visible instance's surfaces come from — the mesh table's, which is the render
    /// server's and of which this module holds no copy. Null gives ONE opaque surface per instance
    /// whose material is the instance's GPU slot, which is enough to assemble and sort a frame and
    /// is not enough to shade one.
    SurfaceQueryFn surfaces = nullptr;
    void* surfaces_user = nullptr;
    /// A record callback per stage, exactly `ForwardFrame`'s. A stage with none is declared and
    /// records nothing, which is a legitimate frame and what a structural test wants.
    FramePassCallback passes[kFramePassKindCount] = {};
};

/// What one assembled frame did. Every number is read off a module's own report rather than
/// recomputed here, so a figure that moves names the module that moved it.
struct AssemblyReport {
    // --- The CPU half ---------------------------------------------------------------------
    CullStatistics cull;
    /// True when the survivors came from the device dispatch rather than from `cull_view`.
    bool culled_on_device = false;
    u32 draws = 0;
    u32 batches = 0;
    u32 opaque = 0;
    u32 transparent = 0;
    ClusterStatistics clusters;
    u32 lights = 0;
    /// Shadow pages this frame asked the cache for, and how many were already resident.
    u32 shadow_pages_requested = 0;
    u32 shadow_pages_resident = 0;
    bool sky_rebuilt = false;
    u32 material_slots = 0;
    u64 temporal_frame = 0;
    bool temporal_invalidated = false;
    /// The post chain, in order, and why it is that long.
    u32 post_stages = 0;
    PostStage post_stage[kMaxPostStages] = {};
    PostChainRefusal post_refusal = PostChainRefusal::None;

    // --- The frame ------------------------------------------------------------------------
    PrepassMode prepass = PrepassMode::DepthOnly;
    u32 passes_declared = 0;
    /// The virtual-texture feedback the frame's resolve compacted. Zero when none is attached.
    bool virtual_texture_declared = false;
    u32 virtual_texture_requests = 0;
    u32 virtual_texture_dropped = 0;
    /// How many bytes of feedback the CPU actually mapped. The number that makes "a per-pixel
    /// request stream SHALL NOT reach the CPU" a measurement rather than a claim.
    u64 virtual_texture_bytes_read = 0;

    /// Filled by `execute`. Zero after `assemble` alone.
    ExecutionResult execution;
    bool executed = false;
};

/// One view's frame, assembled.
///
/// Held across frames, because everything expensive in it is a workspace: the cull partitions, the
/// sort scratch, the shadow cache's slot table, the sky table, the temporal histories. `assemble()`
/// clears what is per-frame and keeps what is not.
///
/// NOT THREAD-SAFE. One of these per view, stepped once per frame on the frame thread — the same
/// contract `TemporalFramework` and `ShadowPageCache` each state for themselves.
class FrameAssembly {
public:
    explicit FrameAssembly(Allocator& allocator) noexcept;
    ~FrameAssembly();

    FrameAssembly(const FrameAssembly&) = delete;
    FrameAssembly& operator=(const FrameAssembly&) = delete;
    FrameAssembly(FrameAssembly&&) = delete;
    FrameAssembly& operator=(FrameAssembly&&) = delete;

    /// Build the workspaces. Idempotent is NOT claimed: call it once.
    ///
    /// Refuses a zero viewport, a non-finite far plane and a zero material capacity, because each
    /// of them produces a frame that compiles and renders nothing — the failure this whole module
    /// exists to stop being invisible.
    [[nodiscard]] Status initialize(const AssemblyDescription& description) noexcept;

    /// Attach a device, so the frame can use the passes that need one: the GPU cull dispatch, and
    /// the virtual-texture feedback resolve.
    ///
    /// Optional and separable. Without it the assembly still culls, sorts, assigns clusters,
    /// requests shadow pages, updates the sky and DECLARES the whole frame — which is what makes
    /// the structural half of this module testable on a machine with no GPU.
    ///
    /// Reports what it could and could not create rather than failing: a device with no compute
    /// queue gets the CPU cull path, and that is the same answer rather than a lesser one.
    [[nodiscard]] Status attach_device(rhi::Device& device) noexcept;

    /// Whether the device cull dispatch is available and will be used.
    [[nodiscard]] bool device_culling() const noexcept { return cull_pass_ready_; }

    /// Attach one virtual texture, so the frame declares its feedback recording, resolve and
    /// read-back. Requires `attach_device` first.
    ///
    /// One texture rather than a set, deliberately: the assembly's job is to put the feedback pass
    /// IN a frame, and a renderer with several virtual textures composes them through
    /// `cy::servers-render-virtual-texturing`'s arbitration before this point. A second one here
    /// would be this module deciding a residency policy, which is not its business.
    [[nodiscard]] Status attach_virtual_texture(const vt::VirtualTextureDesc& desc,
                                                const vt::FeedbackSettings& settings) noexcept;

    [[nodiscard]] bool virtual_texturing() const noexcept { return vt_ready_; }

    /// Assemble one frame into `graph`. Steps 1 to 8 of the header's order.
    ///
    /// `graph` is cleared by the caller between frames, exactly as `ForwardFrame::build` expects.
    [[nodiscard]] Status assemble(const SpatialIndex& index, const AssemblyView& view,
                                  const FrameSinks& sinks, RenderGraph& graph,
                                  AssemblyReport& out) noexcept;

    /// Compile and run the graph, and read back what the device passes produced. Step 9.
    ///
    /// CALL IT INSIDE THE HOST'S DEVICE FRAME — between `Device::begin_frame()` and `end_frame()`.
    /// The executor acquires its command buffer from the device's current frame, and the frame
    /// boundary is the host's loop's rather than a renderer's: `src/runtime/` owns the `while
    /// (running)` and this module must not open a second one.
    [[nodiscard]] Status execute(GraphExecutor& executor, RenderGraph& graph,
                                 AssemblyReport& out) noexcept;

    // --- What the caller's record callbacks read ------------------------------------------
    //
    // Everything a pass needs and cannot build for itself, published as a view onto the
    // assembly's own workspace. Valid until the next `assemble`.

    [[nodiscard]] const DrawList& draws() const noexcept { return draws_; }
    /// The draws of one sort layer — `render::SortLayer::Opaque` and `Transparent` are the two a
    /// forward frame has passes for. A half-open range into `draws().items`.
    [[nodiscard]] Span<const render::DrawItem> layer(render::SortLayer layer) const noexcept;
    [[nodiscard]] const ClusterAssignment& clusters() const noexcept { return clusters_; }
    [[nodiscard]] Span<const GpuLight> lights() const noexcept { return lights_.span(); }
    [[nodiscard]] const MaterialTable& materials() const noexcept { return materials_; }
    [[nodiscard]] MaterialTable& materials() noexcept { return materials_; }
    [[nodiscard]] const ForwardFrame& frame() const noexcept { return frame_; }
    [[nodiscard]] const FrameResources& resources() const noexcept { return frame_.resources(); }
    [[nodiscard]] TemporalFramework& temporal() noexcept { return temporal_; }
    [[nodiscard]] ShadowPageCache& shadows() noexcept { return shadows_; }
    [[nodiscard]] const sky::SkyViewTable& sky() const noexcept { return sky_; }
    /// The ambient irradiance the sky contributes, for the frame's own ambient term.
    [[nodiscard]] Vec3 sky_irradiance() const noexcept { return sky_irradiance_; }
    [[nodiscard]] const AssemblyDescription& description() const noexcept { return description_; }

private:
    [[nodiscard]] Status decide_features(const AssemblyView& view, FrameFeatures& features,
                                         AssemblyReport& out) const noexcept;
    [[nodiscard]] Status run_cull(const SpatialIndex& index, const AssemblyView& view,
                                  AssemblyReport& out) noexcept;
    [[nodiscard]] Status run_device_cull(const SpatialIndex& index, const AssemblyView& view,
                                         AssemblyReport& out) noexcept;
    [[nodiscard]] Status build_draws(const FrameSinks& sinks, AssemblyReport& out) noexcept;
    [[nodiscard]] Status build_lights(const AssemblyView& view, AssemblyReport& out) noexcept;
    [[nodiscard]] Status request_shadow_pages(const AssemblyView& view,
                                              AssemblyReport& out) noexcept;
    [[nodiscard]] Status update_sky(const AssemblyView& view, AssemblyReport& out) noexcept;
    [[nodiscard]] Status declare_frame(const AssemblyView& view, const FrameFeatures& features,
                                       const FrameSinks& sinks, RenderGraph& graph,
                                       AssemblyReport& out) noexcept;

    Allocator* allocator_ = nullptr;
    AssemblyDescription description_;
    ClusterGrid grid_;
    bool initialized_ = false;

    // Culling.
    CullWorkspace workspace_;
    CullResults results_;
    Array<u32> previous_lod_levels_;

    // The draw list and its sort scratch.
    DrawList draws_;
    DrawSortScratch sort_;
    /// Where each sort layer begins in `draws_.items`, plus a terminator. Filled by the sort, so
    /// `layer()` is a subspan rather than a search.
    u32 layer_begin_[static_cast<usize>(render::SortLayer::Count) + 1] = {};

    // Lights and clusters.
    Array<GpuLight> lights_;
    Array<ClusterElement> elements_;
    ClusterAssignment clusters_;

    // The other five of the eight.
    MaterialTable materials_;
    ShadowPageCache shadows_;
    sky::SkyViewTable sky_;
    Vec3 sky_irradiance_{0.0F, 0.0F, 0.0F};
    TemporalFramework temporal_;
    ConsumerId temporal_consumer_;
    HistoryId history_;
    ForwardFrame frame_;

    // The device half.
    rhi::Device* device_ = nullptr;
    gpu_culling::GpuCullPass cull_pass_;
    GpuCullPublication published_;
    bool cull_pass_ready_ = false;
    /// True when this frame declared the device cull passes, so `execute` knows to read back.
    bool cull_pass_declared_ = false;
    vt::VirtualTextureFrame vt_frame_;
    bool vt_ready_ = false;
    bool vt_declared_ = false;

    u64 frame_index_ = 0;
};

}  // namespace cy::rendering::assembly
