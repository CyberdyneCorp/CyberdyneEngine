// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_gpu/gpu_scene.h>

#include <cy/backends/rhi/validation.h>

#include <cstring>

#include "gi_gpu_msl.h"
#include "gi_gpu_spirv.h"

namespace cy::rendering::gi_gpu {
namespace {

using rhi::Access;
using rhi::QueueKind;

/// `[numthreads(64, 1, 1)]` on all three entry points.
constexpr u32 kGroupSize = 64;

/// Words of `GpuGiConstants`, as gi_gpu_common.slang numbers them (uint4 index times four).
constexpr u32 kCounts = 0 * 4;
constexpr u32 kGrid = 1 * 4;
constexpr u32 kSkyZenith = 2 * 4;
constexpr u32 kSkyHorizon = 3 * 4;
constexpr u32 kSkyGround = 4 * 4;
constexpr u32 kShadowCentre = 5 * 4;
constexpr u32 kShadowDir = 6 * 4;
constexpr u32 kShadowRight = 7 * 4;
constexpr u32 kShadowUp = 8 * 4;
constexpr u32 kShadowEnabled = 9 * 4;
constexpr u32 kLevels = 16 * 4;

constexpr u64 kVec4 = 16;
constexpr u64 kCardVec4s = 4;
constexpr u64 kStateVec4s = 2;
constexpr u64 kLightVec4s = 4;
constexpr u64 kResultVec4s = 3;
constexpr u64 kRayVec4s = 2;
constexpr u64 kHitVec4s = 3;

[[nodiscard]] u32 bits(f32 value) noexcept {
    u32 word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

[[nodiscard]] u32 bits(i32 value) noexcept {
    return static_cast<u32>(value);
}

void put(u32* words, u32 at, Vec3 value, u32 w) noexcept {
    words[at + 0] = bits(value.x);
    words[at + 1] = bits(value.y);
    words[at + 2] = bits(value.z);
    words[at + 3] = w;
}

void put(f32* floats, u64 at, Vec3 value, f32 w) noexcept {
    floats[at + 0] = value.x;
    floats[at + 1] = value.y;
    floats[at + 2] = value.z;
    floats[at + 3] = w;
}

/// `gi::CardGrid`'s bucket count for the most cards the scene holds — the largest grid it can build.
[[nodiscard]] u32 max_buckets(u32 cards) noexcept {
    u32 count = 64;
    while (count < cards * 2U) {
        count <<= 1U;
    }
    return count;
}

[[nodiscard]] u32 groups(u32 count) noexcept {
    const u32 needed = (count + kGroupSize - 1U) / kGroupSize;
    return needed == 0 ? 1 : needed;
}

}  // namespace

GpuGiScene::~GpuGiScene() {
    destroy();
}

bool GpuGiScene::supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return device.capabilities().has(rhi::Capability::ComputeShaders) &&
           (format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl);
}

Status GpuGiScene::create(Allocator& allocator, rhi::Device& device,
                          const GpuGiSceneDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device GI scene is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "the device GI scene is a set of compute dispatches and this device has no "
                    "compute stage or no shader format this module ships (SPIR-V or MSL); the host "
                    "surface cache is the same answer");
    }
    if (desc.max_levels == 0 || desc.max_levels > kMaxFieldLevels || desc.max_window_bricks == 0 ||
        desc.max_brick_slots == 0 || desc.max_cards == 0 || desc.max_lights == 0 ||
        desc.max_shadow_resolution == 0 || desc.max_selection == 0 || desc.max_rays == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "GpuGiSceneDescription: every capacity must be non-zero and max_levels at most "
                    "kMaxFieldLevels; the buffers are sized once and cannot grow");
    }
    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;
    if (Status created = create_pipelines(); !created) {
        destroy();
        return created;
    }
    if (Status created = create_buffers(); !created) {
        destroy();
        return created;
    }
    if (Status written = write_descriptors(); !written) {
        destroy();
        return written;
    }
    if (Status sized = card_revisions_.resize(desc.max_cards); !sized) {
        destroy();
        return sized;
    }
    if (Status sized = card_positions_.resize(desc.max_cards); !sized) {
        destroy();
        return sized;
    }
    if (Status sized = card_live_.resize(desc.max_cards); !sized) {
        destroy();
        return sized;
    }
    for (u32 index = 0; index < desc.max_cards; ++index) {
        card_revisions_[index] = 0;
        card_positions_[index] = Vec3{0.0F, 0.0F, 0.0F};
        card_live_[index] = 0;
    }
    std::memset(constants_, 0, sizeof(constants_));
    set_gather(gi::CardGatherSettings{});
    write_constants();
    return ok();
}

