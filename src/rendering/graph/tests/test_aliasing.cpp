// Transient memory aliasing, and the dependency it creates that the resource graph cannot see.
// Tasks 2.2.2 and 7.3.
//
// `rhi-and-render-graph`: "two intermediate render targets [with] non-overlapping lifetimes ...
// SHALL share memory, reducing peak GPU memory". M3's spike measured this on the device with
// VK_EXT_memory_budget: sixteen 4 MiB transients in a read-modify-write chain went from a 64.00 MiB
// heap delta to 8.00 MiB, and the plan and the device agreed exactly. The same arithmetic is
// asserted here with no device, which is what makes it a regression gate rather than a measurement
// somebody has to remember to take.
//
// THE DEFECT THIS FILE ALSO PINS. Aliasing creates dependencies the resource graph cannot see: two
// chains that share no resource, one per queue, whose transients the aliaser put on the same bytes,
// with no semaphore between them. A VkMemoryBarrier2 in the consumer's command buffer synchronises
// nothing across a queue. Validation does not catch it — the spike's control produced zero errors —
// so it is covered here, structurally, by asserting the semaphore exists.

#include <cy/test/test.h>

#include "fixtures.h"

using cy::rendering::CompiledGraph;
using cy::rendering::RenderGraph;
using cy::rendering::ResourceId;
using cy::rhi::Access;
using cy::rhi::QueueKind;
using namespace cy::rendering::test;

namespace {

constexpr cy::u32 kChainLength = 16;
constexpr cy::u64 kMebibyte = 1024ULL * 1024ULL;

/// A read-modify-write chain of `kChainLength` 1024x1024 R32_UINT transients — 4 MiB each. Each
/// step reads the previous image and writes the next, so consecutive lifetimes overlap and
/// alternating ones do not: the whole chain ping-pongs between two slots.
struct Chain {
    RenderGraph graph;
    ResourceId images[kChainLength] = {};
    ResourceId output = cy::rendering::kInvalidResource;

    Chain() noexcept : graph(cy::system_allocator(cy::MemoryDomain::Renderer)) {
        for (ResourceId& image : images) {
            image = graph.create_texture(storage_image("link", 1024));
        }
        output = graph.import_buffer(storage_buffer("result", 4096),
                                     cy::rhi::BufferHandle::from_slot(0, 1));

        graph.add_pass("seed", QueueKind::Graphics).write(images[0], Access::ComputeStorageWrite);
        for (cy::u32 index = 1; index < kChainLength; ++index) {
            graph.add_pass("step", QueueKind::Graphics)
                .read(images[index - 1], Access::ComputeStorageRead)
                .write(images[index], Access::ComputeStorageWrite);
        }
        graph.add_pass("copy out", QueueKind::Graphics)
            .read(images[kChainLength - 1], Access::TransferRead)
            .write(output, Access::TransferWrite);
    }
};

}  // namespace

CY_TEST_CASE("a chain of transients ping-pongs between two slots") {
    Chain chain;
    CY_REQUIRE(chain.graph.status().has_value());

    cy::rendering::CompileOptions options = single_queue_options();
    options.enable_aliasing = true;
    cy::Expected<CompiledGraph, cy::Error> plan = chain.graph.compile(options);
    CY_REQUIRE(plan.has_value());

    // Sixteen 4 MiB images: 64 MiB without aliasing, 8 MiB with it. The same two numbers the device
    // reported through VK_EXT_memory_budget.
    CY_CHECK_EQ(plan->memory.naive_bytes, 64ULL * kMebibyte);
    CY_CHECK_EQ(plan->memory.heap_bytes, 8ULL * kMebibyte);
    CY_CHECK_EQ(plan->memory.placements.size(), kChainLength);

    // Two distinct offsets, and every image is at one of them.
    for (const cy::rendering::Placement& placement : plan->memory.placements) {
        CY_CHECK((placement.offset == 0 || placement.offset == 4ULL * kMebibyte));
        CY_CHECK_EQ(placement.size, 4ULL * kMebibyte);
    }

    // Fourteen alias barriers: every image from the third onward reuses the memory of one that
    // finished. The first two have no predecessor.
    CY_CHECK_EQ(plan->stats.alias_barriers, kChainLength - 2);

    cy::Expected<cy::rendering::PlanAudit, cy::Error> audit =
        cy::rendering::validate_plan(chain.graph, *plan);
    CY_REQUIRE(audit.has_value());
    CY_CHECK_EQ(audit->transient_peak_bytes, 8ULL * kMebibyte);
}

