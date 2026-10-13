// SPDX-License-Identifier: MIT
#include <cy/rendering/raytracing_gpu/device_structures.h>

#include <cstring>
#include <initializer_list>

namespace cy::rendering::rt_gpu {
namespace {

/// Four `float4` per traced instance; see `DeviceStructureBinding`.
constexpr u64 kInstanceRecordBytes = 64;

[[nodiscard]] u32 bits(f32 value) noexcept {
    u32 word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

/// The first three rows of an object-to-world matrix, row-major: the RHI's instance transform.
void rows_of(const Mat4& m, f32 (&out)[12]) noexcept {
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            out[(row * 4U) + column] = m.columns[column][row];
        }
    }
}

[[nodiscard]] bool same_matrix(const Mat4& a, const Mat4& b) noexcept {
    return a == b;
}

}  // namespace

DeviceStructures::DeviceStructures() noexcept = default;

DeviceStructures::~DeviceStructures() {
    destroy();
}

bool DeviceStructures::supported(const rhi::Device& device) noexcept {
    return device.capabilities().has(rhi::Capability::RayTracing);
}

Status DeviceStructures::create(Allocator& allocator, rhi::Device& device,
                                const DeviceStructuresDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device structures are already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "this device reports no Capability::RayTracing, so it builds no acceleration "
                    "structures; the software tier is the answer on it");
    }
    if (desc.max_bottom_levels == 0 || desc.max_instances == 0 || desc.max_vertices == 0 ||
        desc.max_indices == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "DeviceStructuresDescription: every capacity must be non-zero");
    }
    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;

    const auto pool = [&](const char* name, u64 size, rhi::BufferUsage usage,
                          rhi::BufferHandle& out) -> Status {
        rhi::BufferDescription description;
        description.name = name;
        description.size = size;
        description.usage = usage;
        description.memory = rhi::MemoryUse::HostVisibleDeviceLocal;
        Expected<rhi::BufferHandle, Error> made = device.create_buffer(description);
        if (!made) {
            return make_unexpected(made.error());
        }
        out = *made;
        return ok();
    };
    const auto input = rhi::BufferUsage::Storage | rhi::BufferUsage::AccelerationStructureInput;
    Status made =
        pool("rt vertex pool", static_cast<u64>(desc.max_vertices) * 12U, input, positions_);
    if (made) {
        made = pool("rt index pool", static_cast<u64>(desc.max_indices) * 4U, input, indices_);
    }
    if (made) {
        made =
            pool("rt instance table", static_cast<u64>(desc.max_instances) * kInstanceRecordBytes,
                 rhi::BufferUsage::Storage, instance_table_);
    }
    if (made) {
        rhi::AccelerationStructureDescription top;
        top.name = "rt top level";
        top.kind = rhi::AccelerationStructureKind::TopLevel;
        top.max_instances = desc.max_instances;
        top.allow_update = true;
        Expected<rhi::AccelerationStructureHandle, Error> structure =
            device.create_acceleration_structure(top);
        if (structure) {
            top_ = *structure;
        } else {
            made = make_unexpected(structure.error());
        }
    }
    if (made) {
        made = bottoms_.resize(desc.max_bottom_levels);
    }
    if (made) {
        made = top_records_.reserve(desc.max_instances);
    }
    if (made) {
        made = top_instances_.reserve(desc.max_instances);
    }
    if (!made) {
        destroy();
        return made;
    }
    for (Bottom& bottom : bottoms_) {
        bottom = Bottom{};
    }
    totals_.pool_bytes = (static_cast<u64>(desc.max_vertices) * 12U) +
                         (static_cast<u64>(desc.max_indices) * 4U) +
                         (static_cast<u64>(desc.max_instances) * kInstanceRecordBytes);
    return ok();
}