Status GpuGiScene::create_pipelines() noexcept {
    struct Source {
        const char* name;
        Span<const u32> spirv;
        const char* msl;
        usize msl_size;
        const char* msl_entry;
    };
    const Source sources[kPipelineCount] = {
        {"gi trace rays",
         {kGiTraceRaysSpirv, sizeof(kGiTraceRaysSpirv) / sizeof(u32)},
         kGiTraceRaysMsl,
         sizeof(kGiTraceRaysMsl) - 1,
         "cyGiTraceRays"},
        {"gi shade cards",
         {kGiShadeCardsSpirv, sizeof(kGiShadeCardsSpirv) / sizeof(u32)},
         kGiShadeCardsMsl,
         sizeof(kGiShadeCardsMsl) - 1,
         "cyGiShadeCards"},
        {"gi commit cards",
         {kGiCommitCardsSpirv, sizeof(kGiCommitCardsSpirv) / sizeof(u32)},
         kGiCommitCardsMsl,
         sizeof(kGiCommitCardsMsl) - 1,
         "cyGiCommitCards"},
    };

    // ONE SET LAYOUT FOR ALL THREE: the shaders share `GiSceneSet`, and a binding an entry point
    // does not read is a binding it does not read, not a different layout.
    rhi::DescriptorBinding bindings[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = "gi scene set";
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindingCount);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, sizeof(Dispatch)};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = "gi scene layout";
    pipeline_layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> made =
        device_->create_pipeline_layout(pipeline_layout);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    pipeline_layout_ = *made;

    for (u32 which = 0; which < kPipelineCount; ++which) {
        const Source& source = sources[which];
        rhi::ShaderModuleBundle bundle;
        bundle.spirv = source.spirv;
        bundle.msl = {reinterpret_cast<const u8*>(source.msl), source.msl_size};
        bundle.spirv_entry_point = "main";
        bundle.msl_entry_point = source.msl_entry;
        rhi::ValidationMessage message;
        auto module =
            rhi::select_shader_module(bundle, device_->capabilities().native_shader_format(),
                                      source.name, rhi::ShaderStage::Compute, message);
        if (!module.has_value()) {
            return make_unexpected(module.error());
        }
        Expected<rhi::ShaderModuleHandle, Error> shader = device_->create_shader_module(*module);
        if (!shader.has_value()) {
            return make_unexpected(shader.error());
        }
        shaders_[which] = *shader;
        rhi::ComputePipelineDescription pipeline;
        pipeline.name = source.name;
        pipeline.layout = pipeline_layout_;
        pipeline.shader = shaders_[which];
        pipeline.workgroup_size[0] = kGroupSize;
        Expected<rhi::ComputePipelineHandle, Error> created =
            device_->create_compute_pipeline(pipeline);
        if (!created.has_value()) {
            return make_unexpected(created.error());
        }
        pipelines_[which] = *created;
    }
    return ok();
}

