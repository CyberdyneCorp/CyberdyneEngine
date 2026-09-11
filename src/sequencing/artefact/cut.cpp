// The capture of a cut, mid-blend. M8.c section 3, and rule 14's "capture what you build".
//
// ================================================================================================
// WHAT THIS PROGRAM IS, AND WHAT IT IS CAREFUL NOT TO BE
// ================================================================================================
//
// It plays a real authored sequence: two shots on a camera track, a camera-cut track with a
// pre-roll, and a light track beside them. The sequence is compiled by `SequenceCompiler`, played
// by `SequencePlayer`, arbitrated by `arbitrate()`, and applied to a real
// `cy::camera::CameraServer` through `CameraStackBridge`. Every frame it prints THE CAMERA THE
// STACK PRODUCED — the blended pose, the blended lens, the view-projection matrix built by the
// renderer's own `render::Projection::matrix()`, and the stack's own contribution weights.
//
// `cut.py` draws the picture by projecting a handful of world points through those matrices. So the
// picture is drawn FROM the camera the engine produced: if the blend were wrong, or if a shot
// selected the wrong rig, or if something wrote a pose directly, the picture would be wrong.
//
// It is careful NOT to be a renderer. M8.b's artefact took its silhouettes from the sample's own
// table rather than from the mesh the frame resolved, and would have drawn a defect correctly; the
// answer here is that the only thing this program prints about the camera comes out of
// `evaluate_stack()`, and the scene points it prints are declared as the artefact's own — `cut.py`
// labels them as such on the image.

#include <cy/core/math/transform.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/sequencing/camera/bridge.h>
#include <cy/sequencing/compile.h>
#include <cy/sequencing/player.h>
#include <cy/servers/camera/server.h>

#include <cstdio>
#include <cstring>
#include <utility>

using namespace cy;              // NOLINT(google-build-using-namespace) — a capture program's main
using namespace cy::sequencing;  // NOLINT(google-build-using-namespace)