void DeviceStructures::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (Bottom& bottom : bottoms_) {
        if (bottom.structure) {
            device_->destroy_acceleration_structure(bottom.structure);
        }
        bottom = Bottom{};
    }
    if (top_) {
        device_->destroy_acceleration_structure(top_);
    }
    for (rhi::BufferHandle* buffer : {&positions_, &indices_, &instance_table_}) {
        if (*buffer) {
            device_->destroy_buffer(*buffer);
        }
        *buffer = {};
    }
    top_ = {};
    bottoms_.clear();
    free_vertices_.clear();
    free_indices_.clear();
    top_instances_.clear();
    top_records_.clear();
    bottom_builds_.clear();
    vertex_high_ = 0;
    index_high_ = 0;
    top_count_ = 0;
    top_built_ = false;
    top_pending_ = false;
    top_update_ = false;
    frame_ = {};
    pending_ = {};
    totals_ = {};
    device_ = nullptr;
    allocator_ = nullptr;
}

// --- The pools -----------------------------------------------------------------------------------

bool DeviceStructures::allocate(Array<Range>& free, u32 capacity, u32& high, u32 count,
                                u32& base) noexcept {
    // First fit over the returned ranges, then the untouched tail. A world that streams geometry in
    // and out fragments this; the pools are sized for the level, and a refusal is counted rather
    // than silently dropping a structure.
    for (usize index = 0; index < free.size(); ++index) {
        Range& range = free[index];
        if (range.count < count) {
            continue;
        }
        base = range.base;
        range.base += count;
        range.count -= count;
        if (range.count == 0) {
            free[index] = free[free.size() - 1];
            free.pop_back();
        }
        return true;
    }
    if (capacity - high < count) {
        return false;
    }
    base = high;
    high += count;
    return true;
}

void DeviceStructures::release(Array<Range>& free, u32 base, u32 count) noexcept {
    if (count != 0) {
        (void)free.push_back(Range{base, count});
    }
}

void DeviceStructures::release_bottom(Bottom& bottom) noexcept {
    if (bottom.structure) {
        device_->destroy_acceleration_structure(bottom.structure);
    }
    release(free_vertices_, bottom.vertex_base, bottom.vertex_count);
    release(free_indices_, bottom.index_base, bottom.index_count);
    bottom = Bottom{};
}

void DeviceStructures::write_positions(const Bottom& bottom, Span<const Vec3> positions) noexcept {
    auto* target = static_cast<f32*>(device_->buffer_mapped_pointer(positions_));
    for (usize index = 0; index < positions.size(); ++index) {
        f32* vertex = target + ((static_cast<usize>(bottom.vertex_base) + index) * 3U);
        vertex[0] = positions[index].x;
        vertex[1] = positions[index].y;
        vertex[2] = positions[index].z;
    }
}

// --- The mirror ----------------------------------------------------------------------------------