CY_TEST_CASE("the same chain with aliasing off is the control the reduction is measured against") {
    Chain chain;
    cy::rendering::CompileOptions options = single_queue_options();
    options.enable_aliasing = false;
    cy::Expected<CompiledGraph, cy::Error> plan = chain.graph.compile(options);
    CY_REQUIRE(plan.has_value());

    CY_CHECK_EQ(plan->memory.heap_bytes, 64ULL * kMebibyte);
    CY_CHECK_EQ(plan->memory.naive_bytes, 64ULL * kMebibyte);
    // No memory is shared, so nothing needs an alias barrier.
    CY_CHECK_EQ(plan->stats.alias_barriers, 0U);
    CY_CHECK_EQ(plan->stats.alias_edges, 0U);
}

CY_TEST_CASE("an alias barrier names the predecessor's last access, not the resource's own") {
    Chain chain;
    cy::rendering::CompileOptions options = single_queue_options();
    cy::Expected<CompiledGraph, cy::Error> plan = chain.graph.compile(options);
    CY_REQUIRE(plan.has_value());
    CY_REQUIRE_EQ(plan->submits.size(), 1U);

    // The third step is the first that reuses memory: it writes image 2, which sits where image 0
    // was. The barrier is a plain memory barrier — the two resources are different objects over the
    // same bytes, so naming either of them would be misleading.
    const cy::rhi::BarrierBatch& third = plan->submits[0].passes[2].pre;
    CY_CHECK_GE(third.memory.size(), 1U);
    bool found = false;
    for (const cy::rhi::MemoryBarrier& barrier : third.memory) {
        if (barrier.src_stage == cy::rhi::Stage::ComputeShader &&
            barrier.dst_stage == cy::rhi::Stage::ComputeShader) {
            found = true;
        }
    }
    CY_CHECK(found);
}

CY_TEST_CASE(
    "aliasing across queues becomes a semaphore, because a barrier would synchronise nothing") {
    // The defect M3's spike found and fixed, as case "F". Two independent chains sharing no
    // resource, one on each queue, whose transients the aliaser placed on the same memory: the
    // submit writing the aliasing image had no semaphore wait, so it raced the other queue's read
    // of the same bytes. The fix is to add alias edges BEFORE submits are cut, and this is the case
    // that keeps it fixed.
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId compute_scratch = graph.create_texture(storage_image("compute scratch", 512));
    const ResourceId graphics_scratch =
        graph.create_texture(storage_image("graphics scratch", 512));
    const ResourceId compute_out = graph.import_buffer(storage_buffer("compute result"),
                                                       cy::rhi::BufferHandle::from_slot(0, 1));
    const ResourceId graphics_out = graph.import_buffer(storage_buffer("graphics result"),
                                                        cy::rhi::BufferHandle::from_slot(1, 1));

    // Chain one, entirely on the compute queue, finishing before chain two starts.
    graph.add_pass("compute write", QueueKind::AsyncCompute)
        .write(compute_scratch, Access::ComputeStorageWrite);
    graph.add_pass("compute read", QueueKind::AsyncCompute)
        .read(compute_scratch, Access::TransferRead)
        .write(compute_out, Access::TransferWrite);
    // Chain two, entirely on graphics, sharing no resource with chain one.
    graph.add_pass("graphics write", QueueKind::Graphics)
        .write(graphics_scratch, Access::ComputeStorageWrite);
    graph.add_pass("graphics read", QueueKind::Graphics)
        .read(graphics_scratch, Access::TransferRead)
        .write(graphics_out, Access::TransferWrite);
    CY_REQUIRE(graph.status().has_value());

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(two_queue_options());
    CY_REQUIRE(plan.has_value());

    // The aliaser put the two scratch images on the same memory, which they can share because their
    // lifetimes do not overlap.
    CY_REQUIRE_EQ(plan->memory.placements.size(), 2U);
    CY_CHECK_EQ(plan->memory.placements[0].offset, plan->memory.placements[1].offset);
    CY_CHECK_EQ(plan->stats.alias_edges, 1U);

    // And therefore a semaphore, not a barrier: the graphics submit waits on the compute queue even
    // though the two chains share no resource at all.
    cy::u32 waits = 0;
    for (const cy::rendering::Submit& submit : plan->submits) {
        waits += static_cast<cy::u32>(submit.waits.size());
    }
    CY_CHECK_EQ(waits, 1U);
    CY_CHECK(cy::rendering::validate_plan(graph, *plan).has_value());
}

