// SPDX-License-Identifier: MIT
// Physics.swift — ABI 1.3's physics queries, as Swift values. `add-swift-game-api`.
//
// Four queries over the last completed physics step: `raycast`, `raycastAll`, `shapeCast` and
// `overlap`. They are callable in every phase and are deterministic in a fixed update — every list
// is in the engine's total order (distance, then entity, then body; overlaps by entity, each once)
// — so a unit's line-of-sight check in `fixedUpdate` replays exactly. While the physics step itself
// runs they throw `.unavailable`.
//
// ```swift
// extension Physics.Filter { static let units = Physics.Filter(mask: 1 << 3) }
//
// if let hit = try Physics.raycast(ray, filter: .units) { select(hit.entity) }
// let inBlast = try Physics.overlap(.sphere(radius: 6), at: Pose(position: impact))
// ```

import CyberdyneABI
import CyberdyneCore

/// The physics world's scene queries. See the file header for phases and ordering.
public enum Physics {
    /// Which bodies a query considers. The default hits every layer, skips triggers and culls back
    /// faces — `CyQueryFilter`'s own defaults.
    public struct Filter: Equatable, Sendable {
        /// The querying "collider"'s layer, 0 to 31. Bodies whose masks exclude it are not hit.
        public var layer: UInt32
        /// A bit per layer the query hits.
        public var mask: UInt32
        /// What else to include or skip.
        public var options: Options
        /// Entities whose bodies are skipped — typically the caller's own.
        public var ignoring: [Entity]

        /// A filter over `mask`, from `layer`, with `options`, skipping `ignoring`.
        public init(
            layer: UInt32 = 0, mask: UInt32 = .max, options: Options = [],
            ignoring: [Entity] = []
        ) {
            self.layer = layer
            self.mask = mask
            self.options = options
            self.ignoring = ignoring
        }

        /// Everything solid.
        public static let all = Filter()

        /// This filter, also skipping `entities`.
        public func ignoring(_ entities: Entity...) -> Filter {
            var copy = self
            copy.ignoring.append(contentsOf: entities)
            return copy
        }
    }

    /// `CY_QUERY_*`.
    public struct Options: OptionSet, Sendable {
        /// The CY_QUERY_* bits, as the engine reads them.
        public let rawValue: UInt32
        /// Options from raw CY_QUERY_* bits.
        public init(rawValue: UInt32) { self.rawValue = rawValue }

        /// Include sensors, which are skipped by default.
        public static let includeTriggers = Options(rawValue: CY_QUERY_INCLUDE_TRIGGERS)
        /// Hit triangles from behind.
        public static let hitBackFaces = Options(rawValue: CY_QUERY_HIT_BACK_FACES)
        /// Skip static bodies.
        public static let skipStatic = Options(rawValue: CY_QUERY_SKIP_STATIC)
        /// Skip kinematic bodies.
        public static let skipKinematic = Options(rawValue: CY_QUERY_SKIP_KINEMATIC)
        /// Skip dynamic bodies.
        public static let skipDynamic = Options(rawValue: CY_QUERY_SKIP_DYNAMIC)
    }

    /// A query shape. A capsule's axis is its local +Y.
    public enum Shape: Equatable, Sendable {
        case sphere(radius: Float)
        /// `halfHeight` is half the cylindrical section, excluding the caps.
        case capsule(radius: Float, halfHeight: Float)
        case box(halfExtents: Vec3)

        /// In the ABI's spelling.
        public var raw: CyShape {
            var shape = CyShape()
            switch self {
            case .sphere(let radius):
                shape.kind = ShapeKind.sphere.rawValue
                shape.radius = radius
            case .capsule(let radius, let halfHeight):
                shape.kind = ShapeKind.capsule.rawValue
                shape.radius = radius
                shape.half_height = halfHeight
            case .box(let halfExtents):
                shape.kind = ShapeKind.box.rawValue
                shape.half_extents = halfExtents.tuple
            }
            return shape
        }
    }

    /// Where a ray or a sweep touched a body.
    public struct Hit: Equatable, Sendable {
        /// The entity owning the body; `Entity.null` for a body with none.
        public var entity: Entity
        /// The point on the surface, in world space.
        public var point: Vec3
        /// Unit, out of the surface.
        public var normal: Vec3
        /// Metres travelled before the hit.
        public var distance: Float
        /// `distance / maxDistance`.
        public var fraction: Float
        /// The body is a sensor.
        public var isTrigger: Bool
        /// A sweep that already overlapped at its start.
        public var startedPenetrating: Bool

        /// From the ABI's spelling.
        public init(_ raw: CyPhysicsHit) {
            entity = Entity(bits: raw.entity)
            point = Vec3(raw.point)
            normal = Vec3(raw.normal)
            distance = raw.distance
            fraction = raw.fraction
            isTrigger = raw.flags & CY_HIT_TRIGGER != 0
            startedPenetrating = raw.flags & CY_HIT_STARTED_PENETRATING != 0
        }
    }

