// The GPU scene as a publication interface. Task 4.1.4.
//
// Every case here is a property `rendering-architecture` or `vfx-system` states about publication,
// checked rather than described. The file is organised by the three requirements the interface was
// designed against (gpu_scene.h's header comment lists them):
//
//   1. GPU-side publication with no CPU round trip
//   2. one flat array a culler reads with no per-producer indirection
//   3. removal without a full rebuild

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/gpu_scene.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::render;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// Small on purpose: a unit case must fit a millisecond at -O0, and 160 bytes a slot means a
/// capacity of a few thousand is a memset a unit budget would notice.
constexpr u32 kCapacity = 64;

struct Fixture {
    Fixture() noexcept : scene(allocator()) {}

    [[nodiscard]] bool start() noexcept {
        if (!scene.initialize(kCapacity).has_value()) {
            return false;
        }
        auto extract = scene.register_producer("extract", ProducerKind::Extract, Residency::Cpu);
        auto vfx = scene.register_producer("vfx", ProducerKind::Vfx, Residency::Gpu);
        if (!extract || !vfx) {
            return false;
        }
        cpu = *extract;
        gpu = *vfx;
        return true;
    }

    GpuScene scene;
    ProducerId cpu = kInvalidProducer;
    ProducerId gpu = kInvalidProducer;
};

}  // namespace

CY_TEST_CASE("the record's layout is the one the shader is told about") {
    // These are static_asserts in the header, so reaching this case at all means they held. What is
    // checked here is the half a static_assert cannot: that the accessors agree with the layout.
    GpuInstance record;
    record.set_stable_id(0x0123456789ABCDEFULL);
    CY_CHECK_EQ(record.stable_id(), 0x0123456789ABCDEFULL);
    CY_CHECK_EQ(record.stable_id_low, 0x89ABCDEFU);
    CY_CHECK_EQ(record.stable_id_high, 0x01234567U);
    CY_CHECK_FALSE(record.active());
    record.flags = kInstanceActive;
    CY_CHECK(record.active());
}

CY_TEST_CASE("a transform round-trips through the record's 4x3") {
    GpuInstance record;
    const cy::Transform placement{cy::Quat::from_axis_angle(cy::kAxisUp, 0.5F),
                                  cy::Vec3{3.0F, -4.0F, 5.0F}, cy::Vec3{2.0F, 2.0F, 2.0F}};
    write_transform(record, placement);
    const cy::Mat4 recovered = read_transform(record);
    const cy::Mat4 expected = placement.to_matrix();
    for (cy::usize row = 0; row < 3; ++row) {
        for (cy::usize column = 0; column < 4; ++column) {
            CY_CHECK_NEAR(recovered.at(row, column), expected.at(row, column), 1e-5F);
        }
    }
    // The fourth row of an affine matrix is (0, 0, 0, 1) and is not stored; `read_transform` puts
    // it back. That is the whole reason the record is 4x3 rather than 4x4.
    CY_CHECK_NEAR(recovered.at(3, 3), 1.0F, 1e-6F);
}

CY_TEST_CASE("bounds are published with the previous frame's alongside them") {
    GpuInstance record;
    write_bounds(record, cy::Aabb::from_center_extents(cy::Vec3{1.0F, 0.0F, 0.0F},
                                                       cy::Vec3{1.0F, 1.0F, 1.0F}));
    CY_CHECK_NEAR(record.bounds_center[0], 1.0F, 1e-6F);
    CY_CHECK_NEAR(record.previous_radius, 0.0F, 1e-6F);

    write_bounds(record, cy::Aabb::from_center_extents(cy::Vec3{5.0F, 0.0F, 0.0F},
                                                       cy::Vec3{1.0F, 1.0F, 1.0F}));
    // "The GPU scene SHALL retain per-instance previous and current bounds, which shadow
    // invalidation and motion vectors both consume, so neither derives them independently."
    CY_CHECK_NEAR(record.bounds_center[0], 5.0F, 1e-6F);
    CY_CHECK_NEAR(record.previous_center[0], 1.0F, 1e-6F);
}

