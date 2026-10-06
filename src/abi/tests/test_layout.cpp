// The ABI's layout, checked against the compiler rather than trusted. Task 2.1.
//
// `native-abi`: "WHEN a struct crosses the ABI THEN its layout SHALL be fixed-width, explicitly
// padded, and asserted with `static_assert(sizeof(...))` on both sides".
//
// cy/abi/cy_abi.h carries the sizes, so a module compiled by another compiler fails at its own
// compile. This file carries the *offsets*, and it carries them for one reason that the header
// cannot: `tools/abi/abi_describe.py` computes every offset itself, from the declarations alone,
// under the layout model the C ABI fixes — and the description it produces is what the Swift
// overlay and the Rust SDK are generated from. If the model and the compiler ever disagree, the
// overlays are generated against a struct that does not exist, and nothing else in the tree would
// notice. So the numbers below are the model's, written out, and the compiler is asked to agree.
//
// A failure here means one of two things and the fix differs: either a struct was edited and the
// description regenerated (in which case update these numbers with it), or the layout model in
// abi_describe.py is wrong on this platform (in which case the description is what must change).

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/abi/var.h>
#include <cy/test/test.h>

#include <cstddef>

CY_TEST_CASE("every ABI struct has the size the header declares") {
    // The header already static_asserts these; repeating them as runtime checks is what puts them
    // in the test report, so a reader of a passing run can see that the layout was checked at all.
    CY_CHECK_EQ(sizeof(CyVarPayload), 16U);
    CY_CHECK_EQ(sizeof(CyVar), 32U);
    CY_CHECK_EQ(sizeof(CyFieldDesc), 24U);
    CY_CHECK_EQ(sizeof(CyComponentTypeDesc), 32U);
    CY_CHECK_EQ(sizeof(CyBehaviourVTable), 112U);
    CY_CHECK_EQ(sizeof(CyBorrow), 16U);
    CY_CHECK_EQ(sizeof(CyServiceRequest), 40U);
    CY_CHECK_EQ(sizeof(CyServiceEvent), 40U);
    CY_CHECK_EQ(sizeof(CyInterfaceHeader), 16U);
    CY_CHECK_EQ(sizeof(CyModuleInit), 40U);
}

CY_TEST_CASE("every ABI struct has the offsets the description generator computes") {
    CY_CHECK_EQ(offsetof(CyVar, type), 0U);
    CY_CHECK_EQ(offsetof(CyVar, flags), 4U);
    CY_CHECK_EQ(offsetof(CyVar, length), 8U);
    CY_CHECK_EQ(offsetof(CyVar, payload), 16U);

    CY_CHECK_EQ(offsetof(CyFieldDesc, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyFieldDesc, type), 4U);
    CY_CHECK_EQ(offsetof(CyFieldDesc, offset), 8U);
    CY_CHECK_EQ(offsetof(CyFieldDesc, size), 12U);
    CY_CHECK_EQ(offsetof(CyFieldDesc, name), 16U);

    CY_CHECK_EQ(offsetof(CyComponentTypeDesc, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyComponentTypeDesc, field_count), 12U);
    CY_CHECK_EQ(offsetof(CyComponentTypeDesc, name), 16U);
    CY_CHECK_EQ(offsetof(CyComponentTypeDesc, fields), 24U);

    CY_CHECK_EQ(offsetof(CyBehaviourVTable, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, schema_version), 4U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, create), 8U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, user_data), 48U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, frame_update), 56U);  // appended at 1.3
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, enter_tree), 64U);    // appended at 1.5
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, ready), 72U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, enable), 80U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, disable), 88U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, exit_tree), 96U);
    CY_CHECK_EQ(offsetof(CyBehaviourVTable, ui_event), 104U);  // appended at 1.6

    CY_CHECK_EQ(offsetof(CyInterfaceHeader, table_size), 12U);
    CY_CHECK_EQ(offsetof(CyModuleInit, initialize), 16U);
    CY_CHECK_EQ(offsetof(CyBorrow, epoch), 8U);
    CY_CHECK_EQ(offsetof(CyServiceRequest, request_id), 8U);
    CY_CHECK_EQ(offsetof(CyServiceRequest, operation), 16U);
    CY_CHECK_EQ(offsetof(CyServiceEvent, request_id), 8U);
    CY_CHECK_EQ(offsetof(CyServiceEvent, payload), 24U);
}

CY_TEST_CASE("the interface table starts with its header, which is what makes growth readable") {
    // A module reads `table_size` before it reads anything else, so the header cannot move. If it
    // ever did, an older module would read a function pointer as a version number.
    CY_CHECK_EQ(offsetof(CyInterface, header), 0U);
    CY_CHECK_EQ(sizeof(CyInterface) % sizeof(void*), 0U);
}