    /// The nearest hit along `ray`, or nil when nothing is hit.
    public static func raycast(_ ray: Ray, filter: Filter = .all) throws -> Hit? {
        let engine = try GameServices.engine()
        var raw = ray.raw
        var hit = CyPhysicsHit()
        var hasHit = false
        try filter.withRaw { filter in
            try engine.physicsRaycast(ray: &raw, filter: filter, into: &hit, hasHit: &hasHit)
        }
        return hasHit ? Hit(hit) : nil
    }

    /// Every hit along `ray`, nearest first; equal distances in entity order.
    public static func raycastAll(_ ray: Ray, filter: Filter = .all) throws -> [Hit] {
        let engine = try GameServices.engine()
        var raw = ray.raw
        let hits: [CyPhysicsHit] = try filter.withRaw { filter in
            try sized { buffer, capacity, count in
                try engine.physicsRaycastAll(
                    ray: &raw, filter: filter, into: buffer, capacity: capacity, count: count)
            }
        }
        return hits.map(Hit.init)
    }

    /// Sweep `shape` from `start` along `direction` (unit) for up to `maxDistance` metres; the first
    /// hit, or nil. The orientation is kept for the whole sweep.
    public static func shapeCast(
        _ shape: Shape, from start: Pose, direction: Vec3, maxDistance: Float,
        filter: Filter = .all
    ) throws -> Hit? {
        let engine = try GameServices.engine()
        var rawShape = shape.raw
        var rawStart = start.raw
        let rawDirection = [direction.x, direction.y, direction.z]
        var hit = CyPhysicsHit()
        var hasHit = false
        try filter.withRaw { filter in
            try rawDirection.withUnsafeBufferPointer { direction in
                try engine.physicsShapeCast(
                    shape: &rawShape, start: &rawStart, direction: direction.baseAddress,
                    maxDistance: maxDistance, filter: filter, into: &hit, hasHit: &hasHit)
            }
        }
        return hasHit ? Hit(hit) : nil
    }

    /// Every entity whose body overlaps `shape` at `pose`, in entity order, each once.
    public static func overlap(
        _ shape: Shape, at pose: Pose, filter: Filter = .all
    ) throws -> [Entity] {
        let engine = try GameServices.engine()
        var rawShape = shape.raw
        var rawPose = pose.raw
        let entities: [CyEntity] = try filter.withRaw { filter in
            try sized { buffer, capacity, count in
                try engine.physicsOverlap(
                    shape: &rawShape, pose: &rawPose, filter: filter, into: buffer,
                    capacity: capacity, count: count)
            }
        }
        return entities.map(Entity.init(bits:))
    }

    /// `world_chunks`' sizing pattern, from the calling side: ask for the total with no buffer,
    /// then fill one that size, and ask again if the answer grew in between. `Element` is an
    /// imported C struct or integer, so leaving the unwritten tail uninitialised is sound.
    static func sized<Element>(
        _ call: (UnsafeMutablePointer<Element>?, UInt32, UnsafeMutablePointer<UInt32>) throws ->
            Void
    ) throws -> [Element] {
        var total: UInt32 = 0
        try call(nil, 0, &total)
        for _ in 0..<4 {
            guard total > 0 else { return [] }
            let capacity = total
            var written: UInt32 = 0
            let elements = try [Element](unsafeUninitializedCapacity: Int(capacity)) {
                buffer, initialised in
                initialised = 0
                do {
                    try call(buffer.baseAddress, capacity, &written)
                    initialised = Int(min(written, capacity))
                } catch CyberdyneError.status(.bufferTooSmall, _) {
                    initialised = 0
                }
            }
            if written <= capacity { return elements }
            total = written
        }
        throw CyberdyneError.status(.bufferTooSmall, message: "the result kept growing")
    }
}

extension Physics.Filter {
    /// Call `body` with this filter in the ABI's spelling; the ignore list is borrowed for the call.
    func withRaw<Result>(
        _ body: (UnsafePointer<CyQueryFilter>) throws -> Result
    ) rethrows
        -> Result
    {
        let bits = ignoring.map(\.bits)
        return try bits.withUnsafeBufferPointer { ignore in
            var raw = CyQueryFilter()
            raw.struct_size = UInt32(MemoryLayout<CyQueryFilter>.size)
            raw.layer = layer
            raw.mask = mask
            raw.flags = options.rawValue
            raw.ignore = ignore.count > 0 ? ignore.baseAddress : nil
            raw.ignore_count = UInt32(ignore.count)
            return try body(&raw)
        }
    }
}
