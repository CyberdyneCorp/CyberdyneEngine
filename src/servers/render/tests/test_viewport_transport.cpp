// The viewport transport's engine-side endpoint. Task 4.1, `editor-viewport-and-gizmos`.
//
// Two requirements are checked here, and the second is the one that will be forgotten:
//
//   * "The transport SHALL carry, alongside the image, the frame's view state and identifiers, so
//     that picking, gizmo interaction, and overlay alignment are correct for the frame actually
//     presented rather than for the editor's current state."
//   * "Latency and frame pacing SHALL be reported per transport, and the editor SHALL surface when
//     it is viewing a stale or degraded stream."
//
// The last case in this file is the joint one: a click resolved against a frame's own view state
// finds what the user was looking at, and the same click resolved against the editor's newer camera
// finds something else. That is the specification's scenario as an executable difference rather
// than as a comment saying the state is carried.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/picking.h>
#include <cy/servers/render/server.h>
#include <cy/servers/render/viewport_transport.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::render;

namespace {

/// A view looking down −Z from `x`, sized like a viewport.
[[nodiscard]] View view_at(f32 x) noexcept {
    View view;
    view.desc.purpose = ViewPurpose::EditorViewport;
    view.desc.viewport = ViewportRect{0, 0, 1920, 1080};
    view.desc.camera = cy::Transform::from_translation(cy::Vec3{x, 0.0F, 0.0F});
    view.desc.history_id = 0x5EED;
    view.refresh();
    return view;
}

constexpr u64 kMillisecond = 1000;

}  // namespace

CY_TEST_CASE("a published frame carries the view state it was rendered with, not a later one") {
    ViewportTransport transport(ViewportTransportKind::SharedTexture);
    View view = view_at(0.0F);

    const auto frame = transport.publish(view, 10 * kMillisecond, TextureHandle{});
    CY_REQUIRE(frame.has_value());
    CY_CHECK_EQ(*frame, 1ULL);

    // The editor's camera moves the instant the frame is handed over — which is what actually
    // happens, because the interface thread does not wait for the runtime.
    view.desc.camera = cy::Transform::from_translation(cy::Vec3{500.0F, 0.0F, 0.0F});
    view.refresh();

    CY_CHECK_NEAR(transport.latest().state.camera.translation.x, 0.0F, 1e-5F);
    CY_CHECK_EQ(transport.latest().state.history_id, 0x5EEDULL);
    CY_CHECK_EQ(transport.latest().state.viewport.width, 1920U);
    CY_CHECK(transport.latest().valid());
}

CY_TEST_CASE("frame identifiers start at one, so nothing published is distinguishable from zero") {
    ViewportTransport transport(ViewportTransportKind::LocalSurface);
    CY_CHECK_EQ(transport.latest().frame_id, kNoViewportFrame);
    CY_CHECK_FALSE(transport.latest().valid());

    const View view = view_at(0.0F);
    CY_REQUIRE_EQ(*transport.publish(view, 0, TextureHandle{}), 1ULL);
    CY_REQUIRE_EQ(*transport.publish(view, kMillisecond, TextureHandle{}), 2ULL);
}

CY_TEST_CASE("the latest frame wins and the frames nobody saw are counted") {
    // The decision the header argues for: a queue between two free-running clocks accumulates lag,
    // and lag is invisible while a drop is one counter. Three frames published, one acknowledged.
    ViewportTransport transport(ViewportTransportKind::SharedTexture);
    const View view = view_at(0.0F);

    CY_REQUIRE(transport.publish(view, 0, TextureHandle{}).has_value());
    CY_REQUIRE(transport.publish(view, 16 * kMillisecond, TextureHandle{}).has_value());
    const auto third = transport.publish(view, 32 * kMillisecond, TextureHandle{});
    CY_REQUIRE(third.has_value());

    CY_CHECK_EQ(transport.latest().frame_id, *third);
    CY_CHECK_EQ(transport.pacing().published, 3ULL);
    CY_CHECK_EQ(transport.pacing().superseded, 2ULL);

    transport.acknowledge(*third, 36 * kMillisecond);
    CY_CHECK_EQ(transport.pacing().acknowledged, 1ULL);
    // Acknowledging the newest frame means the next publication supersedes nothing.
    CY_REQUIRE(transport.publish(view, 48 * kMillisecond, TextureHandle{}).has_value());
    CY_CHECK_EQ(transport.pacing().superseded, 2ULL);
}

