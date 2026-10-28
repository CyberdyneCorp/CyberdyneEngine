// SPDX-License-Identifier: MIT
// Effects.swift — ABI 1.15's effects and decals, as a game writes them.
//
//     // onReady: resolve once, by the names the host registered.
//     let sparks = try Effect.named("impact.sparks")
//     let scorch = try DecalMaterial.named("impact.scorch")
//
//     // onUpdate: a hit's sparks along the surface's normal, and its mark.
//     try sparks.play(at: hit.point, parameters: [hit.normal.x, hit.normal.y, hit.normal.z, 0])
//     try Decal.spawn(.mark(scorch, at: hit.point, normal: hit.normal, radius: 0.08))
//
// An effect is one of the engine's VFX systems the host cooked, played fire-and-forget in its
// simulation world and drawn by the frame's particle renderer; it releases itself when its last
// particle dies. Its parameters override the effect's declared parameters in declaration order,
// four floats a parameter. A decal is a box projected onto whatever it contains, kept by the
// frame's decal budget: when the budget is full the least worth keeping is evicted —
// deterministically, by age, importance and coverage — possibly the new one, and `spawn` then
// answers nil. Both are PRESENTATION: playing and spawning are refused in `onFixedUpdate` with
// `CyberdyneError.status(.permissionDenied, …)`, because a resimulated tick would leave its mark
// again; resolving a name is allowed in every phase.

import CyberdyneABI
import CyberdyneCore

/// An effect the host registered, through ABI 1.15's `effect_*`.
public struct Effect: Hashable, Sendable {
    /// What `effect_find` handed out.
    public let handle: CyEffect

    /// An effect from its handle.
    public init(handle: CyEffect) {
        self.handle = handle
    }

    /// The effect the host registered under `name`. Throws `.notFound` for a name it has not.
    public static func named(_ name: String) throws -> Effect {
        let engine = try GameServices.engine()
        var handle: CyEffect = 0  // CY_EFFECT_NULL
        try name.withCString { try engine.effectFind(name: $0, into: &handle) }
        return Effect(handle: handle)
    }

    /// Play the effect once at `position`, `scale` times its authored size, its first parameters
    /// overridden by `parameters` — at most eight floats, four a parameter. Throws
    /// `.invalidArgument` for more, `.outOfRange` when the simulation world is full.
    public func play(at position: Vec3, scale: Float = 1, parameters: [Float] = []) throws {
        var raw = CyEffectPlay()
        raw.struct_size = UInt32(MemoryLayout<CyEffectPlay>.size)
        raw.position = position.tuple
        raw.scale = scale
        raw.parameter_count = UInt32(parameters.count)
        withUnsafeMutableBytes(of: &raw.parameters) { lanes in
            let floats = lanes.bindMemory(to: Float.self)
            for (index, value) in parameters.prefix(floats.count).enumerated() {
                floats[index] = value
            }
        }
        try GameServices.engine().effectPlay(effect: handle, play: &raw)
    }
}

/// A decal material the host registered, through ABI 1.15's `decal_find_material`.
public struct DecalMaterial: Hashable, Sendable {
    /// What `decal_find_material` handed out.
    public let handle: CyDecalMaterial

    /// A material from its handle.
    public init(handle: CyDecalMaterial) {
        self.handle = handle
    }

    /// The material the host registered under `name`. Throws `.notFound` for one it has not.
    public static func named(_ name: String) throws -> DecalMaterial {
        let engine = try GameServices.engine()
        var handle: CyDecalMaterial = 0  // CY_DECAL_MATERIAL_NULL
        try name.withCString { try engine.decalFindMaterial(name: $0, into: &handle) }
        return DecalMaterial(handle: handle)
    }
}

/// A decal a game spawned, through ABI 1.15's `decal_*`.
public struct Decal: Hashable, Sendable {
    /// The budget's id.
    public let id: CyDecal

    /// A decal from its id.
    public init(id: CyDecal) {
        self.id = id
    }

