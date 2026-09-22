#pragma once
// Residency for the textures a material samples: cooked pixels in, a bindless slot out. M11.c task
// 3.7.
//
// ================================================================================================
// THE THREE ABSENCES THIS CLOSES, AS M11.c's SPIKE MEASURED THEM
// ================================================================================================
//
// > `RenderServer::create_texture` stores a `TextureRecord` with a name, a format, extents and a
// > byte count AND NO PIXELS. There is no residency or upload path from a cooked texture to a
// > device image anywhere under `src/`.
// >
// > The engine's own global bindless table is WRITE-ONLY. [...] It is never put in a pipeline
// > layout and `bindless_set_` is never bound in a command buffer, so every `BindlessIndex` the
// > device hands out names a descriptor no shader can reach.
//
// The first sentence is not a defect in the render server and this module does not fix it there.
// `RenderServer` is the device-free scene model — it says so, and it computes a texture's byte
// count "from the description rather than [by asking] a device" precisely because it has none. The
// pixels belong in the layer that HAS a device, which is this one. So the server keeps owning the
// record and this table owns the image, keyed by the server's own handle: `slot_of(handle)` is the
// join, and nothing above has to hold two identities for one texture.
//
// The second sentence is now false, and `rhi::Device::global_texture_table_layout()` is what made
// it false. This module is the first consumer in the tree: what `upload` returns is an index into
// the table a pipeline layout can name and a command buffer can bind.
//
// ================================================================================================
// WHY THE UPLOAD IS A RENDER GRAPH AND NOT A COPY
// ================================================================================================
//
// `rhi::CommandBuffer` HAS NO BARRIER CALL, by requirement — "The RHI's public recording API SHALL
// NOT expose barriers, image layout transitions, or queue ownership transfers" — so a module that
// wanted to copy into an image and leave it sampleable cannot say so directly. It declares it: one
// pass writes every image as a transfer destination, a second READS them as a fragment sampled
// read, and the graph derives the transition between. Without the second pass the images stay in
// the transfer layout and the frame that samples them — a different graph, in which they are not
// resources at all — reads them there. That is the same argument `samples/12-beauty` makes for its
// own uploads and `samples/03-first-light` makes for a host read of a buffer.
//
// ONE SUBMISSION FOR THE WHOLE BATCH, and a single staging buffer behind it: a per-texture submit
// and wait costs a device round trip each, and the batch is what a level load actually has.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/pipeline/frame_pipelines.h>
#include <cy/servers/render/server.h>

namespace cy::rendering::pipeline {

/// One texture to make resident: the record the render server holds, and the pixels it does not.
///
/// `pixels` is the mip chain the cooker produced, level 0 first, each level tightly packed, and it
/// must be exactly `TextureRecord::bytes` long — the number the server computed from the
/// description. A payload that disagrees is REFUSED naming both numbers rather than uploaded as far
/// as it goes: a chain cut short leaves the tail mips undefined, and an undefined mip is a texture
/// that looks right until something is minified.
struct TextureUpload {
    render::TextureHandle texture;
    Span<const u8> pixels;
};

/// The material textures resident on the device, and their slots in the global table.
class MaterialTextureTable {
public:
    MaterialTextureTable() noexcept = default;
    ~MaterialTextureTable();

    MaterialTextureTable(const MaterialTextureTable&) = delete;
    MaterialTextureTable& operator=(const MaterialTextureTable&) = delete;
    MaterialTextureTable(MaterialTextureTable&&) = delete;
    MaterialTextureTable& operator=(MaterialTextureTable&&) = delete;

    /// Take the device's global table and give it its one sampler.
    ///
    /// REFUSES ON A DEVICE WITH NO TABLE — the compatibility path, where `global_texture_table()`
    /// is a null handle — rather than uploading images no shader could reach.
    /// `rhi-and-render-graph` requires the reduced capability to be reported rather than silently
    /// degraded, and a caller that gets `Unsupported` here is a caller that can bind its own set
    /// instead.
    [[nodiscard]] Status initialize(rhi::Device& device, Allocator& allocator,
                                    const rhi::SamplerDescription& sampler) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept { return device_ != nullptr; }

