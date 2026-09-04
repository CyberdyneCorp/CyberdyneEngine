// The hard case, and the single-queue fold that falls out of the same declarations. Task 2.2.3.
//
// THIS IS THE CASE THE MILESTONE'S RISK WAS NAMED ON. tasks.md §0.1: "a compute pass writing a
// resource a graphics pass samples, while a second compute pass writes a different subresource of
// the same image, on a separate queue". M3's spike ran it on an RTX 5060 with synchronisation
// validation enabled — 256/256 pixels exactly correct, 0 errors, 0 warnings — and the plan it
// derived is the one asserted here, on a machine with no GPU.
//
// The second half of the file is the reason the first half is not a special case: the same
// declarations with async compute off collapse to one submit, no semaphores and no ownership
// transfers. No branch anywhere in the graph produces that; it is what the derivation does when
// every pass resolves to the same queue.

#include <cy/test/test.h>

#include "fixtures.h"

using cy::rendering::CompiledGraph;
using cy::rendering::RenderGraph;
using cy::rendering::ResourceId;
using cy::rhi::Access;
using cy::rhi::ImageLayout;
using cy::rhi::QueueKind;
using namespace cy::rendering::test;

namespace {

/// The hard case, declared once and compiled twice — with two queues and with one.
struct HardCase {
    RenderGraph graph;
    ResourceId layered = cy::rendering::kInvalidResource;
    ResourceId target = cy::rendering::kInvalidResource;
    ResourceId staging = cy::rendering::kInvalidResource;

    HardCase() noexcept : graph(cy::system_allocator(cy::MemoryDomain::Renderer)) {
        layered = graph.create_texture(storage_image("layers", 16, 2));
        target =
            graph.import_texture(colour_target("swapchain"),
                                 cy::rhi::TextureHandle::from_slot(0, 1), ImageLayout::Undefined);
        staging = graph.import_buffer(storage_buffer("readback", 4096),
                                      cy::rhi::BufferHandle::from_slot(0, 1));

        // Two compute passes writing DIFFERENT array layers of one image, on the compute queue.
        graph.add_pass("write layer 0", QueueKind::AsyncCompute)
            .write(layered, Access::ComputeStorageWrite, cy::rhi::SubresourceRange::layer(0));
        graph.add_pass("write layer 1", QueueKind::AsyncCompute)
            .write(layered, Access::ComputeStorageWrite, cy::rhi::SubresourceRange::layer(1));
        // A graphics pass sampling both layers.
        graph.add_pass("shade", QueueKind::Graphics)
            .read(layered, Access::FragmentSampledRead)
            .write(target, Access::ColorAttachmentWrite);
        // A transfer read back into a host-visible buffer, and the host boundary declared as a
        // dependency rather than assumed.
        graph.add_pass("read back", QueueKind::Graphics)
            .read(target, Access::TransferRead)
            .write(staging, Access::TransferWrite);
        graph.add_pass("host", QueueKind::Graphics).read(staging, Access::HostRead).side_effect();
    }
};

}  // namespace

