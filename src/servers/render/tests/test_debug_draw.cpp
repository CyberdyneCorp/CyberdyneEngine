// Debug visualisation and render statistics. Task 4.1.6.
//
// The two scenarios `rendering-architecture` states about debug draw, and the two properties it
// states about statistics:
//
//   "WHEN a gameplay system submits a debug sphere during simulation THEN it SHALL be double
//    buffered and drawn in the following frame with no allocation per primitive"
//   "WHEN the engine is built for shipping THEN debug view modes and debug draw SHALL be compiled
//    out"
//   "the renderer SHALL accumulate per view and per frame: visible instance count, draw call count,
//    triangle count, pass count, GPU time per pass, CPU time per stage, and memory usage by
//    resource category"
//   "WHEN timestamp queries are available THEN each render graph pass SHALL report its GPU
//    duration, attributable by pass name"
//
// THE COMPILED-OUT CASE IS NOT SKIPPED OUTSIDE A DEVELOPMENT BUILD, IT IS INVERTED. `Profile` and
// `Shipping` are exactly where the requirement has teeth, so the case asserts the opposite
// behaviour there — nothing stored, nothing drawn — rather than being compiled away itself, which
// would leave the requirement unchecked in the only builds it is about.

#include <cy/core/memory/system_allocator.h>
#include <cy/servers/render/debug_draw.h>
#include <cy/servers/render/server.h>
#include <cy/servers/render/statistics.h>
#include <cy/test/test.h>

#include <string_view>

using cy::f32;
using cy::u32;
using cy::u64;
using namespace cy::render;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

/// A store sized to what the case submits. See `RenderServerConfig` for why a test never takes the
/// default: it is 4096 primitives per buffer, reserved and constructed in `initialize()`.
[[nodiscard]] bool start(DebugDrawList& list, u32 primitives = 16, u32 labels = 4) noexcept {
    return list.initialize(primitives, labels).has_value();
}

}  // namespace

CY_TEST_CASE("a primitive submitted this frame is drawn in the next one") {
    DebugDrawList list(allocator());
    CY_REQUIRE(start(list));

    list.sphere(cy::Vec3{1.0F, 2.0F, 3.0F}, 0.5F, 0xFF00FF00U);

    // Still being written: the renderer reading `primitives()` this frame sees nothing, which is
    // what makes submission from a simulation thread safe without a lock.
    CY_CHECK_EQ(list.primitives().size(), 0U);

    list.swap();

    if constexpr (kDebugVisualisationEnabled) {
        CY_REQUIRE_EQ(list.primitives().size(), 1U);
        const DebugPrimitive& primitive = list.primitives()[0];
        CY_CHECK(primitive.shape == DebugShape::Sphere);
        CY_CHECK_NEAR(primitive.a.y, 2.0F, 1e-6F);
        CY_CHECK_NEAR(primitive.radius, 0.5F, 1e-6F);
        // And the buffer now being written is the other one, so the next frame's submissions do not
        // land in the list being drawn.
        list.line(cy::Vec3{}, cy::Vec3{1.0F, 0.0F, 0.0F}, 0xFFFFFFFFU);
        CY_CHECK_EQ(list.primitives().size(), 1U);
    } else {
        CY_CHECK_EQ(list.primitives().size(), 0U);
    }
}

CY_TEST_CASE("every shape the specification names reaches the list") {
    DebugDrawList list(allocator());
    CY_REQUIRE(start(list));

    list.line(cy::Vec3{}, cy::Vec3{1.0F, 0.0F, 0.0F}, 0xFFFFFFFFU);
    list.sphere(cy::Vec3{}, 1.0F, 0xFFFFFFFFU);
    list.box(cy::Aabb{cy::Vec3{-1.0F, -1.0F, -1.0F}, cy::Vec3{1.0F, 1.0F, 1.0F}}, 0xFFFFFFFFU);
    list.capsule(cy::Vec3{}, cy::Vec3{0.0F, 2.0F, 0.0F}, 0.5F, 0xFFFFFFFFU);
    list.frustum(cy::Mat4::identity(), 0xFFFFFFFFU);
    list.label(cy::Vec3{}, "spawn point", 0xFFFFFFFFU);
    list.swap();

    const u32 expected = kDebugVisualisationEnabled ? 5U : 0U;
    CY_CHECK_EQ(list.primitives().size(), expected);
    CY_CHECK_EQ(list.labels().size(), kDebugVisualisationEnabled ? 1U : 0U);
    CY_CHECK_EQ(list.dropped(), 0U);
}