    /// Upload a batch and make every image of it sampleable. One submission.
    [[nodiscard]] Status upload(const render::RenderServer& server,
                                Span<const TextureUpload> uploads) noexcept;

    /// The slot a shader indexes `cyMaterialTextures[]` with, or `kInvalidBindlessIndex` for a
    /// texture this table has not uploaded.
    [[nodiscard]] rhi::BindlessIndex slot_of(render::TextureHandle texture) const noexcept;

    /// Every resident texture as a (slot, view) pair, written into `out`; the return is how many
    /// are resident, so a caller that sized `out` too small can tell.
    ///
    /// WHAT THE FRAME'S OWN SET 0 NEEDS. `set()` above is the DEVICE's table and is what a program
    /// whose set 0 is only that table binds — the material probe of `render.material_binding` is
    /// one. The engine's forward pipeline cannot: its set 0 also carries `cy/globals.slang`'s
    /// block at binding 0, and a pipeline binds one set per index. So `FrameBindings` writes the
    /// same views, at the same slots, into the set that carries both — see
    /// `FrameBindings::set_material_textures`. The slots are this table's either way, which is
    /// what stops the two descriptions of one texture from disagreeing.
    [[nodiscard]] usize slots(Span<MaterialTextureSlot> out) const noexcept;

    /// The two halves a pipeline needs: the layout to name as set `rhi::kGlobalTableSet`, and the
    /// set to bind there. Both are the DEVICE's — this module does not own them and does not
    /// destroy them.
    [[nodiscard]] rhi::DescriptorSetLayoutHandle layout() const noexcept;
    [[nodiscard]] rhi::DescriptorSetHandle set() const noexcept;

    [[nodiscard]] usize resident() const noexcept { return entries_.size(); }
    /// What the resident images cost on the device, which is the sum of what the server computed.
    [[nodiscard]] u64 resident_bytes() const noexcept { return resident_bytes_; }

private:
    /// Create a view per uploaded image and take a slot for it. Transactional: a failure leaves the
    /// table exactly as it was, so `upload`'s own failure path can destroy every image of the batch
    /// without destroying one of them twice.
    [[nodiscard]] Status publish(const render::RenderServer& server,
                                 Span<const TextureUpload> uploads,
                                 Span<const rhi::TextureHandle> images) noexcept;
    /// One image's view and its slot. Leaves nothing behind when it fails.
    [[nodiscard]] Status publish_one(const render::TextureRecord* record,
                                     render::TextureHandle texture,
                                     rhi::TextureHandle image) noexcept;
    /// Give back the slots and views of every entry from `first` on. The IMAGES are not destroyed
    /// here: they are the caller's batch and its own failure path owns them.
    void unpublish_from(const render::RenderServer& server, usize first) noexcept;

    struct Entry {
        render::TextureHandle texture;
        rhi::TextureHandle image;
        rhi::TextureViewHandle view;
        rhi::BindlessIndex slot = rhi::kInvalidBindlessIndex;
    };

    rhi::Device* device_ = nullptr;
    Allocator* allocator_ = nullptr;
    rhi::SamplerHandle sampler_;
    Array<Entry> entries_;
    u64 resident_bytes_ = 0;
};

/// The device format for a cooked one, or `Format::Undefined` where the engine has no device format
/// for it.
///
/// NOT A TOTAL FUNCTION AND DELIBERATELY SO. `render::TextureFormat` carries the ASTC variants "on
/// purpose: a cooked asset for a mobile target is stored in one of these whether or not the machine
/// doing the cooking has a device that can sample it", so a cook for a mobile target reaching a
/// desktop device is a real state and `Undefined` is the honest answer to it. The caller refuses;
/// guessing a format here would sample the bytes as something they are not.
[[nodiscard]] rhi::Format device_format_of(render::TextureFormat format) noexcept;

}  // namespace cy::rendering::pipeline
