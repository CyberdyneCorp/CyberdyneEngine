#pragma once
// THE LAYER ABOVE THE FRAME: pipeline state objects and the binding model they share. M8.c 1b.1.
//
// ================================================================================================
// THE WALL THIS MODULE EXISTS TO REMOVE, IN M8.b's CLOSING GATE'S OWN WORDS
// ================================================================================================
//
// > `FrameAssembly` hands each pass's record callback to its caller and the vertical slice supplies
// > none, which is why the slice's picture is DRAWN rather than CAPTURED. A person could build all
// > of M8.b and still not *see* it without writing their own renderer.
//
// M8.b's frame is real: eight modules called in an order, thirteen stages declared into the render
// graph, every barrier derived, the whole thing compiled and executed on a device with validation
// on. And it records nothing, because `FrameSinks::passes` is empty and `ForwardFrame` is explicit
// that "a pass with no callback still declares its resources" — a legitimate frame, and a blank
// one.
//
// The two seams M8.b left are `FrameSinks::passes` and `SceneIndex`. This module is what plugs into
// the first of them: the pipeline state objects, the descriptor sets, the uploads and the record
// callbacks. `frame_recorder.h` is where they meet.
//
// ================================================================================================
// WHAT IT OWNS, AND WHAT IT REFUSES TO OWN
// ================================================================================================
//
// **It owns the pipelines and the bindings.** Depth, opaque, transparent, temporal and tonemapping
// graphics pipelines, one pipeline layout, three descriptor set layouts on the engine's own set
// convention, and one sampler.
//
// **It owns no geometry.** A mesh's vertex and index buffers are the render server's, and this
// module holds no copy: `frame_recorder.h`'s `GeometrySource` is the seam a caller fills, exactly
// as `FrameSinks::surfaces` is the seam `FrameAssembly` leaves for the same reason.
//
// **It owns no policy.** It creates the pipelines the `PipelineSetup` it is handed names, and the
// setup is derived from an `AssemblyDescription` — the formats, the sample count and the feature
// set are already somebody else's decisions by the time they reach here.
//
// **It does not compile shaders.** The SPIR-V and MSL are committed (`shaders/frame_spirv.h` and
// `shaders/frame_msl.h`), because
// `CY_SHADER_SLANG` is off by default and off in Profile and Shipping, and because `shader-system`
// states outright that "a shipping build SHALL contain compiled backend-native shader artefacts and
// no Slang compiler". `src/rendering/shaders/cy/frame.slang` is the source and its header comment
// carries the invocations.
//
// ================================================================================================
// THE VERTEX STREAMS, AND THE ONE PLACE THE RHI CANNOT SPELL THE ENGINE'S FORMAT
// ================================================================================================
//
// `render::VertexStream` splits a mesh into streams so "a shadow pass over an interleaved vertex
// reads normals, UVs and colours it will not use" stops being true. The depth pipeline below binds
// streams 0 and 1 — it writes a normal and a velocity target and reads no texture coordinate — and
// the forward pipelines bind 0, 1 and 2, which is `render::kDepthPassStreams` made structural
// rather than documented. `kDepthPassStreamCount` and `kForwardPassStreamCount` below are those two
// numbers, declared once because the pipeline and the recorder must agree about them.
//
// **`render::PackedNormalTangent` is two pairs of 16-bit SIGNED NORMALISED components and
// `rhi::Format` has no `Rgba16Snorm`.** So the normal stream this module binds is `Rgba16Sfloat`,
// carrying the same octahedral pair as half floats. That is a real difference from the engine's
// cooked mesh encoding and it is recorded here rather than hidden: closing it means adding a format
// to `src/backends/rhi/`, which is below this layer and not this module's to change.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

#include <cstddef>

