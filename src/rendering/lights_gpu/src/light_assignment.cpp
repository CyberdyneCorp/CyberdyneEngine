// SPDX-License-Identifier: MIT
// The frame's clustered light lists on the device. See light_assignment.h.

#include <cy/rendering/lights_gpu/light_assignment.h>

#include <cy/backends/rhi/command_buffer.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/lighting/lights.h>

#include <cstring>

#include "lights_gpu_msl.h"
#include "lights_gpu_spirv.h"

namespace cy::rendering::lights_gpu {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u32 kStatisticsWords = 4;
constexpr u64 kStatisticsBytes = kStatisticsWords * sizeof(u32);

/// light_assignment.slang's set, in its `ParameterBlock`'s field order.
constexpr rhi::DescriptorKind kBindings[] = {
    rhi::DescriptorKind::StorageBuffer,  // constants
    rhi::DescriptorKind::StorageBuffer,  // elements
    rhi::DescriptorKind::StorageBuffer,  // host headers
    rhi::DescriptorKind::StorageBuffer,  // host indices
    rhi::DescriptorKind::StorageBuffer,  // headers
    rhi::DescriptorKind::StorageBuffer,  // indices
    rhi::DescriptorKind::StorageBuffer,  // statistics
};
constexpr u32 kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);

void record_clear_step(const PassContext& context, void* user) noexcept {
    static_cast<GpuLightAssignment*>(user)->record_clear(context);
}

void record_dispatch_step(const PassContext& context, void* user) noexcept {
    static_cast<GpuLightAssignment*>(user)->record_dispatch(context);
}

void record_readback_step(const PassContext& context, void* user) noexcept {
    static_cast<GpuLightAssignment*>(user)->record_readback(context);
}

void destroy_buffer(rhi::Device& device, rhi::BufferHandle& buffer) noexcept {
    if (!buffer.is_null()) {
        device.destroy_buffer(buffer);
    }
    buffer = {};
}

}  // namespace

GpuClusterElement gpu_cluster_element(const ClusterElement& element) noexcept {
    GpuClusterElement out;
    out.view_position[0] = element.view_position.x;
    out.view_position[1] = element.view_position.y;
    out.view_position[2] = element.view_position.z;
    out.radius = element.radius;
    out.view_direction[0] = element.view_direction.x;
    out.view_direction[1] = element.view_direction.y;
    out.view_direction[2] = element.view_direction.z;
    out.cone_cos = element.cone_cos;
    out.payload = element.payload_index;
    out.layer_mask = element.layer_mask;
    return out;
}

GpuLightAssignment::~GpuLightAssignment() {
    destroy();
}

bool GpuLightAssignment::supported(const rhi::Device& device) noexcept {
    return detail::compute_supported(device);
}

u64 GpuLightAssignment::header_bytes() const noexcept {
    return static_cast<u64>(desc_.grid.cluster_count()) * kClusterElementTypeCount *
           sizeof(ClusterHeader);
}

u64 GpuLightAssignment::index_bytes() const noexcept {
    return (static_cast<u64>(desc_.max_host_indices) +
            (static_cast<u64>(desc_.grid.cluster_count()) * desc_.grid.max_elements_per_cluster)) *
           sizeof(u32);
}

Status GpuLightAssignment::create(Allocator& allocator, rhi::Device& device,
                                  const LightAssignmentDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the light assignment is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "device light assignment is a compute dispatch and this device has no compute "
                    "stage or no shader format this module ships (SPIR-V or MSL)");
    }
    if (desc.grid.cluster_count() == 0 || desc.max_lights == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "LightAssignmentDescription: the grid is empty or no light is allowed");
    }
    if (desc.grid.max_elements_per_cluster == 0 ||
        desc.grid.max_elements_per_cluster > kMaxDeviceListLength ||
        desc.grid.dimensions[2] > kMaxDeviceSlices) {
        return fail(ErrorCode::InvalidArgument,
                    "device light assignment keeps at most 64 lights a cluster and assigns at most "
                    "64 slices: one thread a slice of a 64-thread workgroup, and a 64-entry list a "
                    "thread");
    }
    device_ = &device;
    allocator_ = &allocator;
    desc_ = desc;

    detail::ComputeProgram program;
    program.name = "light assignment";
    program.spirv = {kAssignLightsSpirv, sizeof(kAssignLightsSpirv) / sizeof(u32)};
    program.msl = {reinterpret_cast<const u8*>(kAssignLightsMsl), sizeof(kAssignLightsMsl) - 1};
    program.msl_entry = "cyAssignLights";
    program.bindings = Span<const rhi::DescriptorKind>(kBindings, kBindingCount);
    program.workgroup[0] = 64;
    if (Status made = pipeline_.create(device, program); !made) {
        destroy();
        return made;
    }
    if (Status made = create_buffers(); !made) {
        destroy();
        return made;
    }
    return ok();
}

