#include <cy/rendering/pipeline/material_textures.h>

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/servers/render/types.h>

#include <cstring>

namespace cy::rendering::pipeline {
namespace {

/// One copy and the image it lands in. `rhi::BufferTextureCopy` is the RHI's struct and has no room
/// for a destination, so the pair is kept here rather than in two arrays that could fall out of
/// step.
struct PlannedCopy {
    rhi::BufferTextureCopy region;
    rhi::TextureHandle destination;
};

struct UploadRecording {
    const Array<PlannedCopy>* copies = nullptr;
    rhi::BufferHandle staging;
};

void record_uploads(const PassContext& context, void* user) noexcept {
    const auto* recording = static_cast<const UploadRecording*>(user);
    for (const PlannedCopy& copy : *recording->copies) {
        context.commands->copy_buffer_to_texture(
            recording->staging, copy.destination,
            Span<const rhi::BufferTextureCopy>(&copy.region, 1));
    }
}

[[nodiscard]] u32 mip_extent(u32 base, u32 level) noexcept {
    const u32 shifted = base >> level;
    return shifted != 0 ? shifted : 1U;
}

/// Round `offset` up to `alignment`, where zero and one both mean "no constraint".
[[nodiscard]] u64 align_up(u64 offset, u64 alignment) noexcept {
    if (alignment <= 1) {
        return offset;
    }
    const u64 remainder = offset % alignment;
    return remainder == 0 ? offset : offset + (alignment - remainder);
}

/// An offset both constraints accept. A buffer-to-image copy's source offset must be a multiple of
/// the device's own copy alignment AND of the texel block size, and the larger of the two is the
/// answer because both are powers of two on every device and in every format the engine cooks — 4
/// to 512 bytes on one side, 8 or 16 on the other, so the larger IS a multiple of the smaller.
/// Written down rather than assumed: a device reporting a copy alignment that was not a power of
/// two would need a least common multiple, and this would quietly pick a number one of them
/// rejects.
[[nodiscard]] u64 staging_alignment(u64 copy_alignment, u64 block_bytes) noexcept {
    return copy_alignment > block_bytes ? copy_alignment : block_bytes;
}

/// What `upload` builds before it touches the device again: the images, the copies into them, and
/// where each upload's pixels sit in the one staging buffer they share.
struct UploadPlan {
    Array<rhi::TextureHandle> images;
    Array<PlannedCopy> copies;
    Array<u64> starts;
    u64 staging_bytes = 0;

    explicit UploadPlan(Allocator& allocator) noexcept
        : images(allocator), copies(allocator), starts(allocator) {}
};

/// The record for one upload, checked against what the caller handed over.
[[nodiscard]] Expected<const render::TextureRecord*, Error> checked_record(
    const render::RenderServer& server, const TextureUpload& upload) noexcept {
    const render::TextureRecord* record = server.texture(upload.texture);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound,
                    "a texture upload names a handle the render server does not have");
    }
    if (record->array_layers > 1) {
        return fail(ErrorCode::Unsupported,
                    "this residency path uploads one layer: an array texture needs a copy per "
                    "layer and a view that says so");
    }
    if (upload.pixels.size() != record->bytes) {
        // NOT TRUNCATED AND NOT PADDED. A chain cut short leaves its tail mips undefined, and an
        // undefined mip is a texture that looks correct until something is minified — which is the
        // hardest class of texture defect to see in a still.
        return fail(ErrorCode::InvalidArgument,
                    "a texture upload's pixels are not the size the render server computed for its "
                    "record");
    }
    return record;
}

/// One copy per mip level, tightly packed from `start`.
[[nodiscard]] Status plan_copies(const render::TextureRecord& record, rhi::TextureHandle image,
                                 u64 start, UploadPlan& plan) noexcept {
    u64 offset = start;
    for (u32 level = 0; level < record.mip_levels; ++level) {
        const u32 width = mip_extent(record.width, level);
        const u32 height = mip_extent(record.height, level);
        PlannedCopy copy;
        copy.destination = image;
        copy.region.buffer_offset = offset;
        copy.region.mip_level = static_cast<u16>(level);
        copy.region.texture_extent = rhi::Extent3D{width, height, 1};
        if (Status pushed = plan.copies.push_back(copy); !pushed) {
            return pushed;
        }
        offset += render::texture_format_byte_size(record.format, width, height);
    }
    return ok();
}

