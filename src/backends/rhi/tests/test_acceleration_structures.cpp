// SPDX-License-Identifier: MIT
// Acceleration structures in the RHI's vocabulary, and the refusal of a device that builds none.
// Issue #35, stage 3.
//
// The Vulkan backend's builds are `render.gi_gpu_tracing`'s, on a device. What is checked here
// needs no device: that a backend without ray tracing refuses by name rather than handing out a
// handle that names nothing — the null backend inherits that refusal from `rhi::Device`, exactly as
// D3D12 and Metal do — and that the three access intents a build and a trace declare are rows the
// render graph can derive a barrier from.

#include <cy/test/test.h>

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/memory/system_allocator.h>

#include <cstring>
#include <initializer_list>

namespace {

using cy::rhi::Access;
using cy::rhi::AccessFlags;
using cy::rhi::Stage;

class NullDevice {
public:
    NullDevice() noexcept
        : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)),
          device_(cy::rhi::null::create_null_device(allocator_, cy::rhi::DeviceDescription{})) {}
    ~NullDevice() {
        if (device_.has_value()) {
            cy::rhi::null::destroy_null_device(allocator_, device_.value());
        }
    }
    NullDevice(const NullDevice&) = delete;
    NullDevice& operator=(const NullDevice&) = delete;
    NullDevice(NullDevice&&) = delete;
    NullDevice& operator=(NullDevice&&) = delete;

    [[nodiscard]] cy::rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] bool created() const noexcept { return device_.has_value(); }

private:
    cy::Allocator& allocator_;
    cy::Expected<cy::rhi::Device*, cy::Error> device_;
};

}  // namespace

CY_TEST_CASE("a device without ray tracing refuses an acceleration structure by name") {
    NullDevice null;
    CY_REQUIRE(null.created());
    cy::rhi::Device& device = null.device();
    CY_CHECK_FALSE(device.capabilities().has(cy::rhi::Capability::RayTracing));

    for (const auto kind : {cy::rhi::AccelerationStructureKind::BottomLevel,
                            cy::rhi::AccelerationStructureKind::TopLevel}) {
        cy::rhi::AccelerationStructureDescription description;
        description.kind = kind;
        description.max_triangles = 12;
        description.max_vertices = 8;
        description.max_instances = 4;
        const auto created = device.create_acceleration_structure(description);
        CY_REQUIRE_FALSE(created.has_value());
        CY_CHECK_EQ(created.error().code, cy::ErrorCode::Unsupported);
        // The refusal says WHY, so the caller's log names the capability rather than a failure.
        CY_CHECK(std::strstr(created.error().message, "RayTracing") != nullptr);
    }
    // Nothing was created, so nothing is valid and nothing has a storage buffer to declare.
    const cy::rhi::AccelerationStructureHandle never;
    CY_CHECK_FALSE(device.acceleration_structure_valid(never));
    CY_CHECK(device.acceleration_structure_info(never).storage.is_null());
}

CY_TEST_CASE(
    "a build writes, a top-level build reads, and a ray query reads, at their own stages") {
    const cy::rhi::AccessInfo& build =
        cy::rhi::access_info(Access::AccelerationStructureBuildWrite);
    CY_CHECK_EQ(build.stage, Stage::AccelerationStructureBuild);
    CY_CHECK(build.is_write);
    // A build reads its scratch and an update reads what it rewrites, so the row carries both.
    CY_CHECK(cy::rhi::any(build.access & AccessFlags::AccelerationStructureWrite));
    CY_CHECK(cy::rhi::any(build.access & AccessFlags::AccelerationStructureRead));

    const cy::rhi::AccessInfo& instances =
        cy::rhi::access_info(Access::AccelerationStructureBuildRead);
    CY_CHECK_EQ(instances.stage, Stage::AccelerationStructureBuild);
    CY_CHECK_FALSE(instances.is_write);
    CY_CHECK_EQ(instances.access, AccessFlags::AccelerationStructureRead);

    const cy::rhi::AccessInfo& query =
        cy::rhi::access_info(Access::ComputeAccelerationStructureRead);
    CY_CHECK_EQ(query.stage, Stage::ComputeShader);
    CY_CHECK_FALSE(query.is_write);
    CY_CHECK_EQ(query.access, AccessFlags::AccelerationStructureRead);

    // A structure lives in a buffer and is never an image: all three refuse a texture.
    for (const Access access :
         {Access::AccelerationStructureBuildWrite, Access::AccelerationStructureBuildRead,
          Access::ComputeAccelerationStructureRead}) {
        CY_CHECK_FALSE(cy::rhi::access_valid_for(access, true));
        CY_CHECK(cy::rhi::access_valid_for(access, false));
    }
    CY_CHECK(
        std::strcmp(cy::rhi::descriptor_kind_name(cy::rhi::DescriptorKind::AccelerationStructure),
                    "acceleration-structure") == 0);
}
