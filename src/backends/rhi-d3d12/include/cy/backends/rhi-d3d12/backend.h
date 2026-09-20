#pragma once
// Native Direct3D 12 backend registration and device report. M11.d.5.

#include <cy/backends/rhi/backend.h>

namespace cy::rhi::d3d12 {

inline constexpr const char* kD3D12BackendName = "d3d12";

enum class AdapterClass : u8 {
    Hardware,
    Software,
    Unknown,
};

struct AdapterIdentity {
    char name[128] = {};
    u32 vendor_id = 0;
    u32 device_id = 0;
    AdapterClass classification = AdapterClass::Unknown;
    bool dxgi_software_flag = false;
    u32 resource_heap_tier = 0;
};

enum class HeapResourceClass : u8 { Buffer, Texture, RenderTarget };

/// Classify from identity. `DXGI_ADAPTER_FLAG_SOFTWARE` is deliberately only reported: hosted
/// Windows exposes "Microsoft Basic Render Driver" without setting it on adapter zero.
[[nodiscard]] AdapterClass classify_adapter(const char* name, u32 vendor_id) noexcept;
[[nodiscard]] const char* adapter_class_name(AdapterClass classification) noexcept;
/// Pure Resource Heap Tier classification. Kept device-free so Tier 1 remains testable on the
/// hosted Tier 2 adapter.
[[nodiscard]] MemoryPoolClass memory_pool_class(u32 resource_heap_tier,
                                                HeapResourceClass resource) noexcept;

[[nodiscard]] bool d3d12_backend_available() noexcept;
[[nodiscard]] AdapterIdentity d3d12_runtime_identity() noexcept;
Status register_d3d12_backend() noexcept;

}  // namespace cy::rhi::d3d12