Status GpuLightAssignment::create_buffers() noexcept {
    rhi::Device& device = *device_;
    const u32 slots = device.frames_in_flight() < rhi::kMaxFramesInFlight
                          ? device.frames_in_flight()
                          : rhi::kMaxFramesInFlight;
    struct Ring {
        const char* name;
        u64 size;
        rhi::BufferHandle* buffers;
    };
    const Ring rings[] = {
        {"light assignment elements", u64{desc_.max_lights} * sizeof(GpuClusterElement),
         elements_ring_},
        {"light assignment constants", sizeof(AssignmentConstants), constants_ring_},
        {"light assignment host headers", header_bytes(), host_headers_ring_},
        {"light assignment host indices", u64{desc_.max_host_indices} * sizeof(u32),
         host_indices_ring_},
    };
    for (const Ring& ring : rings) {
        for (u32 slot = 0; slot < slots; ++slot) {
            Expected<rhi::BufferHandle, Error> buffer =
                detail::host_buffer(device, ring.name, ring.size, rhi::BufferUsage::Storage);
            if (!buffer.has_value()) {
                return make_unexpected(buffer.error());
            }
            ring.buffers[slot] = *buffer;
        }
    }
    const rhi::BufferUsage lists = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
    Expected<rhi::BufferHandle, Error> made =
        detail::device_buffer(device, "light assignment headers", header_bytes(), lists);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    headers_ = *made;
    made = detail::device_buffer(device, "light assignment indices", index_bytes(), lists);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    indices_ = *made;
    made = detail::device_buffer(device, "light assignment lights",
                                 u64{desc_.max_lights} * sizeof(GpuLight),
                                 rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDestination);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    lights_ = *made;
    made = detail::device_buffer(device, "light assignment statistics", kStatisticsBytes,
                                 lists | rhi::BufferUsage::TransferDestination);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    statistics_ = *made;
    made = detail::host_buffer(device, "light assignment statistics zero", kStatisticsBytes,
                               rhi::BufferUsage::TransferSource);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    statistics_zero_ = *made;
    std::memset(device.buffer_mapped_pointer(statistics_zero_), 0, kStatisticsBytes);

    rhi::BufferDescription host;
    host.name = "light assignment statistics readback";
    host.size = kStatisticsBytes;
    host.usage = rhi::BufferUsage::TransferDestination;
    host.memory = rhi::MemoryUse::Readback;
    made = device.create_buffer(host);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    statistics_readback_ = *made;
    if (desc_.readback) {
        host.name = "light assignment headers readback";
        host.size = header_bytes();
        made = device.create_buffer(host);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        headers_readback_ = *made;
        host.name = "light assignment indices readback";
        host.size = index_bytes();
        made = device.create_buffer(host);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        indices_readback_ = *made;
    }
    if (device.capabilities().has(rhi::Capability::TimestampQueries)) {
        rhi::QueryPoolDescription timer;
        timer.name = "light assignment timestamps";
        timer.kind = rhi::QueryKind::Timestamp;
        timer.count = 2;
        if (Expected<rhi::QueryPoolHandle, Error> pool = device.create_query_pool(timer); pool) {
            timer_ = *pool;
        }
    }
    return ok();
}

