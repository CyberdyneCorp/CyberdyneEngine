// The camera bridge: a sequence's shot becomes a camera stack contribution, and the blend is the
// stack's own.
//
// THE CASE THIS FILE EXISTS FOR is "a cut drives the camera stack and writes no transform". It is
// asserted three ways, because the criterion is the milestone's:
//
//   1. The contribution is in `CameraStack`, with the priority and the blend the section declared.
//   2. `EvaluatedCamera::pose_overridden` is FALSE at every instant of the cut — the one field that
//      says a pose was written directly, and the bridge never touches the call that sets it.
//   3. Mid-blend, the blended camera is between the two shots and equal to neither, which is what a
//      blend actually is and what a test asserting "two contributions exist" would miss.

#include "fixture.h"

#include <cy/sequencing/camera/bridge.h>
#include <cy/sequencing/player.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::sequencing;
using namespace cy::sequencing::testing;

namespace {

constexpr u32 kCameraBinding = 11;
constexpr u64 kWideRig = 9001;
constexpr u64 kLongRig = 9002;

[[nodiscard]] cy::camera::RigNodeDesc node(const char* id, const char* input,
                                           cy::camera::RigNodeKind kind) noexcept {
    cy::camera::RigNodeDesc desc;
    desc.id = Name::intern(id);
    desc.input = (input == nullptr) ? Name{} : Name::intern(input);
    desc.kind = kind;
    return desc;
}

/// A rig that sits at a fixed offset from what it frames, with a declared lens and no smoothing —
/// so a case can assert positions rather than trends.
[[nodiscard]] cy::camera::RigDefinition shot_rig(Allocator& memory, const char* name, f32 distance,
                                                 f32 fov) noexcept {
    cy::camera::RigDefinition definition(memory);
    definition.name = Name::intern(name);
    (void)definition.nodes.push_back(node("target", nullptr, cy::camera::RigNodeKind::Target));
    cy::camera::RigNodeDesc follow = node("follow", "target", cy::camera::RigNodeKind::Follow);
    follow.follow.space = cy::camera::FollowSpace::World;
    follow.follow.offset = Vec3{0.0F, 1.0F, distance};
    follow.follow.position_half_life = 0.0F;
    (void)definition.nodes.push_back(follow);
    cy::camera::RigNodeDesc look = node("look", "follow", cy::camera::RigNodeKind::LookAt);
    look.look_at.rotation_half_life = 0.0F;
    (void)definition.nodes.push_back(look);
    cy::camera::RigNodeDesc lens = node("lens", "look", cy::camera::RigNodeKind::Lens);
    lens.lens.near_value = fov;
    lens.lens.far_value = fov;
    lens.lens.half_life = 0.0F;
    (void)definition.nodes.push_back(lens);
    (void)definition.nodes.push_back(node("output", "lens", cy::camera::RigNodeKind::Output));
    return definition;
}

/// A two-shot cinematic: a wide shot, then a long lens over it with a one-second blend.
[[nodiscard]] SequenceSource two_shot_sequence() noexcept {
    SequenceSource source(allocator());
    source.name = Name::intern("two shots");
    source.stable_id = 1;
    source.rate = Rate{24, 1};
    source.duration = SequenceTime::from_frame(96);
    (void)source.bindings.push_back(binding_of(kCameraBinding, "camera", BindingKind::Camera));

    Track shot = track_of("shot", TrackKind::Camera, 300, kCameraBinding);
    Section wide = section_over(0, 48, 3000);
    wide.priority = 100;
    wide.blend_in_seconds = 0.0F;
    wide.blend_out_seconds = 1.0F;
    (void)shot.sections.push_back(std::move(wide));
    (void)source.tracks.push_back(std::move(shot));
    return source;
}

struct CameraFixture {
    CameraFixture() noexcept : server(allocator()), bridge(allocator(), server) {}

    [[nodiscard]] Status build() noexcept {
        if (Status configured = server.configure(cy::camera::CameraServerConfig{}); !configured) {
            return configured;
        }
        if (Status started = server.initialize(); !started) {
            return started;
        }
        const Expected<cy::camera::StackHandle, Error> handle = server.create_stack();
        if (!handle) {
            return Status{make_unexpected(handle.error())};
        }
        stack = handle.value();
        return bridge.initialize(stack);
    }

