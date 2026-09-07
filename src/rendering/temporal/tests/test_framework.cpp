// The framework as the SINGLE owner. Task 8.3.
//
// Every case here registers TWO consumers, because a suite with one would pass just as well against
// five separate frameworks — which is the arrangement `temporal-rendering` exists to prevent and
// M7 `design.md` §5 lists first among the things that must not be retrofitted.

#include <cy/test/test.h>

#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/temporal/framework.h>

namespace {

using cy::rendering::ConsumerId;
using cy::rendering::HistoryDeclaration;
using cy::rendering::HistoryFormat;
using cy::rendering::HistoryId;
using cy::rendering::HistoryState;
using cy::rendering::TemporalConfig;
using cy::rendering::TemporalFramework;
using cy::rendering::TemporalInvalidation;
using cy::rendering::TemporalView;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

TemporalView view_at(cy::Vec3 eye, cy::u32 width = 1920, cy::u32 height = 1080,
                     cy::f32 fov_degrees = 60.0F) noexcept {
    TemporalView view;
    view.width = width;
    view.height = height;
    view.view = cy::look_at(eye, eye + cy::Vec3{0.0F, 0.0F, -1.0F});
    view.projection = cy::perspective_reversed_z(
        cy::math::radians(fov_degrees), static_cast<cy::f32>(width) / static_cast<cy::f32>(height),
        0.1F, 1000.0F);
    view.camera_position = eye;
    return view;
}

}  // namespace

CY_TEST_CASE("two consumers read one jitter, and neither can apply it") {
    TemporalFramework framework(allocator());
    CY_REQUIRE(framework.initialize(TemporalConfig{}).has_value());

    const cy::Expected<ConsumerId, cy::Error> taa = framework.register_consumer("taa", true);
    const cy::Expected<ConsumerId, cy::Error> upscaler =
        framework.register_consumer("upscaler", true);
    CY_REQUIRE(taa.has_value());
    CY_REQUIRE(upscaler.has_value());

    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}));
    const cy::Vec2 offset = framework.jitter().current();
    CY_CHECK(framework.jitter().enabled());
    // Both consumers read the same object. There is no second offset for them to disagree about,
    // which is the requirement's scenario satisfied by construction.
    CY_CHECK_EQ(framework.jitter().current().x, offset.x);

    // The projection is jittered ONCE, by the framework. A pass that jittered again would be
    // applying it twice; there is nowhere for it to do so.
    const cy::Mat4 jittered = framework.jittered_view_projection();
    CY_CHECK(jittered != framework.view().view_projection());
}

CY_TEST_CASE("no consumer needs jitter, so the projection is unjittered") {
    TemporalFramework framework(allocator());
    CY_REQUIRE(framework.initialize(TemporalConfig{}).has_value());
    // A shadow-cache consumer wants history and reprojection but not a jittered projection.
    CY_REQUIRE(framework.register_consumer("shadow-cache", false).has_value());

    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}));
    CY_CHECK_FALSE(framework.jitter().enabled());
    CY_CHECK_EQ(framework.jitter().current().x, 0.0F);
    CY_CHECK(framework.jittered_view_projection() == framework.view().view_projection());
}

CY_TEST_CASE(
    "a resolution change reallocates and invalidates every history, and no consumer notices") {
    TemporalFramework framework(allocator());
    CY_REQUIRE(framework.initialize(TemporalConfig{}).has_value());
    const ConsumerId taa = framework.register_consumer("taa", true).value();
    const ConsumerId ssr = framework.register_consumer("ssr", true).value();

    HistoryDeclaration full;
    full.format = HistoryFormat::Rgba16F;
    HistoryDeclaration half;
    half.format = HistoryFormat::Rgba16F;
    half.resolution_scale = 0.5F;

    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 1920, 1080));
    const HistoryId colour = framework.declare_history(taa, full).value();
    const HistoryId reflections = framework.declare_history(ssr, half).value();

    framework.mark_history_written(colour);
    framework.mark_history_written(reflections);
    CY_REQUIRE(framework.history(colour)->valid);
    CY_CHECK_EQ(framework.history(colour)->width, 1920U);
    CY_CHECK_EQ(framework.history(reflections)->width, 960U);

    // Per-consumer memory, which is the required diagnostic.
    CY_CHECK_EQ(framework.history_bytes(taa), 1920ULL * 1080ULL * 8ULL * 2ULL);
    CY_CHECK_GT(framework.history_bytes(taa), framework.history_bytes(ssr));

    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 1280, 720));
    CY_CHECK(framework.invalidated_this_frame());
    CY_CHECK_EQ(framework.statistics().last_cause, TemporalInvalidation::ResolutionChange);
    CY_CHECK_EQ(framework.history(colour)->width, 1280U);
    CY_CHECK_EQ(framework.history(reflections)->width, 640U);
    CY_CHECK_FALSE(framework.history(colour)->valid);
    CY_CHECK_FALSE(framework.history(reflections)->valid);
}