void GpuLightAssignment::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    rhi::Device& device = *device_;
    (void)device.wait_idle();
    for (rhi::BufferHandle* ring :
         {elements_ring_, constants_ring_, host_headers_ring_, host_indices_ring_}) {
        for (u32 slot = 0; slot < rhi::kMaxFramesInFlight; ++slot) {
            destroy_buffer(device, ring[slot]);
        }
    }
    for (rhi::BufferHandle* buffer :
         {&headers_, &indices_, &lights_, &statistics_, &statistics_zero_, &statistics_readback_,
          &headers_readback_, &indices_readback_}) {
        destroy_buffer(device, *buffer);
    }
    if (timer_) {
        device.destroy_query_pool(timer_);
    }
    timer_ = {};
    timed_ = false;
    counted_ = false;
    pipeline_.destroy(device);
    elements_.clear();
    host_headers_.clear();
    host_indices_.clear();
    framed_ = false;
    device_ = nullptr;
    allocator_ = nullptr;
}

LightListResources GpuLightAssignment::import_lists(RenderGraph& graph) noexcept {
    LightListResources lists;
    if (device_ == nullptr) {
        return lists;
    }
    BufferRequest request;
    request.name = "cluster headers";
    request.size = header_bytes();
    request.extra_usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
    headers_resource_ = graph.import_buffer(request, headers_);
    request.name = "cluster indices";
    request.size = index_bytes();
    indices_resource_ = graph.import_buffer(request, indices_);
    request.name = "lights";
    request.size = u64{desc_.max_lights} * sizeof(GpuLight);
    request.extra_usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDestination;
    lights_resource_ = graph.import_buffer(request, lights_);
    lists.headers = headers_resource_;
    lists.indices = indices_resource_;
    lists.lights = lights_resource_;
    return lists;
}

Status GpuLightAssignment::set_frame(const LightAssignmentFrame& frame) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::Unavailable, "light assignment: create() was not called");
    }
    if (frame.lights.size() > desc_.max_lights) {
        return fail(ErrorCode::OutOfRange,
                    "light assignment: more lights than LightAssignmentDescription::max_lights");
    }
    elements_.clear();
    if (Status sized = elements_.resize(frame.lights.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < frame.lights.size(); ++index) {
        elements_[index] = gpu_cluster_element(frame.lights[index]);
    }
    host_headers_.clear();
    host_indices_.clear();
    const bool host_lists = frame.host != nullptr && !frame.host->indices.empty();
    if (host_lists) {
        if (frame.host->headers.size() * sizeof(ClusterHeader) != header_bytes()) {
            return fail(ErrorCode::InvalidArgument,
                        "light assignment: the host's lists are for a different grid");
        }
        if (frame.host->indices.size() > desc_.max_host_indices) {
            return fail(ErrorCode::OutOfRange,
                        "light assignment: the host's lists hold more indices than "
                        "LightAssignmentDescription::max_host_indices");
        }
        if (Status copied = host_headers_.append(frame.host->headers.span()); !copied) {
            return copied;
        }
        if (Status copied = host_indices_.append(frame.host->indices.span()); !copied) {
            return copied;
        }
    }
    const ClusterGrid& grid = desc_.grid;
    constants_ = AssignmentConstants{};
    constants_.dimensions[0] = grid.dimensions[0];
    constants_.dimensions[1] = grid.dimensions[1];
    constants_.dimensions[2] = grid.dimensions[2];
    constants_.dimensions[3] = grid.max_elements_per_cluster;
    constants_.planes[0] = grid.near_plane;
    constants_.planes[1] = grid.far_plane;
    constants_.planes[2] = grid.slice_scale;
    constants_.planes[3] = grid.slice_bias;
    constants_.view[0] = frame.tan_half_fov_y;
    constants_.view[1] = frame.aspect;
    constants_.counts[0] = static_cast<u32>(elements_.size());
    constants_.counts[1] = frame.view_layer_mask;
    constants_.counts[2] = kClusterElementTypeCount;
    constants_.counts[3] = light_base();
    constants_.copies[0] = host_lists ? 1U : 0U;
    constants_.copies[1] = static_cast<u32>(host_indices_.size());
    framed_ = true;
    return ok();
}

FrameStageDeclaration GpuLightAssignment::stage() noexcept {
    return FrameStageDeclaration{&declare_stage, this};
}

