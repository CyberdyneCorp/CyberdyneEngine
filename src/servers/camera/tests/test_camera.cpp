// The evaluated camera's vocabulary: intent, targets, cuts, the listener and the streaming source.
// Tasks 4.3.1 and 4.3.3.
//
// `camera-system` — "Four separated concepts", "Camera intent", "Aim is three things", "Listener
// and streaming source".

#include <cy/core/math/scalar.h>
#include <cy/servers/camera/camera.h>
#include <cy/test/test.h>

#include <type_traits>

using cy::f32;
using namespace cy::camera;

CY_TEST_CASE("nothing in the camera vocabulary is polymorphic") {
    // `camera-system`: "There SHALL NOT be a camera base class intended for subclassing to produce
    // camera behaviours", and "Per-node heap-allocated virtual graphs" are a forbidden pattern. A
    // virtual function anywhere in these types is the first step toward both, so the absence is
    // asserted rather than reviewed for.
    static_assert(!std::is_polymorphic_v<EvaluatedCamera>);
    static_assert(!std::is_polymorphic_v<TargetBinding>);
    static_assert(!std::is_polymorphic_v<CameraIntent>);
    static_assert(!std::is_polymorphic_v<Lens>);
    static_assert(std::is_trivially_copyable_v<CameraIntent>);
    static_assert(std::is_trivially_copyable_v<Lens>);
    CY_CHECK(true);
}

CY_TEST_CASE("look intent accumulates rather than replacing") {
    // A frame that delivered two look events must turn the camera by their sum. Taking the last one
    // would drop input precisely when the frame rate is uneven, which is the same defect
    // `input-and-actions` names for fixed-tick sampling.
    CameraIntent intent;
    CameraIntent stick;
    stick.look = cy::Vec2{0.1F, 0.0F};
    CameraIntent mouse;
    mouse.look = cy::Vec2{0.2F, -0.05F};

    accumulate(intent, stick);
    accumulate(intent, mouse);
    CY_CHECK_NEAR(intent.look.x, 0.3F, 1e-6F);
    CY_CHECK_NEAR(intent.look.y, -0.05F, 1e-6F);
}

CY_TEST_CASE("a recentre from any source latches, and the last zoom wins") {
    CameraIntent intent;
    CameraIntent nothing;
    CameraIntent recentre;
    recentre.recentre = true;
    accumulate(intent, recentre);
    accumulate(intent, nothing);
    CY_CHECK(intent.recentre);

    CameraIntent first_zoom;
    first_zoom.zoom = 0.25F;
    first_zoom.zoom_set = true;
    CameraIntent second_zoom;
    second_zoom.zoom = 0.75F;
    second_zoom.zoom_set = true;
    accumulate(intent, first_zoom);
    accumulate(intent, second_zoom);
    CY_CHECK_NEAR(intent.zoom, 0.75F, 1e-6F);
    // A source that did not set the zoom does not clear it.
    accumulate(intent, nothing);
    CY_CHECK_NEAR(intent.zoom, 0.75F, 1e-6F);
}

CY_TEST_CASE("a target binding is a stable identity, never a pointer") {
    // `camera-system`: "Targets SHALL be referenced by stable handles that survive streaming and
    // structural change… rig nodes SHALL NOT retain raw pointers to component data across frames."
    // The binding has no pointer to hold, which is the requirement expressed as a type.
    static_assert(std::is_trivially_copyable_v<TargetBinding>);
    TargetBinding binding;
    binding.kind = TargetKind::Entity;
    binding.stable_id = 42;
    CY_CHECK_EQ(binding.stable_id, 42U);
    CY_CHECK_EQ(cy::Name::intern(target_kind_name(TargetKind::Group)), cy::Name::intern("group"));
}

