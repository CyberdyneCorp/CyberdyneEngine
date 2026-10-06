// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/swift/overlay_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just generate-swift`, and `just generate-swift --check` fails when this file is stale.

import CyberdyneABI

/// The engine's exported interface table, with one Swift method per entry.
///
/// It is deliberately thin: every method is the imported C call and nothing else, so there is no
/// second model of the ABI anywhere in this package. The ergonomics — Swift `String`s, `Vec3`,
/// component accessors that return rather than fill an out-parameter — are `CyberdyneKit`'s, which
/// is hand-written on purpose (`swift-scripting` names it "the hand-written ergonomic layer").
///
/// `@unchecked Sendable` because it is a pointer to memory the engine owns for the process
/// lifetime and never mutates after `cy_get_interface` returns. The rule that actually governs
/// which thread may call these is in `CyberdyneKit/Concurrency.swift`, and it is stronger than
/// `Sendable`: engine mutation is confined to `@GameActor`.
public struct Interface: @unchecked Sendable {
    public let table: UnsafePointer<CyInterface>

    @inlinable
    public init(_ table: UnsafePointer<CyInterface>) {
        self.table = table
    }

    /// The table the engine actually exported. `native-abi`'s additive growth rule from this side:
    /// the engine may export MORE entries than this overlay knows, never fewer.
    @inlinable public var abiMajor: UInt32 { table.pointee.header.abi_major }
    @inlinable public var abiMinor: UInt32 { table.pointee.header.abi_minor }
    @inlinable public var abiPatch: UInt32 { table.pointee.header.abi_patch }
    @inlinable public var tableSize: UInt32 { table.pointee.header.table_size }

    /// The module's half of the version handshake, `native-abi`'s "Older engine, newer module".
    ///
    /// True when this overlay may call every entry it knows about. A module that gets `false` must
    /// return `false` from `cy_module_entry` — which the loader reports and survives — rather than
    /// calling into a table that stops short of what it was compiled against.
    @inlinable
    public var isCompatible: Bool {
        abiMajor == ABI.major && tableSize >= ABI.interfaceTableSize
    }

    /// Turn a failing `CyResult` into a thrown `CyberdyneError`, carrying the engine's message.
    ///
    /// The message is copied here rather than held: `get_last_error` documents its pointer as valid
    /// only until this thread's next failing call, so a `String` made later would read whatever
    /// failed since.
    @inlinable
    public func check(_ result: CyResult) throws {
        if result == CY_RESULT_OK { return }
        let status = Status(rawValue: Int32(result.rawValue)) ?? .unknown
        var message = ""
        if let text = table.pointee.get_last_error() {
            message = String(cString: text)
        }
        throw CyberdyneError.status(status, message: message)
    }

    @inlinable
    public func log(engine: CyEngine, severity: UInt32, message: UnsafePointer<CChar>?) {
        table.pointee.log(engine, severity, message)
    }

    @inlinable
    public func getLastError() -> UnsafePointer<CChar>? {
        table.pointee.get_last_error()
    }

    @inlinable
    public func getLastErrorCode() -> CyResult {
        table.pointee.get_last_error_code()
    }

    @inlinable
    public func setLastError(result: CyResult, message: UnsafePointer<CChar>?) {
        table.pointee.set_last_error(result, message)
    }

    @inlinable
    public func varMakeString(engine: CyEngine, utf8: UnsafePointer<CChar>?, length: UInt64) -> CyVar {
        table.pointee.var_make_string(engine, utf8, length)
    }

    @inlinable
    public func varMakeBytes(engine: CyEngine, data: UnsafeRawPointer?, size: UInt64) -> CyVar {
        table.pointee.var_make_bytes(engine, data, size)
    }

    @inlinable
    public func varClone(value: UnsafePointer<CyVar>?) -> CyVar {
        table.pointee.var_clone(value)
    }

    @inlinable
    public func varRelease(value: UnsafeMutablePointer<CyVar>?) {
        table.pointee.var_release(value)
    }

    @inlinable
    public func varLiveCount(engine: CyEngine) -> UInt64 {
        table.pointee.var_live_count(engine)
    }

