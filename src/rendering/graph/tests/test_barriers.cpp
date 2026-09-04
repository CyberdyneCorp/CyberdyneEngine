// The hazard rules and the coalescer. Task 2.2.2.
//
// These are the cases that would catch an edit to compile.cpp turning a correct barrier into a
// plausible one. Each asserts on the derived plan rather than on whether a frame rendered, because
// M3's spike established that a validation layer does not catch every wrong barrier — it does not
// catch a missing ownership transfer and it does not catch a missing alias barrier, both of which
// produced zero errors and correct pixels on the device.

#include <cy/test/test.h>

#include "fixtures.h"

using cy::rendering::CompiledGraph;
using cy::rendering::RenderGraph;
using cy::rendering::ResourceId;
using cy::rhi::Access;
using cy::rhi::AccessFlags;
using cy::rhi::ImageLayout;
using cy::rhi::Stage;
using namespace cy::rendering::test;

CY_TEST_CASE("read-after-write carries the writer's stage and access into the reader") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId target =
        graph.import_texture(colour_target("swapchain"), cy::rhi::TextureHandle::from_slot(0, 1),
                             cy::rhi::ImageLayout::Undefined);
    const ResourceId image = graph.create_texture(storage_image("scratch"));

    graph.add_pass("compute", cy::rhi::QueueKind::Graphics)
        .write(image, Access::ComputeStorageWrite);
    graph.add_pass("sample", cy::rhi::QueueKind::Graphics)
        .read(image, Access::FragmentSampledRead)
        .write(target, Access::ColorAttachmentWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_REQUIRE_EQ(plan->submits.size(), 1U);

    // The sampling pass's barrier for `image`: GENERAL -> SHADER_READ_ONLY, compute -> fragment.
    const cy::rhi::BarrierBatch& sample_pre = plan->submits[0].passes[1].pre;
    const cy::rhi::ImageBarrier* found = nullptr;
    for (const cy::rhi::ImageBarrier& barrier : sample_pre.images) {
        if (barrier.resource == image) {
            found = &barrier;
        }
    }
    CY_REQUIRE(found != nullptr);
    CY_CHECK_EQ(found->old_layout, ImageLayout::General);
    CY_CHECK_EQ(found->new_layout, ImageLayout::ShaderReadOnly);
    CY_CHECK_EQ(found->src_stage, Stage::ComputeShader);
    CY_CHECK_EQ(found->src_access, AccessFlags::ShaderStorageWrite);
    CY_CHECK_EQ(found->dst_stage, Stage::FragmentShader);
    CY_CHECK_EQ(found->dst_access, AccessFlags::ShaderSampledRead);
}