namespace {

// TWO CAMERA BINDINGS, one per shot. A shot list names a camera per shot, and that is also what
// keeps each shot's contribution in the stack its own: the bridge holds one entry per binding, so
// the outgoing shot blends out while the incoming one blends in, which is what a cut looks like.
constexpr u32 kCameraA = 11;
constexpr u32 kCameraB = 12;
constexpr u32 kSubjectBinding = 13;
constexpr u64 kWideRig = 9001;
constexpr u64 kTightRig = 9002;
constexpr u32 kViewportWidth = 960;
constexpr u32 kViewportHeight = 540;

/// The frame the two shots overlap on. The sequence below cuts at frame 48 with a one-second blend
/// at 24 frames per second, so the midpoint is frame 60.
constexpr i64 kCutFrame = 48;
constexpr i64 kFrames = 96;

[[nodiscard]] Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

[[nodiscard]] cy::camera::RigNodeDesc node(const char* id, const char* input,
                                           cy::camera::RigNodeKind kind) noexcept {
    cy::camera::RigNodeDesc desc;
    desc.id = Name::intern(id);
    desc.input = (input == nullptr) ? Name{} : Name::intern(input);
    desc.kind = kind;
    return desc;
}

/// A shot: a rig that frames its target from a declared distance and height, through a declared
/// lens. Two of these, and the difference between them is what the blend has to travel.
[[nodiscard]] cy::camera::RigDefinition shot_rig(const char* name, Vec3 offset,
                                                 f32 fov_radians) noexcept {
    cy::camera::RigDefinition definition(allocator());
    definition.name = Name::intern(name);
    (void)definition.nodes.push_back(node("target", nullptr, cy::camera::RigNodeKind::Target));
    cy::camera::RigNodeDesc follow = node("follow", "target", cy::camera::RigNodeKind::Follow);
    follow.follow.space = cy::camera::FollowSpace::World;
    follow.follow.offset = offset;
    follow.follow.position_half_life = 0.0F;
    (void)definition.nodes.push_back(follow);
    cy::camera::RigNodeDesc look = node("look", "follow", cy::camera::RigNodeKind::LookAt);
    look.look_at.rotation_half_life = 0.0F;
    (void)definition.nodes.push_back(look);
    cy::camera::RigNodeDesc lens = node("lens", "look", cy::camera::RigNodeKind::Lens);
    lens.lens.near_value = fov_radians;
    lens.lens.far_value = fov_radians;
    lens.lens.half_life = 0.0F;
    (void)definition.nodes.push_back(lens);
    (void)definition.nodes.push_back(node("output", "lens", cy::camera::RigNodeKind::Output));
    return definition;
}

[[nodiscard]] Channel scalar_channel(const char* property, u32 stable_id) noexcept {
    Channel channel(allocator());
    channel.property = Name::intern(property);
    channel.type = ChannelType::Scalar;
    channel.stable_id = stable_id;
    return channel;
}

/// The cinematic: a wide shot, a long-lens shot over it with a one-second blend, the cut announced
/// two seconds before it happens, and a light that rises through the whole thing.
[[nodiscard]] SequenceSource authored_cut() noexcept {
    SequenceSource source(allocator());
    source.name = Name::intern("the cut");
    source.stable_id = 1;
    source.rate = Rate{24, 1};
    source.domain = ClockDomain::Presentation;
    source.duration = SequenceTime::from_frame(kFrames);
    source.accessibility.camera_motion_intensity = 0.4F;

    BindingDeclaration camera_a;
    camera_a.stable_id = kCameraA;
    camera_a.name = Name::intern("camera.wide");
    camera_a.kind = BindingKind::Camera;
    (void)source.bindings.push_back(camera_a);
    BindingDeclaration camera_b;
    camera_b.stable_id = kCameraB;
    camera_b.name = Name::intern("camera.tight");
    camera_b.kind = BindingKind::Camera;
    (void)source.bindings.push_back(camera_b);
    BindingDeclaration subject;
    subject.stable_id = kSubjectBinding;
    subject.name = Name::intern("subject");
    subject.kind = BindingKind::Entity;
    (void)source.bindings.push_back(subject);

    Track wide_track(allocator());
    wide_track.name = Name::intern("shot A — wide");
    wide_track.kind = TrackKind::Camera;
    wide_track.stable_id = 300;
    wide_track.binding = kCameraA;
    Section wide(allocator());
    wide.name = Name::intern("wide");
    wide.start = SequenceTime::from_frame(0);
    wide.end = SequenceTime::from_frame(kCutFrame);
    wide.priority = 100;
    wide.blend_in_seconds = 0.0F;
    // ONE SECOND OF BLEND OUT, LINEAR. The stack performs it; this section only declares it.
    wide.blend_out_seconds = 1.0F;
    wide.blend_curve = 0;
    wide.framing_binding = kSubjectBinding;
    wide.stable_id = 3000;
    (void)wide_track.sections.push_back(std::move(wide));
    (void)source.tracks.push_back(std::move(wide_track));

    Track tight_track(allocator());
    tight_track.name = Name::intern("shot B — long lens");
    tight_track.kind = TrackKind::Camera;
    tight_track.stable_id = 301;
    tight_track.binding = kCameraB;
    Section tight(allocator());
    tight.name = Name::intern("tight");
    tight.start = SequenceTime::from_frame(kCutFrame);
    tight.end = SequenceTime::from_frame(kFrames);
    tight.priority = 100;
    tight.blend_in_seconds = 1.0F;
    tight.blend_curve = 0;
    tight.blend_out_seconds = 0.5F;
    tight.framing_binding = kSubjectBinding;
    tight.stable_id = 3001;
    (void)tight_track.sections.push_back(std::move(tight));
    (void)source.tracks.push_back(std::move(tight_track));

    Track cut(allocator());
    cut.name = Name::intern("cut to the long lens");
    cut.kind = TrackKind::CameraCut;
    cut.stable_id = 400;
    cut.binding = kCameraB;
    Section moment(allocator());
    moment.name = Name::intern("cut");
    moment.start = SequenceTime::from_frame(kCutFrame);
    moment.end = SequenceTime::from_frame(kCutFrame + 1);
    moment.pre_roll = SequenceTime::from_frame(48);  // announced two seconds ahead
    moment.stable_id = 4000;
    (void)cut.sections.push_back(std::move(moment));
    (void)source.tracks.push_back(std::move(cut));

    Track light(allocator());
    light.name = Name::intern("key light");
    light.kind = TrackKind::Property;
    light.subsystem = SubsystemId::Light;
    light.stable_id = 100;
    light.binding = kSubjectBinding;
    Section lit(allocator());
    lit.name = Name::intern("rise");
    lit.start = SequenceTime::from_frame(0);
    lit.end = SequenceTime::from_frame(kFrames);
    lit.completion = CompletionPolicy::HoldFinal;
    lit.stable_id = 1000;
    Channel intensity = scalar_channel("intensity", 1);
    Key first;
    first.time = SequenceTime::from_frame(0);
    first.value[0] = 0.25F;
    (void)intensity.keys.push_back(first);
    Key last;
    last.time = SequenceTime::from_frame(kFrames);
    last.value[0] = 4.0F;
    (void)intensity.keys.push_back(last);
    (void)lit.channels.push_back(std::move(intensity));
    (void)light.sections.push_back(std::move(lit));
    (void)source.tracks.push_back(std::move(light));

    MarkerDeclaration marker;
    marker.time = SequenceTime::from_frame(kCutFrame);
    marker.name = Name::intern("cut");
    (void)source.markers.push_back(marker);
    return source;
}

[[nodiscard]] Status build_registry(AdapterRegistry& registry) noexcept {
    AdapterProperties light;
    light.name = Name::intern("light");
    light.subsystem = SubsystemId::Light;
    light.supports_capture_restore = false;
    const Expected<u32, Error> id = registry.register_adapter(light);
    if (!id) {
        return Status{make_unexpected(id.error())};
    }
    if (!registry.declare_property(id.value(), Name::intern("intensity"), ChannelType::Scalar)) {
        return fail(ErrorCode::Internal, "intensity");
    }
    AdapterProperties camera;
    camera.name = Name::intern("camera");
    camera.subsystem = SubsystemId::Camera;
    if (!registry.register_adapter(camera)) {
        return fail(ErrorCode::Internal, "camera adapter");
    }
    return ok();
}

void print_matrix(const char* prefix, const Mat4& matrix) noexcept {
    std::printf("%s", prefix);
    for (const Vec4& column : matrix.columns) {
        for (u32 row = 0; row < 4; ++row) {
            std::printf(" %.6f", static_cast<f64>(column[row]));
        }
    }
    std::printf("\n");
}

/// The scene the picture is drawn against. THE ARTEFACT'S OWN, and labelled as such on the image:
/// this milestone's section owns a camera, not a renderer, so what is honest to claim is that the
/// points are projected by the engine's camera and not that the engine drew them.
struct ScenePoint {
    Vec3 position;
    const char* kind = "";
};

constexpr ScenePoint kScene[] = {
    {{0.0F, 0.0F, 0.0F}, "subject"},   {{0.0F, 1.8F, 0.0F}, "subject"},
    {{-4.0F, 0.0F, -3.0F}, "pillar"},  {{-4.0F, 3.0F, -3.0F}, "pillar"},
    {{4.0F, 0.0F, -3.0F}, "pillar"},   {{4.0F, 3.0F, -3.0F}, "pillar"},
    {{-7.0F, 0.0F, 4.0F}, "pillar"},   {{-7.0F, 3.0F, 4.0F}, "pillar"},
    {{7.0F, 0.0F, 4.0F}, "pillar"},    {{7.0F, 3.0F, 4.0F}, "pillar"},
    {{-10.0F, 0.0F, -10.0F}, "floor"}, {{10.0F, 0.0F, -10.0F}, "floor"},
    {{10.0F, 0.0F, 10.0F}, "floor"},   {{-10.0F, 0.0F, 10.0F}, "floor"},
};

}  // namespace

