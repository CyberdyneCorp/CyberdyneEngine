// SPDX-License-Identifier: MIT
// FirstPerson.swift — ABI 1.13's first-person camera and weapon view, as a game writes them.
//
//     // onStart: the player's camera gets first-person behaviours and a weapon view.
//     let eye = try FirstPersonCamera(camera: player, settings: FirstPersonSettings())
//     try eye.setWeaponView(WeaponView(layers: 1 << 5))
//
//     // onUpdate: the mouse's counts, the walk, the sights.
//     try eye.feed(look: mouseDelta, speed: velocity.length, aiming: aimHeld, airborne: !grounded)
//     if fired { try eye.kick(pitch: 3.0, yaw: Float.random(in: -0.4...0.4)) }
//     let state = try eye.state()   // the eye's bob, the weapon's pose, both lenses, the aims
//
// The behaviours are the engine's `cy::camera::FirstPersonCamera`: mouse look with a sensitivity —
// applied HERE, once, so the look action carries no sensitivity processor — head bob by distance
// walked, recoil as a damped spring, and the sights blending the world's lens and the weapon
// view's. The weapon view draws the arms and the weapon (instances on `WeaponView.layers`) with
// their own field of view and near plane, in front of any wall they are pushed into. Everything is
// PRESENTATION: every call is refused in `onFixedUpdate` with
// `CyberdyneError.status(.permissionDenied, …)`. A camera with no behaviours throws `.notFound`.

import CyberdyneABI
import CyberdyneCore

/// A first-person camera's settings. The defaults are the engine's.
public struct FirstPersonSettings: Equatable, Sendable {
    /// Radians a mouse count.
    public var lookSensitivity: Float = 0.0022
    /// Its multiplier with the sights fully up.
    public var aimSensitivity: Float = 0.7
    public var invertY: Bool = false
    /// The pitch is clamped to plus or minus this, radians.
    public var pitchLimit: Float = 1.553_343
    /// The world's vertical field of view from the hip and with the sights up, radians.
    public var hipFieldOfView: Float = 1.308_997
    public var aimFieldOfView: Float = 0.959_931
    /// The weapon view's.
    public var hipWeaponFieldOfView: Float = 0.959_931
    public var aimWeaponFieldOfView: Float = 0.698_132
    /// Seconds the sights take to come half way.
    public var aimHalfLife: Float = 0.06
    /// Metres walked a full bob cycle, the bob's height and sway at the reference speed.
    public var bobStride: Float = 1.6
    public var bobVertical: Float = 0.03
    public var bobLateral: Float = 0.02
    public var bobReferenceSpeed: Float = 4.0
    /// The recoil spring: its frequency in hertz, its damping ratio, the view's share of it.
    public var recoilFrequency: Float = 6.0
    public var recoilDamping: Float = 0.75
    public var recoilCameraShare: Float = 0.6
    /// Accessibility: one leaves no bob and no view recoil.
    public var motionReduction: Float = 0
    /// The weapon in the eye's space (-Z ahead), from the hip and with the sights up.
    public var hipOffset = Vec3(x: 0.2, y: -0.2, z: -0.35)
    public var aimOffset = Vec3(x: 0, y: -0.07, z: -0.28)

    /// The engine's defaults.
    public init() {}

    /// In the ABI's spelling.
    public var raw: CyFirstPersonDesc {
        var out = CyFirstPersonDesc()
        out.struct_size = UInt32(MemoryLayout<CyFirstPersonDesc>.size)
        out.flags = invertY ? CY_FIRST_PERSON_INVERT_Y : 0
        out.look_sensitivity = lookSensitivity
        out.aim_sensitivity = aimSensitivity
        out.pitch_limit = pitchLimit
        out.hip_fov = hipFieldOfView
        out.aim_fov = aimFieldOfView
        out.hip_viewmodel_fov = hipWeaponFieldOfView
        out.aim_viewmodel_fov = aimWeaponFieldOfView
        out.aim_half_life = aimHalfLife
        out.bob_stride = bobStride
        out.bob_vertical = bobVertical
        out.bob_lateral = bobLateral
        out.bob_reference_speed = bobReferenceSpeed
        out.recoil_frequency = recoilFrequency
        out.recoil_damping = recoilDamping
        out.recoil_camera_share = recoilCameraShare
        out.motion_reduction = motionReduction
        out.hip_offset = hipOffset.tuple
        out.aim_offset = aimOffset.tuple
        return out
    }
}

