// A deliberate violation: a file above src/backends/ that names a Vulkan header.
//
// The engine's rule is the same one that keeps SDL inside platform/. The RHI's synchronisation
// vocabulary is engine-owned (cy/backends/rhi/types.h) so that the render graph needs no graphics
// SDK to compile, so that the null backend can run the same code in continuous integration, and so
// that Metal and D3D12 are a directory rather than a rewrite.
//
// tools/layercheck/layercheck.py --check gpuapi must reject this file.

#include <vulkan/vulkan.h>

void record() {
    VkImageMemoryBarrier2 barrier{};
    (void)barrier;
}
