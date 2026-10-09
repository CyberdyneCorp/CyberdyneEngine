// SPDX-License-Identifier: MIT
// WHERE THE WORLD'S STREAMS LIVE, ON A REAL DEVICE, AND WHAT READING THEM COSTS. #77.
//
// ================================================================================================
// THE DEFECT
// ================================================================================================
//
// `m11a:world-budget-on-a-device` failed about one run in three. `samples/10-world` draws its
// vertex, colour and index streams three times a frame with water shading on — the opaque frame,
// the water's refraction picture and its reflection picture — and kept them in `Upload` memory,
// which on a discrete GPU is system memory: every one of the three draws pulled about sixteen
// megabytes across the bus. The two water pictures cost 2.0 to 2.7 ms of device time a frame on
// the RTX 5060 and the whole frame 4 to 5 ms. The stage asks `geometry_memory()` instead now
// (samples/10-world/geometry_memory.h), which answers device-local memory the processor maps
// where the device offers it — and the Vulkan backend never said it did, because nothing set
// `Capability::HostVisibleDeviceLocalMemory`. Both halves are this suite's.
//
// ================================================================================================
// TWO CASES
// ================================================================================================
//
//   * THE CAPABILITY IS THE DRIVER'S ANSWER. The memory types the backend counted are non-empty,
//     and the capability is exactly the derivation `unit.rhi` drives applied to them. A backend
//     that counted nothing, or set the capability from anything else, fails here.
//   * THE MEMORY THE STAGE CHOOSES IS CHEAPER TO READ THREE TIMES. A world-sized stream is read by
//     the device three times, as the frame's three passes read it, once from the memory
//     `geometry_memory()` chooses and once from `Upload`, each timed with the device's own
//     timestamps. Where the device has system memory apart from its own (`host_only` non-zero), the
//     chosen memory must take at most half the time — measured on the RTX 5060 at about a fifth.
//     Where the capability is absent, or every type is device-local (unified memory), there is
//     nothing to choose between and the case says so instead of passing on a comparison it did not
//     make.
//
// The reads are buffer copies rather than draws: the cost under test is the bus, which a copy
// crosses exactly as a vertex fetch does, and a copy needs no pipeline, no shader and no target.

#include <cy/test/test.h>

#include <cy/backends/rhi/command_buffer.h>

#include "10-world/geometry_memory.h"
#include "device.h"

#include <cstdio>
#include <cstring>

namespace {

namespace rhi = cy::rhi;

using cy::u32;
using cy::u64;

/// About what the world's streams are: the terrain's positions and colours and the frame's plant
/// proxies, sky, stars and sea.
constexpr u64 kStreamBytes = u64{16} << 20U;
/// The frame's three passes: the opaque frame and the water's two pictures.
constexpr u32 kReads = 3;
constexpr u64 kWaitNanoseconds = 30'000'000'000ULL;

/// Buffers made for one measurement, destroyed with it.
struct Buffers {
    rhi::Device* device = nullptr;
    rhi::BufferHandle source;
    rhi::BufferHandle destinations[kReads];
    rhi::QueryPoolHandle timer;

    Buffers() = default;
    Buffers(const Buffers&) = delete;
    Buffers& operator=(const Buffers&) = delete;
    ~Buffers() {
        if (device == nullptr) {
            return;
        }
        (void)device->wait_idle();
        device->destroy_buffer(source);
        for (rhi::BufferHandle destination : destinations) {
            device->destroy_buffer(destination);
        }
        if (!timer.is_null()) {
            device->destroy_query_pool(timer);
        }
    }
};

/// The device time, in milliseconds, of reading a `kStreamBytes` stream in `memory` `kReads`
/// times. Each read goes to a destination of its own, so no read waits on another's write.
[[nodiscard]] cy::Expected<double, cy::Error> time_reads(rhi::Device& device,
                                                         rhi::MemoryUse memory) {
    Buffers buffers;
    buffers.device = &device;

    rhi::BufferDescription source;
    source.name = "geometry memory stream";
    source.size = kStreamBytes;
    source.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferSource;
    source.memory = memory;
    cy::Expected<rhi::BufferHandle, cy::Error> made = device.create_buffer(source);
    if (!made) {
        return cy::make_unexpected(made.error());
    }
    buffers.source = *made;
    // WRITTEN THROUGH THE MAPPING, as the stage writes its streams: memory the processor cannot map
    // is not memory the stage can use, whatever it costs the device to read.
    void* mapped = device.buffer_mapped_pointer(buffers.source);
    if (mapped == nullptr) {
        return cy::fail(cy::ErrorCode::Internal, "the stream's memory is not mapped");
    }
    std::memset(mapped, 0x5A, static_cast<std::size_t>(kStreamBytes));

    for (rhi::BufferHandle& destination : buffers.destinations) {
        rhi::BufferDescription description;
        description.name = "geometry memory read";
        description.size = kStreamBytes;
        description.usage = rhi::BufferUsage::TransferDestination;
        description.memory = rhi::MemoryUse::DeviceLocal;
        made = device.create_buffer(description);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        destination = *made;
    }

    rhi::QueryPoolDescription timer;
    timer.name = "geometry memory timestamps";
    timer.kind = rhi::QueryKind::Timestamp;
    timer.count = 2;
    cy::Expected<rhi::QueryPoolHandle, cy::Error> pool = device.create_query_pool(timer);
    if (!pool) {
        return cy::make_unexpected(pool.error());
    }
    buffers.timer = *pool;

    cy::Expected<rhi::CommandBufferHandle, cy::Error> command =
        device.acquire_command_buffer(rhi::QueueKind::Graphics, false);
    if (!command) {
        return cy::make_unexpected(command.error());
    }
    if (cy::Status begun = device.begin_command_buffer(*command); !begun) {
        return cy::make_unexpected(begun.error());
    }
    rhi::CommandBuffer& commands = *device.command_buffer(*command);
    commands.reset_queries(buffers.timer, 0, 2);
    commands.write_timestamp(buffers.timer, 0);
    rhi::BufferCopy region;
    region.size = kStreamBytes;
    for (rhi::BufferHandle destination : buffers.destinations) {
        commands.copy_buffer(buffers.source, destination,
                             cy::Span<const rhi::BufferCopy>(&region, 1));
    }
    commands.write_timestamp(buffers.timer, 1);
    if (cy::Status ended = device.end_command_buffer(*command); !ended) {
        return cy::make_unexpected(ended.error());
    }
    rhi::SubmitInfo submit;
    submit.queue = rhi::QueueKind::Graphics;
    submit.command_buffers = cy::Span<const rhi::CommandBufferHandle>(&*command, 1);
    cy::Expected<u64, cy::Error> value = device.submit(submit);
    if (!value) {
        return cy::make_unexpected(value.error());
    }
    if (cy::Status waited =
            device.wait_timeline(rhi::QueueKind::Graphics, *value, kWaitNanoseconds);
        !waited) {
        return cy::make_unexpected(waited.error());
    }
    u64 stamps[2] = {};
    cy::Expected<u32, cy::Error> read =
        device.read_query_results(buffers.timer, 0, 2, cy::Span<u64>(stamps, 2));
    if (!read) {
        return cy::make_unexpected(read.error());
    }
    if (*read != 2 || stamps[1] < stamps[0]) {
        return cy::fail(cy::ErrorCode::Internal, "the timestamps did not resolve in order");
    }
    return static_cast<double>(stamps[1] - stamps[0]) / 1.0e6;
}

}  // namespace

