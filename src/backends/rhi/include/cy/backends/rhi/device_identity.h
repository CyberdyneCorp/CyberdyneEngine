// SPDX-License-Identifier: MIT
#pragma once
// Device identity classification shared by backend reports. M11.d.5.

#include <cy/backends/rhi/capabilities.h>

namespace cy::rhi {

enum class DeviceClass : u8 { Hardware, Software, Paravirtual, NullBackend, Unknown };

struct DeviceIdentity {
    const char* name = "";
    const char* vendor = "unknown";
    u32 vendor_id = 0;
    DeviceClass classification = DeviceClass::Unknown;
};

[[nodiscard]] DeviceIdentity classify_device_identity(const char* name, u32 vendor_id,
                                                      BackendKind backend) noexcept;
[[nodiscard]] const char* device_class_name(DeviceClass classification) noexcept;

}  // namespace cy::rhi
