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

CY_TEST_CASE(
    "the D3D12 adapter selector labels a software adapter from its identity, not its flag") {
    using cy::rhi::d3d12::AdapterClass;
    using cy::rhi::d3d12::classify_adapter;
    // Every hosted Windows image presents adapter 0 as "Microsoft Basic Render Driver" WITHOUT
    // DXGI_ADAPTER_FLAG_SOFTWARE. The classifier never sees the flag, so the name and vendor
    // decide.
    CY_CHECK_EQ(classify_adapter("Microsoft Basic Render Driver", 0x1414U), AdapterClass::Software);
    CY_CHECK_EQ(classify_adapter("Microsoft Basic Render Driver", 0), AdapterClass::Software);
    CY_CHECK_EQ(classify_adapter("Microsoft Basic Render Driver", 0x10DEU), AdapterClass::Software);
    CY_CHECK_EQ(classify_adapter("AMD Radeon RX 6900 XT", 0x1002U), AdapterClass::Hardware);
    // An adapter it cannot place is unknown, never promoted to hardware by a missing flag.
    CY_CHECK_EQ(classify_adapter("Some Future Adapter", 0xFFFFU), AdapterClass::Unknown);
    CY_CHECK_EQ(classify_adapter("NVIDIA GeForce RTX 5060", 0), AdapterClass::Unknown);
    CY_CHECK_EQ(classify_adapter("", 0x10DEU), AdapterClass::Unknown);
}