    [[nodiscard]] Expected<cy::camera::RigHandle, Error> make_rig(const char* name, f32 distance,
                                                                  f32 fov) noexcept {
        const Expected<cy::camera::DefinitionHandle, Error> definition =
            server.create_definition(shot_rig(allocator(), name, distance, fov));
        if (!definition) {
            return make_unexpected(definition.error());
        }
        cy::camera::RigConfig config;
        config.name = Name::intern(name);
        Expected<cy::camera::RigHandle, Error> rig = server.create_rig(definition.value(), config);
        if (!rig) {
            return make_unexpected(rig.error());
        }
        cy::camera::TargetBinding target;
        target.kind = cy::camera::TargetKind::Entity;
        target.stable_id = 1;
        if (Status bound = server.set_target(rig.value(), target); !bound) {
            return make_unexpected(bound.error());
        }
        return rig;
    }

    cy::camera::CameraServer server;
    cy::camera::StackHandle stack;
    CameraStackBridge bridge;
};

[[nodiscard]] cy::camera::EvaluationContext context_with(const cy::camera::TargetSample& sample,
                                                         f32 delta) noexcept {
    cy::camera::EvaluationContext context;
    context.delta_seconds = delta;
    context.targets = Span<const cy::camera::TargetSample>(&sample, 1);
    context.aspect = 16.0F / 9.0F;
    return context;
}

[[nodiscard]] cy::camera::TargetSample subject_at(Vec3 position) noexcept {
    cy::camera::TargetSample sample;
    sample.stable_id = 1;
    sample.transform = Transform::from_translation(position);
    sample.valid = true;
    return sample;
}

}  // namespace

CY_TEST_CASE("sequence_camera: a shot becomes a stack contribution with its declared blend") {
    CameraFixture fixture;
    CY_REQUIRE(fixture.build());
    const Expected<cy::camera::RigHandle, Error> wide = fixture.make_rig("wide", 8.0F, 1.2F);
    CY_REQUIRE(wide);
    CY_REQUIRE(fixture.bridge.bind_rig(kWideRig, wide.value()));

    CameraRequest request;
    request.binding = kCameraBinding;
    request.rig = kWideRig;
    request.priority = 100;
    request.weight = 1.0F;
    request.blend_in.duration_seconds = 1.0F;
    request.blend_in.curve = 0;  // linear

    CameraBridgeReport report;
    CY_REQUIRE(fixture.bridge.apply(Span<const CameraRequest>(&request, 1), report));
    CY_CHECK_EQ(report.pushed, 1U);

    cy::camera::CameraStack* stack = fixture.server.stack(fixture.stack);
    CY_REQUIRE(stack != nullptr);
    CY_REQUIRE_EQ(stack->size(), 1U);
    CY_CHECK_EQ(stack->entry_at(0).kind, cy::camera::ContributionKind::Cinematic);
    CY_CHECK_EQ(stack->entry_at(0).priority, 100);
    CY_CHECK_NEAR(stack->entry_at(0).blend_in.duration_seconds, 1.0F, 1e-5F);
    CY_CHECK_EQ(stack->entry_at(0).blend_in.curve, cy::camera::BlendCurve::Linear);

    // Applying the same request again does not push a second contribution: a shot is one entry for
    // as long as it is on screen.
    CY_REQUIRE(fixture.bridge.apply(Span<const CameraRequest>(&request, 1), report));
    CY_CHECK_EQ(stack->size(), 1U);

    // And releasing it blends it out rather than removing it.
    CameraRequest release = request;
    release.release = true;
    CY_REQUIRE(fixture.bridge.apply(Span<const CameraRequest>(&release, 1), report));
    CY_CHECK_EQ(report.released, 1U);
    CY_CHECK_EQ(stack->size(), 1U);  // still there, blending out
    stack->advance(2.0F);
    CY_CHECK_EQ(stack->size(), 0U);
}