Status GpuGiScene::create_buffers() noexcept {
    // WHICH MEMORY EACH BUFFER LIVES IN. Everything the host writes is Upload and mapped, written in
    // place — the scene representation, the lights, the selection, the constants, and the card
    // state, which the host writes for a page it allocated or invalidated and the commit dispatch
    // writes for a page it shaded. The two outputs the dispatches write for the host to read are
    // device-local and copied into Readback buffers by a declared transfer.
    const u64 window = desc_.max_window_bricks;
    const u64 shadow = desc_.max_shadow_resolution;
    struct Request {
        const char* name;
        u64 size;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
    };
    const auto storage = rhi::BufferUsage::Storage;
    const auto output = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
    const auto host = rhi::BufferUsage::TransferDestination;
    const Request requests[kBufferCount] = {
        {"gi constants", sizeof(constants_), storage, rhi::MemoryUse::Upload},
        {"gi page table", desc_.max_levels * window * window * window * 4U, storage,
         rhi::MemoryUse::Upload},
        {"gi brick pool", static_cast<u64>(desc_.max_brick_slots) * gi::kBrickVoxels * 4U, storage,
         rhi::MemoryUse::Upload},
        {"gi cards", desc_.max_cards * kCardVec4s * kVec4, storage, rhi::MemoryUse::Upload},
        {"gi card state", desc_.max_cards * kStateVec4s * kVec4, storage, rhi::MemoryUse::Upload},
        {"gi card grid ranges", static_cast<u64>(max_buckets(desc_.max_cards)) * 2U * 4U, storage,
         rhi::MemoryUse::Upload},
        {"gi card grid items", static_cast<u64>(desc_.max_cards) * 4U, storage,
         rhi::MemoryUse::Upload},
        {"gi lights", desc_.max_lights * kLightVec4s * kVec4, storage, rhi::MemoryUse::Upload},
        {"gi shadow depths", shadow * shadow * 4U, storage, rhi::MemoryUse::Upload},
        {"gi selection", static_cast<u64>(desc_.max_selection) * 4U, storage,
         rhi::MemoryUse::Upload},
        {"gi card results", desc_.max_selection * kResultVec4s * kVec4, output,
         rhi::MemoryUse::DeviceLocal},
        {"gi rays", desc_.max_rays * kRayVec4s * kVec4, storage, rhi::MemoryUse::Upload},
        {"gi hits", desc_.max_rays * kHitVec4s * kVec4, output, rhi::MemoryUse::DeviceLocal},
        {"gi card results readback", desc_.max_selection * kResultVec4s * kVec4, host,
         rhi::MemoryUse::Readback},
        {"gi hits readback", desc_.max_rays * kHitVec4s * kVec4, host, rhi::MemoryUse::Readback},
    };
    for (u32 index = 0; index < kBufferCount; ++index) {
        rhi::BufferDescription description;
        description.name = requests[index].name;
        description.size = requests[index].size;
        description.usage = requests[index].usage;
        description.memory = requests[index].memory;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(description);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        buffers_[index] = *buffer;
        sizes_[index] = requests[index].size;
        // Zeroed, so a card that was never written reads as not live and not valid, and a page
        // table that was never written reads as nothing rather than as slot zero.
        if (requests[index].memory == rhi::MemoryUse::Upload) {
            if (void* pointer = device_->buffer_mapped_pointer(*buffer); pointer != nullptr) {
                std::memset(pointer, index == kPageTable ? 0xFF : 0,
                            static_cast<usize>(requests[index].size));
            }
        }
    }
    return ok();
}

Status GpuGiScene::write_descriptors() noexcept {
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(set_layout_, false);
    if (!set.has_value()) {
        return make_unexpected(set.error());
    }
    set_ = *set;
    rhi::DescriptorWrite writes[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = buffers_[index];
        writes[index].buffer_range = 0;
    }
    return device_->update_descriptor_set(set_,
                                          Span<const rhi::DescriptorWrite>(writes, kBindingCount));
}

