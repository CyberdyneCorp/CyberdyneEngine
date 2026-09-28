// SPDX-License-Identifier: MIT
// Decals in the device frame. See decal_table.h.

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/rendering/decals/decal_table.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::rendering::decals {
namespace {

constexpr rhi::Format kFormat = rhi::Format::Rgba8Unorm;

// The record's word offsets, `cy/decal.slang`'s. Stated once here so the packer below reads as the
// layout rather than as a column of numbers.
constexpr u32 kCentre = 0;
constexpr u32 kFadeCosine = 3;
constexpr u32 kAxisX = 4;
constexpr u32 kAlbedoWeight = 7;
constexpr u32 kAxisY = 8;
constexpr u32 kRoughnessWeight = 11;
constexpr u32 kAxisZ = 12;
constexpr u32 kNormalWeight = 15;
constexpr u32 kAlbedo = 16;
constexpr u32 kRoughness = 19;
constexpr u32 kEmission = 20;
constexpr u32 kMetallic = 23;
constexpr u32 kFadeStart = 24;
constexpr u32 kFadeEnd = 25;
constexpr u32 kMetallicWeight = 26;
constexpr u32 kEmissionWeight = 27;
constexpr u32 kChannels = 28;
constexpr u32 kShape = 29;
constexpr u32 kShapeA = 30;
constexpr u32 kShapeB = 31;
constexpr u32 kMask = 32;
constexpr u32 kRelief = 33;
constexpr u32 kEdgeSoftness = 34;
constexpr u32 kSeed = 35;
static_assert(kSeed + 1U == kDecalRecordWords);

[[nodiscard]] u32 bits(f32 value) noexcept {
    u32 word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

void put(u32* record, u32 offset, f32 value) noexcept {
    record[offset] = bits(value);
}

void put3(u32* record, u32 offset, Vec3 value) noexcept {
    record[offset + 0U] = bits(value.x);
    record[offset + 1U] = bits(value.y);
    record[offset + 2U] = bits(value.z);
}

/// A box axis divided by its half-extent, so a position's coordinate in the box is one dot product
/// and "inside" is that coordinate in [-1, 1].
[[nodiscard]] Vec3 scaled_axis(Vec3 axis, f32 half_extent, Vec3 fallback) noexcept {
    const Vec3 unit = normalized_or(axis, fallback);
    const f32 half = half_extent > 1.0e-5F ? half_extent : 1.0e-5F;
    return unit * (1.0F / half);
}

/// The world position minus the origin, in double precision: a decal a kilometre from the world's
/// origin is still placed to the millimetre relative to a camera beside it.
[[nodiscard]] Vec3 relative(Vec3 world, Vec3 origin) noexcept {
    return Vec3{static_cast<f32>(static_cast<f64>(world.x) - static_cast<f64>(origin.x)),
                static_cast<f32>(static_cast<f64>(world.y) - static_cast<f64>(origin.y)),
                static_cast<f32>(static_cast<f64>(world.z) - static_cast<f64>(origin.z))};
}

void write_record(const DecalInstance& decal, const DecalMaterial& material, Vec3 origin,
                  u32* record) noexcept {
    put3(record, kCentre, relative(decal.center, origin));
    // `decal_angle_fade`'s clamp: pi/2 would be no fade at all, and the cosine of exactly pi/2 is
    // not quite zero in f32, so the limit stops a hair short of it as the host's does.
    const f32 angle = std::clamp(decal.fade_angle_radians, 0.0F, 1.5707F);
    put(record, kFadeCosine, std::cos(angle));
    put3(record, kAxisX, scaled_axis(decal.axis_x, decal.half_extent.x, Vec3{1.0F, 0.0F, 0.0F}));
    put3(record, kAxisY, scaled_axis(decal.axis_y, decal.half_extent.y, Vec3{0.0F, 1.0F, 0.0F}));
    put3(record, kAxisZ, scaled_axis(decal.axis_z, decal.half_extent.z, Vec3{0.0F, 0.0F, 1.0F}));
    put(record, kAlbedoWeight, decal.weights.albedo);
    put(record, kRoughnessWeight, decal.weights.roughness);
    put(record, kNormalWeight, decal.weights.normal * decal.normal_strength);
    put(record, kMetallicWeight, decal.weights.metallic);
    put(record, kEmissionWeight, decal.weights.emission);
    put3(record, kAlbedo, material.albedo);
    put(record, kRoughness, material.roughness);
    put3(record, kEmission, material.emission);
    put(record, kMetallic, material.metallic);
    put(record, kFadeStart, decal.fade_start);
    put(record, kFadeEnd, decal.fade_end);
    record[kChannels] = decal.channels;
    record[kShape] = static_cast<u32>(material.shape);
    put(record, kShapeA, material.shape_a);
    put(record, kShapeB, material.shape_b);
    record[kMask] = material.mask_texture;
    put(record, kRelief, material.relief_metres);
    put(record, kEdgeSoftness, material.edge_softness);
    record[kSeed] = material.seed + static_cast<u32>(decal.id);
}

[[nodiscard]] Status check_input(const DecalTableInput& input) noexcept {
    if (input.order.size() != input.decals.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "pack_decal_table: the order is not the same length as the decals — pass "
                    "FrameAssembly::decal_order() for the decals the assembly was handed");
    }
    Array<u8> seen(current_allocator());
    if (Status sized = seen.resize(input.decals.size()); !sized) {
        return sized;
    }
    for (u8& flag : seen) {
        flag = 0;
    }
    for (const u32 index : input.order) {
        if (index >= input.decals.size()) {
            return fail(ErrorCode::OutOfRange,
                        "pack_decal_table: the order names a decal past the end of the span");
        }
        if (seen[index] != 0U) {
            return fail(ErrorCode::InvalidArgument,
                        "pack_decal_table: the order names one decal twice, so it is not a "
                        "permutation and another decal would never be drawn");
        }
        seen[index] = 1;
        if (input.decals[index].material_index >= input.materials.size()) {
            return fail(ErrorCode::OutOfRange,
                        "pack_decal_table: a decal's material_index is past the material span");
        }
    }
    if (input.clusters != nullptr &&
        input.clusters->headers.size() !=
            static_cast<usize>(input.grid.cluster_count()) * kClusterElementTypeCount) {
        return fail(ErrorCode::InvalidArgument,
                    "pack_decal_table: the cluster assignment was not made over the grid passed");
    }
    return ok();
}

void write_header(const DecalTableInput& input, u32 lists_offset, u32 indices_offset,
                  u32* header) noexcept {
    header[0] = kDecalMagic;
    header[1] = kDecalTableVersion;
    header[2] = static_cast<u32>(input.decals.size());
    header[3] = kDecalRecordWords;
    header[4] = kDecalHeaderWords;
    header[5] = input.clusters != nullptr ? 1U : 0U;
    header[6] = lists_offset;
    header[7] = indices_offset;
    if (input.clusters == nullptr) {
        return;
    }
    header[8] = input.grid.dimensions[0];
    header[9] = input.grid.dimensions[1];
    header[10] = input.grid.dimensions[2];
    header[11] = input.grid.max_elements_per_cluster;
    header[12] = bits(input.grid.slice_scale);
    header[13] = bits(input.grid.slice_bias);
    header[14] = bits(input.grid.near_plane);
    header[15] = bits(input.grid.far_plane);
    header[16] = input.width;
    header[17] = input.height;
    // THE ROW THAT TAKES A RELATIVE POSITION TO POSITIVE VIEW DEPTH. The view matrix is world to
    // view and looks down -Z, so the depth of a world point is -(row 2 · (p, 1)); a relative point
    // is p - origin, which moves the origin's share into the constant — summed in double for the
    // reason `relative` gives.
    const Mat4& view = input.view;
    const f64 constant = -((static_cast<f64>(view.at(2, 0)) * static_cast<f64>(input.origin.x)) +
                           (static_cast<f64>(view.at(2, 1)) * static_cast<f64>(input.origin.y)) +
                           (static_cast<f64>(view.at(2, 2)) * static_cast<f64>(input.origin.z)) +
                           static_cast<f64>(view.at(2, 3)));
    header[18] = bits(-view.at(2, 0));
    header[19] = bits(-view.at(2, 1));
    header[20] = bits(-view.at(2, 2));
    header[21] = bits(static_cast<f32>(constant));
}

/// The decal lists, copied out of the assembly's assignment: one (offset, count) pair per cluster,
/// then the ranks. Offsets are into the table, so the shader adds nothing.
[[nodiscard]] Status write_lists(const DecalTableInput& input, u32 lists_offset,
                                 Array<u32>& words) noexcept {
    const u32 clusters = input.grid.cluster_count();
    u32 next = lists_offset + (clusters * 2U);
    for (u32 cluster = 0; cluster < clusters; ++cluster) {
        const ClusterHeader& header =
            input.clusters->headers[(static_cast<usize>(cluster) * kClusterElementTypeCount) +
                                    static_cast<usize>(ClusterElementType::Decal)];
        words[lists_offset + (cluster * 2U)] = next;
        words[lists_offset + (cluster * 2U) + 1U] = header.count;
        next += header.count;
    }
    if (Status sized = words.resize(next); !sized) {
        return sized;
    }
    for (u32 cluster = 0; cluster < clusters; ++cluster) {
        const ClusterHeader& header =
            input.clusters->headers[(static_cast<usize>(cluster) * kClusterElementTypeCount) +
                                    static_cast<usize>(ClusterElementType::Decal)];
        const u32 first = words[lists_offset + (cluster * 2U)];
        for (u32 entry = 0; entry < header.count; ++entry) {
            words[first + entry] = input.clusters->indices[header.offset + entry];
        }
    }
    return ok();
}

using UploadRecording = detail::DecalUploadRecording;

void record_upload(const PassContext& context, void* user) noexcept {
    const auto* recording = static_cast<const UploadRecording*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kDecalTableWidth, recording->rows, 1};
    context.commands->copy_buffer_to_texture(recording->staging, recording->texture,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// One submission: the copy, then the sampled read that leaves the image where a fragment stage
/// can read it — `ProbeVolumeTexture::upload`'s arrangement, for the reason
/// `material_textures.h` gives.
[[nodiscard]] Status run(rhi::Device& device, Allocator& allocator,
                         UploadRecording& recording) noexcept {
    RenderGraph graph(allocator);
    TextureRequest request;
    request.name = "decal table";
    request.format = kFormat;
    request.width = kDecalTableWidth;
    request.height = recording.rows;
    const ResourceId image =
        graph.import_texture(request, recording.texture, rhi::ImageUse::Undefined);
    graph.add_pass("decal table upload", rhi::QueueKind::Graphics)
        .write(image, rhi::Access::TransferWrite)
        .record(&record_upload, &recording);
    graph.add_pass("decal table residency", rhi::QueueKind::Graphics)
        .read(image, rhi::Access::FragmentSampledRead)
        .side_effect();
    if (Status declared = graph.status(); !declared) {
        return declared;
    }
    const Expected<u32, Error> began = device.begin_frame();
    if (!began.has_value()) {
        return make_unexpected(began.error());
    }
    Status executed = ok();
    {
        GraphExecutor executor(allocator, device);
        if (auto result = executor.execute(graph, CompileOptions{}, ExecuteOptions{}); !result) {
            executed = make_unexpected(result.error());
        } else {
            executed = device.wait_idle();
        }
        executor.release();
    }
    if (Status ended = device.end_frame(); !ended && executed) {
        executed = ended;
    }
    return executed;
}

}  // namespace

Status pack_decal_table(const DecalTableInput& input, Array<u32>& words) noexcept {
    if (Status checked = check_input(input); !checked) {
        return checked;
    }
    const auto count = static_cast<u32>(input.decals.size());
    const u32 lists_offset = kDecalHeaderWords + (count * kDecalRecordWords);
    const u32 clusters = input.clusters != nullptr ? input.grid.cluster_count() : 0U;
    words.clear();
    if (Status sized =
            words.resize(static_cast<usize>(lists_offset) + (static_cast<usize>(clusters) * 2U));
        !sized) {
        return sized;
    }
    for (u32& word : words) {
        word = 0;
    }
    write_header(input, lists_offset, lists_offset + (clusters * 2U), words.data());
    for (u32 rank = 0; rank < count; ++rank) {
        const DecalInstance& decal = input.decals[input.order[rank]];
        write_record(
            decal, input.materials[decal.material_index], input.origin,
            words.data() + kDecalHeaderWords + (static_cast<usize>(rank) * kDecalRecordWords));
    }
    if (input.clusters == nullptr) {
        return ok();
    }
    return write_lists(input, lists_offset, words);
}

void write_decal_frame(rhi::BindlessIndex slot, u32 rows, u32 flags,
                       pipeline::FrameViewData& view) noexcept {
    view.decal_control[0] = slot;
    view.decal_control[1] = rows;
    view.decal_control[2] = flags;
    view.decal_control[3] = 0;
}

DecalTableTexture::~DecalTableTexture() {
    shutdown();
}

void DecalTableTexture::initialize(rhi::Device& device, Allocator& allocator) noexcept {
    device_ = &device;
    allocator_ = &allocator;
}

void DecalTableTexture::release_texture() noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (!view_.is_null()) {
        device_->destroy_texture_view(view_);
        view_ = rhi::TextureViewHandle{};
    }
    if (!texture_.is_null()) {
        device_->destroy_texture(texture_);
        texture_ = rhi::TextureHandle{};
    }
    // Sized to the texture, so it goes with it.
    if (!staging_.is_null()) {
        device_->destroy_buffer(staging_);
        staging_ = rhi::BufferHandle{};
    }
    rows_ = 0;
}

void DecalTableTexture::shutdown() noexcept {
    if (device_ != nullptr) {
        (void)device_->wait_idle();
    }
    release_texture();
    device_ = nullptr;
    allocator_ = nullptr;
}

Status DecalTableTexture::recreate(u32 rows) noexcept {
    release_texture();
    rhi::TextureDescription description;
    description.name = "decal table";
    description.format = kFormat;
    description.extent = rhi::Extent3D{kDecalTableWidth, rows, 1};
    description.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
    Expected<rhi::TextureHandle, Error> texture = device_->create_texture(description);
    if (!texture.has_value()) {
        return make_unexpected(texture.error());
    }
    texture_ = *texture;
    rhi::TextureViewDescription view;
    view.name = "decal table";
    view.texture = texture_;
    Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
    if (!made.has_value()) {
        release_texture();
        return make_unexpected(made.error());
    }
    view_ = *made;
    rows_ = rows;
    return ok();
}

Status DecalTableTexture::reserve(u32 rows) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::Unavailable, "DecalTableTexture: initialize() was not called");
    }
    if (!texture_.is_null() && rows <= rows_) {
        return ok();
    }
    return recreate(rows > 0U ? rows : 1U);
}