    @inlinable
    public func engineWorld(engine: CyEngine) -> CyWorld? {
        table.pointee.engine_world(engine)
    }

    @inlinable
    public func worldCreateEntity(world: CyWorld) -> CyEntity {
        table.pointee.world_create_entity(world)
    }

    @inlinable
    public func worldDestroyEntity(world: CyWorld, entity: CyEntity) throws {
        try check(table.pointee.world_destroy_entity(world, entity))
    }

    @inlinable
    public func worldEntityAlive(world: CyWorld, entity: CyEntity) -> Bool {
        table.pointee.world_entity_alive(world, entity)
    }

    @inlinable
    public func worldEpoch(world: CyWorld) -> UInt64 {
        table.pointee.world_epoch(world)
    }

    @inlinable
    public func worldRegisterComponent(world: CyWorld, desc: UnsafePointer<CyComponentTypeDesc>?) -> CyComponentTypeId {
        table.pointee.world_register_component(world, desc)
    }

    @inlinable
    public func worldFindComponent(world: CyWorld, name: UnsafePointer<CChar>?) -> CyComponentTypeId {
        table.pointee.world_find_component(world, name)
    }

    @inlinable
    public func worldAddComponent(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, initial: UnsafeRawPointer?) throws {
        try check(table.pointee.world_add_component(world, entity, component, initial))
    }

    @inlinable
    public func worldRemoveComponent(world: CyWorld, entity: CyEntity, component: CyComponentTypeId) throws {
        try check(table.pointee.world_remove_component(world, entity, component))
    }

    @inlinable
    public func worldHasComponent(world: CyWorld, entity: CyEntity, component: CyComponentTypeId) -> Bool {
        table.pointee.world_has_component(world, entity, component)
    }

    @inlinable
    public func worldBorrowComponent(world: CyWorld, entity: CyEntity, component: CyComponentTypeId) -> CyBorrow {
        table.pointee.world_borrow_component(world, entity, component)
    }

    @inlinable
    public func borrowValid(world: CyWorld, borrow: CyBorrow) -> Bool {
        table.pointee.borrow_valid(world, borrow)
    }

    @inlinable
    public func componentGetVar(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<CyVar>?) throws {
        try check(table.pointee.component_get_var(world, entity, component, field, into))
    }

    @inlinable
    public func componentSetVar(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, value: UnsafePointer<CyVar>?) throws {
        try check(table.pointee.component_set_var(world, entity, component, field, value))
    }

    @inlinable
    public func componentGetF32(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<Float>?) throws {
        try check(table.pointee.component_get_f32(world, entity, component, field, into))
    }

    @inlinable
    public func componentSetF32(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, value: Float) throws {
        try check(table.pointee.component_set_f32(world, entity, component, field, value))
    }

    @inlinable
    public func componentGetVec3(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<Float>?) throws {
        try check(table.pointee.component_get_vec3(world, entity, component, field, into))
    }

    @inlinable
    public func componentSetVec3(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, xyz: UnsafePointer<Float>?) throws {
        try check(table.pointee.component_set_vec3(world, entity, component, field, xyz))
    }

    @inlinable
    public func registerBehaviour(engine: CyEngine, name: UnsafePointer<CChar>?, vtable: UnsafePointer<CyBehaviourVTable>?) -> CyBehaviourType? {
        table.pointee.register_behaviour(engine, name, vtable)
    }

    @inlinable
    public func findBehaviour(engine: CyEngine, name: UnsafePointer<CChar>?) -> CyBehaviourType? {
        table.pointee.find_behaviour(engine, name)
    }

    @inlinable
    public func behaviourGeneration(type: CyBehaviourType) -> UInt32 {
        table.pointee.behaviour_generation(type)
    }

    @inlinable
    public func worldComponentCount(world: CyWorld) -> UInt32 {
        table.pointee.world_component_count(world)
    }

    @inlinable
    public func worldComponentInfo(world: CyWorld, component: CyComponentTypeId, into: UnsafeMutablePointer<CyComponentInfo>?) throws {
        try check(table.pointee.world_component_info(world, component, into))
    }

    @inlinable
    public func worldComponentField(world: CyWorld, component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<CyFieldDesc>?) throws {
        try check(table.pointee.world_component_field(world, component, field, into))
    }