CY_TEST_CASE("pacing reports the interval, the worst interval and what missed the budget") {
    ViewportTransport transport(ViewportTransportKind::EncodedStream);
    ViewportTransportBudget budget;
    budget.requested_interval_micros = 16'667;
    budget.stale_after_micros = 50'000;
    CY_REQUIRE(transport.configure(budget).has_value());

    const View view = view_at(0.0F);
    CY_REQUIRE(transport.publish(view, 0, TextureHandle{}, 40'000).has_value());
    CY_REQUIRE(transport.publish(view, 16'000, TextureHandle{}, 41'000).has_value());
    CY_REQUIRE(transport.publish(view, 116'000, TextureHandle{}, 39'000).has_value());

    CY_CHECK_EQ(transport.pacing().last_interval_micros, 100'000U);
    CY_CHECK_EQ(transport.pacing().worst_interval_micros, 100'000U);
    // One of the two intervals exceeded the request; the 16 ms one did not.
    CY_CHECK_EQ(transport.pacing().late, 1ULL);
    // A mean over the two intervals, kept as a total rather than as a weighted average so that
    // reading it twice gives the same number.
    CY_CHECK_EQ(transport.pacing().mean_interval_micros, 58'000U);
    // An encoded stream reports its payload; a shared texture would report zero.
    CY_CHECK_EQ(transport.latest().payload_bytes, 39'000U);
    CY_CHECK_EQ(transport.latest().image, TextureHandle{});
}

CY_TEST_CASE("a stalled runtime is reported as stale rather than waited for") {
    // "WHEN the runtime stops producing frames THEN the viewport SHALL indicate staleness and the
    // editor SHALL remain interactive." Nothing here blocks: the editor asks how old the image is.
    ViewportTransport transport(ViewportTransportKind::SharedTexture);
    ViewportTransportBudget budget;
    budget.stale_after_micros = 50'000;
    CY_REQUIRE(transport.configure(budget).has_value());

    // Before anything arrives there is no staleness — a transport that has not started is a
    // different message from one that has stopped, and conflating them would make the editor say
    // "stale" on the first frame of every session.
    CY_CHECK_FALSE(transport.is_stale(1'000'000));
    CY_CHECK_EQ(transport.age_micros(1'000'000), 0ULL);

    CY_REQUIRE(transport.publish(view_at(0.0F), 1'000'000, TextureHandle{}).has_value());
    CY_CHECK_FALSE(transport.is_stale(1'020'000));
    CY_CHECK_EQ(transport.age_micros(1'020'000), 20'000ULL);
    CY_CHECK(transport.is_stale(1'080'000));

    // A stale threshold of zero would report every frame as stale on arrival, so it is refused.
    ViewportTransportBudget broken;
    broken.stale_after_micros = 0;
    CY_CHECK_FALSE(transport.configure(broken).has_value());
    CY_CHECK_EQ(transport.budget().stale_after_micros, 50'000U);
}

CY_TEST_CASE("degradation travels with the frame it describes") {
    // "Degradation SHALL be surfaced to the user, so that a lower-quality image is never mistaken
    // for the project's appearance." Set before the publication it applies to, so a quality
    // indicator is never one frame out of date.
    ViewportTransport transport(ViewportTransportKind::EncodedStream);
    const View view = view_at(0.0F);

    CY_REQUIRE(transport.publish(view, 0, TextureHandle{}).has_value());
    CY_CHECK(transport.latest().full_quality());
    CY_CHECK_EQ(transport.latest().degradation, ViewportDegradation::None);

    transport.set_degradation(ViewportDegradation::ReducedResolution, 0.5F);
    CY_REQUIRE(transport.publish(view, kMillisecond, TextureHandle{}).has_value());
    CY_CHECK_FALSE(transport.latest().full_quality());
    CY_CHECK_NEAR(transport.latest().resolution_scale, 0.5F, 1e-5F);

    // A scale outside [0, 1] is a caller mistake and is clamped rather than reported as a
    // resolution larger than the viewport.
    transport.set_degradation(ViewportDegradation::None, 4.0F);
    CY_REQUIRE(transport.publish(view, 2 * kMillisecond, TextureHandle{}).has_value());
    CY_CHECK(transport.latest().full_quality());
    CY_CHECK_NEAR(transport.latest().resolution_scale, 1.0F, 1e-5F);

    CY_CHECK_EQ(cy::Name::intern(viewport_degradation_name(ViewportDegradation::Paused)),
                cy::Name::intern("paused"));
    CY_CHECK_EQ(
        cy::Name::intern(viewport_transport_kind_name(ViewportTransportKind::EncodedStream)),
        cy::Name::intern("encoded-stream"));
}

CY_TEST_CASE("end-to-end latency is measured where it can be: at the acknowledgement") {
    ViewportTransport transport(ViewportTransportKind::SharedTexture);
    const View view = view_at(0.0F);

    CY_REQUIRE(transport.publish(view, 0, TextureHandle{}).has_value());
    transport.acknowledge(1, 8 * kMillisecond);
    CY_CHECK_EQ(transport.pacing().last_latency_micros, 8'000U);
    CY_CHECK_EQ(transport.pacing().worst_latency_micros, 8'000U);

    CY_REQUIRE(transport.publish(view, 16 * kMillisecond, TextureHandle{}).has_value());
    transport.acknowledge(2, 20 * kMillisecond);
    CY_CHECK_EQ(transport.pacing().last_latency_micros, 4'000U);
    CY_CHECK_EQ(transport.pacing().worst_latency_micros, 8'000U);
    CY_CHECK_EQ(transport.pacing().mean_latency_micros, 6'000U);

    // A frame identifier from a previous runtime is ignored rather than refused: reconnecting after
    // a crash legitimately holds one, and turning that into an error would make a restart — which
    // this milestone's artefact does on purpose — look like a defect.
    transport.acknowledge(999, 24 * kMillisecond);
    CY_CHECK_EQ(transport.pacing().acknowledged, 2ULL);

    transport.reset_pacing();
    CY_CHECK_EQ(transport.pacing().published, 0ULL);
    CY_CHECK(transport.latest().valid());
}

CY_TEST_CASE("a clock reading that goes backwards is refused rather than wrapped") {
    ViewportTransport transport(ViewportTransportKind::LocalSurface);
    const View view = view_at(0.0F);
    CY_REQUIRE(transport.publish(view, 100 * kMillisecond, TextureHandle{}).has_value());
    // Equal is accepted: two frames can finish inside one tick of a coarse clock.
    CY_CHECK(transport.publish(view, 100 * kMillisecond, TextureHandle{}).has_value());
    CY_CHECK_FALSE(transport.publish(view, 90 * kMillisecond, TextureHandle{}).has_value());
    CY_CHECK_EQ(transport.pacing().published, 2ULL);
}

CY_TEST_CASE("a click resolves against the frame that was shown, not the camera it has moved to") {
    // THE SCENARIO, and the whole reason the view state is carried:
    //
    //   "WHEN the user clicks in a streamed viewport THEN the hit SHALL be resolved against the
    //   view
    //    state of the frame shown, not a newer one"
    //
    // Two cubes, ten metres apart sideways. The frame is rendered from in front of the left one; by
    // the time the click arrives the editor's camera is in front of the right one. Resolving
    // against the presented frame selects the left; resolving against the editor's camera selects
    // the right, which is what a user would report as "it selected the wrong object".
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::Renderer);
    RenderServer server(allocator);
    RenderServerConfig config;
    config.debug_primitive_capacity = 8;
    config.debug_label_capacity = 4;
    CY_REQUIRE(server.configure(config).has_value());
    CY_REQUIRE(server.initialize().has_value());

    SceneDescription scene_desc;
    scene_desc.name = cy::Name::intern("streamed");
    scene_desc.instance_capacity = 16;
    const auto scene = server.create_scene(scene_desc);
    CY_REQUIRE(scene.has_value());

    MeshDescription mesh_desc;
    mesh_desc.name = cy::Name::intern("cube");
    mesh_desc.vertex_count = 24;
    mesh_desc.index_count = 36;
    MeshSurface surface;
    surface.vertex_count = 24;
    surface.index_count = 36;
    const auto mesh = server.create_mesh(mesh_desc, cy::Span<const MeshSurface>(&surface, 1),
                                         cy::Span<const MeshLod>());
    CY_REQUIRE(mesh.has_value());
    MaterialRecord material_desc;
    material_desc.name = cy::Name::intern("standard");
    const auto material = server.create_material(material_desc);
    CY_REQUIRE(material.has_value());

    for (int index = 0; index < 2; ++index) {
        InstanceDescription desc;
        desc.mesh = *mesh;
        desc.material = *material;
        desc.transform =
            cy::Transform::from_translation(cy::Vec3{index == 0 ? 0.0F : 10.0F, 0.0F, -10.0F});
        desc.stable_id = static_cast<u64>(index) + 1ULL;
        CY_REQUIRE(server.create_instance(*scene, desc).has_value());
    }

    ViewDescription view_desc;
    view_desc.name = cy::Name::intern("viewport");
    view_desc.scene = *scene;
    view_desc.purpose = ViewPurpose::EditorViewport;
    view_desc.viewport = ViewportRect{0, 0, 1920, 1080};
    view_desc.camera = cy::Transform::identity();
    const auto view_handle = server.create_view(view_desc);
    CY_REQUIRE(view_handle.has_value());

    ViewportTransport transport(ViewportTransportKind::EncodedStream);
    cy::Array<DrawItem> draws(allocator);
    ViewStatistics stats;
    View* view = server.view(*view_handle);
    CY_REQUIRE(server.collect_draws(*scene, *view, draws, stats).has_value());
    CY_REQUIRE(transport.publish(*view, 0, TextureHandle{}).has_value());

    // The editor flies to the right while the frame is in flight.
    view->desc.camera = cy::Transform::from_translation(cy::Vec3{10.0F, 0.0F, 0.0F});
    view->refresh();

    cy::Array<PickCandidate> candidates(allocator);
    PickFilter filter;
    filter.max_candidates = 1;
    const cy::Span<const GpuInstance> records = server.scene(*scene)->gpu.instances();

    // Against the frame that was shown. `to_view()` is the transport handing back the view state it
    // carried, and it is the only thing a caller has to remember to use.
    const View presented = transport.latest().state.to_view();
    CY_REQUIRE(pick_ray(records, draws.span(), ray_through_pixel(presented, 960.0F, 540.0F), filter,
                        candidates)
                   .has_value());
    CY_REQUIRE_EQ(candidates.size(), 1U);
    CY_CHECK_EQ(candidates[0].stable_id, 1ULL);

    // Against the editor's newer camera: the wrong object, with the same click and the same frame.
    CY_REQUIRE(pick_ray(records, draws.span(), ray_through_pixel(*view, 960.0F, 540.0F), filter,
                        candidates)
                   .has_value());
    CY_REQUIRE_EQ(candidates.size(), 1U);
    CY_CHECK_EQ(candidates[0].stable_id, 2ULL);
}