CY_TEST_CASE("sequence_camera: an unbound rig is counted, not guessed at") {
    CameraFixture fixture;
    CY_REQUIRE(fixture.build());
    CameraRequest request;
    request.binding = kCameraBinding;
    request.rig = 12345;  // never bound
    CameraBridgeReport report;
    CY_REQUIRE(fixture.bridge.apply(Span<const CameraRequest>(&request, 1), report));
    CY_CHECK_EQ(report.unresolved_rigs, 1U);
    CY_CHECK_EQ(report.pushed, 0U);
}

CY_TEST_CASE("sequence_camera: a cut is raised on the rig, and an anticipated one leads it") {
    CameraFixture fixture;
    CY_REQUIRE(fixture.build());
    const Expected<cy::camera::RigHandle, Error> wide = fixture.make_rig("wide", 8.0F, 1.2F);
    CY_REQUIRE(wide);
    CY_REQUIRE(fixture.bridge.bind_rig(kWideRig, wide.value()));

    CameraRequest announcement;
    announcement.binding = kCameraBinding;
    announcement.rig = kWideRig;
    announcement.cut = true;
    announcement.anticipated = true;
    announcement.cut_lead_seconds = 2.0F;
    CameraBridgeReport report;
    CY_REQUIRE(fixture.bridge.apply(Span<const CameraRequest>(&announcement, 1), report));
    CY_CHECK_EQ(report.anticipated_cuts, 1U);

    const cy::camera::TargetSample sample = subject_at(Vec3{});
    const Expected<const cy::camera::EvaluatedCamera*, Error> evaluated =
        fixture.server.evaluate(wide.value(), context_with(sample, 1.0F / 60.0F));
    CY_REQUIRE(evaluated);
    // The cut reached the camera: its history is invalidated and the deadline is on the streaming
    // source, which is how residency learns about it.
    CY_CHECK_EQ(evaluated.value()->last_cut.reason, cy::camera::CutReason::CinematicStart);
    CY_CHECK(evaluated.value()->last_cut.anticipated);
    CY_CHECK(evaluated.value()->streaming.cut_pending);
    CY_CHECK_NEAR(evaluated.value()->streaming.cut_lead_seconds, 2.0F, 1e-5F);
}

CY_TEST_CASE("sequence_camera: the cut blends, and no transform is ever written") {
    // The milestone's exit criterion, mid-blend rather than at either end.
    CameraFixture fixture;
    CY_REQUIRE(fixture.build());
    const Expected<cy::camera::RigHandle, Error> wide = fixture.make_rig("wide", 12.0F, 1.2F);
    const Expected<cy::camera::RigHandle, Error> tight = fixture.make_rig("tight", 3.0F, 0.5F);
    CY_REQUIRE(wide);
    CY_REQUIRE(tight);
    CY_REQUIRE(fixture.bridge.bind_rig(kWideRig, wide.value()));
    CY_REQUIRE(fixture.bridge.bind_rig(kLongRig, tight.value()));

    CameraBridgeReport report;
    CameraRequest first;
    first.binding = kCameraBinding;
    first.rig = kWideRig;
    first.priority = 100;
    first.blend_in.duration_seconds = 0.0F;
    // The OUTGOING shot's blend out is what keeps it in the stack while the incoming one arrives.
    // At the default half second it would be gone by the incoming shot's midpoint, and the "blend"
    // would be a jump with two entries in it for one frame.
    first.blend_out.duration_seconds = 2.0F;
    first.blend_out.curve = 0;
    CY_REQUIRE(fixture.bridge.apply(Span<const CameraRequest>(&first, 1), report));

    const cy::camera::TargetSample sample = subject_at(Vec3{});
    cy::camera::EvaluatedCamera resolved;
    Array<cy::camera::StackContribution> contributions(allocator());
    CY_REQUIRE(fixture.server.evaluate_stack(fixture.stack, context_with(sample, 0.0F), resolved,
                                             &contributions));
    const f32 wide_z = resolved.pose.translation.z;
    const f32 wide_fov = resolved.lens.vertical_fov_radians();

    // The second shot arrives with a one-second blend.
    CameraRequest second;
    second.binding = kCameraBinding;
    second.rig = kLongRig;
    second.priority = 100;
    second.blend_in.duration_seconds = 1.0F;
    second.blend_in.curve = 0;  // linear, so the halfway point is exactly halfway
    CY_REQUIRE(fixture.bridge.apply(Span<const CameraRequest>(&second, 1), report));
    CY_CHECK_EQ(report.pushed, 2U);

    // Half a second in: BETWEEN the two shots and equal to neither.
    CY_REQUIRE(fixture.server.evaluate_stack(fixture.stack, context_with(sample, 0.5F), resolved,
                                             &contributions));
    const f32 mid_z = resolved.pose.translation.z;
    const f32 mid_fov = resolved.lens.vertical_fov_radians();
    CY_CHECK_LT(mid_z, wide_z);
    CY_CHECK_GT(mid_z, 3.0F);
    CY_CHECK_LT(mid_fov, wide_fov);
    CY_CHECK_GT(mid_fov, 0.5F);
    CY_CHECK_FALSE(resolved.pose_overridden);

    // The contribution report attributes it: two contributions, and the incoming one at half.
    CY_REQUIRE_EQ(contributions.size(), 2U);
    CY_CHECK_NEAR(contributions[1].weight, 0.5F, 0.05F);

    // A second later the blend has finished on the tight shot, and still nothing wrote a pose.
    CY_REQUIRE(fixture.server.evaluate_stack(fixture.stack, context_with(sample, 0.5F), resolved,
                                             &contributions));
    CY_CHECK_NEAR(resolved.pose.translation.z, 3.0F, 0.05F);
    CY_CHECK_FALSE(resolved.pose_overridden);
}

