// The simulation-to-render snapshot, and the firewall around it. Tasks 4.1.2 and 1.3.
//
// Two things are asserted here that are easy to state and easy to lose. The snapshot is published
// by the *commit boundary* rather than by the renderer's own idea of a moment, so the publisher is
// wired to a real `CommitBoundary` in these cases rather than being called directly. And the
// interpolation alpha and the interpolated camera pose are presentation state, so an authoritative
// system cannot name them — which, being a compile-time property, is asserted with `static_assert`
// over a constrained concept rather than by calling anything.

#include <cy/test/test.h>

#include <cy/core/determinism/commit.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/scene/snapshot.h>

namespace {

using cy::determinism::AuthoritativeContext;
using cy::determinism::CommitBoundary;
using cy::determinism::CommitRecord;
using cy::determinism::DerivedContext;
using cy::determinism::PredictedContext;
using cy::determinism::PresentationContext;
using cy::rendering::CameraState;
using cy::rendering::GpuInstance;
using cy::rendering::LightState;
using cy::rendering::SnapshotPublisher;

template <class Context, class Field>
concept CanRead = requires(const Field& field, Context context) { field.read(context); };

using Alpha = cy::determinism::Presentation<cy::f32>;
using CameraPosition = decltype(CameraState::position);

// THE FIREWALL, AS A COMPILE-TIME FACT. An authoritative or predicted system reading the frame's
// interpolation alpha would be reading a value whose meaning depends on when the frame happened to
// be drawn; a derived one would be caching presentation data into the authoritative set in two
// hops. None of the three has an overload that yields it.
static_assert(CanRead<PresentationContext, Alpha>);
static_assert(!CanRead<AuthoritativeContext, Alpha>);
static_assert(!CanRead<PredictedContext, Alpha>);
static_assert(!CanRead<DerivedContext, Alpha>);
static_assert(CanRead<PresentationContext, CameraPosition>);
static_assert(!CanRead<AuthoritativeContext, CameraPosition>);

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

CommitRecord tick_record(cy::u64 tick) noexcept {
    CommitRecord record;
    record.point.tick = tick;
    record.state_version = tick;
    return record;
}

}  // namespace

CY_TEST_CASE("snapshot: nothing is readable before the first commit") {
    SnapshotPublisher publisher(allocator());
    CY_CHECK_FALSE(publisher.readable().valid());
    CY_CHECK_EQ(publisher.published_frames(), 0U);
}

CY_TEST_CASE("snapshot: the commit boundary publishes it, and the renderer does not ask for it") {
    CommitBoundary boundary(allocator());
    SnapshotPublisher publisher(allocator());
    CY_REQUIRE(boundary.observe(publisher).has_value());

    GpuInstance instance;
    instance.importance = cy::rendering::RenderImportance::clamped(0.5f);
    CY_REQUIRE(publisher.extraction().publish(0xABCD, instance).has_value());
    publisher.extraction().set_interpolation_alpha(PresentationContext{}, 0.25f);

    // The publisher is *called*; it has no way to reach a clock or a tick counter of its own.
    CY_REQUIRE(boundary.commit(tick_record(1)).has_value());

    const auto& snapshot = publisher.readable();
    CY_CHECK(snapshot.valid());
    CY_CHECK_EQ(snapshot.state_version(), 1U);
    CY_REQUIRE_EQ(snapshot.published().size(), 1U);
    CY_CHECK_EQ(snapshot.published()[0].identity, 0xABCDU);
    CY_CHECK_EQ(snapshot.interpolation_alpha().read(PresentationContext{}), 0.25f);
    CY_CHECK_EQ(publisher.published_frames(), 1U);
}

CY_TEST_CASE("snapshot: extraction writes the buffer the renderer is not reading") {
    CommitBoundary boundary(allocator());
    SnapshotPublisher publisher(allocator());
    CY_REQUIRE(boundary.observe(publisher).has_value());

    CY_REQUIRE(publisher.extraction().publish(1, GpuInstance{}).has_value());
    CY_REQUIRE(boundary.commit(tick_record(1)).has_value());
    const auto& first = publisher.readable();
    CY_REQUIRE_EQ(first.published().size(), 1U);

    // Filling the next frame's extraction must not disturb the snapshot the renderer holds. This is
    // the "Render reads a consistent world" scenario: the renderer's spans point into storage
    // nothing is writing, because the only writer is working in the other buffer.
    for (cy::u32 i = 0; i < 32; ++i) {
        CY_REQUIRE(publisher.extraction().publish(100 + i, GpuInstance{}).has_value());
    }
    CY_CHECK_EQ(publisher.readable().published().size(), 1U);
    CY_CHECK_EQ(publisher.readable().published()[0].identity, 1U);

    CY_REQUIRE(boundary.commit(tick_record(2)).has_value());
    CY_CHECK_EQ(publisher.readable().published().size(), 32U);
    CY_CHECK_EQ(publisher.readable().state_version(), 2U);
}

CY_TEST_CASE("snapshot: a frame that publishes nothing publishes nothing") {
    CommitBoundary boundary(allocator());
    SnapshotPublisher publisher(allocator());
    CY_REQUIRE(boundary.observe(publisher).has_value());

    CY_REQUIRE(publisher.extraction().publish(7, GpuInstance{}).has_value());
    CY_REQUIRE(boundary.commit(tick_record(1)).has_value());
    CY_REQUIRE(boundary.commit(tick_record(2)).has_value());

    // Not "the previous frame's instances again". An empty delta means nothing changed, and the GPU
    // scene still holds the world.
    CY_CHECK(publisher.readable().published().empty());
    CY_CHECK_EQ(publisher.readable().state_version(), 2U);
}

CY_TEST_CASE("snapshot: extraction is a delta, and removals travel with it") {
    CommitBoundary boundary(allocator());
    SnapshotPublisher publisher(allocator());
    CY_REQUIRE(boundary.observe(publisher).has_value());

    // The "Incremental extraction" scenario in miniature: 100 000 instances exist and 50 moved, so
    // 50 are published — the snapshot's size is the size of the change, not of the world.
    for (cy::u32 i = 0; i < 50; ++i) {
        CY_REQUIRE(publisher.extraction().publish(i, GpuInstance{}).has_value());
    }
    CY_REQUIRE(publisher.extraction().remove(90001).has_value());

    LightState light;
    light.intensity = 1600.0f;
    light.kind = 1;
    CY_REQUIRE(publisher.extraction().add_light(light).has_value());

    CameraState camera;
    camera.position.write(PresentationContext{}, cy::Vec3{0.0f, 2.0f, 5.0f});
    camera.history_identity = 0x5EED;
    CY_REQUIRE(publisher.extraction().add_camera(camera).has_value());

    CY_REQUIRE(boundary.commit(tick_record(4)).has_value());

    CY_CHECK_EQ(publisher.readable().published().size(), 50U);
    CY_REQUIRE_EQ(publisher.readable().removed().size(), 1U);
    CY_CHECK_EQ(publisher.readable().removed()[0], 90001U);
    CY_REQUIRE_EQ(publisher.readable().lights().size(), 1U);
    CY_CHECK_EQ(publisher.readable().lights()[0].intensity, 1600.0f);
    CY_REQUIRE_EQ(publisher.readable().cameras().size(), 1U);
    CY_CHECK_EQ(publisher.readable().cameras()[0].history_identity, 0x5EEDU);
}