void GpuGiScene::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (rhi::BufferHandle& buffer : buffers_) {
        if (buffer) {
            device_->destroy_buffer(buffer);
        }
        buffer = {};
    }
    for (u32 which = 0; which < kPipelineCount; ++which) {
        if (pipelines_[which]) {
            device_->destroy_compute_pipeline(pipelines_[which]);
        }
        if (shaders_[which]) {
            device_->destroy_shader_module(shaders_[which]);
        }
        pipelines_[which] = {};
        shaders_[which] = {};
    }
    if (pipeline_layout_) {
        device_->destroy_pipeline_layout(pipeline_layout_);
    }
    if (set_layout_) {
        device_->destroy_descriptor_set_layout(set_layout_);
    }
    pipeline_layout_ = {};
    set_layout_ = {};
    set_ = {};
    field_uploaded_ = false;
    field_generation_ = 0;
    ray_count_ = 0;
    device_ = nullptr;
    allocator_ = nullptr;
}

void* GpuGiScene::mapped(Binding binding) const noexcept {
    return device_ == nullptr ? nullptr : device_->buffer_mapped_pointer(buffers_[binding]);
}

void GpuGiScene::write_constants() noexcept {
    if (void* target = mapped(kConstants); target != nullptr) {
        std::memcpy(target, constants_, sizeof(constants_));
    }
}

// --- The field ------------------------------------------------------------------------------------

void GpuGiScene::apply_brick(const gi::DistanceField& field, const gi::FieldBrickChange& change,
                             FieldUploadReport& report) noexcept {
    const i32 dim = static_cast<i32>(field_window_);
    const auto wrap = [dim](i32 value) { return static_cast<u32>(((value % dim) + dim) % dim); };
    const u32 cells = field_window_ * field_window_ * field_window_;
    const u32 cell = (((wrap(change.brick[2]) * field_window_) + wrap(change.brick[1])) *
                      field_window_) +
                     wrap(change.brick[0]);
    auto* table = static_cast<u32*>(mapped(kPageTable));
    table[(change.level * cells) + cell] = change.slot;
    report.table_entries += 1;
    report.bytes += sizeof(u32);
    if (change.slot == gi::kEmptyBrick) {
        return;
    }
    auto* pool = static_cast<f32*>(mapped(kBricks));
    const Span<const f32> source = field.brick_pool();
    std::memcpy(pool + (static_cast<usize>(change.slot) * gi::kBrickVoxels),
                source.data() + (static_cast<usize>(change.slot) * gi::kBrickVoxels),
                gi::kBrickVoxels * sizeof(f32));
    report.bricks_uploaded += 1;
    report.bytes += gi::kBrickVoxels * sizeof(f32);
}

Expected<FieldUploadReport, Error> GpuGiScene::upload_field(
    const gi::DistanceField& field) noexcept {
    if (device_ == nullptr) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "the GI scene is not created"});
    }
    const u32 levels = field.level_count();
    const u32 window = field.window_bricks();
    if (levels > desc_.max_levels || window > desc_.max_window_bricks) {
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "the field has more levels or a wider window than the "
                                     "device GI scene was created for"});
    }
    if (field.brick_slot_count() > desc_.max_brick_slots) {
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "the field holds more bricks than the device brick pool was "
                                     "created for; raise max_brick_slots"});
    }

    FieldUploadReport report;
    report.generation = field.generation();
    if (field_uploaded_ && field.generation() == field_generation_ && levels == field_levels_ &&
        window == field_window_) {
        // Nothing scrolled since the last upload, so nothing on the device can be stale.
        field_report_ = report;
        return report;
    }

    // INCREMENTAL WHEN EXACTLY ONE GENERATION BEHIND, which is every frame of a renderer that
    // uploads once per scroll. Anything else — the first upload, a reconfigured field, a frame
    // skipped — rebuilds the table from every brick the field holds.
    report.full = !field_uploaded_ || field.generation() != field_generation_ + 1U ||
                  levels != field_levels_ || window != field_window_;
    field_levels_ = levels;
    field_window_ = window;
    if (report.full) {
        auto* table = static_cast<u32*>(mapped(kPageTable));
        std::memset(table, 0xFF, static_cast<usize>(sizes_[kPageTable]));
        field.visit_bricks([&](const gi::FieldBrickChange& change) {
            apply_brick(field, change, report);
        });
    } else {
        for (const gi::FieldBrickChange& change : field.last_changes()) {
            apply_brick(field, change, report);
        }
    }

    constants_[kCounts + 0] = levels;
    constants_[kCounts + 1] = window;
    for (u32 level = 0; level < levels; ++level) {
        const gi::FieldLevelView view = field.level_view(level);
        put(constants_, kLevels + (level * 8U), Vec3{view.voxel_size, view.brick_size,
                                                     view.far_distance},
            0U);
        constants_[kLevels + (level * 8U) + 4U] = bits(view.origin_brick[0]);
        constants_[kLevels + (level * 8U) + 5U] = bits(view.origin_brick[1]);
        constants_[kLevels + (level * 8U) + 6U] = bits(view.origin_brick[2]);
        constants_[kLevels + (level * 8U) + 7U] = 0;
    }
    constants_[kShadowEnabled + 1] = bits(field.voxel_size());
    write_constants();

    field_generation_ = field.generation();
    field_uploaded_ = true;
    field_report_ = report;
    return report;
}

