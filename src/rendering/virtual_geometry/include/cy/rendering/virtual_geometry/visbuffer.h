#pragma once
// The visibility buffer, material classification and binning, and attribute reconstruction.
// M7 task 7.4.
//
// `virtual-geometry` — "Visibility buffer and material resolve".
//
// ================================================================================================
// WHAT A PIXEL CARRIES, AND WHAT IT DOES NOT
// ================================================================================================
//
// Two words: the index of the visible-cluster record, and the triangle within that cluster. The
// record names the instance and the cluster; the cluster names its page, its material and its
// geometry. That is "instance identifier, primitive identifier" exactly, and the third thing the
// requirement asks for — "sufficient information to recover barycentrics" — is satisfied by it
// being enough to PROJECT the triangle again. `reconstruct_surface()` does that, and so does the
// resolve shader; the suite compares them.
//
// Storing the barycentrics instead would be four more bytes a pixel to save an arithmetic the
// resolve has to do anyway, because a derivative needs the projected triangle and not the weights.
//
// ================================================================================================
// THE CPU HALF IS THE REFERENCE, NOT A FALLBACK
// ================================================================================================
//
// `bin_by_material()` and `reconstruct_surface()` are what `test_visbuffer.cpp` compares the
// device against, the same way `traverse_reference()` is what the traversal dispatch is compared
// against. They are also what the Forward+ path uses — `virtual-geometry` requires virtual geometry
// to work there too, "with the limitations documented", and material binning is precisely the
// benefit that is unavailable on that path.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/virtual_geometry/gpu.h>

namespace cy::rendering::vg {

/// No surface at this pixel.
inline constexpr u32 kNoSurface = 0xFFFFFFFFU;

/// One pixel of the visibility buffer. Two words, matching `uint2` in `vg_visbuffer.slang`.
struct VisibilitySample {
    /// Index into the visible cluster list the traversal produced.
    u32 visible = kNoSurface;
    u32 triangle = 0;

    [[nodiscard]] constexpr bool covered() const noexcept { return visible != kNoSurface; }
};
static_assert(sizeof(VisibilitySample) == 8,
              "VisibilitySample must match cy/vg/vg_visbuffer.slang");

/// What the resolve recovers from an identified primitive.
struct SurfaceAttributes {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 0.0F, 1.0F};
    Vec2 uv{0.0F, 0.0F};
    Vec3 barycentric{0.0F, 0.0F, 0.0F};
    u32 material = 0;
};

/// Reconstruct one pixel's surface from the identified instance and primitive.
///
/// `pixel` is the pixel's CENTRE in viewport pixels, which is what the rasteriser tested and what
/// the shader recovers against — passing the corner instead shifts every attribute by half a texel
/// and looks like a UV bug.
[[nodiscard]] Expected<SurfaceAttributes, Error> reconstruct_surface(
    const DecodedAsset& asset, const GeometryInstance& instance, const VisibleCluster& visible,
    u32 triangle, const Mat4& world_to_clip, Vec2 pixel, u32 width, u32 height,
    Allocator& allocator = current_allocator()) noexcept;

/// Pixels grouped by material. `virtual-geometry` — "Many materials, few passes": "WHEN a view
/// contains thousands of distinct materials THEN material resolve SHALL evaluate them in bins
/// rather than issuing per-material draws."
struct MaterialBins {
    explicit MaterialBins(Allocator& allocator) noexcept;

    MaterialBins(const MaterialBins&) = delete;
    MaterialBins& operator=(const MaterialBins&) = delete;
    MaterialBins(MaterialBins&&) noexcept = default;
    MaterialBins& operator=(MaterialBins&&) noexcept = default;

    /// One entry per material.
    Array<u32> counts;
    /// `material_count + 1` entries: bin `m` is `pixels[offsets[m] .. offsets[m + 1])`.
    Array<u32> offsets;
    /// Pixel indices, grouped. Sorted within a bin by the CPU reference; the GPU's order within a
    /// bin is the order its atomics ran, which is why the suite compares bins as SETS.
    Array<u32> pixels;

    [[nodiscard]] Span<const u32> bin(u32 material) const noexcept;
};

[[nodiscard]] Status bin_by_material(Span<const VisibilitySample> visbuffer,
                                     Span<const VisibleCluster> visible, u32 material_count,
                                     MaterialBins& out) noexcept;