    @inlinable
    public func worldParent(world: CyWorld, entity: CyEntity) -> CyEntity {
        table.pointee.world_parent(world, entity)
    }

    @inlinable
    public func worldSetParent(world: CyWorld, child: CyEntity, parent: CyEntity) throws {
        try check(table.pointee.world_set_parent(world, child, parent))
    }

    @inlinable
    public func worldChildCount(world: CyWorld, entity: CyEntity) -> UInt32 {
        table.pointee.world_child_count(world, entity)
    }

    @inlinable
    public func worldChild(world: CyWorld, entity: CyEntity, index: UInt32) -> CyEntity {
        table.pointee.world_child(world, entity, index)
    }

    @inlinable
    public func worldChunks(world: CyWorld, component: CyComponentTypeId, into: UnsafeMutablePointer<CyChunk>?, capacity: UInt32, count: UnsafeMutablePointer<UInt32>?) throws {
        try check(table.pointee.world_chunks(world, component, into, capacity, count))
    }

    @inlinable
    public func serviceOpen(engine: CyEngine, into: UnsafeMutablePointer<CyServiceSession?>?) throws {
        try check(table.pointee.service_open(engine, into))
    }

    @inlinable
    public func serviceClose(engine: CyEngine, session: CyServiceSession) {
        table.pointee.service_close(engine, session)
    }

    @inlinable
    public func serviceSubmit(engine: CyEngine, session: CyServiceSession, request: UnsafePointer<CyServiceRequest>?) throws {
        try check(table.pointee.service_submit(engine, session, request))
    }

    @inlinable
    public func serviceCancel(engine: CyEngine, session: CyServiceSession, requestId: UInt64) throws {
        try check(table.pointee.service_cancel(engine, session, requestId))
    }

    @inlinable
    public func servicePoll(engine: CyEngine, session: CyServiceSession, event: UnsafeMutablePointer<CyServiceEvent>?, hasEvent: UnsafeMutablePointer<Bool>?) throws {
        try check(table.pointee.service_poll(engine, session, event, hasEvent))
    }

    @inlinable
    public func timeGet(engine: CyEngine, into: UnsafeMutablePointer<CyTime>?) throws {
        try check(table.pointee.time_get(engine, into))
    }

    @inlinable
    public func inputFindAction(engine: CyEngine, name: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyInputAction>?) throws {
        try check(table.pointee.input_find_action(engine, name, into))
    }

    @inlinable
    public func inputActionState(engine: CyEngine, user: UInt32, action: CyInputAction, into: UnsafeMutablePointer<CyInputActionState>?) throws {
        try check(table.pointee.input_action_state(engine, user, action, into))
    }

    @inlinable
    public func inputActionStateByName(engine: CyEngine, user: UInt32, name: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyInputActionState>?) throws {
        try check(table.pointee.input_action_state_by_name(engine, user, name, into))
    }

    @inlinable
    public func inputPointer(engine: CyEngine, user: UInt32, into: UnsafeMutablePointer<CyInputPointer>?) throws {
        try check(table.pointee.input_pointer(engine, user, into))
    }

    @inlinable
    public func inputModifiers(engine: CyEngine, user: UInt32, into: UnsafeMutablePointer<UInt32>?) throws {
        try check(table.pointee.input_modifiers(engine, user, into))
    }

    @inlinable
    public func inputFindContext(engine: CyEngine, name: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyInputContext>?) throws {
        try check(table.pointee.input_find_context(engine, name, into))
    }

    @inlinable
    public func inputPushContext(engine: CyEngine, user: UInt32, context: CyInputContext, priority: Int32) throws {
        try check(table.pointee.input_push_context(engine, user, context, priority))
    }

    @inlinable
    public func inputPopContext(engine: CyEngine, user: UInt32, context: CyInputContext) throws {
        try check(table.pointee.input_pop_context(engine, user, context))
    }

    @inlinable
    public func cameraActive(engine: CyEngine, into: UnsafeMutablePointer<CyCamera>?) throws {
        try check(table.pointee.camera_active(engine, into))
    }

    @inlinable
    public func cameraView(engine: CyEngine, camera: CyCamera, into: UnsafeMutablePointer<CyCameraView>?) throws {
        try check(table.pointee.camera_view(engine, camera, into))
    }

