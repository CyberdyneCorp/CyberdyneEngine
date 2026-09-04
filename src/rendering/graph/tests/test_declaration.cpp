// What a pass may declare, and what it may not. Task 2.2.1.
//
// The declaration IS the barrier. A mistyped one does not fail loudly at record time; it produces a
// barrier that looks right and synchronises the wrong thing, and the symptom appears weeks later on
// one vendor's driver. So the graph refuses the declarations that could only be mistakes, and these
// cases are what keeps that refusal in place.

#include <cy/test/test.h>

#include "fixtures.h"

using cy::rendering::CompiledGraph;
using cy::rendering::RenderGraph;
using cy::rendering::ResourceId;
using cy::rhi::Access;
using namespace cy::rendering::test;

CY_TEST_CASE("a pass declares reads and writes, and the graph compiles them") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image = graph.create_texture(storage_image("scratch"));
    const ResourceId target =
        graph.import_texture(colour_target("swapchain"), cy::rhi::TextureHandle::from_slot(0, 1),
                             cy::rhi::ImageLayout::Undefined);

    graph.add_pass("fill", cy::rhi::QueueKind::Graphics).write(image, Access::ComputeStorageWrite);
    graph.add_pass("shade", cy::rhi::QueueKind::Graphics)
        .read(image, Access::FragmentSampledRead)
        .write(target, Access::ColorAttachmentWrite);

    CY_REQUIRE(graph.status().has_value());
    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_CHECK_EQ(plan->submits.size(), 1U);
    CY_CHECK_EQ(plan->stats.passes_declared, 2U);
    CY_CHECK_EQ(plan->stats.passes_culled, 0U);
}

CY_TEST_CASE("read() with a writing intent is refused, and the graph then refuses to compile") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image = graph.create_texture(storage_image("scratch"));
    graph.add_pass("confused", cy::rhi::QueueKind::Graphics)
        .read(image, Access::ComputeStorageWrite);

    CY_CHECK_FALSE(graph.status().has_value());
    // A graph that failed to declare something compiles to nothing. Compiling anyway would produce
    // a plan with a hole in it, and the hole would surface as a missing barrier.
    CY_CHECK_FALSE(graph.compile(single_queue_options()).has_value());
}

CY_TEST_CASE("write() with a reading intent is refused") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image = graph.create_texture(storage_image("scratch"));
    graph.add_pass("confused", cy::rhi::QueueKind::Graphics)
        .write(image, Access::FragmentSampledRead);
    CY_CHECK_FALSE(graph.status().has_value());
}

CY_TEST_CASE("an intent with no meaning for the resource kind is refused") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId buffer = graph.create_buffer(storage_buffer("results"));
    graph.add_pass("sample a buffer", cy::rhi::QueueKind::Graphics)
        .read(buffer, Access::FragmentSampledRead);
    CY_CHECK_FALSE(graph.status().has_value());
    CY_CHECK_EQ(graph.status().error().code, cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("a subresource range outside the texture is refused, naming the range") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image = graph.create_texture(storage_image("two layers", 16, 2));
    graph.add_pass("layer 5", cy::rhi::QueueKind::Graphics)
        .write(image, Access::ComputeStorageWrite, cy::rhi::SubresourceRange::layer(5));
    CY_CHECK_FALSE(graph.status().has_value());
    CY_CHECK_EQ(graph.status().error().code, cy::ErrorCode::OutOfRange);
}

CY_TEST_CASE("a zero count means all remaining, resolved at declaration") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image = graph.create_texture(storage_image("mips", 64, 3, 4));
    graph.add_pass("whole", cy::rhi::QueueKind::Graphics).write(image, Access::ComputeStorageWrite);
    CY_REQUIRE(graph.status().has_value());

    const cy::Span<const cy::rendering::Use> uses = graph.pass_uses(0);
    CY_REQUIRE_EQ(uses.size(), 1U);
    // Nothing downstream ever sees an "all remaining" sentinel: two ranges cannot be compared for
    // adjacency while either still means "whatever is left", and coalescing depends on that.
    CY_CHECK_EQ(uses[0].range.mip_count, 4);
    CY_CHECK_EQ(uses[0].range.layer_count, 3);
}

CY_TEST_CASE("the usage a resource is created with is unioned from the accesses declared") {
    // A pass author never keeps a usage flag in step with passes written six months later.
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image = graph.create_texture(storage_image("multi-use"));
    graph.add_pass("compute", cy::rhi::QueueKind::Graphics)
        .write(image, Access::ComputeStorageWrite);
    graph.add_pass("sample", cy::rhi::QueueKind::Graphics).read(image, Access::FragmentSampledRead);
    graph.add_pass("copy out", cy::rhi::QueueKind::Graphics)
        .read(image, Access::TransferRead)
        .side_effect();
    CY_REQUIRE(graph.status().has_value());

    const cy::rendering::ResourceInfo& info = graph.resource(image);
    CY_CHECK(cy::rhi::has_usage(info.texture_usage, cy::rhi::TextureUsage::Storage));
    CY_CHECK(cy::rhi::has_usage(info.texture_usage, cy::rhi::TextureUsage::Sampled));
    CY_CHECK(cy::rhi::has_usage(info.texture_usage, cy::rhi::TextureUsage::TransferSource));
    CY_CHECK_FALSE(cy::rhi::has_usage(info.texture_usage, cy::rhi::TextureUsage::ColorAttachment));
}

CY_TEST_CASE("declaring a use against a pass that is no longer being built is refused") {
    // Uses live in one flat array indexed by (first, count), so a pass's uses must be contiguous.
    // Interleaving two passes' declarations would silently attribute a use to the wrong pass, which
    // is a wrong barrier rather than a wrong error message.
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image = graph.create_texture(storage_image("scratch"));
    cy::rendering::PassBuilder first = graph.add_pass("first", cy::rhi::QueueKind::Graphics);
    graph.add_pass("second", cy::rhi::QueueKind::Graphics)
        .write(image, Access::ComputeStorageWrite);
    first.read(image, Access::FragmentSampledRead);
    CY_CHECK_FALSE(graph.status().has_value());
}

CY_TEST_CASE("reset forgets the declarations and the failure, and keeps the allocations") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image = graph.create_texture(storage_image("scratch"));
    graph.add_pass("bad", cy::rhi::QueueKind::Graphics).read(image, Access::ComputeStorageWrite);
    CY_REQUIRE_FALSE(graph.status().has_value());

    graph.reset();
    CY_CHECK(graph.status().has_value());
    CY_CHECK_EQ(graph.pass_count(), 0U);
    CY_CHECK_EQ(graph.resource_count(), 0U);
}