CY_TEST_CASE("the ABI's types are the C types they claim to be") {
    // Fixed width, not "whatever the compiler chose". An `int` in this header would be an ABI that
    // changes shape between two targets that both compile it.
    CY_CHECK_EQ(sizeof(CyEntity), 8U);
    CY_CHECK_EQ(sizeof(CyComponentTypeId), 4U);
    CY_CHECK(static_cast<CyEntity>(CY_ENTITY_NULL) == 0U);

    // The handles are pointers to distinct incomplete types, so the compiler rejects passing one
    // where another is expected. That is checked by the fact that this file compiles at all, and
    // stated here so the property is not deleted by accident.
    CY_CHECK_EQ(sizeof(CyEngine), sizeof(void*));
    CY_CHECK_EQ(sizeof(CyWorld), sizeof(void*));
}

CY_TEST_CASE("the var blob header is the size var_release subtracts") {
    CY_CHECK_EQ(cy::abi::kVarBlobHeaderSize, 32U);
}

// `add-swift-game-api`. The claim above, for the structs ABI 1.3 appended: the overlays are
// generated from abi_describe.py's numbers, so the compiler has to agree with them. One case per
// service group.
CY_TEST_CASE(
    "the 1.3 the pose, the ray and the clock structs have the layout the description computes") {
    CY_CHECK_EQ(sizeof(CyPose), 28U);
    CY_CHECK_EQ(offsetof(CyPose, position), 0U);
    CY_CHECK_EQ(offsetof(CyPose, rotation), 12U);
    CY_CHECK_EQ(sizeof(CyRay), 28U);
    CY_CHECK_EQ(offsetof(CyRay, origin), 0U);
    CY_CHECK_EQ(offsetof(CyRay, direction), 12U);
    CY_CHECK_EQ(offsetof(CyRay, max_distance), 24U);
    CY_CHECK_EQ(sizeof(CyTime), 48U);
    CY_CHECK_EQ(offsetof(CyTime, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyTime, phase), 4U);
    CY_CHECK_EQ(offsetof(CyTime, tick), 8U);
    CY_CHECK_EQ(offsetof(CyTime, fixed_delta), 16U);
    CY_CHECK_EQ(offsetof(CyTime, frame_delta), 24U);
    CY_CHECK_EQ(offsetof(CyTime, interpolation), 32U);
    CY_CHECK_EQ(offsetof(CyTime, flags), 40U);
    CY_CHECK_EQ(offsetof(CyTime, reserved), 44U);
}

CY_TEST_CASE("the 1.3 input structs have the layout the description computes") {
    CY_CHECK_EQ(sizeof(CyInputActionState), 32U);
    CY_CHECK_EQ(offsetof(CyInputActionState, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyInputActionState, flags), 4U);
    CY_CHECK_EQ(offsetof(CyInputActionState, value), 8U);
    CY_CHECK_EQ(offsetof(CyInputActionState, press_count), 20U);
    CY_CHECK_EQ(offsetof(CyInputActionState, release_count), 22U);
    CY_CHECK_EQ(offsetof(CyInputActionState, tick), 24U);
    CY_CHECK_EQ(sizeof(CyInputPointer), 48U);
    CY_CHECK_EQ(offsetof(CyInputPointer, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyInputPointer, flags), 4U);
    CY_CHECK_EQ(offsetof(CyInputPointer, buttons), 8U);
    CY_CHECK_EQ(offsetof(CyInputPointer, buttons_pressed), 12U);
    CY_CHECK_EQ(offsetof(CyInputPointer, buttons_released), 16U);
    CY_CHECK_EQ(offsetof(CyInputPointer, position), 20U);
    CY_CHECK_EQ(offsetof(CyInputPointer, delta), 28U);
    CY_CHECK_EQ(offsetof(CyInputPointer, wheel), 36U);
    CY_CHECK_EQ(offsetof(CyInputPointer, reserved), 44U);
}