    @inlinable
    public func cameraScreenToRay(engine: CyEngine, camera: CyCamera, screen: UnsafePointer<Float>?, into: UnsafeMutablePointer<CyRay>?) throws {
        try check(table.pointee.camera_screen_to_ray(engine, camera, screen, into))
    }

    @inlinable
    public func cameraWorldToScreen(engine: CyEngine, camera: CyCamera, points: UnsafePointer<Float>?, count: UInt32, into: UnsafeMutablePointer<CyScreenPoint>?) throws {
        try check(table.pointee.camera_world_to_screen(engine, camera, points, count, into))
    }

    @inlinable
    public func cameraSetTarget(engine: CyEngine, camera: CyCamera, target: UnsafePointer<CyCameraTarget>?) throws {
        try check(table.pointee.camera_set_target(engine, camera, target))
    }

    @inlinable
    public func cameraSetPose(engine: CyEngine, camera: CyCamera, pose: UnsafePointer<CyPose>?) throws {
        try check(table.pointee.camera_set_pose(engine, camera, pose))
    }

    @inlinable
    public func cameraClearPose(engine: CyEngine, camera: CyCamera) throws {
        try check(table.pointee.camera_clear_pose(engine, camera))
    }

    @inlinable
    public func physicsRaycast(engine: CyEngine, ray: UnsafePointer<CyRay>?, filter: UnsafePointer<CyQueryFilter>?, into: UnsafeMutablePointer<CyPhysicsHit>?, hasHit: UnsafeMutablePointer<Bool>?) throws {
        try check(table.pointee.physics_raycast(engine, ray, filter, into, hasHit))
    }

    @inlinable
    public func physicsRaycastAll(engine: CyEngine, ray: UnsafePointer<CyRay>?, filter: UnsafePointer<CyQueryFilter>?, into: UnsafeMutablePointer<CyPhysicsHit>?, capacity: UInt32, count: UnsafeMutablePointer<UInt32>?) throws {
        try check(table.pointee.physics_raycast_all(engine, ray, filter, into, capacity, count))
    }

    @inlinable
    public func physicsShapeCast(engine: CyEngine, shape: UnsafePointer<CyShape>?, start: UnsafePointer<CyPose>?, direction: UnsafePointer<Float>?, maxDistance: Float, filter: UnsafePointer<CyQueryFilter>?, into: UnsafeMutablePointer<CyPhysicsHit>?, hasHit: UnsafeMutablePointer<Bool>?) throws {
        try check(table.pointee.physics_shape_cast(engine, shape, start, direction, maxDistance, filter, into, hasHit))
    }

    @inlinable
    public func physicsOverlap(engine: CyEngine, shape: UnsafePointer<CyShape>?, pose: UnsafePointer<CyPose>?, filter: UnsafePointer<CyQueryFilter>?, into: UnsafeMutablePointer<CyEntity>?, capacity: UInt32, count: UnsafeMutablePointer<UInt32>?) throws {
        try check(table.pointee.physics_overlap(engine, shape, pose, filter, into, capacity, count))
    }

    @inlinable
    public func navFindPath(engine: CyEngine, request: UnsafePointer<CyNavPathRequest>?, into: UnsafeMutablePointer<Float>?, capacity: UInt32, result: UnsafeMutablePointer<CyNavPathResult>?) throws {
        try check(table.pointee.nav_find_path(engine, request, into, capacity, result))
    }

    @inlinable
    public func navRequestPath(engine: CyEngine, request: UnsafePointer<CyNavPathRequest>?, into: UnsafeMutablePointer<CyNavQuery>?) throws {
        try check(table.pointee.nav_request_path(engine, request, into))
    }

    @inlinable
    public func navPollPath(engine: CyEngine, query: CyNavQuery, into: UnsafeMutablePointer<Float>?, capacity: UInt32, result: UnsafeMutablePointer<CyNavPathResult>?) throws {
        try check(table.pointee.nav_poll_path(engine, query, into, capacity, result))
    }

    @inlinable
    public func navCancelPath(engine: CyEngine, query: CyNavQuery) throws {
        try check(table.pointee.nav_cancel_path(engine, query))
    }