/// A camera's weapon view: the layers the arms and the weapon are on, its lens and its shadows.
public struct WeaponView: Equatable, Sendable {
    /// The layers it draws, and the world view therefore does not. Zero turns it off.
    public var layers: UInt32
    /// Vertical, radians; nil follows the first-person camera's sights.
    public var fieldOfView: Float?
    public var nearPlane: Float
    public var farPlane: Float
    /// The slice of the depth range nearest the eye it takes.
    public var depthFraction: Float
    /// Shadowed by the world's shadow map where it is. On by default.
    public var receivesShadows: Bool
    /// Drawn into the world's shadow map from where it is. Off by default.
    public var castsShadows: Bool
    /// Takes the screen-space contact term too. Off by default.
    public var contactShadows: Bool

    /// Every field.
    public init(
        layers: UInt32, fieldOfView: Float? = nil, nearPlane: Float = 0.01, farPlane: Float = 10,
        depthFraction: Float = 0.0625, receivesShadows: Bool = true, castsShadows: Bool = false,
        contactShadows: Bool = false
    ) {
        self.layers = layers
        self.fieldOfView = fieldOfView
        self.nearPlane = nearPlane
        self.farPlane = farPlane
        self.depthFraction = depthFraction
        self.receivesShadows = receivesShadows
        self.castsShadows = castsShadows
        self.contactShadows = contactShadows
    }

    /// From the ABI's spelling: `fov` is the lens it draws with now.
    public init(_ raw: CyViewmodelDesc) {
        layers = raw.layers
        fieldOfView = raw.fov > 0 ? raw.fov : nil
        nearPlane = raw.near_plane
        farPlane = raw.far_plane
        depthFraction = raw.depth_fraction
        receivesShadows = raw.flags & CY_VIEWMODEL_IGNORE_SHADOWS == 0
        castsShadows = raw.flags & CY_VIEWMODEL_CAST_SHADOWS != 0
        contactShadows = raw.flags & CY_VIEWMODEL_CONTACT_SHADOWS != 0
    }

    /// In the ABI's spelling.
    public var raw: CyViewmodelDesc {
        var out = CyViewmodelDesc()
        out.struct_size = UInt32(MemoryLayout<CyViewmodelDesc>.size)
        out.layers = layers
        out.fov = fieldOfView ?? 0
        out.near_plane = nearPlane
        out.far_plane = farPlane
        out.depth_fraction = depthFraction
        out.flags = (receivesShadows ? 0 : CY_VIEWMODEL_IGNORE_SHADOWS)
            | (castsShadows ? CY_VIEWMODEL_CAST_SHADOWS : 0)
            | (contactShadows ? CY_VIEWMODEL_CONTACT_SHADOWS : 0)
        return out
    }
}

/// What a first-person camera's last frame produced.
public struct FirstPersonState: Equatable, Sendable {
    /// The look the player controls — the control aim — recoil excluded, radians.
    public var yaw: Float
    public var pitch: Float
    /// The recoil spring's displacement: pitch and yaw.
    public var recoil: Vec2
    /// 0 at the hip, 1 with the sights fully up.
    public var aim: Float
    /// The world's and the weapon view's vertical fields of view now.
    public var fieldOfView: Float
    public var weaponFieldOfView: Float
    /// The bob, in the eye's space.
    public var eyeOffset: Vec3
    /// The weapon in the eye's space.
    public var weapon: Pose
    /// Where the camera looks and where the weapon points, in the world.
    public var viewAim: Vec3
    public var weaponAim: Vec3

