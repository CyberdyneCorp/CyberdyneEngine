// A deliberate violation: a render pass emitting its own barrier.
//
// THIS IS THE FILE M3's TASK 2.2.4 ASKS FOR. "Prove it by introducing one" — introducing one, once,
// by hand proves the gate fired on the day somebody ran it. Keeping the violation here as a fixture
// proves it on every pull request instead, which is the property the invariant actually needs: it is
// a property of the thirtieth pass, and the thirtieth pass obeys it because the first one did.
//
// What this pass should have written is a declaration:
//
//     graph.add_pass("depth prepass", QueueKind::Graphics)
//          .write(depth, Access::DepthStencilAttachmentWrite);
//
// tools/layercheck/layercheck.py --check barriers must reject this file.

#include <cy/backends/rhi/device.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering {

void record_depth_prepass(rhi::Device& device, rhi::CommandBufferHandle commands,
                          const rhi::BarrierBatch& batch) {
    rhi::BarrierRecorder& recorder = device.barrier_recorder(rhi::GraphBarrierKey{});
    recorder.record_barriers(commands, batch);
}

}  // namespace cy::rendering