    @inlinable
    public func navAgentConfigure(engine: CyEngine, entity: CyEntity, params: UnsafePointer<CyNavAgentParams>?) throws {
        try check(table.pointee.nav_agent_configure(engine, entity, params))
    }

    @inlinable
    public func navAgentMoveTo(engine: CyEngine, entity: CyEntity, target: UnsafePointer<Float>?) throws {
        try check(table.pointee.nav_agent_move_to(engine, entity, target))
    }

    @inlinable
    public func navAgentStop(engine: CyEngine, entity: CyEntity) throws {
        try check(table.pointee.nav_agent_stop(engine, entity))
    }

    @inlinable
    public func navAgentState(engine: CyEngine, entity: CyEntity, into: UnsafeMutablePointer<CyNavAgentState>?) throws {
        try check(table.pointee.nav_agent_state(engine, entity, into))
    }

    @inlinable
    public func audioFindCue(engine: CyEngine, name: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyAudioCue>?) throws {
        try check(table.pointee.audio_find_cue(engine, name, into))
    }

    @inlinable
    public func audioPlay(engine: CyEngine, play: UnsafePointer<CyAudioPlay>?, voice: UnsafeMutablePointer<CyAudioVoice>?) throws {
        try check(table.pointee.audio_play(engine, play, voice))
    }

    @inlinable
    public func audioStop(engine: CyEngine, voice: CyAudioVoice, fadeOut: Float) throws {
        try check(table.pointee.audio_stop(engine, voice, fadeOut))
    }

    @inlinable
    public func audioVoicePlaying(engine: CyEngine, voice: CyAudioVoice) -> Bool {
        table.pointee.audio_voice_playing(engine, voice)
    }

    @inlinable
    public func audioFindBus(engine: CyEngine, name: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyAudioBus>?) throws {
        try check(table.pointee.audio_find_bus(engine, name, into))
    }

    @inlinable
    public func audioSetBusVolume(engine: CyEngine, bus: CyAudioBus, volume: Float, fade: Float) throws {
        try check(table.pointee.audio_set_bus_volume(engine, bus, volume, fade))
    }

    @inlinable
    public func spawnResolve(engine: CyEngine, asset: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyPrefab>?) throws {
        try check(table.pointee.spawn_resolve(engine, asset, into))
    }

    @inlinable
    public func spawnInstantiate(engine: CyEngine, prefab: CyPrefab, params: UnsafePointer<CySpawnParams>?, root: UnsafeMutablePointer<CyEntity>?) throws {
        try check(table.pointee.spawn_instantiate(engine, prefab, params, root))
    }

    @inlinable
    public func spawnInstantiateMany(engine: CyEngine, prefab: CyPrefab, parent: CyEntity, poses: UnsafePointer<CyPose>?, count: UInt32, roots: UnsafeMutablePointer<CyEntity>?) throws {
        try check(table.pointee.spawn_instantiate_many(engine, prefab, parent, poses, count, roots))
    }

    @inlinable
    public func spawnDestroy(engine: CyEngine, root: CyEntity) throws {
        try check(table.pointee.spawn_destroy(engine, root))
    }

    @inlinable
    public func vfxEffectParameterSet(engine: CyEngine, entity: CyEntity, emitter: UnsafePointer<CChar>?, parameter: UnsafePointer<CChar>?, value: UnsafePointer<CyVar>?) throws {
        try check(table.pointee.vfx_effect_parameter_set(engine, entity, emitter, parameter, value))
    }

    @inlinable
    public func vfxEffectParameterGet(engine: CyEngine, entity: CyEntity, emitter: UnsafePointer<CChar>?, parameter: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyVar>?) throws {
        try check(table.pointee.vfx_effect_parameter_get(engine, entity, emitter, parameter, into))
    }

    @inlinable
    public func registerSystem(engine: CyEngine, desc: UnsafePointer<CySystemDesc>?) throws {
        try check(table.pointee.register_system(engine, desc))
    }

    @inlinable
    public func nodeFind(engine: CyEngine, from: CyEntity, path: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyEntity>?) throws {
        try check(table.pointee.node_find(engine, from, path, into))
    }

