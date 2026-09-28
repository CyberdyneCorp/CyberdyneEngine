// SPDX-License-Identifier: MIT
#pragma once
// Decals in the device frame: the table a fragment shader reads them from, its upload, and the
// view-block words that bind it.
//
// `rendering-lighting-and-shadows` — "Decals". What a decal IS — the projected box, the two fades,
// the ordering, the budget and its eviction — is `cy/rendering/lighting/decals.h`, which has no
// device. Its assignment to clusters, as an element type of its own bounded by its oriented box, is
// `FrameAssembly`'s, beside the lights and by the same pass. This module is the half that has a
// device: it turns a view's decals, in the order the assembly ranked them, into words a shader
// reads, and `cy/decal.slang` is the shader half of the same layout.
//
// ================================================================================================
// A TABLE OF WORDS, IN A TEXTURE, SO NOTHING ABOUT THE FRAME'S BINDINGS CHANGES
// ================================================================================================
//
// The forward frame reads its per-view data through a fixed set of bindings that five other
// modules' committed shaders are compiled against. A decal buffer as a new binding would change
// that set for all of them. Instead the table is a TEXTURE in the frame's bindless texture table —
// the seam ambient occlusion, contact shadows and the irradiance volume already use — and one
// `uint4` appended to the view block names its slot.
//
// `Rgba8Unorm`, one 32-bit word per texel, `kDecalTableWidth` texels a row. Eight-bit unorm because
// every device filters it, the frame's one sampler is linear, and a texel-centre sample of an
// 8-bit channel returns `k / 255` exactly — so the word comes back bit for bit, floats included.
// A half-float texture, which the irradiance volume uses, would round a decal's position to a
// centimetre at fifteen metres and an index past 2048 to a neighbour.
//
// ================================================================================================
// WHAT IS RELATIVE TO WHAT
// ================================================================================================
//
// Every position in the table is RELATIVE to `DecalTableInput::origin` — the frame's camera
// position — subtracted in double precision here, once, exactly as `build_gpu_light` does for a
// light. The frame has no world space, and a decal a kilometre out must not land a centimetre off.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/forward/cluster.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/lighting/decals.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