// --- The cards ------------------------------------------------------------------------------------

Expected<CardUploadReport, Error> GpuGiScene::upload_cards(Span<const gi::SurfacePage> pages,
                                                           f32 lookup_radius) noexcept {
    if (device_ == nullptr) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "the GI scene is not created"});
    }
    if (pages.size() > desc_.max_cards) {
        return make_unexpected(Error{ErrorCode::OutOfRange,
                                     "the surface cache holds more pages than the device GI scene "
                                     "was created for; raise max_cards"});
    }
    CardUploadReport report;
    auto* cards = static_cast<f32*>(mapped(kCards));
    auto* state = static_cast<f32*>(mapped(kCardState));
    bool layout_changed = false;
    for (u32 handle = 0; handle < pages.size(); ++handle) {
        const gi::SurfacePage& page = pages[handle];
        if (page.revision == card_revisions_[handle]) {
            continue;
        }
        const u64 card = static_cast<u64>(handle) * kCardVec4s * 4U;
        put(cards, card + 0U, page.position, page.area);
        put(cards, card + 4U, page.normal, page.roughness);
        put(cards, card + 8U, page.albedo, page.live ? 1.0F : 0.0F);
        put(cards, card + 12U, page.emission, 0.0F);
        const u64 entry = static_cast<u64>(handle) * kStateVec4s * 4U;
        put(state, entry + 0U, gi::SurfaceCache::outgoing(page),
            page.live && page.valid ? 1.0F : 0.0F);
        put(state, entry + 4U, page.accumulated, 0.0F);

        const u8 live = page.live ? 1U : 0U;
        if (live != card_live_[handle] ||
            (page.live && (page.position.x != card_positions_[handle].x ||
                           page.position.y != card_positions_[handle].y ||
                           page.position.z != card_positions_[handle].z))) {
            layout_changed = true;
        }
        card_live_[handle] = live;
        card_positions_[handle] = page.position;
        card_revisions_[handle] = page.revision;
        report.cards_uploaded += 1;
        report.bytes += (kCardVec4s + kStateVec4s) * kVec4;
    }

    if (layout_changed || grid_.cell_metres() != lookup_radius || grid_.ranges().empty()) {
        if (Status built = grid_.build(pages, lookup_radius); !built) {
            return make_unexpected(built.error());
        }
        std::memcpy(mapped(kGridRanges), grid_.ranges().data(), grid_.ranges().size() * 4U);
        if (!grid_.items().empty()) {
            std::memcpy(mapped(kGridItems), grid_.items().data(), grid_.items().size() * 4U);
        }
        report.grid_rebuilt = true;
        report.bytes += (grid_.ranges().size() + grid_.items().size()) * 4U;
        constants_[kGrid + 0] = grid_.bucket_count();
        constants_[kGrid + 1] = bits(grid_.cell_metres());
        constants_[kGrid + 2] = bits(lookup_radius);
        write_constants();
    }
    card_report_ = report;
    return report;
}