PassId GpuLightAssignment::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                         void* user) noexcept {
    return static_cast<GpuLightAssignment*>(user)->declare(graph, inputs);
}

PassId GpuLightAssignment::declare(RenderGraph& graph,
                                   const ScreenSpaceStageInputs& inputs) noexcept {
    // The lists are the ones `import_lists` put in this graph, or the frame would read buffers the
    // dispatch never wrote.
    if (device_ == nullptr || headers_resource_ == kInvalidResource ||
        inputs.target != headers_resource_) {
        return kInvalidPass;
    }
    BufferRequest request;
    request.name = "light assignment statistics";
    request.size = kStatisticsBytes;
    request.extra_usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource |
                          rhi::BufferUsage::TransferDestination;
    statistics_resource_ = graph.import_buffer(request, statistics_);
    request.name = "light assignment statistics readback";
    request.extra_usage = rhi::BufferUsage::TransferDestination;
    statistics_readback_resource_ = graph.import_buffer(request, statistics_readback_);

    const PassId first = graph.add_pass("light assignment clear", QueueKind::Graphics)
                             .write(statistics_resource_, Access::TransferWrite)
                             .record(&record_clear_step, this)
                             .id();
    graph.add_pass("light assignment", QueueKind::Graphics)
        .write(headers_resource_, Access::ComputeStorageWrite)
        .write(indices_resource_, Access::ComputeStorageWrite)
        // The counters are added to, so the dispatch reads what the clear wrote.
        .use(statistics_resource_, Access::ComputeStorageReadWrite)
        .record(&record_dispatch_step, this);
    PassBuilder readback = graph.add_pass("light assignment readback", QueueKind::Graphics);
    readback.read(statistics_resource_, Access::TransferRead)
        .write(statistics_readback_resource_, Access::TransferWrite);
    if (desc_.readback) {
        request.name = "light assignment headers readback";
        request.size = header_bytes();
        headers_readback_resource_ = graph.import_buffer(request, headers_readback_);
        request.name = "light assignment indices readback";
        request.size = index_bytes();
        indices_readback_resource_ = graph.import_buffer(request, indices_readback_);
        readback.read(headers_resource_, Access::TransferRead)
            .read(indices_resource_, Access::TransferRead)
            .write(headers_readback_resource_, Access::TransferWrite)
            .write(indices_readback_resource_, Access::TransferWrite);
    }
    readback.record(&record_readback_step, this);
    PassBuilder host = graph.add_pass("light assignment host", QueueKind::Graphics);
    host.read(statistics_readback_resource_, Access::HostRead);
    if (desc_.readback) {
        host.read(headers_readback_resource_, Access::HostRead)
            .read(indices_readback_resource_, Access::HostRead);
    }
    host.side_effect();
    return first;
}

void GpuLightAssignment::record_clear(const PassContext& context) noexcept {
    rhi::BufferCopy region;
    region.size = kStatisticsBytes;
    context.commands->copy_buffer(statistics_zero_, statistics_,
                                  Span<const rhi::BufferCopy>(&region, 1));
}