    /// What a decal is: a box projected along its local -Z, so its local +Z is the normal of the
    /// surface it marks.
    public struct Description: Equatable, Sendable {
        /// What the mark looks like: a material the host registered.
        public var material: DecalMaterial
        /// World; the box's centre and its frame.
        public var pose: Pose
        /// Metres: the mark's half width and height, and the box's half depth.
        public var halfExtent: Vec3
        /// 0 to 1: what eviction keeps longest.
        public var importance: Float
        /// A higher order covers a lower; equal orders, the later spawn.
        public var sortOrder: Int32
        /// Radians; past it the mark fades rather than stretches. Zero is the engine's 60 degrees.
        public var fadeAngle: Float
        /// Metres from the eye the mark starts fading; zero is the engine's 20.
        public var fadeStart: Float
        /// Metres from the eye it is gone by, past `fadeStart`; zero is the engine's 40.
        public var fadeEnd: Float

        /// Every field.
        public init(
            material: DecalMaterial, pose: Pose, halfExtent: Vec3, importance: Float = 0.5,
            sortOrder: Int32 = 0, fadeAngle: Float = 0, fadeStart: Float = 0, fadeEnd: Float = 0
        ) {
            self.material = material
            self.pose = pose
            self.halfExtent = halfExtent
            self.importance = importance
            self.sortOrder = sortOrder
            self.fadeAngle = fadeAngle
            self.fadeStart = fadeStart
            self.fadeEnd = fadeEnd
        }

        /// A round mark `radius` across at `point` on a surface facing `normal` (unit): the box's
        /// +Z turned onto the normal, `depth` metres deep each side of the surface.
        public static func mark(
            _ material: DecalMaterial, at point: Vec3, normal: Vec3, radius: Float,
            depth: Float = 0.05, importance: Float = 0.5
        ) -> Description {
            Description(
                material: material, pose: Pose(position: point, rotation: turning(onto: normal)),
                halfExtent: Vec3(x: radius, y: radius, z: depth), importance: importance)
        }

        /// In the ABI's spelling.
        public var raw: CyDecalDesc {
            var out = CyDecalDesc()
            out.struct_size = UInt32(MemoryLayout<CyDecalDesc>.size)
            out.material = material.handle
            out.pose = pose.raw
            out.half_extent = halfExtent.tuple
            out.importance = importance
            out.sort_order = sortOrder
            out.fade_angle = fadeAngle
            out.fade_start = fadeStart
            out.fade_end = fadeEnd
            return out
        }
    }

    /// The budget and what it has done.
    public struct Status: Equatable, Sendable {
        /// Marks in the budget now.
        public var live: UInt32
        /// The most it holds before the least worth keeping is evicted.
        public var capacity: UInt32
        /// Spawned since the budget was bound.
        public var spawned: UInt64
        /// Evicted by a full budget, a spawned mark that was the least worth keeping included.
        public var evicted: UInt64

        /// From the ABI's spelling.
        public init(_ raw: CyDecalStatus) {
            live = raw.live
            capacity = raw.capacity
            spawned = raw.spawned
            evicted = raw.evicted
        }
    }

    /// A decal, from this frame; nil when the budget was full and the new one was the least worth
    /// keeping. Throws `.invalidArgument` for a description out of range, `.notFound` for a
    /// material the host did not hand out.
    @discardableResult
    public static func spawn(_ description: Description) throws -> Decal? {
        var raw = description.raw
        var id: CyDecal = 0  // CY_DECAL_NULL
        try GameServices.engine().decalSpawn(desc: &raw, into: &id)
        return id == 0 ? nil : Decal(id: id)
    }

    /// The budget.
    public static var status: Status {
        get throws {
            var raw = CyDecalStatus()
            raw.struct_size = UInt32(MemoryLayout<CyDecalStatus>.size)
            try GameServices.engine().decalStatus(into: &raw)
            return Status(raw)
        }
    }
}

/// The shortest rotation taking +Z onto `normal` (unit): the frame a decal on a surface facing
/// `normal` projects in. Straight down — `normal` against +Z — turns about +X.
func turning(onto normal: Vec3) -> Quat {
    let dot = normal.z
    if dot < -0.999_999 {
        return Quat(x: 1, y: 0, z: 0, w: 0)
    }
    // q = (z × n, 1 + z·n), normalised: half the angle between them.
    let axis = Vec3(x: -normal.y, y: normal.x, z: 0)
    let w = 1 + dot
    let length = (axis.x * axis.x + axis.y * axis.y + w * w).squareRoot()
    return Quat(x: axis.x / length, y: axis.y / length, z: 0, w: w / length)
}
