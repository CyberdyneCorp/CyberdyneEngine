// SPDX-License-Identifier: MIT
// Lights.swift — ABI 1.11's lights, as a game writes them.
//
//     // onUpdate: a muzzle flash for fifty milliseconds, gone on its own.
//     try Light.spawn(.point(at: muzzle, colour: Vec3(x: 1, y: 0.7, z: 0.3), candela: 9000,
//                            range: 6, lifetime: 0.05))
//
//     // A torch the character carries: lives until destroyed, moved every frame.
//     let torchLight = Light.Description.point(at: hand, candela: 300, range: 8)
//     let torch = try Light.spawn(torchLight)
//     try torch.move(torchLight, to: Pose(position: hand))
//
//     // A glowing panel: an area light, in nits.
//     try Light.spawn(.rect(pose: wall, width: 1.2, height: 0.4, nits: 4000))
//
// The pool is the engine's `cy::rendering::TransientLights`: a light is a slot, so spawning one
// allocates nothing, and the host appends the live lights to the frame every frame — clustered,
// sampled, handed to the GI like an authored lamp. A light is PRESENTATION: every call is refused in
// `onFixedUpdate` with `CyberdyneError.status(.permissionDenied, …)`, because a resimulated tick
// would spawn its flash again. A full pool throws `.outOfRange`; a light that ended, `.notFound`.

import CyberdyneABI
import CyberdyneCore

/// A light a game makes, through ABI 1.11's `light_*`.
public struct Light: Equatable, Hashable, Sendable {
    /// The pool slot and its generation. A handle to a light that ended never answers for the
    /// light now in its slot.
    public let handle: CyLight

    /// A light from its handle.
    public init(handle: CyLight) {
        self.handle = handle
    }

    /// What a light is, and how it lives.
    public struct Description: Equatable, Sendable {
        /// What it is: a point or a spot, or one of the four area lights.
        public var kind: LightKind
        /// Where it is and which way it faces: forward (local -Z) is a spot's axis and a panel's
        /// facing, right (local +X) a panel's width and a tube's length.
        public var pose: Pose
        /// Linear colour.
        public var colour: Vec3
        /// Candela for a point or a spot, nits for an area light.
        public var intensity: Float
        /// Metres. Zero derives an area light's reach from its brightness.
        public var range: Float
        /// A spot's cone, inner and outer, in radians.
        public var innerCone: Float
        public var outerCone: Float
        /// An area light's size, in metres: a rect's width and height, a disc's or a sphere's
        /// diameter (`width`), a tube's length and diameter.
        public var width: Float
        public var height: Float
        /// A rect or a disc that emits from both faces.
        public var twoSided: Bool
        /// Seconds; zero lives until destroyed.
        public var lifetime: Float
        /// How it fades along its lifetime.
        public var fade: LightFade
        /// The channels it lights; zero lights every channel.
        public var layers: UInt32

        /// Every field.
        public init(
            kind: LightKind, pose: Pose = Pose(), colour: Vec3 = Vec3(x: 1, y: 1, z: 1),
            intensity: Float, range: Float = 0, innerCone: Float = 0, outerCone: Float = 0.785,
            width: Float = 0, height: Float = 0, twoSided: Bool = false, lifetime: Float = 0,
            fade: LightFade = .quadratic, layers: UInt32 = 0
        ) {
            self.kind = kind
            self.pose = pose
            self.colour = colour
            self.intensity = intensity
            self.range = range
            self.innerCone = innerCone
            self.outerCone = outerCone
            self.width = width
            self.height = height
            self.twoSided = twoSided
            self.lifetime = lifetime
            self.fade = fade
            self.layers = layers
        }

        /// A point light: a flash, a spark, a torch.
        public static func point(
            at position: Vec3, colour: Vec3 = Vec3(x: 1, y: 1, z: 1), candela: Float, range: Float,
            lifetime: Float = 0, fade: LightFade = .quadratic
        ) -> Description {
            Description(
                kind: .point, pose: Pose(position: position), colour: colour, intensity: candela,
                range: range, lifetime: lifetime, fade: fade)
        }

        /// A spot along `pose`'s forward axis.
        public static func spot(
            pose: Pose, colour: Vec3 = Vec3(x: 1, y: 1, z: 1), candela: Float, range: Float,
            innerCone: Float, outerCone: Float, lifetime: Float = 0, fade: LightFade = .quadratic
        ) -> Description {
            Description(
                kind: .spot, pose: pose, colour: colour, intensity: candela, range: range,
                innerCone: innerCone, outerCone: outerCone, lifetime: lifetime, fade: fade)
        }

        /// A rectangular panel facing `pose`'s forward axis.
        public static func rect(
            pose: Pose, width: Float, height: Float, colour: Vec3 = Vec3(x: 1, y: 1, z: 1), nits: Float,
            range: Float = 0, twoSided: Bool = false, lifetime: Float = 0
        ) -> Description {
            Description(
                kind: .rect, pose: pose, colour: colour, intensity: nits, range: range,
                width: width, height: height, twoSided: twoSided, lifetime: lifetime)
        }