void DeviceStructures::bottom_built(u32 slot, const rt::BottomLevelView& view) noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (slot >= bottoms_.size()) {
        pending_.refusals += 1;
        return;
    }
    Bottom& bottom = bottoms_[slot];
    const auto vertices = static_cast<u32>(view.positions.size());
    const auto indices = static_cast<u32>(view.indices.size());
    // Procedural geometry is bounds and an intersection shader; this module has no intersection
    // shader, so the device traces nothing for it and the instance is left out of the top level.
    if (view.procedural || vertices == 0 || indices < 3) {
        if (bottom.live) {
            release_bottom(bottom);
        }
        return;
    }
    const bool fits = bottom.live && bottom.vertex_count == vertices &&
                      bottom.index_count == indices && bottom.maintenance == view.maintenance;
    if (!fits) {
        if (bottom.live) {
            release_bottom(bottom);
        }
        u32 vertex_base = 0;
        u32 index_base = 0;
        if (!allocate(free_vertices_, desc_.max_vertices, vertex_high_, vertices, vertex_base)) {
            pending_.refusals += 1;
            return;
        }
        if (!allocate(free_indices_, desc_.max_indices, index_high_, indices, index_base)) {
            release(free_vertices_, vertex_base, vertices);
            pending_.refusals += 1;
            return;
        }
        rhi::AccelerationStructureDescription structure;
        structure.name = "rt bottom level";
        structure.kind = rhi::AccelerationStructureKind::BottomLevel;
        structure.max_triangles = indices / 3U;
        structure.max_vertices = vertices;
        // Only a refit geometry is ever updated in place; the others are built, so they do not pay
        // for the update scratch.
        structure.allow_update = view.maintenance == rt::MaintenancePolicy::Refit;
        Expected<rhi::AccelerationStructureHandle, Error> created =
            device_->create_acceleration_structure(structure);
        if (!created) {
            release(free_vertices_, vertex_base, vertices);
            release(free_indices_, index_base, indices);
            pending_.refusals += 1;
            return;
        }
        bottom.structure = *created;
        bottom.maintenance = view.maintenance;
        bottom.vertex_base = vertex_base;
        bottom.vertex_count = vertices;
        bottom.index_base = index_base;
        bottom.index_count = indices;
        bottom.live = true;
    }
    write_positions(bottom, view.positions);
    auto* target = static_cast<u32*>(device_->buffer_mapped_pointer(indices_));
    std::memcpy(target + bottom.index_base, view.indices.data(), view.indices.size() * sizeof(u32));
    bottom.declared_error_metres = view.declared_error_metres;
    bottom.pending_build = true;
    bottom.pending_update = false;
    bottom.rebuilt = true;
}

void DeviceStructures::bottom_refit(u32 slot, const rt::BottomLevelView& view) noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (slot >= bottoms_.size()) {
        pending_.refusals += 1;
        return;
    }
    Bottom& bottom = bottoms_[slot];
    // A refit is an in-place update of the same triangles. Anything else — a slot the device never
    // built, a vertex count that moved, a structure created without the update flag — is a build.
    if (!bottom.live || bottom.vertex_count != view.positions.size() ||
        bottom.maintenance != rt::MaintenancePolicy::Refit) {
        bottom_built(slot, view);
        return;
    }
    write_positions(bottom, view.positions);
    if (!bottom.pending_build) {
        bottom.pending_update = true;
    }
}

void DeviceStructures::bottom_evicted(u32 slot) noexcept {
    if (device_ == nullptr || slot >= bottoms_.size() || !bottoms_[slot].live) {
        return;
    }
    release_bottom(bottoms_[slot]);
    pending_.bottom_evictions += 1;
}

bool DeviceStructures::same_top(Span<const rt::TopLevelInstance> instances,
                                bool& transforms_equal) const noexcept {
    transforms_equal = true;
    if (!top_built_ || instances.size() != top_instances_.size()) {
        return false;
    }
    for (usize index = 0; index < instances.size(); ++index) {
        const rt::TopLevelInstance& now = instances[index];
        const rt::TopLevelInstance& then = top_instances_[index];
        if (now.bottom != then.bottom || now.instance_id != then.instance_id ||
            bottoms_[now.bottom].rebuilt) {
            return false;
        }
        if (!same_matrix(now.transform, then.transform)) {
            transforms_equal = false;
        }
    }
    return true;
}

void DeviceStructures::write_instance_table(Span<const rt::TopLevelInstance> instances) noexcept {
    auto* words = static_cast<u32*>(device_->buffer_mapped_pointer(instance_table_));
    for (usize index = 0; index < instances.size(); ++index) {
        const rt::TopLevelInstance& instance = instances[index];
        const Bottom& bottom = bottoms_[instance.bottom];
        u32* record = words + (index * 16U);
        record[0] = bottom.vertex_base;
        record[1] = bottom.index_base;
        record[2] = bottom.index_count / 3U;
        record[3] = instance.instance_id;
        // The normal matrix's COLUMNS, as `AccelerationService::trace_instance` multiplies them:
        // `m * v` is `c0 * v.x + c1 * v.y + c2 * v.z`, and the shader adds in that order.
        const Mat3 normal = normal_matrix(instance.transform.upper3x3());
        for (u32 column = 0; column < 3; ++column) {
            record[4 + (column * 4) + 0] = bits(normal.columns[column].x);
            record[4 + (column * 4) + 1] = bits(normal.columns[column].y);
            record[4 + (column * 4) + 2] = bits(normal.columns[column].z);
            record[4 + (column * 4) + 3] = 0;
        }
        record[7] = bits(bottom.declared_error_metres);
    }
}