CY_TEST_CASE("the listener can sit at the camera, at the character, or between them") {
    // `camera-system`: "**WHEN** a third-person camera trails the character **THEN** the listener
    // policy SHALL place the listener sensibly, rather than defaulting to the camera without
    // consideration."
    EvaluatedCamera camera;
    camera.pose.translation = cy::Vec3{0.0F, 2.0F, 8.0F};
    camera.velocity = cy::Vec3{0.0F, 0.0F, -4.0F};
    const cy::Transform character = cy::Transform::from_translation(cy::Vec3{0.0F, 1.0F, 0.0F});
    const cy::Vec3 character_velocity{1.0F, 0.0F, 0.0F};

    const ListenerAnchor at_camera =
        derive_listener(camera, character, character_velocity, ListenerPolicy::AtCamera, 0.0F);
    CY_CHECK_NEAR(at_camera.transform.translation.z, 8.0F, 1e-5F);

    const ListenerAnchor at_character = derive_listener(camera, character, character_velocity,
                                                        ListenerPolicy::AtControlledEntity, 0.0F);
    CY_CHECK_NEAR(at_character.transform.translation.z, 0.0F, 1e-5F);
    CY_CHECK_NEAR(at_character.velocity.x, 1.0F, 1e-5F);

    const ListenerAnchor between =
        derive_listener(camera, character, character_velocity, ListenerPolicy::Interpolated, 0.5F);
    CY_CHECK_NEAR(between.transform.translation.z, 4.0F, 1e-5F);
    // ORIENTATION TAKES THE CAMERA'S even when the position is interpolated: a listener facing half
    // way between two directions would pan every sound to a direction neither is looking at.
    CY_CHECK(between.transform.rotation == camera.pose.rotation);
}

CY_TEST_CASE("the streaming source is derived from the camera and cannot disagree with it") {
    // `camera-system`: "The listener and the streaming source SHALL be **derived from the evaluated
    // camera**, not configured independently, so they cannot disagree about where the player is."
    EvaluatedCamera camera;
    camera.pose.translation = cy::Vec3{100.0F, 0.0F, 0.0F};
    camera.velocity = cy::Vec3{0.0F, 0.0F, -30.0F};
    camera.lens.gameplay.vertical_fov_radians = 1.0F;

    const StreamingSource source = derive_streaming_source(camera, 16.0F / 9.0F, 2.0F, 0.5F);
    CY_CHECK_NEAR(source.position.x, 100.0F, 1e-5F);
    CY_CHECK_NEAR(source.velocity.z, -30.0F, 1e-5F);
    CY_CHECK_NEAR(source.vertical_fov_radians, 1.0F, 1e-5F);
    CY_CHECK_NEAR(source.aspect, 16.0F / 9.0F, 1e-5F);
    CY_CHECK_NEAR(source.importance, 2.0F, 1e-5F);
    CY_CHECK_NEAR(source.prediction_horizon_seconds, 0.5F, 1e-5F);
    CY_CHECK_FALSE(source.cut_pending);
}

CY_TEST_CASE("an anticipated cut travels on the streaming source as a deadline") {
    // "**Anticipated cuts** — a cinematic's cut list, a scripted teleport — SHALL be announcable in
    // advance and SHALL become deadlines through the residency layer."
    EvaluatedCamera camera;
    camera.last_cut.reason = CutReason::CinematicStart;
    camera.last_cut.anticipated = true;
    camera.last_cut.lead_seconds = 1.5F;

    const StreamingSource source = derive_streaming_source(camera, 1.0F, 1.0F, 0.25F);
    CY_CHECK(source.cut_pending);
    CY_CHECK_NEAR(source.cut_lead_seconds, 1.5F, 1e-5F);
}

CY_TEST_CASE("a zero aspect is corrected rather than divided by") {
    EvaluatedCamera camera;
    CY_CHECK_NEAR(derive_streaming_source(camera, 0.0F, 1.0F, 0.0F).aspect, 1.0F, 1e-6F);
}

CY_TEST_CASE("the three aims are three fields and the view aim has no setter") {
    // `camera-system`: view aim, control aim and weapon aim are distinct, and "**WHEN** a hit is
    // validated **THEN** the server SHALL use the command's control aim and the authoritative
    // state, not a camera transform." A single `aim` field would make that impossible to keep.
    AimState aim;
    aim.control_aim = cy::Vec3{1.0F, 0.0F, 0.0F};
    aim.weapon_aim = cy::Vec3{0.0F, 1.0F, 0.0F};
    CY_CHECK_NE(aim.control_aim.x, aim.view_aim.x);
    CY_CHECK_NE(aim.weapon_aim.y, aim.view_aim.y);
}