CY_TEST_CASE("geometry memory: the capability is what the device's memory types say") {
    cy::render_test::DeviceFixture fixture("vulkan", "cy_test_render_geometry_memory");
    if (!fixture.is(rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }
    const rhi::DeviceCapabilities& capabilities = fixture.device().capabilities();
    const rhi::MemoryObservation& observed = capabilities.memory_observation();
    std::fprintf(stderr,
                 "%s: %u memory types, %u device-local and mappable, %u system memory alone -> "
                 "HostVisibleDeviceLocalMemory=%d\n",
                 capabilities.device_name(), observed.types, observed.device_local_mappable,
                 observed.host_only,
                 static_cast<int>(capabilities.has(rhi::Capability::HostVisibleDeviceLocalMemory)));

    // Every Vulkan device lists at least one memory type, so zero is a backend that never looked.
    CY_CHECK_GT(observed.types, 0U);
    CY_CHECK_EQ(capabilities.has(rhi::Capability::HostVisibleDeviceLocalMemory),
                rhi::device_offers_host_visible_device_local(observed));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("geometry memory: the stage's choice reads the world's streams cheaper") {
    cy::render_test::DeviceFixture fixture("vulkan", "cy_test_render_geometry_memory");
    if (!fixture.is(rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }
    rhi::Device& device = fixture.device();
    const rhi::DeviceCapabilities& capabilities = device.capabilities();
    if (!capabilities.has(rhi::Capability::TimestampQueries)) {
        std::fprintf(stderr, "%s has no timestamp queries, so nothing here can be timed\n",
                     capabilities.device_name());
        return;
    }
    if (!capabilities.has(rhi::Capability::HostVisibleDeviceLocalMemory)) {
        std::fprintf(stderr,
                     "%s offers no device-local memory the processor maps; the stage keeps its "
                     "streams in Upload memory and there is no second choice to compare\n",
                     capabilities.device_name());
        return;
    }
    // THE STAGE'S CHOICE, NOT THE ONE THIS CASE WOULD MAKE. A `geometry_memory()` that went back to
    // `Upload` fails here, and makes the two measurements below the same memory as well.
    const rhi::MemoryUse chosen = cy::sample::world::geometry_memory(capabilities);
    CY_CHECK(chosen == rhi::MemoryUse::HostVisibleDeviceLocal);

    // Once each before measuring, so neither measurement pays for the first touch of its memory.
    CY_REQUIRE(time_reads(device, chosen).has_value());
    CY_REQUIRE(time_reads(device, rhi::MemoryUse::Upload).has_value());
    const cy::Expected<double, cy::Error> device_local = time_reads(device, chosen);
    const cy::Expected<double, cy::Error> upload = time_reads(device, rhi::MemoryUse::Upload);
    CY_REQUIRE(device_local.has_value());
    CY_REQUIRE(upload.has_value());
    std::fprintf(stderr,
                 "%s: %u reads of %llu MiB take %.3f ms from the stage's memory and %.3f ms from "
                 "Upload memory\n",
                 capabilities.device_name(), kReads,
                 static_cast<unsigned long long>(kStreamBytes >> 20U), *device_local, *upload);

    if (capabilities.memory_observation().host_only == 0) {
        std::fprintf(stderr,
                     "every memory type of %s is device-local (unified memory), so the two are the "
                     "same memory and their times are not compared\n",
                     capabilities.device_name());
        return;
    }
    CY_CHECK_LE(*device_local * 2.0, *upload);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
