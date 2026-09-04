// Culling: a pass whose output nothing consumes is removed. Task 2.2.2.
//
// `rhi-and-render-graph`: "a debug visualisation pass writes a texture nothing samples and is not
// marked side-effecting" — the graph SHALL remove it, "and the renderer SHALL not need to branch on
// the debug flag".
//
// ONE CONSEQUENCE WORTH KNOWING, and it is why two of these cases exist: any write to an imported
// resource is a culling root, because the graph cannot see who reads it afterwards. A culling test
// therefore only means something when the shared resource is graph-owned.

#include <cy/test/test.h>

#include "fixtures.h"

using cy::rendering::CompiledGraph;
using cy::rendering::RenderGraph;
using cy::rendering::ResourceId;
using cy::rhi::Access;
using namespace cy::rendering::test;

CY_TEST_CASE("a pass whose output nothing consumes is culled, with no branch in the renderer") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId target =
        graph.import_texture(colour_target("swapchain"), cy::rhi::TextureHandle::from_slot(0, 1),
                             cy::rhi::ImageLayout::Undefined);
    const ResourceId overlay = graph.create_texture(storage_image("debug overlay"));

    graph.add_pass("shade", cy::rhi::QueueKind::Graphics)
        .write(target, Access::ColorAttachmentWrite);
    const cy::rendering::PassId debug =
        graph.add_pass("debug overlay", cy::rhi::QueueKind::Graphics)
            .write(overlay, Access::ComputeStorageWrite)
            .id();

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_CHECK_EQ(plan->stats.passes_culled, 1U);
    CY_REQUIRE_EQ(plan->culled.size(), 1U);
    CY_CHECK_EQ(plan->culled[0], debug);
    CY_CHECK_FALSE(plan_contains_pass(*plan, debug));
    // The culled pass's transient is never placed either: a resource with no surviving use has no
    // lifetime and costs no memory.
    CY_CHECK_EQ(plan->memory.placements.size(), 0U);
}

CY_TEST_CASE("a side-effecting pass survives with no consumed output") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId scratch = graph.create_texture(storage_image("scratch"));
    const cy::rendering::PassId pass = graph.add_pass("readback", cy::rhi::QueueKind::Graphics)
                                           .write(scratch, Access::ComputeStorageWrite)
                                           .side_effect()
                                           .id();

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_CHECK_EQ(plan->stats.passes_culled, 0U);
    CY_CHECK(plan_contains_pass(*plan, pass));
}

CY_TEST_CASE(
    "a write to an imported resource is a root, so a culling test needs a graph-owned one") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId imported =
        graph.import_texture(storage_image("shared"), cy::rhi::TextureHandle::from_slot(1, 1),
                             cy::rhi::ImageLayout::Undefined);
    const cy::rendering::PassId pass =
        graph.add_pass("write the shared image", cy::rhi::QueueKind::Graphics)
            .write(imported, Access::ComputeStorageWrite)
            .id();

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    // Not culled, and not because it was marked: the graph cannot see who reads an imported
    // resource after the frame, so it must assume somebody does.
    CY_CHECK_EQ(plan->stats.passes_culled, 0U);
    CY_CHECK(plan_contains_pass(*plan, pass));
}

CY_TEST_CASE("culling reaches transitively through a chain of producers") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId target =
        graph.import_texture(colour_target("swapchain"), cy::rhi::TextureHandle::from_slot(0, 1),
                             cy::rhi::ImageLayout::Undefined);
    const ResourceId first = graph.create_texture(storage_image("first"));
    const ResourceId second = graph.create_texture(storage_image("second"));
    const ResourceId orphan = graph.create_texture(storage_image("orphan"));

    graph.add_pass("produce first", cy::rhi::QueueKind::Graphics)
        .write(first, Access::ComputeStorageWrite);
    graph.add_pass("first to second", cy::rhi::QueueKind::Graphics)
        .read(first, Access::ComputeStorageRead)
        .write(second, Access::ComputeStorageWrite);
    graph.add_pass("shade", cy::rhi::QueueKind::Graphics)
        .read(second, Access::FragmentSampledRead)
        .write(target, Access::ColorAttachmentWrite);
    // Two passes that produce something nothing on the path to the target consumes.
    graph.add_pass("orphan producer", cy::rhi::QueueKind::Graphics)
        .write(orphan, Access::ComputeStorageWrite);
    graph.add_pass("orphan consumer", cy::rhi::QueueKind::Graphics)
        .read(orphan, Access::ComputeStorageRead);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_CHECK_EQ(plan->stats.passes_culled, 2U);
    CY_CHECK_EQ(plan->stats.passes_declared, 5U);
}

CY_TEST_CASE("subresource precision is load-bearing for culling, not only for barrier count") {
    // M3's spike measured this as case "A2-precise": with the consumer reading layer 0 only, the
    // layer-1 writer is CULLED, and the frame still renders correctly. A graph that tracked whole
    // images would keep it — which is a pass's worth of work per frame, forever, for nothing.
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId target =
        graph.import_texture(colour_target("swapchain"), cy::rhi::TextureHandle::from_slot(0, 1),
                             cy::rhi::ImageLayout::Undefined);
    const ResourceId layered = graph.create_texture(storage_image("two layers", 16, 2));

    const cy::rendering::PassId writer0 =
        graph.add_pass("write layer 0", cy::rhi::QueueKind::Graphics)
            .write(layered, Access::ComputeStorageWrite, cy::rhi::SubresourceRange::layer(0))
            .id();
    const cy::rendering::PassId writer1 =
        graph.add_pass("write layer 1", cy::rhi::QueueKind::Graphics)
            .write(layered, Access::ComputeStorageWrite, cy::rhi::SubresourceRange::layer(1))
            .id();
    graph.add_pass("sample layer 0", cy::rhi::QueueKind::Graphics)
        .read(layered, Access::FragmentSampledRead, cy::rhi::SubresourceRange::layer(0))
        .write(target, Access::ColorAttachmentWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_CHECK(plan_contains_pass(*plan, writer0));
    CY_CHECK_FALSE(plan_contains_pass(*plan, writer1));
    CY_CHECK_EQ(plan->stats.passes_culled, 1U);
}

CY_TEST_CASE("the coarse version of the same graph keeps the second writer") {
    // The control for the case above: declare whole-image ranges and the layer-1 writer survives,
    // because now the consumer really does read what it wrote.
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId target =
        graph.import_texture(colour_target("swapchain"), cy::rhi::TextureHandle::from_slot(0, 1),
                             cy::rhi::ImageLayout::Undefined);
    const ResourceId layered = graph.create_texture(storage_image("two layers", 16, 2));

    graph.add_pass("write both layers", cy::rhi::QueueKind::Graphics)
        .write(layered, Access::ComputeStorageWrite);
    const cy::rendering::PassId second =
        graph.add_pass("write both again", cy::rhi::QueueKind::Graphics)
            .write(layered, Access::ComputeStorageWrite)
            .id();
    graph.add_pass("sample both", cy::rhi::QueueKind::Graphics)
        .read(layered, Access::FragmentSampledRead)
        .write(target, Access::ColorAttachmentWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_CHECK(plan_contains_pass(*plan, second));
    CY_CHECK_EQ(plan->stats.passes_culled, 0U);
}