void DeviceStructures::top_level(Span<const rt::TopLevelInstance> instances) noexcept {
    if (device_ == nullptr) {
        return;
    }
    // The instances whose geometry the device built. A procedural one, or one the pools refused,
    // is left out here as the service leaves out an instance whose structure is not built yet.
    Array<rt::TopLevelInstance> traced;
    if (!traced.reserve(instances.size())) {
        pending_.refusals += 1;
        return;
    }
    for (const rt::TopLevelInstance& instance : instances) {
        if (instance.bottom < bottoms_.size() && bottoms_[instance.bottom].live) {
            (void)traced.push_back(instance);
        }
    }
    if (traced.size() > desc_.max_instances) {
        pending_.refusals += 1;
        return;
    }

    bool transforms_equal = true;
    const bool same = same_top(traced.span(), transforms_equal);
    bool refit_below = false;
    for (const rt::TopLevelInstance& instance : traced) {
        refit_below = refit_below || bottoms_[instance.bottom].pending_update;
    }
    if (same && transforms_equal && !refit_below) {
        // The top level on the device is this one. A still frame records nothing.
        return;
    }
    top_update_ = same;
    top_pending_ = true;

    top_instances_.clear();
    top_records_.clear();
    for (const rt::TopLevelInstance& instance : traced) {
        (void)top_instances_.push_back(instance);
        rhi::AccelerationStructureInstance record;
        rows_of(instance.transform, record.transform);
        record.custom_index = static_cast<u32>(top_records_.size());
        record.bottom = bottoms_[instance.bottom].structure;
        (void)top_records_.push_back(record);
    }
    top_count_ = static_cast<u32>(traced.size());
    write_instance_table(traced.span());
}

// --- Recording -----------------------------------------------------------------------------------

void DeviceStructures::record_bottoms(const PassContext& context, void* user) noexcept {
    const auto* self = static_cast<const DeviceStructures*>(user);
    context.commands->build_acceleration_structures(self->bottom_builds_.span());
}

void DeviceStructures::record_top(const PassContext& context, void* user) noexcept {
    const auto* self = static_cast<const DeviceStructures*>(user);
    context.commands->build_acceleration_structures(
        Span<const rhi::AccelerationStructureBuild>(&self->top_build_, 1));
}

ResourceId DeviceStructures::import_structure(
    RenderGraph& graph, rhi::AccelerationStructureHandle structure) const noexcept {
    const rhi::AccelerationStructureInfo info = device_->acceleration_structure_info(structure);
    BufferRequest request;
    request.name =
        info.kind == rhi::AccelerationStructureKind::TopLevel ? "rt top level" : "rt bottom level";
    request.size = info.structure_bytes;
    return graph.import_buffer_once(request, info.storage);
}

ResourceId DeviceStructures::import_top(RenderGraph& graph) noexcept {
    if (device_ == nullptr) {
        return kInvalidResource;
    }
    return import_structure(graph, top_);
}

