// SPDX-License-Identifier: MIT
#include <cy/backends/rhi/device_identity.h>
#include <cy/test/test.h>
#include <cstring>

CY_TEST_CASE("a software device is labelled from its identity even when its flag lies") {
    using namespace cy::rhi;
    const DeviceIdentity basic =
        classify_device_identity("Microsoft Basic Render Driver", 0x1414U, BackendKind::D3D12);
    CY_CHECK_EQ(basic.classification, DeviceClass::Software);
    CY_CHECK_EQ(std::strcmp(basic.vendor, "Microsoft"), 0);
    const DeviceIdentity name_only =
        classify_device_identity("Microsoft Basic Render Driver", 0, BackendKind::D3D12);
    CY_CHECK_EQ(name_only.classification, DeviceClass::Software);
    CY_CHECK_EQ(
        classify_device_identity("Apple Paravirtual device", 0, BackendKind::Metal).classification,
        DeviceClass::Paravirtual);
}

CY_TEST_CASE("a paravirtual device that forwards its host's vendor is not labelled hardware") {
    using namespace cy::rhi;
    // Mesa's Venus driver reports the host GPU inside its own name and passes the host's PCI vendor
    // through, so a vendor-first classifier reads a guest's virtual GPU as the host's hardware.
    CY_CHECK_EQ(classify_device_identity("Virtio-GPU Venus (NVIDIA GeForce RTX 5060)", 0x10DEU,
                                         BackendKind::Vulkan)
                    .classification,
                DeviceClass::Paravirtual);
    CY_CHECK_EQ(
        classify_device_identity("SVGA3D; build: RELEASE; LLVM;", 0x15ADU, BackendKind::Vulkan)
            .classification,
        DeviceClass::Paravirtual);
    CY_CHECK_EQ(
        classify_device_identity("Parallels Display Adapter (WDDM)", 0x1AB8U, BackendKind::D3D12)
            .classification,
        DeviceClass::Paravirtual);
}

CY_TEST_CASE("a device report names the device that answered, its vendor and its class") {
    using namespace cy::rhi;
    const DeviceIdentity nvidia =
        classify_device_identity("NVIDIA GeForce RTX 5060", 0x10DEU, BackendKind::Vulkan);
    CY_CHECK_EQ(std::strcmp(nvidia.name, "NVIDIA GeForce RTX 5060"), 0);
    CY_CHECK_EQ(std::strcmp(nvidia.vendor, "NVIDIA"), 0);
    CY_CHECK_EQ(nvidia.vendor_id, 0x10DEU);
    CY_CHECK_EQ(nvidia.classification, DeviceClass::Hardware);
    CY_CHECK_EQ(std::strcmp(device_class_name(nvidia.classification), "hardware"), 0);
    const DeviceIdentity unknown =
        classify_device_identity("Some Future Adapter", 0xFFFFU, BackendKind::D3D12);
    CY_CHECK_EQ(unknown.classification, DeviceClass::Unknown);
    CY_CHECK_EQ(std::strcmp(device_class_name(unknown.classification), "unknown"), 0);
}