Expected<ResourceId, Error> DecalTableTexture::declare_upload(RenderGraph& graph) noexcept {
    if (device_ == nullptr || texture_.is_null()) {
        return fail(ErrorCode::Unavailable,
                    "DecalTableTexture::declare_upload: reserve() the texture before the frame");
    }
    if (staging_.is_null()) {
        rhi::BufferDescription description;
        description.name = "decal table staging";
        description.size = static_cast<u64>(kDecalTableWidth) * rows_ * sizeof(u32);
        description.usage = rhi::BufferUsage::TransferSource;
        description.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> made = device_->create_buffer(description);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        staging_ = *made;
    }
    recording_.staging = staging_;
    recording_.texture = texture_;
    recording_.rows = rows_;
    TextureRequest request;
    request.name = "decal table";
    request.format = kFormat;
    request.width = kDecalTableWidth;
    request.height = rows_;
    // UNDEFINED EVERY FRAME, because the copy rewrites every texel: the previous contents are
    // discarded rather than preserved, which is the truth and not a shortcut.
    const ResourceId image = graph.import_texture(request, texture_, rhi::ImageUse::Undefined);
    graph.add_pass("decal table upload", rhi::QueueKind::Graphics)
        .write(image, rhi::Access::TransferWrite)
        .record(&record_upload, &recording_);
    return image;
}