// ================================================================================================
// THE DEVICE PATH
// ================================================================================================

struct VisbufferOptions {
    u32 width = 512;
    u32 height = 512;
    /// The number of bins. A material index at or above it is not binned and not resolved, which is
    /// reported rather than silently dropped.
    u32 material_count = 16;
};

/// What one frame's visibility pass produced, read back after the frame completed.
struct VisbufferReadback {
    explicit VisbufferReadback(Allocator& allocator) noexcept;

    VisbufferReadback(const VisbufferReadback&) = delete;
    VisbufferReadback& operator=(const VisbufferReadback&) = delete;
    VisbufferReadback(VisbufferReadback&&) noexcept = default;

    Array<VisibilitySample> samples;
    Array<u32> bin_counts;
    Array<u32> bin_offsets;
    Array<u32> bin_pixels;
    /// The resolved surface per pixel: the world normal mapped into [0, 1] in xyz, and the u
    /// coordinate in w. What the M7 resolve evaluates; a material program replaces it at section 6.
    Array<Vec4> resolved;

    [[nodiscard]] u32 covered_pixels() const noexcept;
};

/// The visibility buffer and its resolve, on the device. Consumes the traversal's visible list
/// without a round trip through the host.
class VisbufferPass {
public:
    VisbufferPass(Allocator& allocator, rhi::Device& device) noexcept;
    ~VisbufferPass();

    VisbufferPass(const VisbufferPass&) = delete;
    VisbufferPass& operator=(const VisbufferPass&) = delete;
    VisbufferPass(VisbufferPass&&) = delete;
    VisbufferPass& operator=(VisbufferPass&&) = delete;

    /// `payload` is the concatenated page payload of every asset in `scene`, in the order the
    /// assets were added, and `offsets` is where each asset's payload starts in it. The pass reads
    /// it as bytes: at M7 the whole payload is resident and the geometry cache's page table gates
    /// traversal rather than rasterisation, which README.md states plainly.
    [[nodiscard]] Status initialise(const GpuScene& scene, Span<const DecodedAsset* const> assets,
                                    Span<const u8> payload, Span<const u32> payload_offsets,
                                    const VisbufferOptions& options) noexcept;

    /// Declare the visibility pass and the resolve into `graph`, reading the traversal's buffers.
    [[nodiscard]] Status record(RenderGraph& graph, const GpuTraversal& traversal,
                                const Mat4& world_to_clip) noexcept;

    [[nodiscard]] Status read_back(VisbufferReadback& out) const noexcept;

private:
    struct PassState {
        VisbufferPass* self = nullptr;
        u32 pass = 0;
        u32 groups = 0;
    };

    [[nodiscard]] Expected<rhi::BufferHandle, Error> make_buffer(const char* name, u64 bytes,
                                                                 rhi::BufferUsage usage,
                                                                 rhi::MemoryUse memory) noexcept;
    [[nodiscard]] Status upload(rhi::BufferHandle target, const void* data, u64 bytes) noexcept;
    static void record_pass(const PassContext& context, void* user) noexcept;
    void dispatch(const PassContext& context, const PassState& state) noexcept;

    Allocator& allocator_;
    rhi::Device& device_;
    VisbufferOptions options_;
    bool initialised_ = false;
    u32 vertex_stride_ = 0;
    u32 normal_offset_ = 0xFFFFFFFFU;
    u32 uv_offset_ = 0xFFFFFFFFU;
    f32 position_scale_ = 1.0F;
    f32 normal_scale_ = 1.0F;
    f32 uv_scale_ = 1.0F;
    u8 push_[128] = {};

    rhi::BufferHandle geometry_;
    rhi::BufferHandle payload_;
    rhi::BufferHandle visbuffer_;
    rhi::BufferHandle depth_;
    rhi::BufferHandle bin_counts_;
    rhi::BufferHandle bin_offsets_;
    rhi::BufferHandle bin_cursor_;
    rhi::BufferHandle bin_pixels_;
    rhi::BufferHandle resolved_;
    rhi::BufferHandle vis_args_;
    rhi::BufferHandle staging_;
    rhi::BufferHandle readback_;

    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::DescriptorSetHandle descriptors_;
    rhi::ShaderModuleHandle modules_[7];
    rhi::ComputePipelineHandle pipelines_[7];

    Array<PassState> states_;
};

}  // namespace cy::rendering::vg