CY_TEST_CASE("sequence_camera: a player drives the stack end to end") {
    // The whole path: an authored sequence, compiled, played, arbitrated, applied — and the camera
    // stack ends up with the shot the timeline declared.
    Registry registry(allocator());
    CY_REQUIRE(registry.build());
    Program program(allocator());
    CompileReport report(allocator());
    SequenceCompiler compiler(allocator(), registry.registry);
    CY_REQUIRE(compiler.compile(two_shot_sequence(), CompileOptions{}, program, report));

    CameraFixture fixture;
    CY_REQUIRE(fixture.build());
    const Expected<cy::camera::RigHandle, Error> wide = fixture.make_rig("wide", 8.0F, 1.2F);
    CY_REQUIRE(wide);
    CY_REQUIRE(fixture.bridge.bind_rig(kWideRig, wide.value()));

    SequencePlayer player(allocator(), registry.registry, PlaybackScope::LocalPlayer, 0);
    const BindingResolution bindings[] = {BindingResolution{kCameraBinding, kWideRig, true}};
    PlayRequest request;
    request.bindings = bindings;
    const Expected<InstanceId, Error> instance = player.create(program, request);
    CY_REQUIRE(instance);
    CY_REQUIRE(player.play(instance.value()));

    DispatchBatches batches(allocator());
    ArbitrationReport arbitration(allocator());
    CameraBridgeReport bridge_report;
    for (i32 frame = 0; frame < 24; ++frame) {
        batches.clear();
        CY_REQUIRE(player.advance(41666667, batches));
        batches.sort();
        CY_REQUIRE(arbitrate(batches, arbitration));
        CY_REQUIRE(fixture.bridge.apply(arbitration.cameras.span(), bridge_report));
    }
    CY_CHECK_EQ(bridge_report.pushed, 1U);
    cy::camera::CameraStack* stack = fixture.server.stack(fixture.stack);
    CY_REQUIRE(stack != nullptr);
    CY_CHECK_EQ(stack->size(), 1U);

    // Past the section's end the shot is released and blends out.
    for (i32 frame = 0; frame < 30; ++frame) {
        batches.clear();
        CY_REQUIRE(player.advance(41666667, batches));
        batches.sort();
        CY_REQUIRE(arbitrate(batches, arbitration));
        CY_REQUIRE(fixture.bridge.apply(arbitration.cameras.span(), bridge_report));
    }
    CY_CHECK_EQ(bridge_report.released, 1U);
}