CY_TEST_CASE("refusing to alias across queues is a policy a caller can choose") {
    // Aliasing buys memory and spends parallelism: an alias edge serialises work that shares
    // nothing but bytes. `rhi-and-render-graph` does not decide that trade, so the graph does not
    // either — it is a compile option, and this case is what documents that turning it off works.
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId compute_scratch = graph.create_texture(storage_image("compute scratch", 512));
    const ResourceId graphics_scratch =
        graph.create_texture(storage_image("graphics scratch", 512));
    const ResourceId compute_out = graph.import_buffer(storage_buffer("compute result"),
                                                       cy::rhi::BufferHandle::from_slot(0, 1));
    const ResourceId graphics_out = graph.import_buffer(storage_buffer("graphics result"),
                                                        cy::rhi::BufferHandle::from_slot(1, 1));

    graph.add_pass("compute write", QueueKind::AsyncCompute)
        .write(compute_scratch, Access::ComputeStorageWrite);
    graph.add_pass("compute read", QueueKind::AsyncCompute)
        .read(compute_scratch, Access::TransferRead)
        .write(compute_out, Access::TransferWrite);
    graph.add_pass("graphics write", QueueKind::Graphics)
        .write(graphics_scratch, Access::ComputeStorageWrite);
    graph.add_pass("graphics read", QueueKind::Graphics)
        .read(graphics_scratch, Access::TransferRead)
        .write(graphics_out, Access::TransferWrite);

    cy::rendering::CompileOptions options = two_queue_options();
    options.alias_across_queues = false;
    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(options);
    CY_REQUIRE(plan.has_value());

    // Two slots rather than one, and no alias edge — so the two queues stay independent.
    CY_REQUIRE_EQ(plan->memory.placements.size(), 2U);
    CY_CHECK_NE(plan->memory.placements[0].offset, plan->memory.placements[1].offset);
    CY_CHECK_EQ(plan->stats.alias_edges, 0U);
    cy::u32 waits = 0;
    for (const cy::rendering::Submit& submit : plan->submits) {
        waits += static_cast<cy::u32>(submit.waits.size());
    }
    CY_CHECK_EQ(waits, 0U);
}

CY_TEST_CASE("two transients whose lifetimes overlap never share memory") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId first = graph.create_texture(storage_image("first", 256));
    const ResourceId second = graph.create_texture(storage_image("second", 256));
    const ResourceId out =
        graph.import_buffer(storage_buffer("result"), cy::rhi::BufferHandle::from_slot(0, 1));

    graph.add_pass("write both", QueueKind::Graphics)
        .write(first, Access::ComputeStorageWrite)
        .write(second, Access::ComputeStorageWrite);
    graph.add_pass("read both", QueueKind::Graphics)
        .read(first, Access::ComputeStorageRead)
        .read(second, Access::ComputeStorageRead)
        .write(out, Access::TransferWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_REQUIRE_EQ(plan->memory.placements.size(), 2U);
    CY_CHECK_NE(plan->memory.placements[0].offset, plan->memory.placements[1].offset);
    CY_CHECK_EQ(plan->memory.heap_bytes, plan->memory.naive_bytes);
    // The audit checks the same property independently, which is what makes it a gate rather than
    // an assertion in one test.
    CY_CHECK(cy::rendering::validate_plan(graph, *plan).has_value());
}
