#pragma once
// Shared scaffolding for the render graph's suites.
//
// Every case builds its own graph. A graph is cheap — three arrays and no device — and sharing one
// would make each case depend on the order the others ran in, which is exactly the property a
// derivation test must not have.

#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/graph/visualise.h>

namespace cy::rendering::test {

/// The queue configuration M3's spike measured on: graphics on family 0, a dedicated async compute
/// queue on family 2. Stated here rather than in each case so that "two queues, two families" reads
/// as one decision.
inline CompileOptions two_queue_options() noexcept {
    CompileOptions options;
    options.enable_async_compute = true;
    options.queue_available[static_cast<u32>(rhi::QueueKind::Graphics)] = true;
    options.queue_available[static_cast<u32>(rhi::QueueKind::AsyncCompute)] = true;
    options.queue_family[static_cast<u32>(rhi::QueueKind::Graphics)] = 0;
    options.queue_family[static_cast<u32>(rhi::QueueKind::AsyncCompute)] = 2;
    options.query_memory = &synthetic_memory_query;
    return options;
}

/// The same declarations with async compute off: one queue, one family. This is the null backend's
/// and continuous integration's normal path, and it must fall out of the same code.
inline CompileOptions single_queue_options() noexcept {
    CompileOptions options;
    options.enable_async_compute = false;
    options.queue_available[static_cast<u32>(rhi::QueueKind::Graphics)] = true;
    options.query_memory = &synthetic_memory_query;
    return options;
}

inline TextureRequest colour_target(const char* name, u32 size = 16) noexcept {
    TextureRequest request;
    request.name = name;
    request.format = rhi::Format::Rgba8Unorm;
    request.width = size;
    request.height = size;
    return request;
}

inline TextureRequest storage_image(const char* name, u32 size = 16, u16 layers = 1,
                                    u16 mips = 1) noexcept {
    TextureRequest request;
    request.name = name;
    request.format = rhi::Format::R32Uint;
    request.width = size;
    request.height = size;
    request.array_layers = layers;
    request.mip_levels = mips;
    return request;
}

inline BufferRequest storage_buffer(const char* name, u64 size = 1024) noexcept {
    BufferRequest request;
    request.name = name;
    request.size = size;
    return request;
}

/// Count the barriers of one kind across a whole plan, so a case can assert "this frame emits
/// exactly one ownership release" rather than walking the plan itself.
struct BarrierCounts {
    u32 image = 0;
    u32 buffer = 0;
    u32 memory = 0;
    u32 layout_transitions = 0;
    u32 ownership_releases = 0;
    u32 ownership_acquires = 0;
};

inline BarrierCounts count_barriers(const CompiledGraph& plan) noexcept {
    BarrierCounts counts;
    for (const Submit& submit : plan.submits) {
        for (const ScheduledPass& scheduled : submit.passes) {
            counts.image += static_cast<u32>(scheduled.pre.images.size());
            counts.buffer += static_cast<u32>(scheduled.pre.buffers.size());
            counts.memory += static_cast<u32>(scheduled.pre.memory.size());
            for (const rhi::ImageBarrier& barrier : scheduled.pre.images) {
                if (barrier.old_layout != barrier.new_layout) {
                    ++counts.layout_transitions;
                }
                if (barrier.src_queue_family != barrier.dst_queue_family) {
                    ++counts.ownership_acquires;
                }
            }
        }
        counts.ownership_releases +=
            static_cast<u32>(submit.release.images.size() + submit.release.buffers.size());
    }
    return counts;
}

/// The first barrier in the plan that touches `resource`, or null. Used where a case wants to
/// assert on a specific transition rather than on a count.
inline const rhi::ImageBarrier* find_image_barrier(const CompiledGraph& plan,
                                                   ResourceId resource) noexcept {
    for (const Submit& submit : plan.submits) {
        for (const ScheduledPass& scheduled : submit.passes) {
            for (const rhi::ImageBarrier& barrier : scheduled.pre.images) {
                if (barrier.resource == resource) {
                    return &barrier;
                }
            }
        }
    }
    return nullptr;
}

inline bool plan_contains_pass(const CompiledGraph& plan, PassId pass) noexcept {
    for (const Submit& submit : plan.submits) {
        for (const ScheduledPass& scheduled : submit.passes) {
            if (scheduled.pass == pass) {
                return true;
            }
        }
    }
    return false;
}

/// A null device, destroyed with the fixture. For the cases that run a plan rather than inspect it.
class NullFixture {
public:
    NullFixture() noexcept
        : allocator_(system_allocator(MemoryDomain::Gpu)),
          device_(rhi::null::create_null_device(allocator_, description())) {}

    ~NullFixture() {
        if (device_.has_value()) {
            rhi::null::destroy_null_device(allocator_, device_.value());
        }
    }

    NullFixture(const NullFixture&) = delete;
    NullFixture& operator=(const NullFixture&) = delete;
    NullFixture(NullFixture&&) = delete;
    NullFixture& operator=(NullFixture&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return device_.has_value(); }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] Allocator& allocator() const noexcept { return allocator_; }

private:
    static rhi::DeviceDescription description() noexcept {
        rhi::DeviceDescription desc;
        desc.enable_validation = true;
        return desc;
    }

    Allocator& allocator_;
    Expected<rhi::Device*, Error> device_;
};

}  // namespace cy::rendering::test