namespace cy::rendering::decals {

// --- The layout, `cy/decal.slang`'s ----------------------------------------------------------

inline constexpr u32 kDecalMagic = 0x43594443U;  // 'CYDC'
inline constexpr u32 kDecalTableVersion = 1;
inline constexpr u32 kDecalHeaderWords = 24;
inline constexpr u32 kDecalRecordWords = 36;
inline constexpr u32 kDecalTableWidth = 1024;

/// The coverage a decal's material draws over its box. `cy/decal.slang`'s `kCyDecalShape*`.
///
/// AUTHORED SHAPES, not measurements. A burn's outline, a ring marker and a growth's patches are
/// what an artist paints into a mask, and these are procedural stand-ins so a decal needs no
/// texture; `DecalMaterial::mask_texture` multiplies whichever shape is chosen when a material
/// has a painted mask.
enum class DecalShape : u32 {
    /// Full coverage: the whole box.
    Box = 0,
    /// A disc whose rim noise breaks up. `shape_a` noise frequency (cells across the decal),
    /// `shape_b` breakup amplitude (fraction of the radius).
    Splat,
    /// A band. `shape_a` its radius and `shape_b` its half width, both as fractions of the half
    /// extent — an RTS ground marker.
    Ring,
    /// Noise above a threshold, gone before the box's edge. `shape_a` noise frequency, `shape_b`
    /// the threshold in [0, 1] — growth: moss, lichen, damp.
    Patches,
    Count,
};

/// What a decal's material index resolves to. The requirement's "material reference": a
/// `DecalInstance` names one of these by `material_index`, and `pack_decal_table` resolves it.
///
/// Every value is what the decal WRITES, before lighting, blended by the decal's own
/// `DecalBlendWeights` — so these are surface properties in the frame's units (linear albedo,
/// perceptual roughness), never a colour on screen.
struct DecalMaterial {
    Vec3 albedo{0.5F, 0.5F, 0.5F};
    f32 roughness = 0.5F;
    f32 metallic = 0.0F;
    /// Radiance, in the frame's units. Weighted by `DecalBlendWeights::emission`.
    Vec3 emission{0.0F, 0.0F, 0.0F};
    DecalShape shape = DecalShape::Box;
    f32 shape_a = 0.0F;
    f32 shape_b = 0.0F;
    /// A bindless slot whose ALPHA multiplies the shape's coverage, or `kNoMaterialTexture`.
    u32 mask_texture = pipeline::kNoMaterialTexture;
    /// The decal's height at full coverage, in metres. The normal is tilted by its gradient — a
    /// char crust a couple of millimetres proud of a floor, a moss cushion a centimetre — and
    /// weighted by `DecalBlendWeights::normal` and the decal's `normal_strength`. Zero leaves the
    /// receiver's normal untouched.
    f32 relief_metres = 0.0F;
    /// The fraction of the box's depth over which the decal fades out at its near and far faces,
    /// so a box whose face cuts a curved receiver leaves no hard line. Zero is a hard box.
    f32 edge_softness = 0.0F;
    /// Varies the noise of `Splat` and `Patches`; the decal's own id is added, so two impacts of
    /// one material are not the same mark.
    u32 seed = 0;
};

/// What one view's table is built from.
struct DecalTableInput {
    /// The decals and the order the frame applies them in — `AssemblyView::decals` and
    /// `FrameAssembly::decal_order()`. Record `r` of the table is `decals[order[r]]`, and a cluster
    /// list names ranks, so the two MUST be the pair the assembly assigned.
    Span<const DecalInstance> decals;
    Span<const u32> order;
    /// Indexed by `DecalInstance::material_index`. An index past the end is refused.
    Span<const DecalMaterial> materials;
    /// The world position the frame's relative space is centred on: the camera.
    Vec3 origin{0.0F, 0.0F, 0.0F};

    /// OPTIONAL: carry the decal lists in the table, for a shader that has no cluster buffers of
    /// its own. Null leaves them out; the engine's forward frame reads the assembly's own lists
    /// from the buffers its light loop reads.
    const ClusterAssignment* clusters = nullptr;
    /// The grid `clusters` was assigned over, the world-to-view matrix, and the render extent in
    /// pixels. Read only with `clusters`.
    ClusterGrid grid{};
    Mat4 view = Mat4::identity();
    u32 width = 0;
    u32 height = 0;
};

/// Pack a view's decals into the table `cy/decal.slang` reads. `words` is replaced.
///
/// Refuses an order that is not a permutation of the decals' indices and a material index past the
/// material span, naming which — both produce a frame that draws the wrong decal where a decal
/// should be, which no picture would say.
[[nodiscard]] Status pack_decal_table(const DecalTableInput& input, Array<u32>& words) noexcept;

/// The table's rows at `kDecalTableWidth` words a row.
[[nodiscard]] constexpr u32 decal_table_rows(usize words) noexcept {
    return static_cast<u32>((words + kDecalTableWidth - 1U) / kDecalTableWidth);
}

// --- The view block ----------------------------------------------------------------------------

/// `decal_control[2]`'s bits, `cy/frame.slang`'s `kCyDecal*`.
///
/// `kDecalListsInTable`: read the decal lists from the table rather than from the frame's cluster
/// buffers — the path a sample's own shader takes, offered to the frame so the two can be compared.
/// `kDecalWalkAll`: apply every decal to every pixel with no cluster lookup — the declared
/// fallback, and the reference a clustered frame is held to.
inline constexpr u32 kDecalListsInTable = 1U;
inline constexpr u32 kDecalWalkAll = 2U;

/// Name the table in the frame's view block. A frame that never calls this uploads
/// `kNoMaterialTexture` and draws no decals, arithmetic for arithmetic the frame before decals.
void write_decal_frame(rhi::BindlessIndex slot, u32 rows, u32 flags,
                       pipeline::FrameViewData& view) noexcept;

// --- The texture -------------------------------------------------------------------------------

namespace detail {
/// What a recorded copy reads when its pass runs. Internal to `DecalTableTexture`.
struct DecalUploadRecording {
    rhi::BufferHandle staging;
    rhi::TextureHandle texture;
    u32 rows = 0;
};
}  // namespace detail

/// The table on the device.
///
/// GROWS AND NEVER SHRINKS: the texture is recreated only when a table needs more rows than it
/// has, and the rows past the table's end are left as they were — the shader reads the table's
/// own header for its extent, so they are never read. `rows()` is the texture's height, which is
/// what the shader needs to find a texel's centre.
class DecalTableTexture {
public:
    DecalTableTexture() noexcept = default;
    ~DecalTableTexture();