CY_TEST_CASE("submission past the capacity is dropped and counted rather than growing") {
    DebugDrawList list(allocator());
    CY_REQUIRE(start(list, 4, 1));

    for (u32 index = 0; index < 10; ++index) {
        list.sphere(cy::Vec3{static_cast<f32>(index), 0.0F, 0.0F}, 1.0F, 0xFFFFFFFFU);
    }
    list.swap();

    if constexpr (kDebugVisualisationEnabled) {
        // Four kept, six dropped. A missing debug sphere is otherwise indistinguishable from a bug
        // in whatever the sphere was drawn to investigate, which is why the count exists.
        CY_CHECK_EQ(list.primitives().size(), 4U);
        CY_CHECK_EQ(list.dropped(), 6U);
        CY_CHECK_EQ(list.capacity(), 4U);
    } else {
        CY_CHECK_EQ(list.primitives().size(), 0U);
    }
}

CY_TEST_CASE("a shipping build stores no debug primitive at all") {
    // The requirement, from the side that matters. `kDebugVisualisationEnabled` follows
    // CY_DEVELOPMENT, so this case is the Profile and Shipping assertion and the development
    // build's is the branch above it.
    DebugDrawList list(allocator());
    // Sized rather than defaulted: the default reserves 4096 primitives in each of two buffers, and
    // constructing them is a millisecond — a `unit` budget spent measuring a constant. What this
    // case is about is whether ANY storage exists, which a capacity of eight answers just as well.
    CY_REQUIRE(start(list, 8, 2));
    list.sphere(cy::Vec3{}, 1.0F, 0xFFFFFFFFU);
    list.swap();

    if constexpr (kDebugVisualisationEnabled) {
        CY_CHECK_EQ(list.primitives().size(), 1U);
        CY_CHECK_EQ(list.capacity(), 8U);
    } else {
        CY_CHECK_EQ(list.primitives().size(), 0U);
        CY_CHECK_EQ(list.labels().size(), 0U);
        CY_CHECK_EQ(list.capacity(), 0U);
    }
}

CY_TEST_CASE("a debug view mode round-trips through its name") {
    for (u32 index = 0; index < kDebugViewModeCount; ++index) {
        const auto mode = static_cast<DebugViewMode>(index);
        CY_CHECK(debug_view_mode_from_name(debug_view_mode_name(mode)) == mode);
    }
    // An unknown name is `Off` rather than an assertion: the value arrives from a console command
    // and a typo falls back to normal shading rather than stopping the frame.
    CY_CHECK(debug_view_mode_from_name("no-such-mode") == DebugViewMode::Off);
    CY_CHECK(debug_view_mode_from_name(nullptr) == DebugViewMode::Off);
}

CY_TEST_CASE("a frame's totals are the sum of its views") {
    FrameStatistics frame;
    frame.reset();

    ViewStatistics main;
    main.name = cy::Name::intern("main");
    main.instances_considered = 100;
    main.instances_visible = 40;
    main.draw_calls = 30;
    main.draws_merged = 10;
    main.triangles = 12000;
    main.passes = 4;

    ViewStatistics shadow;
    shadow.name = cy::Name::intern("shadow");
    shadow.purpose = ViewPurpose::Shadow;
    shadow.instances_visible = 25;
    shadow.draw_calls = 25;
    shadow.triangles = 8000;
    shadow.passes = 1;

    accumulate(frame, main);
    accumulate(frame, shadow);

    CY_CHECK_EQ(frame.views, 2U);
    CY_CHECK_EQ(frame.instances_visible, 65U);
    CY_CHECK_EQ(frame.draw_calls, 55U);
    CY_CHECK_EQ(frame.draws_merged, 10U);
    CY_CHECK_EQ(frame.triangles, 20000ULL);
    CY_CHECK_EQ(frame.passes, 5U);
}