    @inlinable
    public func physicsApplyForce(engine: CyEngine, entity: CyEntity, force: UnsafePointer<Float>?) throws {
        try check(table.pointee.physics_apply_force(engine, entity, force))
    }

    @inlinable
    public func physicsApplyImpulse(engine: CyEngine, entity: CyEntity, impulse: UnsafePointer<Float>?, point: UnsafePointer<Float>?) throws {
        try check(table.pointee.physics_apply_impulse(engine, entity, impulse, point))
    }

    @inlinable
    public func physicsApplyTorque(engine: CyEngine, entity: CyEntity, torque: UnsafePointer<Float>?) throws {
        try check(table.pointee.physics_apply_torque(engine, entity, torque))
    }

    @inlinable
    public func physicsSetVelocity(engine: CyEngine, entity: CyEntity, linear: UnsafePointer<Float>?, angular: UnsafePointer<Float>?) throws {
        try check(table.pointee.physics_set_velocity(engine, entity, linear, angular))
    }

    @inlinable
    public func physicsGetVelocity(engine: CyEngine, entity: CyEntity, linear: UnsafeMutablePointer<Float>?, angular: UnsafeMutablePointer<Float>?) throws {
        try check(table.pointee.physics_get_velocity(engine, entity, linear, angular))
    }

    @inlinable
    public func characterCreate(engine: CyEngine, entity: CyEntity, desc: UnsafePointer<CyCharacterDesc>?) throws {
        try check(table.pointee.character_create(engine, entity, desc))
    }

    @inlinable
    public func characterDestroy(engine: CyEngine, entity: CyEntity) throws {
        try check(table.pointee.character_destroy(engine, entity))
    }

    @inlinable
    public func characterMove(engine: CyEngine, entity: CyEntity, input: UnsafePointer<CyCharacterInput>?) throws {
        try check(table.pointee.character_move(engine, entity, input))
    }

    @inlinable
    public func characterState(engine: CyEngine, entity: CyEntity, into: UnsafeMutablePointer<CyCharacterState>?) throws {
        try check(table.pointee.character_state(engine, entity, into))
    }

    @inlinable
    public func uiRoot(engine: CyEngine, into: UnsafeMutablePointer<CyUiElement>?) throws {
        try check(table.pointee.ui_root(engine, into))
    }

    @inlinable
    public func uiCreate(engine: CyEngine, parent: CyUiElement, desc: UnsafePointer<CyUiElementDesc>?, into: UnsafeMutablePointer<CyUiElement>?) throws {
        try check(table.pointee.ui_create(engine, parent, desc, into))
    }

    @inlinable
    public func uiDestroy(engine: CyEngine, element: CyUiElement) throws {
        try check(table.pointee.ui_destroy(engine, element))
    }

    @inlinable
    public func uiSetLayout(engine: CyEngine, element: CyUiElement, layout: UnsafePointer<CyUiLayout>?) throws {
        try check(table.pointee.ui_set_layout(engine, element, layout))
    }

    @inlinable
    public func uiSetStyle(engine: CyEngine, element: CyUiElement, style: UnsafePointer<CyUiStyle>?) throws {
        try check(table.pointee.ui_set_style(engine, element, style))
    }

    @inlinable
    public func uiSetText(engine: CyEngine, element: CyUiElement, text: UnsafePointer<CChar>?, colour: UInt32, pixelScale: UInt32) throws {
        try check(table.pointee.ui_set_text(engine, element, text, colour, pixelScale))
    }

    @inlinable
    public func uiSetImage(engine: CyEngine, element: CyUiElement, page: UInt32, uv: UnsafePointer<Float>?) throws {
        try check(table.pointee.ui_set_image(engine, element, page, uv))
    }

    @inlinable
    public func uiSetProgress(engine: CyEngine, element: CyUiElement, value: Float) throws {
        try check(table.pointee.ui_set_progress(engine, element, value))
    }

    @inlinable
    public func uiSetVisibility(engine: CyEngine, element: CyUiElement, visibility: UInt32) throws {
        try check(table.pointee.ui_set_visibility(engine, element, visibility))
    }