Status GpuGiScene::upload_lights(Span<const gi::GiLight> lights) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the GI scene is not created");
    }
    if (lights.size() > desc_.max_lights) {
        return fail(ErrorCode::OutOfRange,
                    "more lights than the device GI scene was created for; raise max_lights");
    }
    auto* target = static_cast<f32*>(mapped(kLights));
    for (usize index = 0; index < lights.size(); ++index) {
        const gi::GiLight& light = lights[index];
        const u64 base = static_cast<u64>(index) * kLightVec4s * 4U;
        put(target, base + 0U, light.position, light.intensity);
        put(target, base + 4U, light.direction, light.range);
        put(target, base + 8U, light.colour, light.directional ? 1.0F : 0.0F);
        put(target, base + 12U, Vec3{0.0F, 0.0F, 0.0F}, 0.0F);
    }
    constants_[kCounts + 2] = static_cast<u32>(lights.size());
    write_constants();
    return ok();
}

Status GpuGiScene::upload_shadow_map(const gi::ShadowMap* map) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the GI scene is not created");
    }
    if (map == nullptr || map->depths().empty()) {
        constants_[kShadowEnabled + 0] = 0;
        write_constants();
        return ok();
    }
    const gi::ShadowMapSettings& settings = map->settings();
    if (settings.resolution > desc_.max_shadow_resolution) {
        return fail(ErrorCode::OutOfRange,
                    "the shadow map is larger than the device GI scene was created for");
    }
    std::memcpy(mapped(kShadowDepths), map->depths().data(), map->depths().size() * sizeof(f32));
    put(constants_, kShadowCentre, settings.centre, bits(settings.half_extent_metres));
    put(constants_, kShadowDir, map->direction(), bits(settings.depth_range_metres));
    put(constants_, kShadowRight, map->right(), settings.resolution);
    put(constants_, kShadowUp, map->up(), bits(settings.bias_metres));
    constants_[kShadowEnabled + 0] = 1;
    write_constants();
    return ok();
}

void GpuGiScene::set_gather(const gi::CardGatherSettings& gather) noexcept {
    constants_[kCounts + 3] = gather.rays;
    constants_[kGrid + 3] = bits(gather.max_distance_metres);
    put(constants_, kSkyZenith, gather.sky.zenith, bits(gather.sky.intensity));
    put(constants_, kSkyHorizon, gather.sky.horizon, 0U);
    put(constants_, kSkyGround, gather.sky.ground, 0U);
    write_constants();
}

// --- Recording ------------------------------------------------------------------------------------

ResourceId GpuGiScene::import(RenderGraph& graph, Binding binding) noexcept {
    static constexpr const char* kNames[kBufferCount] = {
        "gi constants",  "gi page table",    "gi brick pool",  "gi cards",
        "gi card state", "gi grid ranges",   "gi grid items",  "gi lights",
        "gi shadow",     "gi selection",     "gi results",     "gi rays",
        "gi hits",       "gi results host",  "gi hits host",
    };
    BufferRequest request;
    request.name = kNames[binding];
    request.size = sizes_[binding];
    return graph.import_buffer(request, buffers_[binding]);
}

void GpuGiScene::record_dispatch(const PassContext& context, void* user) noexcept {
    const auto* recording = static_cast<const Recording*>(user);
    GpuGiScene& self = *recording->self;
    context.commands->bind_compute_pipeline(self.pipelines_[recording->pipeline]);
    context.commands->bind_descriptor_sets(self.pipeline_layout_, 0,
                                           Span<const rhi::DescriptorSetHandle>(&self.set_, 1));
    context.commands->push_constants(
        self.pipeline_layout_, rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&recording->dispatch), sizeof(Dispatch)));
    context.commands->dispatch(groups(recording->dispatch.count), 1, 1);
}