CY_TEST_CASE("write-after-read takes the reader's stage and none of its access bits") {
    // THE RULE THAT IS EASIEST TO GET WRONG. A write-after-read needs an execution dependency, not
    // a memory one: nothing was written, so there is nothing to flush. Naming the read's access
    // bits in srcAccessMask asks the implementation to make a write visible that never happened.
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image = graph.import_texture(
        storage_image("shared"), cy::rhi::TextureHandle::from_slot(1, 1), ImageLayout::General);

    graph.add_pass("read it", cy::rhi::QueueKind::Graphics).read(image, Access::ComputeStorageRead);
    graph.add_pass("overwrite it", cy::rhi::QueueKind::Graphics)
        .write(image, Access::ComputeStorageWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    const cy::rhi::BarrierBatch& write_pre = plan->submits[0].passes[1].pre;
    CY_REQUIRE_EQ(write_pre.images.size(), 1U);
    const cy::rhi::ImageBarrier& barrier = write_pre.images[0];
    CY_CHECK_EQ(barrier.src_stage, Stage::ComputeShader);
    CY_CHECK_EQ(barrier.src_access, AccessFlags::None);
    CY_CHECK_EQ(barrier.dst_stage, Stage::ComputeShader);
    CY_CHECK_EQ(barrier.dst_access, AccessFlags::ShaderStorageWrite);
    // The layout does not change: both intents are storage accesses, so both want GENERAL.
    CY_CHECK_EQ(barrier.old_layout, ImageLayout::General);
    CY_CHECK_EQ(barrier.new_layout, ImageLayout::General);
}

CY_TEST_CASE("write-after-write names the previous write on both halves") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image = graph.import_texture(
        storage_image("shared"), cy::rhi::TextureHandle::from_slot(1, 1), ImageLayout::General);
    graph.add_pass("first", cy::rhi::QueueKind::Graphics).write(image, Access::ComputeStorageWrite);
    graph.add_pass("second", cy::rhi::QueueKind::Graphics)
        .write(image, Access::ComputeStorageWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    const cy::rhi::BarrierBatch& second = plan->submits[0].passes[1].pre;
    CY_REQUIRE_EQ(second.images.size(), 1U);
    CY_CHECK_EQ(second.images[0].src_access, AccessFlags::ShaderStorageWrite);
    CY_CHECK_EQ(second.images[0].src_stage, Stage::ComputeShader);
}

CY_TEST_CASE("read-after-read emits nothing when the layout already agrees") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId target =
        graph.import_texture(colour_target("swapchain"), cy::rhi::TextureHandle::from_slot(0, 1),
                             ImageLayout::Undefined);
    const ResourceId image =
        graph.import_texture(storage_image("shared"), cy::rhi::TextureHandle::from_slot(1, 1),
                             ImageLayout::ShaderReadOnly);

    graph.add_pass("sample once", cy::rhi::QueueKind::Graphics)
        .read(image, Access::FragmentSampledRead)
        .write(target, Access::ColorAttachmentWrite);
    graph.add_pass("sample again", cy::rhi::QueueKind::Graphics)
        .read(image, Access::FragmentSampledRead)
        .write(target, Access::ColorAttachmentWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    // The second sampling pass needs nothing for `image`: no hazard, no layout change. Two sampling
    // passes therefore overlap, which is the whole point of not emitting a barrier for a read pair.
    for (const cy::rhi::ImageBarrier& barrier : plan->submits[0].passes[1].pre.images) {
        CY_CHECK_NE(barrier.resource, image);
    }
}

CY_TEST_CASE("a transient's first use transitions from UNDEFINED, which is what discards it") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId target =
        graph.import_texture(colour_target("swapchain"), cy::rhi::TextureHandle::from_slot(0, 1),
                             ImageLayout::Undefined);
    const ResourceId image = graph.create_texture(storage_image("scratch"));
    graph.add_pass("fill", cy::rhi::QueueKind::Graphics).write(image, Access::ComputeStorageWrite);
    graph.add_pass("sample", cy::rhi::QueueKind::Graphics)
        .read(image, Access::FragmentSampledRead)
        .write(target, Access::ColorAttachmentWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    const cy::rhi::ImageBarrier* first = find_image_barrier(*plan, image);
    CY_REQUIRE(first != nullptr);
    CY_CHECK_EQ(first->old_layout, ImageLayout::Undefined);
    CY_CHECK_EQ(first->new_layout, ImageLayout::General);
    // Discarding the contents is exactly what makes memory reuse legal, so this is not cosmetic.
}

CY_TEST_CASE("a whole-image transition is one barrier, a mip range is one, a single cell is one") {
    // M3's spike measured this as case "coalesce", on a 4-mip 4-layer image and with no device.
    // Coalescing is not an optimisation to defer: the two-layer ownership release in the hard case
    // must be ONE barrier, and this merge is what makes it one.
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image =
        graph.import_texture(storage_image("4x4", 64, 4, 4),
                             cy::rhi::TextureHandle::from_slot(1, 1), ImageLayout::Undefined);

    graph.add_pass("whole image", cy::rhi::QueueKind::Graphics)
        .write(image, Access::ComputeStorageWrite);
    // Both readers are marked side-effecting: they write nothing, and a pass whose output nothing
    // consumes is culled — which is correct, and is not what this case is about.
    graph.add_pass("mips 1 and 2", cy::rhi::QueueKind::Graphics)
        .read(image, Access::FragmentSampledRead, cy::rhi::SubresourceRange{1, 2, 0, 0})
        .side_effect();
    graph.add_pass("one cell", cy::rhi::QueueKind::Graphics)
        .read(image, Access::ComputeSampledRead, cy::rhi::SubresourceRange{0, 1, 2, 1})
        .side_effect();

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_REQUIRE_EQ(plan->submits.size(), 1U);
    CY_REQUIRE_EQ(plan->submits[0].passes.size(), 3U);

    // Sixteen cells, one barrier.
    const cy::rhi::BarrierBatch& whole = plan->submits[0].passes[0].pre;
    CY_REQUIRE_EQ(whole.images.size(), 1U);
    CY_CHECK_EQ(whole.images[0].range.base_mip, 0);
    CY_CHECK_EQ(whole.images[0].range.mip_count, 4);
    CY_CHECK_EQ(whole.images[0].range.layer_count, 4);

    // Eight cells over two mips and four layers, one barrier with levelCount 2 and layerCount 4.
    const cy::rhi::BarrierBatch& mips = plan->submits[0].passes[1].pre;
    CY_REQUIRE_EQ(mips.images.size(), 1U);
    CY_CHECK_EQ(mips.images[0].range.base_mip, 1);
    CY_CHECK_EQ(mips.images[0].range.mip_count, 2);
    CY_CHECK_EQ(mips.images[0].range.base_layer, 0);
    CY_CHECK_EQ(mips.images[0].range.layer_count, 4);

    // Exactly one (mip, layer), and the barrier says exactly that.
    const cy::rhi::BarrierBatch& cell = plan->submits[0].passes[2].pre;
    CY_REQUIRE_EQ(cell.images.size(), 1U);
    CY_CHECK_EQ(cell.images[0].range.base_mip, 0);
    CY_CHECK_EQ(cell.images[0].range.mip_count, 1);
    CY_CHECK_EQ(cell.images[0].range.base_layer, 2);
    CY_CHECK_EQ(cell.images[0].range.layer_count, 1);
}

CY_TEST_CASE("two adjacent layer ranges reaching the same state coalesce into one barrier") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId target =
        graph.import_texture(colour_target("swapchain"), cy::rhi::TextureHandle::from_slot(0, 1),
                             ImageLayout::Undefined);
    const ResourceId layered = graph.create_texture(storage_image("two layers", 16, 2));

    graph.add_pass("layer 0", cy::rhi::QueueKind::Graphics)
        .write(layered, Access::ComputeStorageWrite, cy::rhi::SubresourceRange::layer(0));
    graph.add_pass("layer 1", cy::rhi::QueueKind::Graphics)
        .write(layered, Access::ComputeStorageWrite, cy::rhi::SubresourceRange::layer(1));
    graph.add_pass("sample both", cy::rhi::QueueKind::Graphics)
        .read(layered, Access::FragmentSampledRead)
        .write(target, Access::ColorAttachmentWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    const cy::rhi::BarrierBatch& sample = plan->submits[0].passes[2].pre;
    cy::u32 for_layered = 0;
    for (const cy::rhi::ImageBarrier& barrier : sample.images) {
        if (barrier.resource != layered) {
            continue;
        }
        ++for_layered;
        CY_CHECK_EQ(barrier.range.base_layer, 0);
        CY_CHECK_EQ(barrier.range.layer_count, 2);
    }
    CY_CHECK_EQ(for_layered, 1U);
}

CY_TEST_CASE("the host boundary is a dependency like any other") {
    // A recordless side-effecting pass declaring HostRead makes the graph emit the transfer-to-host
    // barrier. Relying on coherent memory and a fence instead is how a read-back becomes
    // intermittently wrong on one driver. (M3 spike, gotcha 6f.)
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId staging = graph.import_buffer(storage_buffer("readback", 4096),
                                                   cy::rhi::BufferHandle::from_slot(0, 1));
    graph.add_pass("copy out", cy::rhi::QueueKind::Graphics).write(staging, Access::TransferWrite);
    graph.add_pass("host reads it", cy::rhi::QueueKind::Graphics)
        .read(staging, Access::HostRead)
        .side_effect();

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    const cy::rhi::BarrierBatch& host = plan->submits[0].passes[1].pre;
    CY_REQUIRE_EQ(host.memory.size(), 1U);
    CY_CHECK_EQ(host.memory[0].src_stage, Stage::Copy);
    CY_CHECK_EQ(host.memory[0].src_access, AccessFlags::TransferWrite);
    CY_CHECK_EQ(host.memory[0].dst_stage, Stage::Host);
    CY_CHECK_EQ(host.memory[0].dst_access, AccessFlags::HostRead);
}

CY_TEST_CASE("a depth attachment transitions once and keeps reversed-Z's comparison out of it") {
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    cy::rendering::TextureRequest depth_request = colour_target("depth", 128);
    depth_request.format = cy::rhi::Format::D32Sfloat;
    const ResourceId depth = graph.create_texture(depth_request);
    const ResourceId target =
        graph.import_texture(colour_target("swapchain", 128),
                             cy::rhi::TextureHandle::from_slot(0, 1), ImageLayout::Undefined);

    graph.add_pass("depth prepass", cy::rhi::QueueKind::Graphics)
        .write(depth, Access::DepthStencilAttachmentWrite);
    graph.add_pass("shade", cy::rhi::QueueKind::Graphics)
        .read(depth, Access::DepthStencilAttachmentRead)
        .write(target, Access::ColorAttachmentWrite);

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());
    const cy::rhi::ImageBarrier* first = find_image_barrier(*plan, depth);
    CY_REQUIRE(first != nullptr);
    CY_CHECK_EQ(first->new_layout, ImageLayout::DepthStencilAttachment);
    // The aspect comes from the format, not from a flag a pass had to remember to set.
    CY_CHECK_EQ(first->aspect, cy::rhi::ImageAspect::Depth);
    // Both stages, because depth is tested before the fragment shader and written after it.
    CY_CHECK_EQ(first->dst_stage, Stage::EarlyFragmentTests | Stage::LateFragmentTests);
}