        /// A disc facing `pose`'s forward axis, `diameter` across.
        public static func disc(
            pose: Pose, diameter: Float, colour: Vec3 = Vec3(x: 1, y: 1, z: 1), nits: Float,
            range: Float = 0, twoSided: Bool = false, lifetime: Float = 0
        ) -> Description {
            Description(
                kind: .disc, pose: pose, colour: colour, intensity: nits, range: range,
                width: diameter, twoSided: twoSided, lifetime: lifetime)
        }

        /// A glowing sphere, `diameter` across: an explosion's fireball.
        public static func sphere(
            at position: Vec3, diameter: Float, colour: Vec3 = Vec3(x: 1, y: 1, z: 1), nits: Float,
            range: Float = 0, lifetime: Float = 0, fade: LightFade = .quadratic
        ) -> Description {
            Description(
                kind: .sphere, pose: Pose(position: position), colour: colour, intensity: nits,
                range: range, width: diameter, lifetime: lifetime, fade: fade)
        }

        /// A tube along `pose`'s right axis.
        public static func tube(
            pose: Pose, length: Float, diameter: Float, colour: Vec3 = Vec3(x: 1, y: 1, z: 1),
            nits: Float, range: Float = 0, lifetime: Float = 0
        ) -> Description {
            Description(
                kind: .tube, pose: pose, colour: colour, intensity: nits, range: range,
                width: length, height: diameter, lifetime: lifetime)
        }

        /// From the ABI's spelling. An unknown kind reads as a point, an unknown fade as quadratic.
        public init(_ raw: CyLightDesc) {
            kind = LightKind(rawValue: raw.kind) ?? .point
            pose = Pose(raw.pose)
            colour = Vec3(raw.color)
            intensity = raw.intensity
            range = raw.range
            innerCone = raw.inner_cone
            outerCone = raw.outer_cone
            width = raw.size.0
            height = raw.size.1
            twoSided = raw.flags & CY_LIGHT_TWO_SIDED != 0
            lifetime = raw.lifetime
            fade = LightFade(rawValue: raw.fade) ?? .quadratic
            layers = raw.layer_mask
        }

        /// In the ABI's spelling.
        public var raw: CyLightDesc {
            var out = CyLightDesc()
            out.struct_size = UInt32(MemoryLayout<CyLightDesc>.size)
            out.kind = kind.rawValue
            out.pose = pose.raw
            out.color = colour.tuple
            out.intensity = intensity
            out.range = range
            out.inner_cone = innerCone
            out.outer_cone = outerCone
            out.size = (width, height)
            out.lifetime = lifetime
            out.fade = fade.rawValue
            out.flags = twoSided ? CY_LIGHT_TWO_SIDED : 0
            out.layer_mask = layers
            return out
        }
    }

    /// The pool and what it has done.
    public struct Status: Equatable, Sendable {
        /// Lights alive now.
        public var live: UInt32
        /// The most alive at once.
        public var capacity: UInt32
        /// Spawned since the pool was made.
        public var spawned: UInt64
        /// Whose lifetime ended.
        public var expired: UInt64
        /// Spawns a full pool refused.
        public var refused: UInt64

        /// From the ABI's spelling.
        public init(_ raw: CyLightStatus) {
            live = raw.live
            capacity = raw.capacity
            spawned = raw.spawned
            expired = raw.expired
            refused = raw.refused
        }
    }

    /// A light, from this frame. Throws `.invalidArgument` for a negative number, `.outOfRange`
    /// with every slot of the pool taken.
    @discardableResult
    public static func spawn(_ description: Description) throws -> Light {
        var raw = description.raw
        var handle = CY_LIGHT_NULL
        try GameServices.engine().lightSpawn(desc: &raw, into: &handle)
        return Light(handle: handle)
    }

    /// Replace the light's description — keeping its age; its lifetime and fade are its spawn's.
    /// Throws `.notFound` for a light that ended.
    public func update(_ description: Description) throws {
        var raw = description.raw
        try GameServices.engine().lightUpdate(light: handle, desc: &raw)
    }

    /// Move the light, keeping everything else: `description` with a new pose.
    public func move(_ description: Description, to pose: Pose) throws {
        var moved = description
        moved.pose = pose
        try update(moved)
    }

    /// End the light now. Throws `.notFound` for one that already ended.
    public func destroy() throws {
        try GameServices.engine().lightDestroy(light: handle)
    }

    /// Whether the light is alive: false for one whose lifetime ended, or that was destroyed.
    public var isAlive: Bool {
        get throws {
            var alive: UInt32 = 0
            try GameServices.engine().lightAlive(light: handle, into: &alive)
            return alive != 0
        }
    }

    /// The pool.
    public static var status: Status {
        get throws {
            var raw = CyLightStatus()
            raw.struct_size = UInt32(MemoryLayout<CyLightStatus>.size)
            try GameServices.engine().lightStatus(into: &raw)
            return Status(raw)
        }
    }
}