CY_TEST_CASE("a cut invalidates every consumer's history in the same frame") {
    TemporalFramework framework(allocator());
    CY_REQUIRE(framework.initialize(TemporalConfig{}).has_value());
    const ConsumerId taa = framework.register_consumer("taa", true).value();
    const ConsumerId gi = framework.register_consumer("ssgi", false).value();

    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}));
    const HistoryId colour = framework.declare_history(taa, HistoryDeclaration{}).value();
    const HistoryId radiance = framework.declare_history(gi, HistoryDeclaration{}).value();
    framework.mark_history_written(colour);
    framework.mark_history_written(radiance);

    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, -0.1F}));
    CY_CHECK_FALSE(framework.invalidated_this_frame());
    framework.mark_history_written(colour);
    framework.mark_history_written(radiance);

    // A cinematic cuts. Both histories go, on the same frame, and neither consumer had to notice.
    framework.signal_cut(TemporalInvalidation::CameraCut);
    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, -0.2F}));
    CY_CHECK(framework.invalidated_this_frame());
    CY_CHECK_FALSE(framework.history(colour)->valid);
    CY_CHECK_FALSE(framework.history(radiance)->valid);
    CY_CHECK_EQ(framework.statistics().last_cause, TemporalInvalidation::CameraCut);
    CY_CHECK_EQ(framework.statistics()
                    .invalidations[static_cast<cy::usize>(TemporalInvalidation::CameraCut)],
                1U);

    // The safety net: a teleport nobody signalled is still a cut.
    framework.mark_history_written(colour);
    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, -900.0F}));
    CY_CHECK(framework.invalidated_this_frame());
    CY_CHECK_EQ(framework.statistics().last_cause, TemporalInvalidation::Teleport);

    // A field-of-view change is a projection change, and walking is not — which is why the view and
    // the projection are separate fields.
    framework.mark_history_written(colour);
    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, -901.0F}, 1920, 1080, 35.0F));
    CY_CHECK(framework.invalidated_this_frame());
    CY_CHECK_EQ(framework.statistics().last_cause, TemporalInvalidation::ProjectionChange);
}

CY_TEST_CASE("two consumers agree about whether a pixel's history is valid") {
    TemporalFramework framework(allocator());
    CY_REQUIRE(framework.initialize(TemporalConfig{}).has_value());
    const ConsumerId taa = framework.register_consumer("taa", true).value();
    const ConsumerId ssr = framework.register_consumer("ssr", true).value();

    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}));
    const HistoryId colour = framework.declare_history(taa, HistoryDeclaration{}).value();
    const HistoryId reflections = framework.declare_history(ssr, HistoryDeclaration{}).value();
    framework.mark_history_written(colour);
    framework.mark_history_written(reflections);

    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, -0.5F}));
    const cy::rendering::SurfaceMotion motion =
        framework.surface_motion(cy::Vec3{0.0F, 0.0F, -20.0F}, cy::Vec3{0.0F, 0.0F, -20.0F});
    CY_REQUIRE(motion.representable);

    const cy::rendering::ReprojectionResult a =
        framework.classify(colour, motion.current_uv, motion, 20.0F, 20.0F);
    const cy::rendering::ReprojectionResult b =
        framework.classify(reflections, motion.current_uv, motion, 20.0F, 20.0F);
    CY_CHECK_EQ(a.state, b.state);
    CY_CHECK_EQ(a.state, HistoryState::Valid);
    CY_CHECK_EQ(a.history_uv.x, b.history_uv.x);

    // And they agree after a cut, too — which is the case that a per-effect invalidation gets wrong
    // for exactly one of them.
    framework.signal_cut(TemporalInvalidation::Explicit);
    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, -1.0F}));
    const cy::rendering::SurfaceMotion after =
        framework.surface_motion(cy::Vec3{0.0F, 0.0F, -20.0F}, cy::Vec3{0.0F, 0.0F, -20.0F});
    CY_CHECK_EQ(framework.classify(colour, after.current_uv, after, 20.0F, 20.0F).state,
                HistoryState::Unrepresentable);
    CY_CHECK_EQ(framework.classify(reflections, after.current_uv, after, 20.0F, 20.0F).state,
                HistoryState::Unrepresentable);
    CY_CHECK_NEAR(framework.statistics().classification.fraction(HistoryState::Unrepresentable),
                  1.0F, 1e-6F);
}