namespace cy::rendering::pipeline {

// --- The descriptor set convention ------------------------------------------------------------
//
// `cy/backends/shader/reflection.h`'s, restated as the three indices this module's pipeline layout
// actually uses. Set 3 (per draw) is deliberately absent: the per-draw datum is one word and it is
// a push constant, which costs no descriptor traffic at all.

inline constexpr u32 kGlobalSet = 0;
inline constexpr u32 kViewSet = 1;
inline constexpr u32 kPassSet = 2;
inline constexpr u32 kSetCount = 3;

// --- Set 0 carries the globals block AND the material texture table. M11.c task 3.7 -------------
//
// THE REASON THE FORWARD PATH SAMPLED NOTHING, and it was not a missing feature anywhere. The RHI
// has had a global bindless table since M11.c's first half — declared at (set 0, binding 1) and
// (set 0, binding 2) by `cy/material.slang`, filled by `Device::bind_texture_globally`, made
// resident by `MaterialTextureTable` — and this module's set 0 carried `cy/globals.slang`'s block
// at binding 0 and nothing else. A PIPELINE BINDS ONE SET PER INDEX, so a program that wants both
// needs a set 0 that HAS both, and there was none.
//
// THE SLOT INDEX IS STILL THE DEVICE'S. What is written here at `array_index = slot` is the view
// the device's own table holds at that slot, and the slot is what `Device::bind_texture_globally`
// handed out; this module allocates no indices of its own. So a material's slot word means the
// same thing to this set, to the device's table, and to every other consumer of either — the two
// sets can differ in WHICH DESCRIPTORS ARE WRITTEN, never in what a number means. The frame's own
// set is what a pipeline layout on the engine's set convention can name; see
// `FrameBindings::set_material_textures`.
inline constexpr u32 kGlobalBindingGlobals = 0;
inline constexpr u32 kGlobalBindingMaterialTextures = 1;
inline constexpr u32 kGlobalBindingMaterialSampler = 2;
inline constexpr u32 kGlobalBindingCount = 3;

/// How many slots of the global table the frame's own set 0 can name.
///
/// A FIXED COUNT AND NOT THE DEVICE'S 16384: every allocation of this per-frame set costs that many
/// descriptors out of the frame's pool, and the pool holds 512 of them. A frame that is handed a
/// slot at or past this index is REFUSED naming both numbers rather than drawing a surface that
/// samples an unwritten descriptor — which is undefined, and looks like a texture on most drivers.
inline constexpr u32 kMaterialTextureSlots = 128;

// The numbers are the SHADER's, reached from the RHI's own copy of them rather than written twice.
// `rhi/pipeline.h` says why they live there: "the shader declared these first".
static_assert(kGlobalSet == rhi::kGlobalTableSet);
static_assert(kGlobalBindingMaterialTextures == rhi::kGlobalTableTextureBinding);
static_assert(kGlobalBindingMaterialSampler == rhi::kGlobalTableSamplerBinding);

/// One resident texture as the frame's set 0 needs it: the slot the device gave it, and the view to
/// write there. `MaterialTextureTable::slots()` produces these.
struct MaterialTextureSlot {
    rhi::BindlessIndex slot = rhi::kInvalidBindlessIndex;
    rhi::TextureViewHandle view;
};

/// The sentinel `cy/frame.slang` spells `kCyNoMaterialTexture`: "no texture here". It is
/// `rhi::kInvalidBindlessIndex`, because the value it stands in for IS a bindless index — and zero
/// cannot do the job, since zero is a perfectly good slot of the table.
inline constexpr u32 kNoMaterialTexture = rhi::kInvalidBindlessIndex;

/// Bindings within the view set. The same numbers `cy/frame.slang` declares, and the reason they
/// are named here is that the two files are the only places they appear.
inline constexpr u32 kViewBindingFrame = 0;
inline constexpr u32 kViewBindingLights = 1;
inline constexpr u32 kViewBindingClusterHeaders = 2;
inline constexpr u32 kViewBindingClusterIndices = 3;
inline constexpr u32 kViewBindingDrawInstances = 4;
inline constexpr u32 kViewBindingInstances = 5;
inline constexpr u32 kViewBindingMaterials = 6;
inline constexpr u32 kViewBindingCount = 7;

inline constexpr u32 kPassBindingSceneColor = 0;
inline constexpr u32 kPassBindingSampler = 1;
inline constexpr u32 kPassBindingHistory = 2;
inline constexpr u32 kPassBindingVelocity = 3;
inline constexpr u32 kPassBindingDepth = 4;
inline constexpr u32 kPassBindingCount = 5;

/// The vertex stream bindings, in `render::VertexStream`'s own order.
inline constexpr u32 kPositionStream = 0;
inline constexpr u32 kNormalStream = 1;
inline constexpr u32 kUvStream = 2;

inline constexpr u32 kPositionStreamStride = 12;  // Rgb32Sfloat
inline constexpr u32 kNormalStreamStride = 8;     // Rgba16Sfloat: octahedral normal, then tangent
inline constexpr u32 kUvStreamStride = 8;         // Rg32Sfloat

/// How many streams each pass binds, declared ONCE because two files have to agree about it.
///
/// The depth pass takes the position and the packed normal and leaves the UVs alone — it writes a
/// normal and a velocity target and reads neither texture coordinate. `frame_pipelines.cpp`
/// declares this many vertex bindings on the pipeline and `frame_recorder.cpp` binds this many
/// buffers before the draw; when those were two literals they disagreed for three commits, and
/// every depth draw in that window fetched an attribute from a binding nothing was bound to.
inline constexpr u32 kDepthPassStreamCount = 2;
inline constexpr u32 kForwardPassStreamCount = 3;

/// Encode a normal and a tangent into one 8-byte normal-stream vertex.
///
/// THE BRIDGE BETWEEN THE COOKED ENCODING AND THE VERTEX INPUT, and the reason it exists is a real
/// gap rather than a preference: `render::PackedNormalTangent` stores the same two octahedral pairs
/// as 16-bit SIGNED NORMALISED components, and `rhi::Format` has no `Rgba16Snorm` for a vertex
/// attribute to be declared with. Half floats carry [-1, 1] with about eleven bits of mantissa,
/// which is more than the snorm form has, at the same eight bytes. Closing the gap properly means
/// adding a format to `src/backends/rhi/`, which is below this layer.
void pack_normal_stream(Vec3 normal, Vec3 tangent, u16 out[4]) noexcept;

// --- The blocks the shader reads ---------------------------------------------------------------

/// `cy/globals.slang`'s `CyGlobalsData`, at set 0 binding 0.
struct alignas(16) GlobalsData {
    f32 time_seconds = 0.0F;
    f32 delta_seconds = 0.0F;
    /// Exposure in stops. `cy/fullscreen.slang`'s resolve reads it and nothing else does.
    f32 exposure_stops = 0.0F;
    f32 wind_strength = 0.0F;
    f32 wind_direction_and_speed[4] = {0.0F, 0.0F, 0.0F, 0.0F};
};

static_assert(sizeof(GlobalsData) == 32);

/// `cy/frame.slang`'s `CyFrameData`, at set 1 binding 0. Every offset below is asserted against the
/// offsets Slang produced for the std140 block — see the static assertions at the foot of the file.
struct alignas(16) FrameViewData {
    /// Camera-relative to clip, as four explicit rows. See `frame.slang`'s header for why this is
    /// not a matrix type: a matrix in a constant block has a layout that is a property of the
    /// compiler's flags rather than of either side.
    f32 relative_to_clip[16] = {};
    /// Previous frame's unjittered camera-relative transform, used by the velocity prepass.
    f32 previous_relative_to_clip[16] = {};
    /// Camera-relative to view. Only the third row is read — the cluster slice needs a view-space
    /// depth — and all four are carried so a later pass needs no second upload.
    f32 relative_to_view[16] = {};
    /// rgb: the sky's irradiance, which is the frame's ambient term. w: occlusion strength.
    f32 ambient_and_occlusion[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    /// xy: the render extent in pixels. zw: its reciprocal.
    f32 extent_and_inverse[4] = {};
    /// The cluster grid, bit for bit: `ClusterGrid` is asserted to be 32 bytes and to match
    /// `cy/cluster.slang`'s in `forward/cluster.h`, so this is a copy rather than a translation.
    u32 cluster_dimensions[3] = {0, 0, 0};
    u32 max_elements_per_cluster = 0;
    f32 near_plane = 0.1F;
    f32 far_plane = 1000.0F;
    f32 slice_scale = 0.0F;
    f32 slice_bias = 0.0F;
    /// x: lights. y: words per material block. z: `kClusterElementTypeCount`. w: 1 when the cluster
    /// lists were assigned and 0 when the frame had no grid, in which case the shading loop walks
    /// every light — a DECLARED fallback rather than a frame that quietly shades nothing.
    u32 counts[4] = {0, 0, 0, 0};
    /// Word offsets into a material block: base colour, roughness, metallic, emission. DERIVED from
    /// the `MaterialProgram` the table was described with, never hardcoded.
    u32 material_offsets[4] = {0, 0, 0, 0};
    /// x: history valid, y: history feedback. z/w reserved.
    f32 temporal_feedback[4] = {};
    /// current jitter in xy and previous jitter in zw, in pixel units.
    f32 temporal_jitter[4] = {};
    /// Word offsets of the material block's TEXTURE SLOTS — x is the base colour texture's —
    /// derived from the `MaterialProgram` exactly as `material_offsets` above is.
    ///
    /// DEFAULTED TO "NONE", WHICH IS THE WHOLE OF THE BACKWARD COMPATIBILITY. `cy/frame.slang`
    /// samples nothing when x is `kNoMaterialTexture`, so a caller written before this field
    /// existed uploads the frame it always uploaded and photographs the picture it always
    /// photographed. A caller that wants a textured surface says so by writing this field, and
    /// then owns making the slots resident — see `FrameBindings::set_material_textures`.
    u32 material_textures[4] = {kNoMaterialTexture, kNoMaterialTexture, kNoMaterialTexture,
                                kNoMaterialTexture};
};

static_assert(sizeof(FrameViewData) == 336, "CyFrameData's std140 block is 336 bytes");
static_assert(offsetof(FrameViewData, material_textures) == 320);
static_assert(offsetof(FrameViewData, previous_relative_to_clip) == 64);
static_assert(offsetof(FrameViewData, relative_to_view) == 128);
static_assert(offsetof(FrameViewData, ambient_and_occlusion) == 192);
static_assert(offsetof(FrameViewData, extent_and_inverse) == 208);
static_assert(offsetof(FrameViewData, cluster_dimensions) == 224);
static_assert(offsetof(FrameViewData, near_plane) == 240);
static_assert(offsetof(FrameViewData, counts) == 256);
static_assert(offsetof(FrameViewData, material_offsets) == 272);

/// `cy/frame.slang`'s `CyInstanceTransform`: one instance's placement, model to CAMERA-RELATIVE.
///
/// Supplied by the caller rather than read out of `render::GpuScene`, for the reason
/// `FrameAssembly` gives about the mesh table: the GPU scene is the render server's and this module
/// holds no second copy of it. A caller that has one publishes its rows here.
struct alignas(16) InstanceTransform {
    f32 row0[4] = {1.0F, 0.0F, 0.0F, 0.0F};
    f32 row1[4] = {0.0F, 1.0F, 0.0F, 0.0F};
    f32 row2[4] = {0.0F, 0.0F, 1.0F, 0.0F};
    /// rgb: a per-instance tint the material's base colour is multiplied by. a: unused.
    f32 tint[4] = {1.0F, 1.0F, 1.0F, 1.0F};
};

static_assert(sizeof(InstanceTransform) == 64);

/// The one push constant: which draw is being recorded.
struct DrawPush {
    u32 draw_index = 0;
};

// --- The pipelines ------------------------------------------------------------------------------

/// What the pipelines are created for. Every field is a decision somebody else already made — the
/// formats and the sample count come off an `AssemblyDescription`, and the feature booleans come
/// off the `FrameFeatures` the post chain derived.
struct PipelineSetup {
    rhi::Format color_format = rhi::Format::Rgba16Sfloat;
    rhi::Format depth_format = rhi::Format::D32Sfloat;
    rhi::Format output_format = rhi::Format::Rgba8Unorm;
    u32 sample_count = 1;
    /// Create the alpha-blended pipeline. Off when `FrameFeatures::transparency` is off, in which
    /// case the frame declares no transparent pass for it to be bound in.
    bool transparency = true;
    /// Create the tonemapping resolve. Off when `FrameFeatures::post_process` is off, in which case
    /// the frame's colour target never reaches the output through this module.
    bool tonemap = true;
    /// The prepass attachments its shader writes. Velocity implies normal.
    bool prepass_normal = false;
    bool prepass_velocity = false;
};

/// Which pipeline a pass binds.
enum class FramePipelineKind : u8 {
    Depth = 0,
    Opaque,
    Transparent,
    Resolve,
    Temporal,
    Count,
};

inline constexpr u32 kFramePipelineKindCount = static_cast<u32>(FramePipelineKind::Count);

[[nodiscard]] const char* frame_pipeline_kind_name(FramePipelineKind kind) noexcept;

/// The pipeline state objects one view's frame binds, and the layout they share.
///
/// Created once and held across frames: a pipeline is the expensive thing in a renderer and
/// `shader-system`'s `PipelineStateCache` states outright that "blocking the frame to compile a
/// pipeline state SHALL NOT occur in shipping builds". Nothing here is created inside `assemble`.
class FramePipelines {
public:
    FramePipelines() noexcept = default;
    ~FramePipelines();

