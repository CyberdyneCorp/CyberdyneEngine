#pragma once
// Virtual texturing's device half: the feedback pass, the GPU resolve, and the page table a shader
// samples. M7 tasks 4.1 and 4.2.
//
// `src/servers/render/virtual_texturing/README.md` names this module in advance and says what it
// owes: "The Vulkan-side upload, the page table image, the sampling shaders and the analytic
// derivative reconstruction under a visibility buffer are `src/rendering/` work and arrive with M7.
// `apply_staged()`'s list is exactly the buffer that uploader copies from, which is why the
// batching lives here rather than there." This is that module, and `upload_page_table` is that
// uploader.
//
// ================================================================================================
// THE TWO CLAIMS THIS FILE EXISTS TO MAKE TRUE, AND HOW EACH IS CHECKED
// ================================================================================================
//
// **A feedback buffer written by a shader and resolved without a per-pixel stream reaching the
// CPU.** `record_feedback` runs one thread per pixel and increments one word per PAGE;
// `resolve_feedback` compacts the non-zero words into `(address, samples)` pairs on the device.
// `read_back()` maps the compact list and the three header words and NOTHING ELSE —
// `feedback_bytes_read()` is that number, and the suite asserts it against the pixel count so that
// "no per-pixel stream" is a measurement.
//
// **A page table sampled by a shader, and the mip tail's guarantee held on the device.**
// `upload_page_table` copies the CPU table into a buffer indexed by `PageTable::linear_index`, and
// `sample_pages` walks from the level asked for towards the coarsest exactly as
// `VirtualTextureSystem::sample()` does. The suite dispatches it over the WHOLE address space and
// asserts `missing` is false everywhere once the tail is resident — and true somewhere before it
// is, so the case is not vacuously green.
//
// ================================================================================================
// WHAT IT REFUSES
// ================================================================================================
//
// A texture whose pyramid is larger than `PageTable::kFlatEntryLimit`. The feedback count array and
// the page-table buffer are both one entry per addressable page, which is the flat form's cost and
// the flat form's ceiling; a 512k-texel virtual space needs a hash on the device and that is not
// here. Refusing names the limit rather than allocating 128 MB of mostly-zero words.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/graph/graph.h>
#include <cy/servers/render/virtual_texturing/address.h>
#include <cy/servers/render/virtual_texturing/feedback.h>
#include <cy/servers/render/virtual_texturing/page_table.h>

namespace cy::rendering::vt {

using render::vt::FeedbackRequest;
using render::vt::PageTable;
using render::vt::VirtualAddress;
using render::vt::VirtualTextureDesc;

/// How the frame's feedback dispatch is configured.
struct FeedbackSettings {
    /// The grid the recording pass runs over — a stand-in for the shading pass's pixels. One thread
    /// each.
    u32 grid_width = 256;
    u32 grid_height = 256;
    /// `virtual-texturing`'s density lever: one sample per this many pixels. Clamped into
    /// [`kMinFeedbackDensity`, `kMaxFeedbackDensity`], which is where the requirement puts it.
    u32 density = render::vt::kMinFeedbackDensity;
    /// Added to the mip each pixel asks for. Positive asks for coarser pages, which is what texture
    /// pressure does to a feedback stream.
    i32 mip_bias = 0;
    /// How many compacted requests the output may hold. Overflow is COUNTED, never silent.
    u32 request_capacity = 1024;
};

/// What one frame's dispatches produced, read back.
struct VirtualTextureFrameReadback {
    /// One entry per page that was sampled at least once, in ascending mip-major address order.
    Span<const FeedbackRequest> requests;
    /// How many samples those requests account for. The sum of every `samples` field, computed on
    /// the device.
    u32 total_samples = 0;
    /// Requests that did not fit in `request_capacity`. Reported rather than dropped silently, for
    /// the reason `FeedbackBuffer::dropped()` exists.
    u32 dropped = 0;
    /// How many bytes the CPU actually mapped this frame. The number that makes "a per-pixel
    /// request stream SHALL NOT reach the CPU" a measurement rather than a claim.
    u64 bytes_read = 0;
};

/// One page's answer from `sample_pages`, in the layout the shader writes.
struct GpuSampleResult {
    u32 missing = 1;
    u32 resident_mip = render::vt::kNoResidentMip;
    u32 physical_tile = render::vt::kNoPhysicalTile;
    u32 deficit = 0;
};

static_assert(sizeof(GpuSampleResult) == 16, "GpuSampleResult is one shader-visible word4");

/// The device half of one virtual texture's frame.
///
/// The lifecycle is: `create` once for a texture description, then per frame `upload_page_table`,
/// `declare`, execute the graph, and `read_back`. `declare` records nothing itself — it adds a
/// clear, three dispatches and a read-back copy, and the graph derives every barrier between them.
class VirtualTextureFrame {
public:
    VirtualTextureFrame() = default;
    ~VirtualTextureFrame();