/// One image per upload, and where its pixels go in the staging buffer.
[[nodiscard]] Status plan_uploads(rhi::Device& device, const render::RenderServer& server,
                                  Span<const TextureUpload> uploads, UploadPlan& plan) noexcept {
    const u64 copy_alignment = device.capabilities().limits().optimal_buffer_copy_offset_alignment;
    for (const TextureUpload& upload : uploads) {
        Expected<const render::TextureRecord*, Error> record = checked_record(server, upload);
        if (!record) {
            return make_unexpected(record.error());
        }
        const render::TextureRecord& stored = **record;
        const rhi::Format format = device_format_of(stored.format);
        if (format == rhi::Format::Undefined) {
            return fail(ErrorCode::Unsupported,
                        "a cooked texture is in a format this device has none for; "
                        "device_format_of() names which");
        }

        rhi::TextureDescription description;
        description.name = stored.name.c_str();
        description.format = format;
        description.extent = rhi::Extent3D{stored.width, stored.height, 1};
        description.mip_levels = stored.mip_levels;
        description.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
        Expected<rhi::TextureHandle, Error> image = device.create_texture(description);
        if (!image) {
            return make_unexpected(image.error());
        }
        if (Status pushed = plan.images.push_back(*image); !pushed) {
            return pushed;
        }

        const u64 block_bytes = render::texture_format_info(stored.format).bytes_per_block;
        const u64 start =
            align_up(plan.staging_bytes, staging_alignment(copy_alignment, block_bytes));
        if (Status pushed = plan.starts.push_back(start); !pushed) {
            return pushed;
        }
        if (Status planned = plan_copies(stored, *image, start, plan); !planned) {
            return planned;
        }
        plan.staging_bytes = start + stored.bytes;
    }
    return ok();
}

/// Import every image, copy into it, and declare the sampled read that makes the graph leave it in
/// a layout a fragment stage can read. See the header for why the second pass is not optional.
[[nodiscard]] Status declare_passes(RenderGraph& graph, const UploadPlan& plan,
                                    UploadRecording& recording, rhi::Device& device,
                                    Allocator& allocator) noexcept {
    Array<ResourceId> imported(allocator);
    for (const rhi::TextureHandle image : plan.images) {
        const rhi::TextureDescription* description = device.texture_description(image);
        if (description == nullptr) {
            return fail(ErrorCode::Internal, "an image this table just created has no description");
        }
        // THE DESCRIPTION IS ASKED OF THE DEVICE rather than restated: the graph creates the view
        // it barriers from this request, and Vulkan refuses a view whose format differs from its
        // image's unless the image was created mutable.
        TextureRequest request;
        request.name = description->name;
        request.format = description->format;
        request.width = description->extent.width;
        request.height = description->extent.height;
        request.mip_levels = description->mip_levels;
        if (Status pushed =
                imported.push_back(graph.import_texture(request, image, rhi::ImageUse::Undefined));
            !pushed) {
            return pushed;
        }
    }

    auto uploads = graph.add_pass("material texture uploads", rhi::QueueKind::Graphics);
    for (const ResourceId id : imported) {
        uploads.write(id, rhi::Access::TransferWrite);
    }
    uploads.record(&record_uploads, &recording);

    auto residency = graph.add_pass("material texture residency", rhi::QueueKind::Graphics);
    for (const ResourceId id : imported) {
        residency.read(id, rhi::Access::FragmentSampledRead);
    }
    residency.side_effect();
    return graph.status();
}

