#pragma once
// The frame: the pass order, declared into the render graph. Tasks 4.3.2 and 4.3.3.
//
// `rendering-forward-clustered` — "Pass order" lists thirteen stages, in order, and requires that
// "WHEN ambient occlusion, SSR, and TAA are all disabled THEN their passes SHALL be absent from the
// graph and their targets unallocated". This file is that list, and the absence is not a branch in
// a renderer — it is a pass that was never declared, so the graph never allocates its target, never
// derives a barrier for it, and never records it.
//
// ================================================================================================
// THIS FILE DECLARES; IT DOES NOT RECORD
// ================================================================================================
//
// Every pass is declared with its reads and its writes and a record callback the CALLER supplies.
// That division is the M3 invariant seen from the pass author's side: `ForwardFrame` knows the
// frame STRUCTURE — what depends on what — and the caller knows how to draw. Neither one can write
// a barrier, because there is nowhere in either interface for one.
//
// The consequence worth stating: a pass with no callback still declares its resources, and the
// graph still derives every barrier around it. So a frame can be built, compiled and asserted on
// with no device, no shaders and no draws — which is what `unit.forward_frame` does, and it is the
// reason the pass order is a test rather than a diagram.
//
// ================================================================================================
// WHY THE PREPASS MODE IS DERIVED AND NOT SET
// ================================================================================================
//
// "The pipeline SHALL render a depth prepass before shading, with a mode selected from what later
// passes require" — and its scenario: "WHEN SSAO is enabled and TAA is not THEN the prepass SHALL
// run in `DepthNormal` mode, and no motion vector target SHALL be allocated."
//
// So the mode is a FUNCTION of the feature set, computed by `select_prepass_mode()`, and the frame
// allocates exactly the targets that mode names. A settable mode would be a second source of truth
// that a feature toggle could contradict, and the symptom would be a velocity buffer nothing writes
// or a normal buffer SSAO reads and nobody filled.
//
// ================================================================================================
// THE DEPTH TEST AFTER THE PREPASS, WHICH IS A CONVENTION AND NOT AN OPTIMISATION
// ================================================================================================
//
// "WHEN the opaque pass runs THEN it SHALL test depth with `Equal` against the prepass result and
// SHALL NOT write depth." That is why the opaque pass declares `DepthStencilAttachmentRead` rather
// than `...Write`: the declaration is what the graph derives the barrier from, and a pass that
// declared a write would get a write barrier for a buffer it does not write and would serialise
// against the prepass for nothing.

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/forward/cluster.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering {

/// `rendering-forward-clustered`'s three prepass modes.
enum class PrepassMode : u8 {
    /// Depth only. Nothing later needs prepass data.
    DepthOnly = 0,
    /// Depth plus octahedron-encoded normal and roughness. Screen-space effects need it.
    DepthNormal,
    /// The above plus motion vectors. Anything temporal needs it.
    DepthNormalVelocity,
    Count,
};

[[nodiscard]] const char* prepass_mode_name(PrepassMode mode) noexcept;

/// What a frame has switched on. Every one of these removes or adds passes rather than branching
/// inside them.
struct FrameFeatures {
    bool depth_prepass = true;
    bool ambient_occlusion = false;
    bool screen_space_gi = false;
    bool screen_space_reflections = false;
    /// Temporal antialiasing or temporal upscaling. Either one needs motion vectors.
    bool temporal = false;
    bool motion_blur = false;
    bool sky = true;
    bool transparency = true;
    /// A transparent material sampling scene colour needs a copy of the opaque result made before
    /// the transparent pass — "and the graph SHALL synchronise it", which it does because the copy
    /// is a declared pass and the read is a declared read.
    bool transparent_refraction = false;
    bool post_process = true;
    bool ui = true;
    /// Virtual geometry's hardware rasteriser: a stage after the depth prepass that writes the
    /// visibility target and the frame's own depth. Off by default, and a caller turns it on by
    /// handing the frame to `vg::ForwardVisibility`, which supplies the stage's record callback —
    /// see the header comment on `FramePassKind::VirtualGeometry`.
    bool virtual_geometry = false;
    /// 1, 2, 4 or 8. Above 1 the colour and depth are resolved before the screen-space passes.
    u32 msaa_samples = 1;
};

/// The mode the features require. See the header comment for why this is derived.
[[nodiscard]] PrepassMode select_prepass_mode(const FrameFeatures& features) noexcept;

/// The stages, in the specification's order. `FramePassKind` is what a caller attaches a record
/// callback to and what a statistics report attributes a timing to.
enum class FramePassKind : u8 {
    Prepare = 0,
    Shadow,
    DepthPrepass,
    DepthResolve,
    /// Virtual geometry's visibility pass: the visible clusters the traversal selected, drawn by a
    /// vertex and fragment shader into a one-word visibility target and into the frame's depth.
    ///
    /// NOT ONE OF THE SPECIFICATION'S THIRTEEN, and placed where it is for a reason the pass order
    /// can check. It writes the SAME depth the prepass wrote, so it comes after the prepass (and
    /// its MSAA resolve, which a frame with virtual geometry refuses) and before every stage that
    /// reads depth — the screen-space passes and the opaque pass's `Equal` test. A mesh and a
    /// cluster hierarchy then occlude one another through one depth buffer rather than being
    /// composited.
    VirtualGeometry,
    ClusterAssignment,
    AmbientOcclusion,
    ScreenSpaceGi,
    Opaque,
    Sky,
    ScreenSpaceReflections,
    OpaqueColorCopy,
    Transparent,
    Resolve,
    Temporal,
    PostProcess,
    UiAndDebug,
    /// The blit that puts the last colour the frame produced into `output`. Declared ONLY when the
    /// composite did not already write the output directly — with post-processing on, tonemapping
    /// writes the swapchain image and this pass does not exist. It is a sub-stage of the
    /// specification's thirteenth rather than a fourteenth stage, and it is separate from `Present`
    /// because the two do different things: one moves pixels, the other moves a layout.
    Composite,
    /// The presentation transition. A RECORDLESS, side-effecting pass that declares
    /// `Access::Present` against the output and nothing else — so the graph derives the transition
    /// to the presentable layout the same way it derives every other one, and no renderer code
    /// names a layout. The same shape the M3 spike used for the host boundary.
    Present,
    Count,
};

inline constexpr u32 kFramePassKindCount = static_cast<u32>(FramePassKind::Count);

[[nodiscard]] const char* frame_pass_kind_name(FramePassKind kind) noexcept;

/// A read a stage's record callback performs on a resource some other pass produced, declared so
/// the graph derives the dependency. `vertex_reads` below is the common case of this; a callback
/// that draws indirectly or pulls vertices out of storage buffers needs the general one.
struct FrameResourceRead {
    ResourceId resource = kInvalidResource;
    /// A READING intent. The graph refuses a writing one rather than deriving a wrong barrier.
    rhi::Access access = rhi::Access::VertexStorageRead;
};

/// A caller's record callback for one stage.
struct FramePassCallback {
    FramePassCallback() = default;
    FramePassCallback(RecordFn function, void* context,
                      Span<const ResourceId> resources = {}) noexcept
        : record(function), user(context), vertex_reads(resources) {}

    RecordFn record = nullptr;
    void* user = nullptr;
    /// Device buffers read by commands recorded through this callback. Declaring them here lets
    /// the graph derive compute-to-vertex barriers for caller-owned geometry producers.
    Span<const ResourceId> vertex_reads;
    /// Any other resources the callback reads, with the intent it reads them with.
    Span<const FrameResourceRead> reads;
};

/// What a stage that declares ITS OWN PASSES is handed: the prepass outputs it reads and the target
/// the rest of the frame reads it through.
struct ScreenSpaceStageInputs {
    ResourceId depth = kInvalidResource;
    ResourceId normal_roughness = kInvalidResource;
    /// The stage's product. The frame's later passes declare their reads against this resource, so
    /// the declarer's last pass must write it.
    ResourceId target = kInvalidResource;
    u32 width = 0;
    u32 height = 0;
};

/// Declares a stage as several passes. Returns the FIRST pass it declared, or `kInvalidPass` to
/// refuse — which fails the frame's build rather than leaving the stage out.
using DeclareStageFn = PassId (*)(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                  void* user) noexcept;

/// A stage whose producer needs more than one pass: ambient occlusion is a horizon search followed
/// by a filter cascade, and a barrier between two dispatches can only come from the graph, so the
/// producer declares the passes itself at the stage's position in the order. A null `declare` keeps
/// the stage the single pass `ForwardFrame` declares, with the caller's record callback.
struct FrameStageDeclaration {
    DeclareStageFn declare = nullptr;
    void* user = nullptr;
};

/// The resources one frame declares. `kInvalidResource` for anything the feature set left out —
/// which is how "their targets unallocated" is observable rather than asserted.
struct FrameResources {
    ResourceId depth = kInvalidResource;
    /// Multisample depth, when MSAA is on. `depth` is then the resolved single-sample copy.
    ResourceId depth_multisampled = kInvalidResource;
    ResourceId normal_roughness = kInvalidResource;
    ResourceId velocity = kInvalidResource;
    ResourceId color = kInvalidResource;
    ResourceId color_multisampled = kInvalidResource;
    ResourceId opaque_color_copy = kInvalidResource;
    ResourceId ambient_occlusion = kInvalidResource;
    ResourceId screen_space_gi = kInvalidResource;
    ResourceId reflections = kInvalidResource;
    ResourceId temporal_previous = kInvalidResource;
    ResourceId temporal_history = kInvalidResource;
    ResourceId post_color = kInvalidResource;
    /// Where the frame ends up: the caller's imported swapchain image, or a frame-owned texture
    /// when the caller imported none.
    ResourceId output = kInvalidResource;
    /// The cluster grid's two buffers, and the light list the assignment reads.
    ResourceId cluster_headers = kInvalidResource;
    ResourceId cluster_indices = kInvalidResource;
    ResourceId lights = kInvalidResource;
    /// `GpuDrawInstance` records, indexed by a draw's `first_instance`.
    ResourceId draw_instances = kInvalidResource;
    /// Virtual geometry's visibility target: one `(identity << 8) | triangle` payload a pixel,
    /// stored complemented so that a cleared texel reads as "no surface". Only with
    /// `FrameFeatures::virtual_geometry`.
    ResourceId visibility = kInvalidResource;
};

/// One declared pass, so a caller can find a pass it wants to inspect or time.
struct FramePass {
    FramePassKind kind = FramePassKind::Count;
    const char* name = "";
    PassId pass = kInvalidPass;
};

struct FrameDescription {
    u32 width = 0;
    u32 height = 0;
    rhi::Format color_format = rhi::Format::Rgba16Sfloat;
    rhi::Format depth_format = rhi::Format::D32Sfloat;
    /// Octahedral normal in two channels and roughness in the rest — `rendering-forward-clustered`
    /// asks for 16-bit channels, which is what this format gives.
    rhi::Format normal_format = rhi::Format::Rgba16Sfloat;
    rhi::Format velocity_format = rhi::Format::Rg16Sfloat;
    /// Thirty-two bits is the visibility payload exactly: 24 of surface identity and 8 of triangle,
    /// the word `vg_visbuffer.slang`'s compute rasteriser packs beside its depth key.
    rhi::Format visibility_format = rhi::Format::R32Uint;
    FrameFeatures features;
    /// The grid the assignment pass dispatches over. `cluster_count() == 0` skips the pass, which
    /// is what a view with no lights costs.
    ClusterGrid cluster_grid;
    u32 light_count = 0;
    /// How many `GpuDrawInstance` records the frame's draws index. Sizes the buffer; zero skips it.
    u32 draw_instance_count = 0;
    /// The swapchain image, imported by the caller. `kInvalidResource` makes the frame create its
    /// own target, which is what a headless test and an offscreen capture want.
    ResourceId output = kInvalidResource;
    /// Optional directional shadow targets supplied by the view owner.
    ResourceId shadow_color = kInvalidResource;
    ResourceId shadow_depth = kInvalidResource;
    /// Device-backed temporal histories imported by the assembly. When absent, structural tests
    /// receive graph-owned stand-ins so the pass remains inspectable without a device.
    ResourceId temporal_previous = kInvalidResource;
    ResourceId temporal_current = kInvalidResource;
    /// The ambient occlusion target, imported by its producer. Absent, the frame creates the
    /// single-channel transient the stage has always declared. Read only with
    /// `features.ambient_occlusion`.
    ResourceId ambient_occlusion_target = kInvalidResource;
    /// The producer that declares the ambient occlusion stage's passes. See
    /// `FrameStageDeclaration`.
    FrameStageDeclaration ambient_occlusion_stage;
    /// The queue the cluster assignment runs on. Async compute where the device has one; the graph
    /// folds it onto graphics where it does not, from the same declarations.
    rhi::QueueKind cluster_queue = rhi::QueueKind::Graphics;
    /// Record callbacks, indexed by `FramePassKind`. A missing one declares the pass and records
    /// nothing, which is a legitimate frame — see the header comment.
    FramePassCallback callbacks[kFramePassKindCount] = {};
};

/// Declares one view's frame into a graph.
///
/// Stateless between frames: `build()` clears and re-declares. A caller keeps one per view for its
/// arrays, not for its state.
class ForwardFrame {
public:
    explicit ForwardFrame(Allocator& allocator) noexcept;

    ForwardFrame(const ForwardFrame&) = delete;
    ForwardFrame& operator=(const ForwardFrame&) = delete;

    /// Declare every pass the feature set calls for, in the specification's order.
    ///
    /// Fails, rather than declaring a partial frame, when the description is inconsistent — a zero
    /// viewport, an MSAA count that is not 1, 2, 4 or 8. The graph's own `status()` is checked by
    /// the caller before it compiles; this one is checked here because a frame with half its passes
    /// would compile happily and render nothing.
    [[nodiscard]] Status build(RenderGraph& graph, const FrameDescription& description) noexcept;

    [[nodiscard]] const FrameResources& resources() const noexcept { return resources_; }
    [[nodiscard]] Span<const FramePass> passes() const noexcept { return passes_.span(); }
    [[nodiscard]] PrepassMode prepass_mode() const noexcept { return prepass_mode_; }

    /// The pass declared for a stage, or `kInvalidPass` when the feature set left it out.
    [[nodiscard]] PassId pass_of(FramePassKind kind) const noexcept;

    /// The state the three declaration groups thread through each other. Public because the free
    /// functions that declare individual stages are file-local in frame.cpp and cannot see a
    /// private nested type; there is nothing in it a caller could usefully touch.
    struct BuildState;

private:
    Status declare_resources(RenderGraph& graph, const FrameDescription& description) noexcept;
    Status declare_passes(RenderGraph& graph, const FrameDescription& description) noexcept;
    /// The three groups the pass order divides into, split so that each is about its own stages
    /// rather than about the bookkeeping between them.
    void declare_prepare_and_depth(RenderGraph& graph, BuildState& state) noexcept;
    void declare_shading(RenderGraph& graph, BuildState& state) noexcept;
    void declare_post_chain(RenderGraph& graph, BuildState& state) noexcept;

    /// Record a declared pass. A no-op for `kInvalidPass` — a stage the feature set left out — and
    /// a no-op once a failure has been recorded, so a declaration sequence reads as a sequence
    /// rather than as fifteen error checks. `status_` is what `build()` returns.
    void stage(FramePassKind kind, const char* name, PassId pass) noexcept;
    /// Hand a stage to the producer that declares its passes, and record the first of them.
    void declare_produced_stage(RenderGraph& graph, const BuildState& state,
                                const FrameStageDeclaration& producer, FramePassKind kind,
                                const char* name, ResourceId target) noexcept;

    Array<FramePass> passes_;
    FrameResources resources_{};
    PrepassMode prepass_mode_ = PrepassMode::DepthOnly;
    Status status_;
};

}  // namespace cy::rendering