CY_TEST_CASE("cross-queue work becomes a semaphore and an ownership transfer, never a barrier") {
    HardCase scene;
    CY_REQUIRE(scene.graph.status().has_value());

    cy::Expected<CompiledGraph, cy::Error> plan = scene.graph.compile(two_queue_options());
    CY_REQUIRE(plan.has_value());

    // Two submits: the compute queue's, then the graphics queue's.
    CY_REQUIRE_EQ(plan->submits.size(), 2U);
    CY_CHECK_EQ(plan->submits[0].queue, QueueKind::AsyncCompute);
    CY_CHECK_EQ(plan->submits[0].passes.size(), 2U);
    CY_CHECK_EQ(plan->submits[1].queue, QueueKind::Graphics);
    CY_CHECK_EQ(plan->submits[1].passes.size(), 3U);

    // ONE semaphore wait, on the producing queue's timeline. Not a pipeline barrier: a barrier
    // synchronises nothing between two command streams, and dropping this wait is what produced
    // SYNC-HAZARD-WRITE-RACING-WRITE on the device.
    CY_CHECK_EQ(plan->submits[0].waits.size(), 0U);
    CY_REQUIRE_EQ(plan->submits[1].waits.size(), 1U);
    CY_CHECK_EQ(plan->submits[1].waits[0].queue, QueueKind::AsyncCompute);
    CY_CHECK_EQ(plan->submits[1].waits[0].value, plan->submits[0].signal_value);

    // ONE coalesced ownership release over layers [0, 2), at the end of the producing submit.
    CY_REQUIRE_EQ(plan->submits[0].release.images.size(), 1U);
    const cy::rhi::ImageBarrier& release = plan->submits[0].release.images[0];
    CY_CHECK_EQ(release.resource, scene.layered);
    CY_CHECK_EQ(release.old_layout, ImageLayout::General);
    CY_CHECK_EQ(release.new_layout, ImageLayout::ShaderReadOnly);
    CY_CHECK_EQ(release.src_queue_family, 2U);
    CY_CHECK_EQ(release.dst_queue_family, 0U);
    CY_CHECK_EQ(release.range.base_layer, 0);
    CY_CHECK_EQ(release.range.layer_count, 2);

    // The matching acquire, in the consuming pass's pre-batch, with IDENTICAL layouts, families and
    // subresource range. Both halves come from one hazard, which is why they cannot drift.
    const cy::rhi::BarrierBatch& shade = plan->submits[1].passes[0].pre;
    const cy::rhi::ImageBarrier* acquire = nullptr;
    for (const cy::rhi::ImageBarrier& barrier : shade.images) {
        if (barrier.resource == scene.layered) {
            acquire = &barrier;
        }
    }
    CY_REQUIRE(acquire != nullptr);
    CY_CHECK_EQ(acquire->old_layout, release.old_layout);
    CY_CHECK_EQ(acquire->new_layout, release.new_layout);
    CY_CHECK_EQ(acquire->src_queue_family, release.src_queue_family);
    CY_CHECK_EQ(acquire->dst_queue_family, release.dst_queue_family);
    CY_CHECK(acquire->range == release.range);

    // The plan's own audit checks the same invariants, plus the ones a validation layer does not.
    cy::Expected<cy::rendering::PlanAudit, cy::Error> audit =
        cy::rendering::validate_plan(scene.graph, *plan);
    CY_REQUIRE(audit.has_value());
    CY_CHECK_EQ(audit->ownership_releases, 1U);
    CY_CHECK_EQ(audit->ownership_acquires, 1U);
    CY_CHECK_EQ(audit->semaphore_waits, 1U);
}

CY_TEST_CASE("the ignored halves of an ownership transfer are ALL_COMMANDS, not NONE") {
    // The specification ignores dstStage/dstAccess on the release and srcStage/srcAccess on the
    // acquire, and NONE is the canonical spelling — but VVL 1.3.275 then models the release's
    // layout transition as an unbarriered write and reports SYNC-HAZARD-WRITE-AFTER-WRITE against
    // the acquire's transition even with a correct semaphore. M3's spike isolated it three ways;
    // ALL_COMMANDS in the ignored halves silenced it at no cost and masked no real hazard.
    // Re-test when the validation layer is newer than 1.3.275 — and change this case with it.
    HardCase scene;
    cy::Expected<CompiledGraph, cy::Error> plan = scene.graph.compile(two_queue_options());
    CY_REQUIRE(plan.has_value());

    CY_REQUIRE_EQ(plan->submits[0].release.images.size(), 1U);
    CY_CHECK_EQ(plan->submits[0].release.images[0].dst_stage, cy::rhi::Stage::AllCommands);
    CY_CHECK_EQ(plan->submits[0].release.images[0].dst_access, cy::rhi::AccessFlags::None);

    const cy::rhi::BarrierBatch& shade = plan->submits[1].passes[0].pre;
    for (const cy::rhi::ImageBarrier& barrier : shade.images) {
        if (barrier.resource == scene.layered) {
            CY_CHECK_EQ(barrier.src_stage, cy::rhi::Stage::AllCommands);
            CY_CHECK_EQ(barrier.src_access, cy::rhi::AccessFlags::None);
        }
    }
}

