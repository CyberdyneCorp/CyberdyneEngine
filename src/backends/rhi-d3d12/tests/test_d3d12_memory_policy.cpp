// SPDX-License-Identifier: MIT

#include <cy/test/test.h>

#include <cy/backends/rhi-d3d12/backend.h>

CY_TEST_CASE("D3D12 Resource Heap Tier 1 partitions incompatible resources without a device") {
    using cy::rhi::d3d12::HeapResourceClass;
    using cy::rhi::d3d12::memory_pool_class;
    const cy::rhi::MemoryPoolClass tier1_buffer = memory_pool_class(1, HeapResourceClass::Buffer);
    const cy::rhi::MemoryPoolClass tier1_texture = memory_pool_class(1, HeapResourceClass::Texture);
    const cy::rhi::MemoryPoolClass tier1_target =
        memory_pool_class(1, HeapResourceClass::RenderTarget);
    CY_CHECK(meet(tier1_buffer, tier1_texture).empty());
    CY_CHECK(meet(tier1_texture, tier1_target).empty());

    const cy::rhi::MemoryPoolClass tier2_buffer = memory_pool_class(2, HeapResourceClass::Buffer);
    const cy::rhi::MemoryPoolClass tier2_target =
        memory_pool_class(2, HeapResourceClass::RenderTarget);
    CY_CHECK_FALSE(meet(tier2_buffer, tier2_target).empty());
}
