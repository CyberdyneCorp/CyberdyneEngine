// SPDX-License-Identifier: MIT

#include <cy/backends/rhi-d3d12/backend.h>

#include <cstring>

namespace cy::rhi::d3d12 {

Expected<Device*, Error> create_d3d12_device(Allocator& allocator,
                                             const DeviceDescription& desc) noexcept;
void destroy_d3d12_device(Allocator& allocator, Device* device) noexcept;
bool d3d12_device_present() noexcept;
AdapterIdentity query_d3d12_identity() noexcept;

AdapterClass classify_adapter(const char* name, u32 vendor_id) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return AdapterClass::Unknown;
    }
    // Microsoft's software implementations have vendor 0x1414. Keep the observed names as well:
    // identity strings survive the hosted-runner defect where adapter zero omits the software bit.
    if (vendor_id == 0x1414U || std::strstr(name, "Microsoft Basic Render Driver") != nullptr ||
        std::strstr(name, "WARP") != nullptr || std::strstr(name, "Software Adapter") != nullptr) {
        return AdapterClass::Software;
    }
    // PCI SIG vendor IDs are evidence of a physical-vendor adapter. Unknown IDs stay unknown
    // rather than being upgraded to hardware by absence of a software flag.
    switch (vendor_id) {
        case 0x1002U:  // AMD
        case 0x1010U:  // Imagination
        case 0x106BU:  // Apple
        case 0x10DEU:  // NVIDIA
        case 0x13B5U:  // ARM
        case 0x5143U:  // Qualcomm
        case 0x8086U:  // Intel
            return AdapterClass::Hardware;
        default:
            return AdapterClass::Unknown;
    }
}

const char* adapter_class_name(AdapterClass classification) noexcept {
    switch (classification) {
        case AdapterClass::Hardware:
            return "hardware";
        case AdapterClass::Software:
            return "software";
        case AdapterClass::Unknown:
            return "unknown";
    }
    return "unknown";
}

bool d3d12_backend_available() noexcept {
    return d3d12_device_present();
}

AdapterIdentity d3d12_runtime_identity() noexcept {
    return query_d3d12_identity();
}

Status register_d3d12_backend() noexcept {
    BackendRegistration registration;
    registration.name = kD3D12BackendName;
    registration.kind = BackendKind::D3D12;
    registration.create = &create_d3d12_device;
    registration.destroy = &destroy_d3d12_device;
    registration.is_available = &d3d12_device_present;
    return register_backend(registration);
}

}  // namespace cy::rhi::d3d12