    FramePipelines(const FramePipelines&) = delete;
    FramePipelines& operator=(const FramePipelines&) = delete;
    FramePipelines(FramePipelines&&) = delete;
    FramePipelines& operator=(FramePipelines&&) = delete;

    /// Create the modules, the layouts and the pipelines. Call once.
    ///
    /// Refuses a zero sample count and a sample count that is not 1, 2, 4 or 8, for the reason
    /// `ForwardFrame::build` refuses the same thing: a pipeline created with a count the frame's
    /// attachments do not have is a validation error at draw time rather than at creation time.
    [[nodiscard]] Status initialize(rhi::Device& device, const PipelineSetup& setup) noexcept;

    /// Release everything. Idempotent, and called by the destructor — a caller that tears down
    /// while the device is still busy calls `rhi::Device::wait_idle()` first, which is the same
    /// contract every other device-owning object in the tree states.
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const PipelineSetup& setup() const noexcept { return setup_; }

    [[nodiscard]] rhi::PipelineLayoutHandle layout() const noexcept { return layout_; }
    [[nodiscard]] rhi::DescriptorSetLayoutHandle set_layout(u32 set) const noexcept;
    [[nodiscard]] rhi::GraphicsPipelineHandle pipeline(FramePipelineKind kind) const noexcept;
    [[nodiscard]] rhi::SamplerHandle linear_clamp() const noexcept { return sampler_; }
    /// The sampler bound at (set 0, binding 2) — `cy/material.slang`'s `cyMaterialSampler`.
    ///
    /// NOT `linear_clamp()`, and the difference is the picture rather than a preference. The post
    /// chain samples a full-screen target and wants clamped edges and no mip chain; a material
    /// samples an authored texture across a UV that tiles and wants REPEAT and the cooked mip
    /// levels. One sampler for both would either tile the scene colour or read a wrapped surface
    /// at level 0 and alias.
    [[nodiscard]] rhi::SamplerHandle material_sampler() const noexcept { return material_sampler_; }