CY_TEST_CASE("a producer reserves a contiguous run and writes it") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    auto range = fixture.scene.reserve(fixture.cpu, 4);
    CY_REQUIRE(range.has_value());
    CY_CHECK_EQ(range->count, 4U);

    auto slots = fixture.scene.writable(fixture.cpu, *range);
    CY_REQUIRE(slots.has_value());
    CY_CHECK_EQ(slots->size(), 4U);
    // Contiguous, because a GPU producer addresses its output as `base + thread_index`.
    CY_CHECK_EQ(&(*slots)[3] - slots->data(), 3);
}

CY_TEST_CASE("a device-side producer cannot be written from the CPU") {
    // REQUIREMENT 1. `vfx-system`: "Mesh particles SHALL NOT require ECS entities, per-particle CPU
    // submission, or CPU readback." A CPU write into a device-written range would be exactly the
    // round trip that forbids, so the interface refuses it rather than making it merely slow.
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    auto range = fixture.scene.reserve(fixture.gpu, 8);
    CY_REQUIRE(range.has_value());

    auto slots = fixture.scene.writable(fixture.gpu, *range);
    CY_REQUIRE_FALSE(slots.has_value());
    CY_CHECK_EQ(slots.error().code, cy::ErrorCode::PermissionDenied);
}

CY_TEST_CASE(
    "one reservation serves a million particles' worth of slots without a CPU cost per one") {
    // The shape of the VFX case, at unit scale: an effect reserves once and the slots are its own.
    // What is asserted is that reservation is O(1) in the particle count — one call, one range —
    // rather than a per-particle registration.
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    auto range = fixture.scene.reserve(fixture.gpu, 32);
    CY_REQUIRE(range.has_value());
    const ProducerInfo* info = fixture.scene.producer(fixture.gpu);
    CY_REQUIRE(info != nullptr);
    CY_CHECK_EQ(info->reservations, 1U);
    CY_CHECK_EQ(info->reserved_slots, 32U);
}

CY_TEST_CASE("a consumer reads one flat array and cannot tell what produced a record") {
    // REQUIREMENT 2. `virtual-geometry`: "Instance culling SHALL read the GPU scene, so virtual
    // geometry does not traverse ECS entities or maintain its own instance list."
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    auto extracted = fixture.scene.reserve(fixture.cpu, 2);
    auto particles = fixture.scene.reserve(fixture.gpu, 3);
    CY_REQUIRE(extracted.has_value());
    CY_REQUIRE(particles.has_value());

    auto slots = fixture.scene.writable(fixture.cpu, *extracted);
    CY_REQUIRE(slots.has_value());
    for (GpuInstance& record : *slots) {
        record.flags = kInstanceActive | kInstanceVisible;
    }

    // The consumer walks [0, high_water()) over one contiguous array. There is no producer table to
    // consult and nothing in a record that says where it came from.
    CY_CHECK_EQ(fixture.scene.high_water(), 5U);
    u32 active = 0;
    const cy::Span<const GpuInstance> records = fixture.scene.instances();
    for (u32 slot = 0; slot < fixture.scene.high_water(); ++slot) {
        active += records[slot].active() ? 1U : 0U;
    }
    CY_CHECK_EQ(active, 2U);
}

CY_TEST_CASE("releasing a producer's range removes its instances without a rebuild") {
    // REQUIREMENT 3. "WHEN an effect, entity, or UI document is destroyed THEN its instances SHALL
    // be removed from the GPU scene without requiring a full rebuild."
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    auto range = fixture.scene.reserve(fixture.cpu, 3);
    CY_REQUIRE(range.has_value());
    auto slots = fixture.scene.writable(fixture.cpu, *range);
    CY_REQUIRE(slots.has_value());
    for (GpuInstance& record : *slots) {
        record.flags = kInstanceActive | kInstanceVisible;
        record.layer_mask = kAllLayers;
    }

    fixture.scene.clear_dirty();
    CY_REQUIRE(fixture.scene.release(fixture.cpu, *range).has_value());

    // The records are inactive, and the range is dirty so the clear reaches the device. Both
    // matter: an inactive record the GPU never sees is a record the GPU still draws.
    for (u32 slot = range->first; slot < range->end(); ++slot) {
        CY_CHECK_FALSE(fixture.scene.at(slot).active());
        CY_CHECK_EQ(fixture.scene.at(slot).layer_mask, kNoLayers);
    }
    CY_CHECK_EQ(fixture.scene.dirty_ranges().size(), 1U);
    CY_CHECK_EQ(fixture.scene.dirty_ranges()[0], *range);
}