CY_TEST_CASE("the 1.3 camera structs have the layout the description computes") {
    CY_CHECK_EQ(sizeof(CyCameraView), 68U);
    CY_CHECK_EQ(offsetof(CyCameraView, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyCameraView, flags), 4U);
    CY_CHECK_EQ(offsetof(CyCameraView, pose), 8U);
    CY_CHECK_EQ(offsetof(CyCameraView, vertical_fov), 36U);
    CY_CHECK_EQ(offsetof(CyCameraView, ortho_height), 40U);
    CY_CHECK_EQ(offsetof(CyCameraView, near_plane), 44U);
    CY_CHECK_EQ(offsetof(CyCameraView, far_plane), 48U);
    CY_CHECK_EQ(offsetof(CyCameraView, viewport), 52U);
    CY_CHECK_EQ(sizeof(CyScreenPoint), 16U);
    CY_CHECK_EQ(offsetof(CyScreenPoint, position), 0U);
    CY_CHECK_EQ(offsetof(CyScreenPoint, depth), 8U);
    CY_CHECK_EQ(offsetof(CyScreenPoint, flags), 12U);
    CY_CHECK_EQ(sizeof(CyCameraTarget), 48U);
    CY_CHECK_EQ(offsetof(CyCameraTarget, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyCameraTarget, flags), 4U);
    CY_CHECK_EQ(offsetof(CyCameraTarget, entity), 8U);
    CY_CHECK_EQ(offsetof(CyCameraTarget, position), 16U);
    CY_CHECK_EQ(offsetof(CyCameraTarget, yaw), 28U);
    CY_CHECK_EQ(offsetof(CyCameraTarget, pitch), 32U);
    CY_CHECK_EQ(offsetof(CyCameraTarget, distance), 36U);
    CY_CHECK_EQ(offsetof(CyCameraTarget, blend_seconds), 40U);
    CY_CHECK_EQ(offsetof(CyCameraTarget, reserved), 44U);
}

CY_TEST_CASE("the 1.3 physics queries structs have the layout the description computes") {
    CY_CHECK_EQ(sizeof(CyShape), 24U);
    CY_CHECK_EQ(offsetof(CyShape, kind), 0U);
    CY_CHECK_EQ(offsetof(CyShape, radius), 4U);
    CY_CHECK_EQ(offsetof(CyShape, half_height), 8U);
    CY_CHECK_EQ(offsetof(CyShape, half_extents), 12U);
    CY_CHECK_EQ(sizeof(CyQueryFilter), 32U);
    CY_CHECK_EQ(offsetof(CyQueryFilter, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyQueryFilter, layer), 4U);
    CY_CHECK_EQ(offsetof(CyQueryFilter, mask), 8U);
    CY_CHECK_EQ(offsetof(CyQueryFilter, flags), 12U);
    CY_CHECK_EQ(offsetof(CyQueryFilter, ignore), 16U);
    CY_CHECK_EQ(offsetof(CyQueryFilter, ignore_count), 24U);
    CY_CHECK_EQ(offsetof(CyQueryFilter, reserved), 28U);
    CY_CHECK_EQ(sizeof(CyPhysicsHit), 48U);
    CY_CHECK_EQ(offsetof(CyPhysicsHit, flags), 0U);
    CY_CHECK_EQ(offsetof(CyPhysicsHit, reserved), 4U);
    CY_CHECK_EQ(offsetof(CyPhysicsHit, entity), 8U);
    CY_CHECK_EQ(offsetof(CyPhysicsHit, point), 16U);
    CY_CHECK_EQ(offsetof(CyPhysicsHit, normal), 28U);
    CY_CHECK_EQ(offsetof(CyPhysicsHit, distance), 40U);
    CY_CHECK_EQ(offsetof(CyPhysicsHit, fraction), 44U);
}