/// Stage the pixels, run the two passes, and wait. ONE SUBMISSION for the whole batch: a submit and
/// a wait per texture costs a device round trip each, and a batch is what a level load actually
/// has.
[[nodiscard]] Status run_uploads(rhi::Device& device, Allocator& allocator, const UploadPlan& plan,
                                 Span<const TextureUpload> uploads) noexcept {
    rhi::BufferDescription staging;
    staging.name = "material texture staging";
    staging.size = plan.staging_bytes;
    staging.usage = rhi::BufferUsage::TransferSource;
    staging.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(staging);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    auto* mapped = static_cast<u8*>(device.buffer_mapped_pointer(*buffer));
    if (mapped == nullptr) {
        device.destroy_buffer(*buffer);
        return fail(ErrorCode::Internal, "the material texture staging buffer is not mapped");
    }
    for (usize index = 0; index < uploads.size(); ++index) {
        std::memcpy(mapped + plan.starts[index], uploads[index].pixels.data(),
                    uploads[index].pixels.size());
    }

    RenderGraph graph(allocator);
    UploadRecording recording;
    recording.copies = &plan.copies;
    recording.staging = *buffer;
    Status executed = declare_passes(graph, plan, recording, device, allocator);
    if (executed) {
        if (const Expected<u32, Error> began = device.begin_frame(); !began) {
            executed = make_unexpected(began.error());
        } else {
            {
                GraphExecutor executor(allocator, device);
                if (auto result = executor.execute(graph, CompileOptions{}, ExecuteOptions{});
                    !result) {
                    executed = make_unexpected(result.error());
                } else {
                    executed = device.wait_idle();
                }
                executor.release();
            }
            if (Status ended = device.end_frame(); !ended && executed) {
                executed = ended;
            }
        }
    }
    device.destroy_buffer(*buffer);
    return executed;
}

}  // namespace

rhi::Format device_format_of(render::TextureFormat format) noexcept {
    switch (format) {
        case render::TextureFormat::R8Unorm:
            return rhi::Format::R8Unorm;
        case render::TextureFormat::Rg8Unorm:
            return rhi::Format::Rg8Unorm;
        case render::TextureFormat::Rgba8Unorm:
            return rhi::Format::Rgba8Unorm;
        case render::TextureFormat::Rgba8Srgb:
            return rhi::Format::Rgba8Srgb;
        case render::TextureFormat::R16Sfloat:
            return rhi::Format::R16Sfloat;
        case render::TextureFormat::Rg16Sfloat:
            return rhi::Format::Rg16Sfloat;
        case render::TextureFormat::Rgba16Sfloat:
            return rhi::Format::Rgba16Sfloat;
        case render::TextureFormat::R32Sfloat:
            return rhi::Format::R32Sfloat;
        case render::TextureFormat::B10G11R11Ufloat:
            return rhi::Format::B10G11R11Ufloat;
        case render::TextureFormat::Bc1RgbaUnorm:
            return rhi::Format::Bc1RgbaUnorm;
        case render::TextureFormat::Bc1RgbaSrgb:
            return rhi::Format::Bc1RgbaSrgb;
        case render::TextureFormat::Bc3Unorm:
            return rhi::Format::Bc3Unorm;
        case render::TextureFormat::Bc3Srgb:
            return rhi::Format::Bc3Srgb;
        case render::TextureFormat::Bc4Unorm:
            return rhi::Format::Bc4Unorm;
        case render::TextureFormat::Bc5Unorm:
            return rhi::Format::Bc5Unorm;
        case render::TextureFormat::Bc6HUfloat:
            return rhi::Format::Bc6HUfloat;
        case render::TextureFormat::Bc7Unorm:
            return rhi::Format::Bc7Unorm;
        case render::TextureFormat::Bc7Srgb:
            return rhi::Format::Bc7Srgb;
        default:
            // Rgb9E5Ufloat and every ASTC variant. See the header: a cook for a mobile target
            // reaching a desktop device is a real state, and this is the honest answer to it.
            return rhi::Format::Undefined;
    }
}

MaterialTextureTable::~MaterialTextureTable() {
    shutdown();
}

Status MaterialTextureTable::initialize(rhi::Device& device, Allocator& allocator,
                                        const rhi::SamplerDescription& sampler) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "the material texture table is already initialized");
    }
    if (device.global_texture_table().is_null() || device.global_texture_table_layout().is_null()) {
        return fail(ErrorCode::Unsupported,
                    "this device has no global texture table: it is on the compatibility path, "
                    "where a material samples through a set of its own");
    }
    Expected<rhi::SamplerHandle, Error> handle = device.create_sampler(sampler);
    if (!handle) {
        return make_unexpected(handle.error());
    }
    if (Status shared = device.set_global_sampler(*handle); !shared) {
        device.destroy_sampler(*handle);
        return shared;
    }
    device_ = &device;
    allocator_ = &allocator;
    sampler_ = *handle;
    entries_ = Array<Entry>(allocator);
    resident_bytes_ = 0;
    return ok();
}