CY_TEST_CASE("a release from the wrong producer is refused") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    auto range = fixture.scene.reserve(fixture.cpu, 2);
    CY_REQUIRE(range.has_value());

    const cy::Status wrong_owner = fixture.scene.release(fixture.gpu, *range);
    CY_REQUIRE_FALSE(wrong_owner.has_value());
    CY_CHECK_EQ(wrong_owner.error().code, cy::ErrorCode::PermissionDenied);
    // And the range is still the CPU producer's.
    CY_CHECK(fixture.scene.release(fixture.cpu, *range).has_value());
}

CY_TEST_CASE("dirty ranges coalesce, so marking each instance separately costs one upload") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    auto range = fixture.scene.reserve(fixture.cpu, 8);
    CY_REQUIRE(range.has_value());
    fixture.scene.clear_dirty();

    for (u32 offset = 0; offset < 8; ++offset) {
        CY_REQUIRE(fixture.scene.mark_dirty(InstanceRange{range->first + offset, 1}).has_value());
    }
    CY_CHECK_EQ(fixture.scene.dirty_ranges().size(), 1U);
    CY_CHECK_EQ(fixture.scene.dirty_ranges()[0].count, 8U);
}

CY_TEST_CASE("disjoint dirty ranges stay separate and are ordered") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    CY_REQUIRE(fixture.scene.reserve(fixture.cpu, 16).has_value());
    fixture.scene.clear_dirty();

    // Marked out of order on purpose: the list is address-ordered, not insertion-ordered, so the
    // uploader walks the buffer forwards whatever order the producers ran in.
    CY_REQUIRE(fixture.scene.mark_dirty(InstanceRange{10, 2}).has_value());
    CY_REQUIRE(fixture.scene.mark_dirty(InstanceRange{2, 2}).has_value());
    CY_REQUIRE_EQ(fixture.scene.dirty_ranges().size(), 2U);
    CY_CHECK_EQ(fixture.scene.dirty_ranges()[0].first, 2U);
    CY_CHECK_EQ(fixture.scene.dirty_ranges()[1].first, 10U);
}

CY_TEST_CASE("freed runs coalesce, so a store does not fragment itself") {
    Fixture fixture;
    CY_REQUIRE(fixture.start());

    auto first = fixture.scene.reserve(fixture.cpu, 16);
    auto second = fixture.scene.reserve(fixture.cpu, 16);
    auto third = fixture.scene.reserve(fixture.cpu, 16);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(third.has_value());

    CY_REQUIRE(fixture.scene.release(fixture.cpu, *first).has_value());
    CY_REQUIRE(fixture.scene.release(fixture.cpu, *third).has_value());
    CY_REQUIRE(fixture.scene.release(fixture.cpu, *second).has_value());

    // Three releases in an order that leaves a hole in the middle until the last one. Coalescing on
    // release is what makes the store one run again rather than three.
    const GpuSceneStatistics stats = fixture.scene.statistics();
    CY_CHECK_EQ(stats.free_blocks, 1U);
    CY_CHECK_EQ(stats.largest_free_run, kCapacity);
    CY_CHECK_EQ(stats.reserved_slots, 0U);
}

CY_TEST_CASE("a store that cannot fit a run says so rather than growing") {
    // The store never grows: a device-side producer holds buffer offsets a move would invalidate,
    // and the failure has to reach the caller so it can decide.
    Fixture fixture;
    CY_REQUIRE(fixture.start());
    const auto too_big = fixture.scene.reserve(fixture.cpu, kCapacity + 1U);
    CY_REQUIRE_FALSE(too_big.has_value());
    CY_CHECK_EQ(too_big.error().code, cy::ErrorCode::OutOfMemory);
    CY_CHECK_EQ(fixture.scene.capacity(), kCapacity);
}