CY_TEST_CASE("the 1.3 navigation structs have the layout the description computes") {
    CY_CHECK_EQ(sizeof(CyNavPathRequest), 64U);
    CY_CHECK_EQ(offsetof(CyNavPathRequest, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyNavPathRequest, world), 4U);
    CY_CHECK_EQ(offsetof(CyNavPathRequest, start), 8U);
    CY_CHECK_EQ(offsetof(CyNavPathRequest, end), 20U);
    CY_CHECK_EQ(offsetof(CyNavPathRequest, extents), 32U);
    CY_CHECK_EQ(offsetof(CyNavPathRequest, node_budget), 44U);
    CY_CHECK_EQ(offsetof(CyNavPathRequest, area_mask), 48U);
    CY_CHECK_EQ(offsetof(CyNavPathRequest, capabilities), 56U);
    CY_CHECK_EQ(sizeof(CyNavPathResult), 24U);
    CY_CHECK_EQ(offsetof(CyNavPathResult, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyNavPathResult, flags), 4U);
    CY_CHECK_EQ(offsetof(CyNavPathResult, point_count), 8U);
    CY_CHECK_EQ(offsetof(CyNavPathResult, state), 12U);
    CY_CHECK_EQ(offsetof(CyNavPathResult, cost), 16U);
    CY_CHECK_EQ(offsetof(CyNavPathResult, length), 20U);
    CY_CHECK_EQ(sizeof(CyNavAgentParams), 48U);
    CY_CHECK_EQ(offsetof(CyNavAgentParams, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyNavAgentParams, world), 4U);
    CY_CHECK_EQ(offsetof(CyNavAgentParams, radius), 8U);
    CY_CHECK_EQ(offsetof(CyNavAgentParams, height), 12U);
    CY_CHECK_EQ(offsetof(CyNavAgentParams, max_speed), 16U);
    CY_CHECK_EQ(offsetof(CyNavAgentParams, max_acceleration), 20U);
    CY_CHECK_EQ(offsetof(CyNavAgentParams, arrival_distance), 24U);
    CY_CHECK_EQ(offsetof(CyNavAgentParams, priority), 28U);
    CY_CHECK_EQ(offsetof(CyNavAgentParams, area_mask), 32U);
    CY_CHECK_EQ(offsetof(CyNavAgentParams, capabilities), 40U);
    CY_CHECK_EQ(sizeof(CyNavAgentState), 56U);
    CY_CHECK_EQ(offsetof(CyNavAgentState, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyNavAgentState, status), 4U);
    CY_CHECK_EQ(offsetof(CyNavAgentState, flags), 8U);
    CY_CHECK_EQ(offsetof(CyNavAgentState, reserved), 12U);
    CY_CHECK_EQ(offsetof(CyNavAgentState, position), 16U);
    CY_CHECK_EQ(offsetof(CyNavAgentState, velocity), 28U);
    CY_CHECK_EQ(offsetof(CyNavAgentState, target), 40U);
    CY_CHECK_EQ(offsetof(CyNavAgentState, remaining_distance), 52U);
}

CY_TEST_CASE("the 1.3 audio and spawning structs have the layout the description computes") {
    CY_CHECK_EQ(sizeof(CyAudioPlay), 56U);
    CY_CHECK_EQ(offsetof(CyAudioPlay, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CyAudioPlay, flags), 4U);
    CY_CHECK_EQ(offsetof(CyAudioPlay, cue), 8U);
    CY_CHECK_EQ(offsetof(CyAudioPlay, attach_to), 16U);
    CY_CHECK_EQ(offsetof(CyAudioPlay, bus), 24U);
    CY_CHECK_EQ(offsetof(CyAudioPlay, position), 32U);
    CY_CHECK_EQ(offsetof(CyAudioPlay, volume), 44U);
    CY_CHECK_EQ(offsetof(CyAudioPlay, pitch), 48U);
    CY_CHECK_EQ(offsetof(CyAudioPlay, fade_in_seconds), 52U);
    CY_CHECK_EQ(sizeof(CySpawnParams), 56U);
    CY_CHECK_EQ(offsetof(CySpawnParams, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CySpawnParams, flags), 4U);
    CY_CHECK_EQ(offsetof(CySpawnParams, parent), 8U);
    CY_CHECK_EQ(offsetof(CySpawnParams, pose), 16U);
    CY_CHECK_EQ(offsetof(CySpawnParams, scale), 44U);
}

CY_TEST_CASE("the 1.3 service handles are integers, never addresses") {
    CY_CHECK_EQ(sizeof(CyInputAction), 4U);
    CY_CHECK_EQ(sizeof(CyInputContext), 8U);
    CY_CHECK_EQ(sizeof(CyCamera), 8U);
    CY_CHECK_EQ(sizeof(CyNavQuery), 8U);
    CY_CHECK_EQ(sizeof(CyAudioCue), 8U);
    CY_CHECK_EQ(sizeof(CyAudioBus), 8U);
    CY_CHECK_EQ(sizeof(CyAudioVoice), 8U);
    CY_CHECK_EQ(sizeof(CyPrefab), 8U);
}

// ABI 1.5: scheduled systems and character controllers.
CY_TEST_CASE("the 1.5 system structs have the layout the description computes") {
    CY_CHECK_EQ(sizeof(CySystemAccess), 8U);
    CY_CHECK_EQ(offsetof(CySystemAccess, component), 0U);
    CY_CHECK_EQ(offsetof(CySystemAccess, mode), 4U);
    CY_CHECK_EQ(sizeof(CySystemDesc), 48U);
    CY_CHECK_EQ(offsetof(CySystemDesc, struct_size), 0U);
    CY_CHECK_EQ(offsetof(CySystemDesc, stage), 4U);
    CY_CHECK_EQ(offsetof(CySystemDesc, name), 8U);
    CY_CHECK_EQ(offsetof(CySystemDesc, access), 16U);
    CY_CHECK_EQ(offsetof(CySystemDesc, access_count), 24U);
    CY_CHECK_EQ(offsetof(CySystemDesc, reserved), 28U);
    CY_CHECK_EQ(offsetof(CySystemDesc, run), 32U);
    CY_CHECK_EQ(offsetof(CySystemDesc, user_data), 40U);
}