    VirtualTextureFrame(const VirtualTextureFrame&) = delete;
    VirtualTextureFrame& operator=(const VirtualTextureFrame&) = delete;
    VirtualTextureFrame(VirtualTextureFrame&&) = delete;
    VirtualTextureFrame& operator=(VirtualTextureFrame&&) = delete;

    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const VirtualTextureDesc& desc,
                                const FeedbackSettings& settings) noexcept;

    /// Change the density lever between frames. `virtual-texturing` requires it to be "driven by
    /// camera motion, texture pressure, resolution, and budget", so it is a setter rather than a
    /// creation parameter.
    void set_density(u32 pixels_per_sample) noexcept;
    [[nodiscard]] u32 density() const noexcept { return settings_.density; }
    void set_mip_bias(i32 bias) noexcept;

    /// Copy the CPU page table into the buffer the sampling shader reads, entry for entry.
    ///
    /// `PageTable::linear_index` decides where each entry goes, which is why that function is
    /// public — a second implementation of the arithmetic here would be a sample resolving to the
    /// wrong page rather than to no page.
    [[nodiscard]] Status upload_page_table(const PageTable& table) noexcept;

    [[nodiscard]] Status declare(RenderGraph& graph) noexcept;

    /// The compacted feedback. Valid until the next frame's execution.
    [[nodiscard]] Expected<VirtualTextureFrameReadback, Error> read_back() noexcept;

    /// The whole address space's sample results, indexed by `PageTable::linear_index`. Read back
    /// only because a test asserts on it; a shipped frame samples the table in the shader that
    /// needed the texel and never maps this.
    [[nodiscard]] Expected<Span<const GpuSampleResult>, Error> sample_results() noexcept;

    /// Which page a given pixel of the recording grid would ask for, computed on the CPU exactly as
    /// `record_feedback` computes it. The mirror the suite compares the device against.
    [[nodiscard]] VirtualAddress address_of_pixel(u32 x, u32 y) const noexcept;
    /// Whether that pixel reports at the current density. `FeedbackBuffer::samples_pixel`'s rule.
    [[nodiscard]] bool samples_pixel(u32 x, u32 y) const noexcept;

    [[nodiscard]] u32 entry_count() const noexcept { return entry_count_; }
    [[nodiscard]] const VirtualTextureDesc& description() const noexcept { return desc_; }

    void destroy() noexcept;

private:
    /// One mip level, as the shader reads it. Four words: tiles across, tiles down, the level's
    /// first entry in the flat table, and how many entries one layer of it has.
    struct MipLevel {
        u32 tiles_x = 0;
        u32 tiles_y = 0;
        u32 first_entry = 0;
        u32 entries_per_layer = 0;
    };

    /// The constant block all three entry points read. 48 bytes.
    struct View {
        u32 texture_id = 0;
        u32 mip_count = 0;
        u32 layers = 0;
        u32 entry_count = 0;
        u32 density = 1;
        u32 grid_width = 0;
        u32 grid_height = 0;
        u32 request_capacity = 0;
        i32 mip_bias = 0;
        u32 reserved0 = 0;
        u32 reserved1 = 0;
        u32 reserved2 = 0;
    };

    struct Buffers {
        rhi::BufferHandle view;
        rhi::BufferHandle mips;
        rhi::BufferHandle page_samples;
        rhi::BufferHandle page_samples_zero;
        rhi::BufferHandle resolve_header;
        rhi::BufferHandle requests;
        rhi::BufferHandle page_table;
        rhi::BufferHandle sample_out;
        rhi::BufferHandle header_readback;
        rhi::BufferHandle requests_readback;
        rhi::BufferHandle sample_readback;
    };

    [[nodiscard]] Status create_pipelines() noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] Status write_descriptors() noexcept;
    void write_view() noexcept;

    static void record_clear(const PassContext& context, void* user) noexcept;
    static void record_feedback(const PassContext& context, void* user) noexcept;
    static void record_resolve(const PassContext& context, void* user) noexcept;
    static void record_sample(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    VirtualTextureDesc desc_{};
    FeedbackSettings settings_{};
    u32 entry_count_ = 0;
    u64 bytes_read_ = 0;

    rhi::ShaderModuleHandle record_shader_;
    rhi::ShaderModuleHandle resolve_shader_;
    rhi::ShaderModuleHandle sample_shader_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::ComputePipelineHandle record_pipeline_;
    rhi::ComputePipelineHandle resolve_pipeline_;
    rhi::ComputePipelineHandle sample_pipeline_;
    rhi::DescriptorSetHandle descriptor_set_;
    Buffers buffers_{};
};

}  // namespace cy::rendering::vt