    DecalTableTexture(const DecalTableTexture&) = delete;
    DecalTableTexture& operator=(const DecalTableTexture&) = delete;
    DecalTableTexture(DecalTableTexture&&) = delete;
    DecalTableTexture& operator=(DecalTableTexture&&) = delete;

    /// Remember the device. Nothing is created until the first `upload`.
    void initialize(rhi::Device& device, Allocator& allocator) noexcept;
    void shutdown() noexcept;

    /// Copy `words` to the device in a device frame of its own: call it OUTSIDE the host's
    /// `begin_frame`/`end_frame`, as `MaterialTextureTable::upload` and
    /// `ProbeVolumeTexture::upload` are.
    [[nodiscard]] Status upload(Span<const u32> words) noexcept;

    /// Create the texture with at least `rows` rows now, for a caller that writes the view into a
    /// descriptor set once, before its first frame, and uploads with `declare_upload` inside
    /// frames — where the texture can no longer be recreated.
    [[nodiscard]] Status reserve(u32 rows) noexcept;

    /// Declare the upload INTO a frame's graph, for a table that is known only once the frame is
    /// assembled — its lists are the assembly's. The pass is declared NOW, before the assembly
    /// declares the pass that samples the table, so the graph orders the copy first; the words are
    /// handed over afterwards with `stage`, which must happen before the graph executes, because
    /// the copy reads the staging buffer when it records. The resource returned is what the
    /// sampling pass declares a `FragmentSampledRead` of.
    ///
    /// ONE TEXTURE AND ONE STAGING BUFFER, so the previous frame's upload and reads must have
    /// completed when this is called — a capture that reads its frame back has. The whole texture
    /// is rewritten, so it is imported `Undefined`: `reserve` it first, because inside a frame it
    /// cannot be recreated.
    [[nodiscard]] Expected<ResourceId, Error> declare_upload(RenderGraph& graph) noexcept;

    /// The words `declare_upload`'s pass copies. Refuses a table larger than the reserved texture.
    [[nodiscard]] Status stage(Span<const u32> words) noexcept;

    /// The (slot, view) pair the frame's set 0 needs, at a slot the caller chose.
    [[nodiscard]] pipeline::MaterialTextureSlot slot(rhi::BindlessIndex index) const noexcept {
        return pipeline::MaterialTextureSlot{index, view_};
    }
    [[nodiscard]] rhi::TextureViewHandle view() const noexcept { return view_; }
    [[nodiscard]] bool ready() const noexcept { return !view_.is_null(); }
    [[nodiscard]] u32 rows() const noexcept { return rows_; }
    [[nodiscard]] u32 uploads() const noexcept { return uploads_; }

private:
    [[nodiscard]] Status recreate(u32 rows) noexcept;
    void release_texture() noexcept;

    rhi::Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    rhi::TextureHandle texture_;
    rhi::TextureViewHandle view_;
    /// `declare_upload`'s, kept until the next call or `shutdown`: the copy reads it when the
    /// frame executes, after this object has returned.
    rhi::BufferHandle staging_;
    detail::DecalUploadRecording recording_;
    u32 rows_ = 0;
    u32 uploads_ = 0;
};

}  // namespace cy::rendering::decals