    /// From the ABI's spelling.
    public init(_ raw: CyFirstPersonState) {
        yaw = raw.yaw
        pitch = raw.pitch
        recoil = Vec2(raw.recoil)
        aim = raw.aim
        fieldOfView = raw.fov
        weaponFieldOfView = raw.viewmodel_fov
        eyeOffset = Vec3(raw.eye_offset)
        weapon = Pose(raw.weapon)
        viewAim = Vec3(raw.view_aim)
        weaponAim = Vec3(raw.weapon_aim)
    }
}

/// A camera with first-person behaviours, through ABI 1.13's `first_person_*` and `viewmodel_*`.
public struct FirstPersonCamera: Hashable, Sendable {
    /// The camera the behaviours belong to.
    public let camera: Camera

    /// Give `camera` first-person behaviours, or new settings keeping its state. Throws
    /// `.invalidArgument` for a setting out of range, `.outOfRange` with every slot taken.
    public init(camera: Camera, settings: FirstPersonSettings) throws {
        self.camera = camera
        try configure(settings)
    }

    /// A camera that was given behaviours elsewhere — by another module, or before a reload. Calls
    /// on one that has none throw `.notFound`.
    public init(attachedTo camera: Camera) {
        self.camera = camera
    }

    /// New settings — a sensitivity changed in the options — keeping the look and the rest.
    public func configure(_ settings: FirstPersonSettings) throws {
        var raw = settings.raw
        try GameServices.engine().firstPersonConfigure(camera: camera.raw, desc: &raw)
    }

    /// This frame's input: the mouse's counts (no sensitivity: it is applied once, here), the speed
    /// over the ground, the sights held, off the ground. Several calls in one frame add their look.
    public func feed(look: Vec2, speed: Float, aiming: Bool = false, airborne: Bool = false) throws {
        var raw = CyFirstPersonInput()
        raw.struct_size = UInt32(MemoryLayout<CyFirstPersonInput>.size)
        raw.look = look.tuple
        raw.move_speed = speed
        raw.flags = (aiming ? CY_FIRST_PERSON_AIMING : 0) | (airborne ? CY_FIRST_PERSON_AIRBORNE : 0)
        try GameServices.engine().firstPersonInput(camera: camera.raw, input: &raw)
    }

    /// A shot's kick into the recoil spring, radians a second up and right.
    public func kick(pitch: Float, yaw: Float = 0) throws {
        try GameServices.engine().firstPersonKick(camera: camera.raw, pitch: pitch, yaw: yaw)
    }

    /// A cut: the look set, the bob, the recoil and the sights at rest.
    public func reset(yaw: Float, pitch: Float) throws {
        try GameServices.engine().firstPersonReset(camera: camera.raw, yaw: yaw, pitch: pitch)
    }

    /// What the last frame produced.
    public func state() throws -> FirstPersonState {
        var raw = CyFirstPersonState()
        raw.struct_size = UInt32(MemoryLayout<CyFirstPersonState>.size)
        try GameServices.engine().firstPersonState(camera: camera.raw, into: &raw)
        return FirstPersonState(raw)
    }

    /// Take the behaviours, and the weapon view, away.
    public func release() throws {
        try GameServices.engine().firstPersonRelease(camera: camera.raw)
    }

    /// Set the weapon view; `WeaponView(layers: 0)` turns it off.
    public func setWeaponView(_ view: WeaponView) throws {
        var raw = view.raw
        try GameServices.engine().viewmodelConfigure(camera: camera.raw, desc: &raw)
    }

    /// The weapon view as it was set, its field of view the one it draws with now.
    public func weaponView() throws -> WeaponView {
        var raw = CyViewmodelDesc()
        raw.struct_size = UInt32(MemoryLayout<CyViewmodelDesc>.size)
        try GameServices.engine().viewmodelGet(camera: camera.raw, into: &raw)
        return WeaponView(raw)
    }
}