void GpuLightAssignment::record_dispatch(const PassContext& context) noexcept {
    if (!framed_) {
        return;
    }
    rhi::Device& device = *device_;
    // WRITTEN WHEN THE FRAME RECORDS, into the ring slot of the frame being recorded:
    // `begin_frame` has already waited for the frame that last used it.
    const u32 slot = device.frame_slot() % rhi::kMaxFramesInFlight;
    const auto write = [&](rhi::BufferHandle buffer, const void* data, usize bytes) noexcept {
        void* mapped = buffer.is_null() ? nullptr : device.buffer_mapped_pointer(buffer);
        if (mapped != nullptr && bytes > 0) {
            std::memcpy(mapped, data, bytes);
        }
        return mapped != nullptr;
    };
    if (!write(constants_ring_[slot], &constants_, sizeof(constants_)) ||
        !write(elements_ring_[slot], elements_.data(),
               elements_.size() * sizeof(GpuClusterElement)) ||
        !write(host_headers_ring_[slot], host_headers_.data(),
               host_headers_.size() * sizeof(ClusterHeader)) ||
        !write(host_indices_ring_[slot], host_indices_.data(),
               host_indices_.size() * sizeof(u32))) {
        return;
    }
    const rhi::DescriptorWrite writes[kBindingCount] = {
        detail::storage_write(0, constants_ring_[slot]),
        detail::storage_write(1, elements_ring_[slot]),
        detail::storage_write(2, host_headers_ring_[slot]),
        detail::storage_write(3, host_indices_ring_[slot]),
        detail::storage_write(4, headers_),
        detail::storage_write(5, indices_),
        detail::storage_write(6, statistics_),
    };
    Expected<rhi::DescriptorSetHandle, Error> set =
        pipeline_.set(device, Span<const rhi::DescriptorWrite>(writes, kBindingCount));
    if (!set.has_value()) {
        return;
    }
    rhi::CommandBuffer& commands = *context.commands;
    if (timer_) {
        commands.reset_queries(timer_, 0, 2);
        commands.write_timestamp(timer_, 0);
    }
    commands.bind_compute_pipeline(pipeline_.pipeline());
    commands.bind_descriptor_sets(pipeline_.layout(), 0,
                                  Span<const rhi::DescriptorSetHandle>(&*set, 1));
    commands.dispatch(desc_.grid.dimensions[0], desc_.grid.dimensions[1], 1);
    if (timer_) {
        commands.write_timestamp(timer_, 1);
        timed_ = true;
    }
    counted_ = true;
}

void GpuLightAssignment::record_readback(const PassContext& context) noexcept {
    rhi::BufferCopy region;
    region.size = kStatisticsBytes;
    context.commands->copy_buffer(statistics_, statistics_readback_,
                                  Span<const rhi::BufferCopy>(&region, 1));
    if (!desc_.readback) {
        return;
    }
    region.size = header_bytes();
    context.commands->copy_buffer(headers_, headers_readback_,
                                  Span<const rhi::BufferCopy>(&region, 1));
    region.size = index_bytes();
    context.commands->copy_buffer(indices_, indices_readback_,
                                  Span<const rhi::BufferCopy>(&region, 1));
}

AssignmentStatistics GpuLightAssignment::statistics() const noexcept {
    AssignmentStatistics statistics;
    if (device_ == nullptr || !counted_) {
        return statistics;
    }
    const auto* words =
        static_cast<const u32*>(device_->buffer_mapped_pointer(statistics_readback_));
    if (words == nullptr) {
        return statistics;
    }
    statistics.overflow = words[0];
    statistics.assignments = words[1];
    statistics.max_per_cluster = words[2];
    return statistics;
}

f64 GpuLightAssignment::last_device_ms() const noexcept {
    if (device_ == nullptr || !timer_ || !timed_) {
        return -1.0;
    }
    u64 stamps[2] = {};
    Expected<u32, Error> read = device_->read_query_results(timer_, 0, 2, Span<u64>(stamps, 2));
    if (!read || *read != 2 || stamps[1] < stamps[0]) {
        return -1.0;
    }
    return static_cast<f64>(stamps[1] - stamps[0]) / 1.0e6;
}

Status GpuLightAssignment::read_back(Span<ClusterHeader> headers,
                                     Span<u32> indices) const noexcept {
    if (device_ == nullptr || headers_readback_.is_null()) {
        return fail(ErrorCode::Unavailable,
                    "light assignment: created without `readback`, so there is nothing to read");
    }
    if (headers.size_bytes() != header_bytes() || indices.size_bytes() != index_bytes()) {
        return fail(ErrorCode::InvalidArgument,
                    "light assignment: the readback spans are not the lists' sizes");
    }
    const void* mapped_headers = device_->buffer_mapped_pointer(headers_readback_);
    const void* mapped_indices = device_->buffer_mapped_pointer(indices_readback_);
    if (mapped_headers == nullptr || mapped_indices == nullptr) {
        return fail(ErrorCode::Internal, "light assignment: the readback buffers are not mapped");
    }
    std::memcpy(headers.data(), mapped_headers, headers.size_bytes());
    std::memcpy(indices.data(), mapped_indices, indices.size_bytes());
    return ok();
}

}  // namespace cy::rendering::lights_gpu