CY_TEST_CASE("a pass timing is attributable by name, and an unmeasured one says so") {
    FrameStatistics frame;
    frame.reset();

    frame.add_pass_timing("depth prepass", 250000, true);
    frame.add_pass_timing("opaque", 0, false);

    CY_REQUIRE_EQ(frame.pass_timing_count, 2U);
    CY_CHECK(std::string_view(frame.pass_timings[0].name) == std::string_view("depth prepass"));
    CY_CHECK_EQ(frame.pass_timings[0].gpu_nanoseconds, 250000ULL);
    CY_CHECK(frame.pass_timings[0].measured);
    // Zero and "not measured" are different answers. A report that could not tell them apart would
    // print a pass that ran as free.
    CY_CHECK_FALSE(frame.pass_timings[1].measured);
    CY_CHECK_EQ(frame.pass_timings_dropped, 0U);
}

CY_TEST_CASE("more passes than the report holds are dropped and counted, not allocated for") {
    FrameStatistics frame;
    frame.reset();
    for (u32 index = 0; index < kMaxTimedPasses + 3U; ++index) {
        frame.add_pass_timing("pass", index, true);
    }
    CY_CHECK_EQ(frame.pass_timing_count, kMaxTimedPasses);
    CY_CHECK_EQ(frame.pass_timings_dropped, 3U);
}

CY_TEST_CASE("stage time and memory report per category and in total") {
    FrameStatistics frame;
    frame.reset();
    frame.stage_milliseconds[static_cast<u32>(FrameStage::Extract)] = 0.5F;
    frame.stage_milliseconds[static_cast<u32>(FrameStage::Cull)] = 1.5F;
    frame.memory_bytes[static_cast<u32>(MemoryCategory::Textures)] = 1024;
    frame.memory_bytes[static_cast<u32>(MemoryCategory::Meshes)] = 512;

    CY_CHECK_NEAR(frame.total_cpu_milliseconds(), 2.0F, 1e-6F);
    CY_CHECK_EQ(frame.total_memory_bytes(), 1536ULL);

    // Every stage and category has a name, so a report iterates rather than naming seven fields.
    for (u32 stage = 0; stage < kFrameStageCount; ++stage) {
        CY_CHECK(frame_stage_name(static_cast<FrameStage>(stage))[0] != '\0');
    }
    for (u32 category = 0; category < kMemoryCategoryCount; ++category) {
        CY_CHECK(memory_category_name(static_cast<MemoryCategory>(category))[0] != '\0');
    }
}

CY_TEST_CASE("resetting a frame keeps its index and forgets everything else") {
    FrameStatistics frame;
    frame.frame_index = 41;
    frame.draw_calls = 900;
    frame.add_pass_timing("stale", 1, true);
    ++frame.frame_index;
    frame.reset();

    CY_CHECK_EQ(frame.frame_index, 42ULL);
    CY_CHECK_EQ(frame.draw_calls, 0U);
    CY_CHECK_EQ(frame.pass_timing_count, 0U);
}

CY_TEST_CASE("the server's report names the scene whose occupancy it carries") {
    RenderServer server(allocator());
    RenderServerConfig config;
    config.debug_primitive_capacity = 8;
    config.debug_label_capacity = 2;
    CY_REQUIRE(server.configure(config).has_value());
    CY_REQUIRE(server.initialize().has_value());

    SceneDescription scene_desc;
    scene_desc.name = cy::Name::intern("report");
    scene_desc.instance_capacity = 16;
    const auto scene = server.create_scene(scene_desc);
    CY_REQUIRE(scene.has_value());

    server.refresh_statistics(*scene);
    CY_CHECK_EQ(server.frame_statistics().gpu_scene_capacity, 16U);

    // A null handle is a legal argument and means "resources only": a host that has no scene yet
    // still wants the memory report.
    server.refresh_statistics(SceneHandle{});
    CY_CHECK_EQ(server.frame_statistics().gpu_scene_capacity, 0U);
}

CY_TEST_CASE("configuring a server after it started is refused rather than half-applied") {
    RenderServer server(allocator());
    RenderServerConfig config;
    config.debug_primitive_capacity = 8;
    config.debug_label_capacity = 2;
    CY_REQUIRE(server.configure(config).has_value());
    CY_REQUIRE(server.initialize().has_value());
    const auto refused = server.configure(RenderServerConfig{});
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().code == cy::ErrorCode::PermissionDenied);
}
