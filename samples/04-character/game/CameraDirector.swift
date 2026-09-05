// CameraDirector.swift — the third-person camera, as a game decision. Task 5.1.
//
// `camera-system` separates four concepts, and the one this file is on the game's side of is
// INTENT: what should be framed, and where the player is looking from. Where that puts the camera
// is the camera server's answer, derived from a rig; this behaviour never writes a pose, because
// there is no `set_pose()` to write to and there should not be.
//
// A SECOND BEHAVIOUR RATHER THAN A SECOND METHOD ON `Character`. The camera outlives the character
// in every game that has a death screen, follows a drone in the mission that has one, and is
// authored by a different person. Keeping it a separate behaviour on the same entity is what makes
// `camera-system`'s "the target binding SHALL be independent of control" expressible here rather
// than merely true elsewhere.
//
// IT IS CREATED BEFORE `Character`, and the order matters exactly once: `Character` reads this
// tick's yaw to decide which way "forward" is. Behaviour instances are updated in creation order —
// `cy::abi::BehaviourRuntime` iterates its instance array — so creating the director first is what
// makes the character's frame the camera's current frame rather than last tick's. The host's
// bridge.cpp states the same order at the call site.

import CyberdyneKit

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

@Behaviour(name: "CameraDirector", schema: 1)
final class CameraDirector: Behaviour {
    // --- The framing, which is this game's and not the engine's ------------------------------------

    /// Radians of camera rotation per radian of look intent. The input layer has already applied the
    /// player's sensitivity and inversion; this is the game's own multiplier on top, which is why it
    /// is one by default and exists at all.
    @Export(range: 0 ... 4) var yawScale: Float = 1.0
    @Export(range: 0 ... 4) var pitchScale: Float = 1.0
    @Export(range: -1.5 ... 0) var minPitch: Float = -0.6
    @Export(range: 0 ... 1.5) var maxPitch: Float = 1.1

    /// How high above the character's origin the camera looks. Its head, not its feet.
    @Export(range: 0 ... 3) var focusHeight: Float = 1.5
    /// The rig's offset from the focus at zoom 0, in the rig's own frame.
    @Export var shoulderOffset: Vec3 = Vec3(x: 0.45, y: 0.2, z: 0)
    @Export(range: 0.5 ... 20) var nearDistance: Float = 3.2
    @Export(range: 0.5 ... 30) var farDistance: Float = 6.5
    @Export(range: 0 ... 1) var positionHalfLife: Float = 0.11
    @Export(range: 0 ... 1) var rotationHalfLife: Float = 0.06
    /// Vertical field of view in radians, at zoom 0 and at zoom 1. Pulling back also opens the lens
    /// slightly, which is the coherent zoom `camera-system` asks for rather than two controls.
    @Export(range: 0.3 ... 2) var nearFieldOfView: Float = 1.0472
    @Export(range: 0.3 ... 2) var farFieldOfView: Float = 1.1345

    /// How fast the camera pulls back as the character speeds up. Zoom is ONE normalised parameter
    /// and this is the game's mapping onto it — from standing still to this speed is 0 to 1.
    @Export(range: 0.5 ... 20) var zoomAtSpeed: Float = 7.6
    /// Seconds for the zoom to close half the gap to the speed it implies, so a sprint does not snap.
    @Export(range: 0 ... 2) var zoomHalfLife: Float = 0.35

    // --- Private state ------------------------------------------------------------------------------

    private var yaw: Float = 0
    private var pitch: Float = 0.12
    private var zoom: Float = 0

    // --- Resolved once --------------------------------------------------------------------------------

    private var input = ComponentType(id: 0)
    private var state = ComponentType(id: 0)
    private var intent = ComponentType(id: 0)

    private var inputLook: UInt32 = 0
    private var statePosition: UInt32 = 0
    private var stateSpeed: UInt32 = 0
    private var intentFocus: UInt32 = 0
    private var intentYaw: UInt32 = 0
    private var intentPitch: UInt32 = 0
    private var intentZoom: UInt32 = 0

    override func onCreate() throws {
        guard let world else { throw CyberdyneError.invalidHandle }
        let registry = Registry(world: world)

        input = try registry.type(PlayerInput.self)
        state = try registry.type(CharacterState.self)
        intent = try registry.type(CameraIntent.self)
        let spec = try registry.type(CameraSpec.self)

        inputLook = PlayerInput.field("look")
        statePosition = CharacterState.field("position")
        stateSpeed = CharacterState.field("speed")
        intentFocus = CameraIntent.field("focus")
        intentYaw = CameraIntent.field("yaw")
        intentPitch = CameraIntent.field("pitch")
        intentZoom = CameraIntent.field("zoom")

        for component in [input, state, intent, spec] where !world.has(component, on: entity) {
            try world.add(component, to: entity)
        }

        try world.setVec3(shoulderOffset, entity, spec, field: CameraSpec.field("offset"))
        try world.setFloat(nearDistance, entity, spec, field: CameraSpec.field("nearDistance"))
        try world.setFloat(farDistance, entity, spec, field: CameraSpec.field("farDistance"))
        try world.setFloat(positionHalfLife, entity, spec,
                           field: CameraSpec.field("positionHalfLife"))
        try world.setFloat(rotationHalfLife, entity, spec,
                           field: CameraSpec.field("rotationHalfLife"))
        try world.setFloat(nearFieldOfView, entity, spec,
                           field: CameraSpec.field("nearFieldOfView"))
        try world.setFloat(farFieldOfView, entity, spec, field: CameraSpec.field("farFieldOfView"))

        // Publish the opening frame before the first tick, so the character's first step is taken in
        // a camera frame that exists rather than in the identity one.
        try publish()
    }

    override func onFixedUpdate(_ delta: Double) throws {
        guard let world else { return }
        let step = Float(delta)

        let look = try world.vec3(entity, input, field: inputLook)
        yaw += look.x * yawScale
        pitch = min(max(pitch + look.y * pitchScale, minPitch), maxPitch)

        // Wrapped rather than left to grow: a yaw that has accumulated for an hour is a float whose
        // sine costs precision, and nothing downstream can tell the difference.
        let turn = Float.pi * 2
        if yaw > turn || yaw < -turn {
            yaw -= turn * (yaw / turn).rounded(.towardZero)
        }

        let speed = try world.float(entity, state, field: stateSpeed)
        let target = min(speed / zoomAtSpeed, 1)
        // A HALF-LIFE, NOT A LERP FACTOR. `camera-system`: two steps of dt must leave the same value
        // as one step of 2·dt, which `lerp(current, target, k)` does not — see the camera server's
        // smoothing suite, whose test for this is the composition property rather than a residue.
        zoom += (target - zoom) * (zoomHalfLife > 0 ? 1 - exp2f(-step / zoomHalfLife) : 1)

        try publish()
    }

    override func onAfterReload(restored: Set<String>) throws {
        // Yaw, pitch and zoom are private: a reload restarts the camera behind the character rather
        // than pretending to remember where the player was looking. Publishing immediately keeps the
        // first tick after a reload from running against a stale frame.
        try publish()
    }

    private func publish() throws {
        guard let world else { return }
        let position = try world.vec3(entity, state, field: statePosition)
        try world.setVec3(Vec3(x: position.x, y: position.y + focusHeight, z: position.z),
                          entity, intent, field: intentFocus)
        try world.setFloat(yaw, entity, intent, field: intentYaw)
        try world.setFloat(pitch, entity, intent, field: intentPitch)
        try world.setFloat(zoom, entity, intent, field: intentZoom)
    }
}