Status DeviceStructures::declare(RenderGraph& graph) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device structures are not created");
    }
    frame_ = pending_;
    pending_ = DeviceStructureReport{};

    // Every queued bottom level first, so the pass that builds them is declared in one piece.
    bottom_builds_.clear();
    for (Bottom& bottom : bottoms_) {
        if (!bottom.live || (!bottom.pending_build && !bottom.pending_update)) {
            continue;
        }
        rhi::AccelerationStructureBuild build;
        build.target = bottom.structure;
        build.mode = bottom.pending_build ? rhi::AccelerationStructureBuildMode::Build
                                          : rhi::AccelerationStructureBuildMode::Update;
        build.triangles.vertices = positions_;
        build.triangles.vertex_offset = static_cast<u64>(bottom.vertex_base) * 12U;
        build.triangles.vertex_count = bottom.vertex_count;
        build.triangles.vertex_stride = 12;
        build.triangles.indices = indices_;
        build.triangles.index_offset = static_cast<u64>(bottom.index_base) * 4U;
        build.triangles.triangle_count = bottom.index_count / 3U;
        if (Status pushed = bottom_builds_.push_back(build); !pushed) {
            return pushed;
        }
        if (bottom.pending_build) {
            frame_.bottom_builds += 1;
        } else {
            frame_.bottom_updates += 1;
        }
        bottom.pending_build = false;
        bottom.pending_update = false;
    }
    if (!bottom_builds_.empty()) {
        PassBuilder pass = graph.add_pass("rt bottom levels", rhi::QueueKind::Graphics);
        for (const rhi::AccelerationStructureBuild& build : bottom_builds_) {
            pass.write(import_structure(graph, build.target),
                       rhi::Access::AccelerationStructureBuildWrite);
        }
        pass.record(&record_bottoms, this);
    }

    if (top_pending_) {
        top_build_ = rhi::AccelerationStructureBuild{};
        top_build_.target = top_;
        top_build_.mode = top_update_ ? rhi::AccelerationStructureBuildMode::Update
                                      : rhi::AccelerationStructureBuildMode::Build;
        top_build_.instances = top_records_.span();
        PassBuilder top = graph.add_pass("rt top level", rhi::QueueKind::Graphics);
        // Each bottom level the instances reference, once: a thousand instances of one mesh are
        // one read of one structure.
        Array<u8> referenced;
        if (Status sized = referenced.resize(bottoms_.size()); !sized) {
            return sized;
        }
        for (u8& flag : referenced) {
            flag = 0;
        }
        for (const rt::TopLevelInstance& instance : top_instances_) {
            referenced[instance.bottom] = 1;
        }
        for (usize slot = 0; slot < bottoms_.size(); ++slot) {
            if (referenced[slot] != 0) {
                top.read(import_structure(graph, bottoms_[slot].structure),
                         rhi::Access::AccelerationStructureBuildRead);
            }
        }
        top.write(import_top(graph), rhi::Access::AccelerationStructureBuildWrite);
        top.record(&record_top, this);
        if (top_update_) {
            frame_.top_updates += 1;
        } else {
            frame_.top_builds += 1;
        }
        top_pending_ = false;
        top_built_ = true;
    } else if (top_built_) {
        frame_.top_unchanged += 1;
    }
    for (Bottom& bottom : bottoms_) {
        bottom.rebuilt = false;
    }

    account();
    totals_.bottom_builds += frame_.bottom_builds;
    totals_.bottom_updates += frame_.bottom_updates;
    totals_.bottom_evictions += frame_.bottom_evictions;
    totals_.top_builds += frame_.top_builds;
    totals_.top_updates += frame_.top_updates;
    totals_.top_unchanged += frame_.top_unchanged;
    totals_.refusals += frame_.refusals;
    return graph.status();
}

void DeviceStructures::account() noexcept {
    u64 bytes = device_->acceleration_structure_info(top_).structure_bytes;
    for (const Bottom& bottom : bottoms_) {
        if (bottom.live) {
            bytes += device_->acceleration_structure_info(bottom.structure).structure_bytes;
        }
    }
    frame_.structure_bytes = bytes;
    frame_.traced_instances = top_count_;
    frame_.pool_bytes = totals_.pool_bytes;
    totals_.structure_bytes = bytes;
    totals_.traced_instances = top_count_;
}

DeviceStructureBinding DeviceStructures::binding() const noexcept {
    DeviceStructureBinding out;
    out.top = top_;
    out.positions = positions_;
    out.indices = indices_;
    out.instances = instance_table_;
    return out;
}

}  // namespace cy::rendering::rt_gpu