void MaterialTextureTable::shutdown() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (const Entry& entry : entries_) {
        device_->release_bindless_index(entry.slot);
        device_->destroy_texture_view(entry.view);
        device_->destroy_texture(entry.image);
    }
    entries_.clear();
    device_->destroy_sampler(sampler_);
    sampler_ = rhi::SamplerHandle{};
    resident_bytes_ = 0;
    device_ = nullptr;
    allocator_ = nullptr;
}

rhi::BindlessIndex MaterialTextureTable::slot_of(render::TextureHandle texture) const noexcept {
    for (const Entry& entry : entries_) {
        if (entry.texture == texture) {
            return entry.slot;
        }
    }
    return rhi::kInvalidBindlessIndex;
}

rhi::DescriptorSetLayoutHandle MaterialTextureTable::layout() const noexcept {
    return device_ != nullptr ? device_->global_texture_table_layout()
                              : rhi::DescriptorSetLayoutHandle{};
}

rhi::DescriptorSetHandle MaterialTextureTable::set() const noexcept {
    return device_ != nullptr ? device_->global_texture_table() : rhi::DescriptorSetHandle{};
}

Status MaterialTextureTable::publish_one(const render::TextureRecord* record,
                                         render::TextureHandle texture,
                                         rhi::TextureHandle image) noexcept {
    rhi::TextureViewDescription description;
    description.name = record != nullptr ? record->name.c_str() : "material texture";
    description.texture = image;
    Expected<rhi::TextureViewHandle, Error> view = device_->create_texture_view(description);
    if (!view) {
        return make_unexpected(view.error());
    }
    Entry entry;
    entry.texture = texture;
    entry.image = image;
    entry.view = *view;
    entry.slot = device_->bind_texture_globally(*view, sampler_);
    if (entry.slot == rhi::kInvalidBindlessIndex) {
        device_->destroy_texture_view(entry.view);
        return fail(ErrorCode::Internal,
                    "the device's global texture table refused a slot for a resident material "
                    "texture");
    }
    if (Status pushed = entries_.push_back(entry); !pushed) {
        device_->release_bindless_index(entry.slot);
        device_->destroy_texture_view(entry.view);
        return pushed;
    }
    resident_bytes_ += record != nullptr ? record->bytes : 0;
    return ok();
}

void MaterialTextureTable::unpublish_from(const render::RenderServer& server,
                                          usize first) noexcept {
    for (usize index = first; index < entries_.size(); ++index) {
        device_->release_bindless_index(entries_[index].slot);
        device_->destroy_texture_view(entries_[index].view);
        const render::TextureRecord* record = server.texture(entries_[index].texture);
        resident_bytes_ -= record != nullptr ? record->bytes : 0;
    }
    (void)entries_.resize(first);
}

Status MaterialTextureTable::publish(const render::RenderServer& server,
                                     Span<const TextureUpload> uploads,
                                     Span<const rhi::TextureHandle> images) noexcept {
    const usize first = entries_.size();
    for (usize index = 0; index < uploads.size(); ++index) {
        const render::TextureRecord* record = server.texture(uploads[index].texture);
        if (Status added = publish_one(record, uploads[index].texture, images[index]); !added) {
            // TRANSACTIONAL: the slots and views this call took are given back, so the caller's own
            // failure path can destroy every image of the batch without destroying one twice.
            unpublish_from(server, first);
            return added;
        }
    }
    return ok();
}

Status MaterialTextureTable::upload(const render::RenderServer& server,
                                    Span<const TextureUpload> uploads) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the material texture table is not initialized");
    }
    if (uploads.empty()) {
        return ok();
    }
    rhi::Device& device = *device_;

    UploadPlan plan(*allocator_);
    Status planned = plan_uploads(device, server, uploads, plan);
    if (planned) {
        planned = run_uploads(device, *allocator_, plan, uploads);
    }
    if (planned) {
        planned = publish(server, uploads,
                          Span<const rhi::TextureHandle>(plan.images.data(), plan.images.size()));
    }
    if (!planned) {
        // NOTHING HALF-RESIDENT. A batch that failed leaves no image behind: a texture created and
        // never uploaded is one a later frame would sample as undefined, which is the exact defect
        // this path exists to make impossible.
        for (const rhi::TextureHandle image : plan.images) {
            device.destroy_texture(image);
        }
    }
    return planned;
}

}  // namespace cy::rendering::pipeline