    /// How many pipeline states were created. The number that separates "the layer is wired up"
    /// from "the layer exists": temporal resolve is retained even when a frame leaves it unused.
    [[nodiscard]] u32 created() const noexcept { return created_; }

private:
    [[nodiscard]] Status create_modules(rhi::Device& device) noexcept;
    [[nodiscard]] Status create_layouts(rhi::Device& device) noexcept;
    [[nodiscard]] Status create_pipelines(rhi::Device& device, const PipelineSetup& setup) noexcept;
    [[nodiscard]] Status create_geometry_pipeline(rhi::Device& device, const PipelineSetup& setup,
                                                  FramePipelineKind kind) noexcept;
    [[nodiscard]] Status create_resolve_pipeline(rhi::Device& device,
                                                 const PipelineSetup& setup) noexcept;
    [[nodiscard]] Status create_temporal_pipeline(rhi::Device& device,
                                                  const PipelineSetup& setup) noexcept;

    rhi::Device* device_ = nullptr;
    PipelineSetup setup_;
    rhi::ShaderModuleHandle depth_vertex_;
    rhi::ShaderModuleHandle depth_fragment_;
    rhi::ShaderModuleHandle forward_vertex_;
    rhi::ShaderModuleHandle forward_fragment_;
    rhi::ShaderModuleHandle resolve_vertex_;
    rhi::ShaderModuleHandle resolve_fragment_;
    rhi::ShaderModuleHandle temporal_fragment_;
    rhi::DescriptorSetLayoutHandle sets_[kSetCount];
    rhi::PipelineLayoutHandle layout_;
    rhi::SamplerHandle sampler_;
    rhi::SamplerHandle material_sampler_;
    rhi::GraphicsPipelineHandle pipelines_[kFramePipelineKindCount];
    u32 created_ = 0;
    bool ready_ = false;
};

}  // namespace cy::rendering::pipeline