CY_TEST_CASE("the 1.5 character structs have the layout the description computes") {
    CY_CHECK_EQ(sizeof(CyCharacterDesc), 76U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, flags), 4U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, radius), 8U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, height), 12U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, max_slope_radians), 16U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, step_offset), 20U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, skin_width), 24U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, gravity_scale), 28U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, mass), 32U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, push_force), 36U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, layer), 40U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, mask), 44U);
    CY_CHECK_EQ(offsetof(CyCharacterDesc, start), 48U);
    CY_CHECK_EQ(sizeof(CyCharacterInput), 24U);
    CY_CHECK_EQ(offsetof(CyCharacterInput, flags), 4U);
    CY_CHECK_EQ(offsetof(CyCharacterInput, desired_velocity), 8U);
    CY_CHECK_EQ(offsetof(CyCharacterInput, jump_speed), 20U);
    CY_CHECK_EQ(sizeof(CyCharacterState), 72U);
    CY_CHECK_EQ(offsetof(CyCharacterState, ground), 4U);
    CY_CHECK_EQ(offsetof(CyCharacterState, flags), 8U);
    CY_CHECK_EQ(offsetof(CyCharacterState, reserved), 12U);
    CY_CHECK_EQ(offsetof(CyCharacterState, ground_entity), 16U);
    CY_CHECK_EQ(offsetof(CyCharacterState, position), 24U);
    CY_CHECK_EQ(offsetof(CyCharacterState, velocity), 36U);
    CY_CHECK_EQ(offsetof(CyCharacterState, ground_normal), 48U);
    CY_CHECK_EQ(offsetof(CyCharacterState, platform_velocity), 60U);
}

// ABI 1.6: the runtime interface.
CY_TEST_CASE("the 1.6 interface structs have the layout the description computes") {
    CY_CHECK_EQ(sizeof(CyUiEvent), 40U);
    CY_CHECK_EQ(offsetof(CyUiEvent, kind), 4U);
    CY_CHECK_EQ(offsetof(CyUiEvent, element), 8U);
    CY_CHECK_EQ(offsetof(CyUiEvent, owner), 16U);
    CY_CHECK_EQ(offsetof(CyUiEvent, position), 24U);
    CY_CHECK_EQ(offsetof(CyUiEvent, button), 32U);
    CY_CHECK_EQ(offsetof(CyUiEvent, reserved), 36U);

    CY_CHECK_EQ(sizeof(CyUiElementDesc), 24U);
    CY_CHECK_EQ(offsetof(CyUiElementDesc, kind), 4U);
    CY_CHECK_EQ(offsetof(CyUiElementDesc, name), 8U);
    CY_CHECK_EQ(offsetof(CyUiElementDesc, owner), 16U);

    CY_CHECK_EQ(sizeof(CyUiLayout), 144U);
    CY_CHECK_EQ(offsetof(CyUiLayout, flags), 24U);
    CY_CHECK_EQ(offsetof(CyUiLayout, gap), 28U);
    CY_CHECK_EQ(offsetof(CyUiLayout, preferred), 32U);
    CY_CHECK_EQ(offsetof(CyUiLayout, maximum), 48U);
    CY_CHECK_EQ(offsetof(CyUiLayout, margin), 56U);
    CY_CHECK_EQ(offsetof(CyUiLayout, padding), 72U);
    CY_CHECK_EQ(offsetof(CyUiLayout, flex_grow), 88U);
    CY_CHECK_EQ(offsetof(CyUiLayout, aspect_ratio), 96U);
    CY_CHECK_EQ(offsetof(CyUiLayout, anchor_min), 100U);
    CY_CHECK_EQ(offsetof(CyUiLayout, offset_max), 124U);
    CY_CHECK_EQ(offsetof(CyUiLayout, grid_column), 132U);
    CY_CHECK_EQ(offsetof(CyUiLayout, grid_columns), 140U);
    CY_CHECK_EQ(offsetof(CyUiLayout, reserved), 142U);

    CY_CHECK_EQ(sizeof(CyUiStyle), 32U);
    CY_CHECK_EQ(offsetof(CyUiStyle, background), 8U);
    CY_CHECK_EQ(offsetof(CyUiStyle, accent), 16U);
    CY_CHECK_EQ(offsetof(CyUiStyle, border_width), 20U);
    CY_CHECK_EQ(offsetof(CyUiStyle, corner_radius), 24U);
}