Status DecalTableTexture::stage(Span<const u32> words) noexcept {
    if (staging_.is_null()) {
        return fail(ErrorCode::Unavailable,
                    "DecalTableTexture::stage: declare_upload() the frame's copy first");
    }
    if (words.size() < kDecalHeaderWords || words[0] != kDecalMagic) {
        return fail(ErrorCode::InvalidArgument,
                    "DecalTableTexture: the words are not a decal table — pack_decal_table first");
    }
    if (decal_table_rows(words.size()) > rows_) {
        return fail(ErrorCode::OutOfRange,
                    "DecalTableTexture::stage: the table has more rows than the texture reserved, "
                    "and a texture cannot be recreated inside a frame");
    }
    auto* mapped = static_cast<u8*>(device_->buffer_mapped_pointer(staging_));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "the decal table staging buffer is not mapped");
    }
    const usize size = static_cast<usize>(kDecalTableWidth) * rows_ * sizeof(u32);
    std::memset(mapped, 0, size);
    std::memcpy(mapped, words.data(), words.size() * sizeof(u32));
    uploads_ += 1;
    return ok();
}

Status DecalTableTexture::upload(Span<const u32> words) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::Unavailable, "DecalTableTexture: initialize() was not called");
    }
    if (words.size() < kDecalHeaderWords || words[0] != kDecalMagic) {
        return fail(ErrorCode::InvalidArgument,
                    "DecalTableTexture: the words are not a decal table — pack_decal_table first");
    }
    const u32 needed = decal_table_rows(words.size());
    if (texture_.is_null() || needed > rows_) {
        // Doubling, so a table that grows by a few decals a frame does not recreate every frame.
        u32 rows = rows_ > 0U ? rows_ : 1U;
        while (rows < needed) {
            rows *= 2U;
        }
        if (Status made = recreate(rows); !made) {
            return made;
        }
    }
    // The whole texture, so the rows past this table's end hold zeroes rather than a larger
    // table's leftovers: nothing reads them, and a capture of the texture then shows only this one.
    const u64 size = static_cast<u64>(kDecalTableWidth) * rows_ * sizeof(u32);
    rhi::BufferDescription description;
    description.name = "decal table staging";
    description.size = size;
    description.usage = rhi::BufferUsage::TransferSource;
    description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> staging = device_->create_buffer(description);
    if (!staging.has_value()) {
        return make_unexpected(staging.error());
    }
    auto* mapped = static_cast<u8*>(device_->buffer_mapped_pointer(*staging));
    if (mapped == nullptr) {
        device_->destroy_buffer(*staging);
        return fail(ErrorCode::Internal, "the decal table staging buffer is not mapped");
    }
    // A word's bytes in memory order ARE the texel's red, green, blue and alpha on a little-endian
    // host, which is every host the engine builds for; `cyDecalWordFromTexel` reads them back in
    // that order.
    std::memset(mapped, 0, static_cast<usize>(size));
    std::memcpy(mapped, words.data(), words.size() * sizeof(u32));

    UploadRecording recording;
    recording.staging = *staging;
    recording.texture = texture_;
    recording.rows = rows_;
    Status ran = run(*device_, *allocator_, recording);
    device_->destroy_buffer(*staging);
    if (!ran) {
        return ran;
    }
    uploads_ += 1;
    return ok();
}

}  // namespace cy::rendering::decals