int main() {
    AdapterRegistry registry(allocator());
    if (!build_registry(registry)) {
        std::fprintf(stderr, "the adapter registry did not build\n");
        return 1;
    }

    Program program(allocator());
    CompileReport report(allocator());
    {
        SequenceCompiler compiler(allocator(), registry);
        const SequenceSource source = authored_cut();
        if (Status compiled = compiler.compile(source, CompileOptions{}, program, report);
            !compiled) {
            std::fprintf(stderr, "the sequence did not compile: %u error(s)\n", report.errors);
            for (const Diagnostic& diagnostic : report.diagnostics.span()) {
                std::fprintf(stderr, "  %s: %s\n", diagnostic_code_name(diagnostic.code),
                             diagnostic.message);
            }
            return 1;
        }
    }
    std::printf("segments = %u\n", report.segments);
    std::printf("channels = %u\n", report.channels);
    std::printf("markers = %u\n", report.markers);
    std::printf("bucket_count = %u\n", program.bucket_count());

    cy::camera::CameraServer server(allocator());
    if (!server.configure(cy::camera::CameraServerConfig{}) || !server.initialize()) {
        std::fprintf(stderr, "the camera server did not start\n");
        return 1;
    }
    const Expected<cy::camera::StackHandle, Error> stack = server.create_stack();
    if (!stack) {
        std::fprintf(stderr, "no camera stack\n");
        return 1;
    }
    CameraStackBridge bridge(allocator(), server);
    if (!bridge.initialize(stack.value())) {
        std::fprintf(stderr, "the bridge did not initialize\n");
        return 1;
    }

    struct RigSetup {
        u64 identity = 0;
        const char* name = "";
        Vec3 offset;
        f32 fov = 1.0F;
    };
    cy::camera::RigHandle wide_handle;
    cy::camera::RigHandle tight_handle;
    const RigSetup setups[] = {{kWideRig, "wide", Vec3{6.0F, 4.0F, 14.0F}, 1.2F},
                               {kTightRig, "tight", Vec3{-1.5F, 1.7F, 4.0F}, 0.45F}};
    for (const RigSetup& setup : setups) {
        const Expected<cy::camera::DefinitionHandle, Error> definition =
            server.create_definition(shot_rig(setup.name, setup.offset, setup.fov));
        if (!definition) {
            std::fprintf(stderr, "rig %s did not compile\n", setup.name);
            return 1;
        }
        cy::camera::RigConfig config;
        config.name = Name::intern(setup.name);
        const Expected<cy::camera::RigHandle, Error> rig =
            server.create_rig(definition.value(), config);
        if (!rig) {
            std::fprintf(stderr, "rig %s was not created\n", setup.name);
            return 1;
        }
        if (!bridge.bind_rig(setup.identity, rig.value())) {
            return 1;
        }
        if (setup.identity == kWideRig) {
            wide_handle = rig.value();
        } else {
            tight_handle = rig.value();
        }
    }

    // ONE instance. Each shot names its own camera binding, so the wide rig and the tight rig are
    // both resolved up front and the timeline decides which of them is contributing when.
    SequencePlayer player(allocator(), registry, PlaybackScope::LocalPlayer, 0);
    const BindingResolution bindings[] = {BindingResolution{kCameraA, kWideRig, true},
                                          BindingResolution{kCameraB, kTightRig, true},
                                          BindingResolution{kSubjectBinding, 1, true}};
    PlayRequest request;
    request.bindings = bindings;
    const Expected<InstanceId, Error> instance = player.create(program, request);
    if (!instance) {
        std::fprintf(stderr, "the instance was not created\n");
        return 1;
    }
    if (!player.play(instance.value())) {
        std::fprintf(stderr, "playback did not start\n");
        return 1;
    }

    DispatchBatches batches(allocator());
    ArbitrationReport arbitration(allocator());
    CameraBridgeReport bridge_report;
    Array<cy::camera::StackContribution> contributions(allocator());
    const cy::camera::TargetSample subject = [] {
        cy::camera::TargetSample sample;
        sample.stable_id = 1;
        sample.transform = Transform::from_translation(Vec3{0.0F, 0.0F, 0.0F});
        sample.valid = true;
        return sample;
    }();

    std::printf("viewport = %u %u\n", kViewportWidth, kViewportHeight);
    for (const ScenePoint& point : kScene) {
        std::printf("point = %.3f %.3f %.3f %s\n", static_cast<f64>(point.position.x),
                    static_cast<f64>(point.position.y), static_cast<f64>(point.position.z),
                    point.kind);
    }

    const f32 aspect = static_cast<f32>(kViewportWidth) / static_cast<f32>(kViewportHeight);
    u32 blend_frames = 0;
    u32 pose_overrides = 0;
    f32 light_at_end = 0.0F;
    for (i64 frame = 0; frame < kFrames + 12; ++frame) {
        batches.clear();
        if (!player.advance(41666667, batches)) {
            std::fprintf(stderr, "advance failed at frame %lld\n", static_cast<long long>(frame));
            return 1;
        }
        batches.sort();
        if (!arbitrate(batches, arbitration)) {
            return 1;
        }
        if (!bridge.apply(arbitration.cameras.span(), bridge_report)) {
            return 1;
        }
        for (const ResolvedValue& value : arbitration.values.span()) {
            if (value.subsystem == SubsystemId::Light) {
                light_at_end = value.value.components[0];
            }
        }

        cy::camera::EvaluationContext context;
        context.delta_seconds = 1.0F / 24.0F;
        context.targets = Span<const cy::camera::TargetSample>(&subject, 1);
        context.aspect = aspect;
        // AN EMPTY STACK IS THE END OF THE CINEMATIC, NOT A FAILURE. `evaluate_stack` advances the
        // blends first and then refuses to blend nothing, so the frame on which the last
        // contribution finishes blending out comes back as `NotFound` — which is the camera
        // system's own contract and not a defect. The capture stops there; anything else that fails
        // is still a failure.
        cy::camera::EvaluatedCamera resolved;
        if (!server.evaluate_stack(stack.value(), context, resolved, &contributions)) {
            const cy::camera::CameraStack* remaining = server.stack(stack.value());
            if (remaining != nullptr && remaining->empty()) {
                break;
            }
            std::fprintf(stderr, "the stack did not evaluate at frame %lld\n",
                         static_cast<long long>(frame));
            return 1;
        }
        pose_overrides += resolved.pose_overridden ? 1U : 0U;

        // MATCHED BY RIG, not by position. Entries leave the stack when their blend out finishes,
        // so index 0 is the wide shot before the cut and the tight one after it — reading by index
        // would have reported the tight shot's weight as the wide shot's for the whole second half.
        f32 wide_weight = 0.0F;
        f32 tight_weight = 0.0F;
        for (const cy::camera::StackContribution& contribution : contributions.span()) {
            if (contribution.rig == wide_handle) {
                wide_weight = contribution.weight;
            } else if (contribution.rig == tight_handle) {
                tight_weight = contribution.weight;
            }
        }
        if (wide_weight > 0.01F && tight_weight > 0.01F) {
            ++blend_frames;
        }

        const Mat4 view = inverse(resolved.pose).to_matrix();
        const Mat4 projection = resolved.lens.to_projection().matrix(aspect);
        std::printf(
            "frame = %lld %.4f %.4f %.5f %.4f %.4f %.4f %u\n", static_cast<long long>(frame),
            static_cast<f64>(wide_weight), static_cast<f64>(tight_weight),
            static_cast<f64>(resolved.lens.vertical_fov_radians()),
            static_cast<f64>(resolved.pose.translation.x),
            static_cast<f64>(resolved.pose.translation.y),
            static_cast<f64>(resolved.pose.translation.z), static_cast<u32>(contributions.size()));
        print_matrix("viewproj =", projection * view);
    }

    std::printf("cut_frame = %lld\n", static_cast<long long>(kCutFrame));
    std::printf("blend_frames = %u\n", blend_frames);
    std::printf("pose_overrides = %u\n", pose_overrides);
    std::printf("pushed = %u\n", bridge_report.pushed);
    std::printf("released = %u\n", bridge_report.released);
    std::printf("cuts = %u\n", bridge_report.cuts);
    std::printf("anticipated_cuts = %u\n", bridge_report.anticipated_cuts);
    std::printf("framing_targets = %u\n", bridge_report.framing_targets_set);
    std::printf("unresolved_rigs = %u\n", bridge_report.unresolved_rigs);
    std::printf("light_final = %.4f\n", static_cast<f64>(light_at_end));
    return 0;
}
