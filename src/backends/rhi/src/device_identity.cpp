// SPDX-License-Identifier: MIT

#include <cy/backends/rhi/device_identity.h>

#include <cstring>

namespace cy::rhi {
namespace {
[[nodiscard]] bool has(const char* name, const char* fragment) noexcept {
    return name != nullptr && std::strstr(name, fragment) != nullptr;
}
[[nodiscard]] const char* vendor_name(u32 id) noexcept {
    switch (id) {
        case 0x1002U:
            return "AMD";
        case 0x1010U:
            return "Imagination";
        case 0x106BU:
            return "Apple";
        case 0x10DEU:
            return "NVIDIA";
        case 0x13B5U:
            return "Arm";
        case 0x1414U:
            return "Microsoft";
        case 0x5143U:
            return "Qualcomm";
        case 0x8086U:
            return "Intel";
        default:
            return "unknown";
    }
}
}  // namespace

DeviceIdentity classify_device_identity(const char* name, u32 vendor_id,
                                        BackendKind backend) noexcept {
    DeviceIdentity identity{name != nullptr ? name : "", vendor_name(vendor_id), vendor_id,
                            DeviceClass::Unknown};
    if (backend == BackendKind::Null) {
        identity.vendor = "Cyberdyne";
        identity.classification = DeviceClass::NullBackend;
    } else if (has(name, "Apple Paravirtual") || has(name, "Paravirtual")) {
        identity.classification = DeviceClass::Paravirtual;
    } else if (vendor_id == 0x1414U || has(name, "Microsoft Basic Render Driver") ||
               has(name, "WARP") || has(name, "llvmpipe") || has(name, "lavapipe") ||
               has(name, "SwiftShader") || has(name, "Software Rasterizer")) {
        identity.classification = DeviceClass::Software;
    } else if (vendor_id == 0x1002U || vendor_id == 0x1010U || vendor_id == 0x106BU ||
               vendor_id == 0x10DEU || vendor_id == 0x13B5U || vendor_id == 0x5143U ||
               vendor_id == 0x8086U || has(name, "Apple M") || has(name, "NVIDIA") ||
               has(name, "AMD Radeon") || has(name, "Intel")) {
        identity.classification = DeviceClass::Hardware;
    }
    return identity;
}

const char* device_class_name(DeviceClass classification) noexcept {
    switch (classification) {
        case DeviceClass::Hardware:
            return "hardware";
        case DeviceClass::Software:
            return "software";
        case DeviceClass::Paravirtual:
            return "paravirtual";
        case DeviceClass::NullBackend:
            return "null-backend";
        case DeviceClass::Unknown:
            return "unknown";
    }
    return "unknown";
}
}  // namespace cy::rhi