CY_TEST_CASE("with async compute off the same declarations collapse to one submit") {
    // The null backend's and continuous integration's normal path. No branch produces this: it is
    // what the derivation does when every pass resolves to the same queue. Nothing about the
    // declarations changed between this case and the one above.
    HardCase scene;
    cy::Expected<CompiledGraph, cy::Error> plan = scene.graph.compile(single_queue_options());
    CY_REQUIRE(plan.has_value());

    CY_CHECK_EQ(plan->submits.size(), 1U);
    CY_CHECK_EQ(plan->submits[0].passes.size(), 5U);
    CY_CHECK_EQ(plan->submits[0].waits.size(), 0U);
    CY_CHECK(plan->submits[0].release.empty());

    const BarrierCounts counts = count_barriers(*plan);
    CY_CHECK_EQ(counts.ownership_releases, 0U);
    CY_CHECK_EQ(counts.ownership_acquires, 0U);

    // The two layer transitions coalesce into ONE barrier at the sampling pass, exactly as they did
    // on the device: the same coalescer, with nothing about queues involved.
    const cy::rhi::BarrierBatch& shade = plan->submits[0].passes[2].pre;
    cy::u32 for_layered = 0;
    for (const cy::rhi::ImageBarrier& barrier : shade.images) {
        if (barrier.resource != scene.layered) {
            continue;
        }
        ++for_layered;
        CY_CHECK_EQ(barrier.range.layer_count, 2);
        CY_CHECK_EQ(barrier.src_queue_family, cy::rhi::kQueueFamilyIgnored);
    }
    CY_CHECK_EQ(for_layered, 1U);
}

CY_TEST_CASE("a queue the device does not have folds onto graphics with no special case") {
    // `rhi-and-render-graph`: "the renderer SHALL branch on capabilities, never on backend
    // identity". The graph asks whether the queue exists and folds if it does not — which is the
    // same code path as async compute being switched off.
    HardCase scene;
    cy::rendering::CompileOptions options = two_queue_options();
    options.queue_available[static_cast<cy::u32>(QueueKind::AsyncCompute)] = false;

    cy::Expected<CompiledGraph, cy::Error> plan = scene.graph.compile(options);
    CY_REQUIRE(plan.has_value());
    CY_CHECK_EQ(plan->submits.size(), 1U);
    CY_CHECK_EQ(plan->submits[0].queue, QueueKind::Graphics);
    CY_CHECK_EQ(count_barriers(*plan).ownership_releases, 0U);
}

CY_TEST_CASE("a chain alternating queues every step transfers ownership in both directions") {
    // M3's spike measured this as case "E-pingpong": 12 submits, an ownership transfer in both
    // directions on every hop, aliasing underneath, 0 errors and correct data. The interesting
    // property to keep is that the plan is still one submit per hop rather than one big one.
    RenderGraph graph(cy::system_allocator(cy::MemoryDomain::Renderer));
    const ResourceId image =
        graph.import_texture(storage_image("ping-pong", 32),
                             cy::rhi::TextureHandle::from_slot(1, 1), ImageLayout::Undefined);

    constexpr cy::u32 kSteps = 8;
    for (cy::u32 step = 0; step < kSteps; ++step) {
        const QueueKind queue = (step % 2) == 0 ? QueueKind::AsyncCompute : QueueKind::Graphics;
        graph.add_pass("step", queue).use(image, Access::ComputeStorageReadWrite);
    }
    CY_REQUIRE(graph.status().has_value());

    cy::Expected<CompiledGraph, cy::Error> plan = graph.compile(two_queue_options());
    CY_REQUIRE(plan.has_value());
    CY_CHECK_EQ(plan->submits.size(), kSteps);

    const BarrierCounts counts = count_barriers(*plan);
    // Every hop but the first crosses a family, in alternating directions.
    CY_CHECK_EQ(counts.ownership_releases, kSteps - 1);
    CY_CHECK_EQ(counts.ownership_acquires, kSteps - 1);

    cy::Expected<cy::rendering::PlanAudit, cy::Error> audit =
        cy::rendering::validate_plan(graph, *plan);
    CY_REQUIRE(audit.has_value());
    CY_CHECK_EQ(audit->semaphore_waits, kSteps - 1);
}