void GpuGiScene::record_copy(const PassContext& context, void* user) noexcept {
    const auto* copy = static_cast<const Copy*>(user);
    const rhi::BufferCopy region{0, 0, copy->bytes};
    context.commands->copy_buffer(copy->self->buffers_[copy->source],
                                  copy->self->buffers_[copy->destination],
                                  Span<const rhi::BufferCopy>(&region, 1));
}

// --- The trace batch ------------------------------------------------------------------------------

Status GpuGiScene::set_rays(Span<const GpuTraceRay> rays) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the GI scene is not created");
    }
    if (rays.empty() || rays.size() > desc_.max_rays) {
        return fail(ErrorCode::OutOfRange,
                    "a trace batch must hold between one ray and max_rays rays");
    }
    auto* target = static_cast<f32*>(mapped(kRays));
    for (usize index = 0; index < rays.size(); ++index) {
        const u64 base = static_cast<u64>(index) * kRayVec4s * 4U;
        put(target, base + 0U, rays[index].origin, rays[index].max_distance);
        put(target, base + 4U, rays[index].direction, rays[index].t_min);
    }
    ray_count_ = static_cast<u32>(rays.size());
    return ok();
}

Status GpuGiScene::declare_trace(RenderGraph& graph) noexcept {
    if (device_ == nullptr || ray_count_ == 0 || !field_uploaded_) {
        return fail(ErrorCode::InvalidArgument,
                    "a trace needs a created scene, an uploaded field and a batch from set_rays()");
    }
    const ResourceId constants = import(graph, kConstants);
    const ResourceId table = import(graph, kPageTable);
    const ResourceId bricks = import(graph, kBricks);
    const ResourceId rays = import(graph, kRays);
    const ResourceId hits = import(graph, kHits);
    const ResourceId host = import(graph, kHitsReadback);

    trace_ = Recording{this, kTrace, Dispatch{ray_count_, 0, {0, 0}}};
    graph.add_pass("gi trace rays", QueueKind::Graphics)
        .read(constants, Access::ComputeStorageRead)
        .read(table, Access::ComputeStorageRead)
        .read(bricks, Access::ComputeStorageRead)
        .read(rays, Access::ComputeStorageRead)
        .write(hits, Access::ComputeStorageWrite)
        .record(&record_dispatch, &trace_);

    hits_copy_ = Copy{this, kHits, kHitsReadback, ray_count_ * kHitVec4s * kVec4};
    graph.add_pass("gi hits readback", QueueKind::Graphics)
        .read(hits, Access::TransferRead)
        .write(host, Access::TransferWrite)
        .record(&record_copy, &hits_copy_);
    graph.add_pass("gi hits host", QueueKind::Graphics).read(host, Access::HostRead).side_effect();
    return graph.status();
}

Status GpuGiScene::read_back_hits(Span<GpuTraceHit> out) const noexcept {
    if (device_ == nullptr || out.size() > ray_count_) {
        return fail(ErrorCode::InvalidArgument, "the hit span is larger than the last batch");
    }
    const auto* hits = static_cast<const f32*>(mapped(kHitsReadback));
    if (hits == nullptr) {
        return fail(ErrorCode::Internal, "the GI hits readback buffer is not mapped");
    }
    for (usize index = 0; index < out.size(); ++index) {
        const f32* record = hits + (index * kHitVec4s * 4U);
        GpuTraceHit& hit = out[index];
        hit.hit = record[0] != 0.0F;
        hit.t = record[1];
        hit.closest_approach_metres = record[2];
        hit.exhausted = record[3] != 0.0F;
        hit.position = Vec3{record[4], record[5], record[6]};
        hit.normal = Vec3{record[8], record[9], record[10]};
    }
    return ok();
}

}  // namespace cy::rendering::gi_gpu