    @inlinable
    public func uiSetOpacity(engine: CyEngine, element: CyUiElement, opacity: Float) throws {
        try check(table.pointee.ui_set_opacity(engine, element, opacity))
    }

    @inlinable
    public func uiElementRect(engine: CyEngine, element: CyUiElement, into: UnsafeMutablePointer<Float>?) throws {
        try check(table.pointee.ui_element_rect(engine, element, into))
    }

    @inlinable
    public func uiHitTest(engine: CyEngine, position: UnsafePointer<Float>?, into: UnsafeMutablePointer<CyUiElement>?) throws {
        try check(table.pointee.ui_hit_test(engine, position, into))
    }

    @inlinable
    public func uiFocus(engine: CyEngine, into: UnsafeMutablePointer<CyUiElement>?) throws {
        try check(table.pointee.ui_focus(engine, into))
    }

    @inlinable
    public func uiSetFocus(engine: CyEngine, element: CyUiElement) throws {
        try check(table.pointee.ui_set_focus(engine, element))
    }

    @inlinable
    public func animationAttach(engine: CyEngine, entity: CyEntity, desc: UnsafePointer<CyAnimatorDesc>?) throws {
        try check(table.pointee.animation_attach(engine, entity, desc))
    }

    @inlinable
    public func animationDetach(engine: CyEngine, entity: CyEntity) throws {
        try check(table.pointee.animation_detach(engine, entity))
    }

    @inlinable
    public func animationPlay(engine: CyEngine, entity: CyEntity, state: UnsafePointer<CChar>?, crossfade: Float) throws {
        try check(table.pointee.animation_play(engine, entity, state, crossfade))
    }

    @inlinable
    public func animationStop(engine: CyEngine, entity: CyEntity, blend: Float) throws {
        try check(table.pointee.animation_stop(engine, entity, blend))
    }

    @inlinable
    public func animationSetFloat(engine: CyEngine, entity: CyEntity, parameter: UnsafePointer<CChar>?, value: Float) throws {
        try check(table.pointee.animation_set_float(engine, entity, parameter, value))
    }

    @inlinable
    public func animationSetBool(engine: CyEngine, entity: CyEntity, parameter: UnsafePointer<CChar>?, value: Bool) throws {
        try check(table.pointee.animation_set_bool(engine, entity, parameter, value))
    }

    @inlinable
    public func animationFireTrigger(engine: CyEngine, entity: CyEntity, parameter: UnsafePointer<CChar>?) throws {
        try check(table.pointee.animation_fire_trigger(engine, entity, parameter))
    }

    @inlinable
    public func animationGetFloat(engine: CyEngine, entity: CyEntity, parameter: UnsafePointer<CChar>?, into: UnsafeMutablePointer<Float>?) throws {
        try check(table.pointee.animation_get_float(engine, entity, parameter, into))
    }

    @inlinable
    public func animationState(engine: CyEngine, entity: CyEntity, into: UnsafeMutablePointer<CyAnimatorState>?) throws {
        try check(table.pointee.animation_state(engine, entity, into))
    }

    @inlinable
    public func animationEvents(engine: CyEngine, into: UnsafeMutablePointer<CyAnimationEvent>?, capacity: UInt32, count: UnsafeMutablePointer<UInt32>?) throws {
        try check(table.pointee.animation_events(engine, into, capacity, count))
    }

    @inlinable
    public func animationRootMotion(engine: CyEngine, entity: CyEntity, into: UnsafeMutablePointer<CyRootMotion>?) throws {
        try check(table.pointee.animation_root_motion(engine, entity, into))
    }

    @inlinable
    public func animationTakeRootMotion(engine: CyEngine, entity: CyEntity, into: UnsafeMutablePointer<CyRootMotion>?) throws {
        try check(table.pointee.animation_take_root_motion(engine, entity, into))
    }

    @inlinable
    public func animationSetRootMotion(engine: CyEngine, entity: CyEntity, mode: UInt32) throws {
        try check(table.pointee.animation_set_root_motion(engine, entity, mode))
    }

    @inlinable
    public func animationJointPose(engine: CyEngine, entity: CyEntity, joint: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyPose>?) throws {
        try check(table.pointee.animation_joint_pose(engine, entity, joint, into))
    }
}