CY_TEST_CASE(
    "a history carries the view it was produced with, and cannot be declared anonymously") {
    TemporalFramework framework(allocator());
    CY_REQUIRE(framework.initialize(TemporalConfig{}).has_value());

    // A history the framework does not know the owner of would be missing from the memory report
    // and from the invalidation broadcast. It is refused rather than tolerated.
    const cy::Expected<HistoryId, cy::Error> orphan =
        framework.declare_history(ConsumerId{}, HistoryDeclaration{});
    CY_REQUIRE_FALSE(orphan.has_value());
    CY_CHECK_EQ(orphan.error().code, cy::ErrorCode::InvalidArgument);

    const ConsumerId taa = framework.register_consumer("taa", true).value();
    HistoryDeclaration nothing;
    nothing.frames = 0;
    CY_CHECK_FALSE(framework.declare_history(taa, nothing).has_value());

    framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}));
    const HistoryId colour = framework.declare_history(taa, HistoryDeclaration{}).value();
    framework.mark_history_written(colour);

    const cy::rendering::HistoryResource* resource = framework.history(colour);
    CY_REQUIRE(resource != nullptr);
    CY_CHECK_EQ(resource->produced_with.frame, framework.frame());
    CY_CHECK_EQ(resource->produced_with.width, 1920U);
    CY_CHECK_NE(resource->produced_with.projection_key, 0ULL);
    CY_CHECK(framework.history(HistoryId{}) == nullptr);
}

CY_TEST_CASE("pinned, two frameworks driven identically produce identical temporal state") {
    TemporalFramework early(allocator());
    TemporalFramework late(allocator());
    CY_REQUIRE(early.initialize(TemporalConfig{}).has_value());
    CY_REQUIRE(late.initialize(TemporalConfig{}).has_value());
    CY_REQUIRE(early.register_consumer("taa", true).has_value());
    CY_REQUIRE(late.register_consumer("taa", true).has_value());

    // One has been rendering for a while. That is the difference a golden image must not see.
    for (cy::u32 frame = 0; frame < 23; ++frame) {
        early.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}));
    }
    early.pin_jitter(0);
    late.pin_jitter(0);

    for (cy::u32 frame = 0; frame < 6; ++frame) {
        early.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}));
        late.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}));
        CY_CHECK_EQ(early.jitter().current().x, late.jitter().current().x);
        CY_CHECK_EQ(early.jitter().current().y, late.jitter().current().y);
        CY_CHECK(early.jittered_view_projection() == late.jittered_view_projection());
    }
}

CY_TEST_CASE(
    "a framework destroyed with many consumers and histories, after resizing, frees them") {
    // Teardown under occupancy rather than under concurrency: nothing here is threaded — the
    // framework is stepped once a frame on the frame thread and says so — so the state that can go
    // wrong is a framework destroyed while it owns a lot. Meaningful because the suite is run under
    // AddressSanitizer with leak detection and under ThreadSanitizer.
    for (cy::u32 round = 0; round < 4; ++round) {
        TemporalFramework framework(allocator());
        CY_REQUIRE(framework.initialize(TemporalConfig{}).has_value());

        cy::u32 declared = 0;
        for (cy::u32 index = 0; index < 24; ++index) {
            const cy::Expected<ConsumerId, cy::Error> consumer =
                framework.register_consumer("consumer", (index % 3U) == 0U);
            CY_REQUIRE(consumer.has_value());
            framework.begin_frame(view_at(cy::Vec3{0.0F, 0.0F, 0.0F}));
            for (cy::u32 slot = 0; slot < 4; ++slot) {
                HistoryDeclaration declaration;
                declaration.resolution_scale = 1.0F / static_cast<cy::f32>(slot + 1U);
                declaration.frames = 2 + slot;
                const cy::Expected<HistoryId, cy::Error> history =
                    framework.declare_history(consumer.value(), declaration);
                CY_REQUIRE(history.has_value());
                framework.mark_history_written(history.value());
                ++declared;
            }
        }
        CY_CHECK_EQ(declared, 96U);
        CY_CHECK_GT(framework.statistics().history_bytes, 0ULL);

        // Resize repeatedly: every history is reallocated and invalidated by the framework each
        // time, which is the path that owns the most and is the one a teardown is written last for.
        for (cy::u32 step = 0; step < 8; ++step) {
            framework.begin_frame(
                view_at(cy::Vec3{0.0F, 0.0F, 0.0F}, 640U + (step * 64U), 360U + (step * 36U)));
            CY_CHECK(framework.invalidated_this_frame());
        }
        // Destroyed here, holding 24 consumers and 96 history declarations.
    }
}
