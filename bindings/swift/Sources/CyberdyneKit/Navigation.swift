// SPDX-License-Identifier: MIT
// Navigation.swift — ABI 1.3's path queries and crowd agents, as Swift values.
// `add-swift-game-api`.
//
// Two ways to get a path, and one way to walk it:
//
//   * `Navigation.findPath(from:to:)` answers now, in any phase. A* is deterministic, so it is
//     safe in `fixedUpdate` — but it costs the whole search in the calling tick.
//   * `Navigation.requestPath(from:to:)` queues the search (fixed update only) and `poll()` on the
//     returned `PathQuery` answers `.pending` until it lands a fixed number of ticks later, whatever
//     the machine's load: the same tick on every run and on every lockstep peer.
//   * `NavAgent(entity)` makes an entity a crowd agent: `configure()` once, then `move(to:)` in a
//     fixed update, and read `state` anywhere. Orders take effect in the tick's navigation update,
//     so the order a tick's scripts issue them in never matters.
//
// ```swift
// let agent = NavAgent(unit)
// try agent.configure(.init(maxSpeed: 6))          // at spawn
// try agent.move(to: rallyPoint)                   // in fixedUpdate
// if try agent.state.justArrived { idle(unit) }    // "has it arrived" is a flag, not a distance
// ```

import CyberdyneABI
import CyberdyneCore

/// Path queries over the navigation mesh. See the file header for which phase allows what.
public enum Navigation {
    /// What a path search may traverse. Zero in any field means the engine's default.
    public struct PathOptions: Equatable, Sendable {
        /// The navigation world; 0 is the default one.
        public var world: UInt32
        /// Half-extents the endpoints are snapped to the mesh within.
        public var extents: Vec3
        /// A* expansions before the best partial result.
        public var nodeBudget: UInt32
        /// Traversable area types, a bit each.
        public var areaMask: UInt64
        /// Off-mesh link capabilities, a bit each.
        public var capabilities: UInt64

        /// Every field defaulted unless given.
        public init(
            world: UInt32 = 0, extents: Vec3 = Vec3(), nodeBudget: UInt32 = 0,
            areaMask: UInt64 = 0, capabilities: UInt64 = 0
        ) {
            self.world = world
            self.extents = extents
            self.nodeBudget = nodeBudget
            self.areaMask = areaMask
            self.capabilities = capabilities
        }

        func request(from start: Vec3, to end: Vec3) -> CyNavPathRequest {
            var raw = CyNavPathRequest()
            raw.struct_size = UInt32(MemoryLayout<CyNavPathRequest>.size)
            raw.world = world
            raw.start = start.tuple
            raw.end = end.tuple
            raw.extents = extents.tuple
            raw.node_budget = nodeBudget
            raw.area_mask = areaMask
            raw.capabilities = capabilities
            return raw
        }
    }

    /// A straightened path. `points` is empty when no path was found.
    public struct Path: Equatable, Sendable {
        /// The path's corners, start first.
        public var points: [Vec3]
        /// A path was found — possibly partial.
        public var found: Bool
        /// It ends at the reachable point closest to the target, not at the target.
        public var isPartial: Bool
        /// The search ran out of node budget; the path is its best partial answer.
        public var budgetExceeded: Bool
        /// The search's path cost.
        public var cost: Float
        /// Metres along the path.
        public var length: Float

        init(points: [Vec3], result: CyNavPathResult) {
            self.points = points
            found = result.flags & CY_NAV_PATH_FOUND != 0
            isPartial = result.flags & CY_NAV_PATH_PARTIAL != 0
            budgetExceeded = result.flags & CY_NAV_PATH_BUDGET_EXCEEDED != 0
            cost = result.cost
            length = result.length
        }
    }

    /// What a queued search says when polled.
    public enum Poll: Equatable, Sendable {
        /// Not landed yet; poll again next tick.
        case pending
        /// Landed; the query is spent.
        case ready(Path)
        /// Cancelled before it landed; the query is spent.
        case cancelled
    }

    /// A path from `start` to `end`, now. Any phase; safe from a job worker.
    public static func findPath(
        from start: Vec3, to end: Vec3, options: PathOptions = PathOptions()
    ) throws -> Path {
        let engine = try GameServices.engine()
        var request = options.request(from: start, to: end)
        return try readPath { buffer, capacity, result in
            try engine.navFindPath(
                request: &request, into: buffer, capacity: capacity, result: result)
        }
    }

    /// Queue a search from `start` to `end`. Fixed update only.
    public static func requestPath(
        from start: Vec3, to end: Vec3, options: PathOptions = PathOptions()
    ) throws -> PathQuery {
        let engine = try GameServices.engine()
        var request = options.request(from: start, to: end)
        var query: CyNavQuery = CY_NAV_QUERY_NULL
        try engine.navRequestPath(request: &request, into: &query)
        return PathQuery(raw: query)
    }

    /// Run `call` with a point buffer until the whole path fits: the first attempt is sized for a
    /// typical path, and a BUFFER_TOO_SMALL answer carries the size to retry with.
    static func readPath(
        _ call: (UnsafeMutablePointer<Float>?, UInt32, UnsafeMutablePointer<CyNavPathResult>) throws
            -> Void
    ) throws -> Path {
        var capacity: UInt32 = 32
        for _ in 0..<4 {
            var result = CyNavPathResult()
            result.struct_size = UInt32(MemoryLayout<CyNavPathResult>.size)
            var floats = [Float](repeating: 0, count: Int(capacity) * 3)
            do {
                try floats.withUnsafeMutableBufferPointer { buffer in
                    try call(buffer.baseAddress, capacity, &result)
                }
            } catch CyberdyneError.status(.bufferTooSmall, _) {
                capacity = result.point_count
                continue
            }
            if result.state == NavQueryState.pending.rawValue {
                return Path(points: [], result: result)
            }
            let count = Int(result.point_count)
            let points = (0..<count).map { index in
                Vec3(x: floats[index * 3], y: floats[index * 3 + 1], z: floats[index * 3 + 2])
            }
            return Path(points: points, result: result)
        }
        throw CyberdyneError.status(.bufferTooSmall, message: "the path kept growing")
    }
}

/// A queued path search. Poll it once per fixed update until it is not `.pending`.
public struct PathQuery: Hashable, Sendable {
    /// The engine's handle; never `CY_NAV_QUERY_NULL`.
    public let raw: CyNavQuery

    public init(raw: CyNavQuery) {
        self.raw = raw
    }

    /// The search's state; `.ready` hands over the path and spends the query. Fixed update only.
    /// Throws `.notFound` for a query already spent.
    public func poll() throws -> Navigation.Poll {
        let engine = try GameServices.engine()
        var state = CyNavPathResult()
        let path = try Navigation.readPath { buffer, capacity, result in
            try engine.navPollPath(query: raw, into: buffer, capacity: capacity, result: result)
            state = result.pointee
        }
        switch NavQueryState(rawValue: state.state) {
        case .pending: return .pending
        case .cancelled: return .cancelled
        default: return .ready(path)
        }
    }

    /// Cancel the search. Fixed update only. Throws `.notFound` once spent.
    public func cancel() throws {
        try GameServices.engine().navCancelPath(query: raw)
    }
}

/// An entity as a crowd agent: it follows paths with local avoidance, and its motion goes where the
/// game's host sends it (a character controller, or the transform).
public struct NavAgent: Hashable, Sendable {
    /// The agent's entity.
    public let entity: Entity

    public init(_ entity: Entity) {
        self.entity = entity
    }

    /// An agent's shape and limits. Zero in any field means the engine's default.
    public struct Parameters: Equatable, Sendable {
        public var world: UInt32
        public var radius: Float
        public var height: Float
        public var maxSpeed: Float
        public var maxAcceleration: Float
        /// How close to the target counts as there.
        public var arrivalDistance: Float
        /// Higher yields less.
        public var priority: UInt32
        public var areaMask: UInt64
        public var capabilities: UInt64

        /// Every field defaulted unless given.
        public init(
            world: UInt32 = 0, radius: Float = 0, height: Float = 0, maxSpeed: Float = 0,
            maxAcceleration: Float = 0, arrivalDistance: Float = 0, priority: UInt32 = 0,
            areaMask: UInt64 = 0, capabilities: UInt64 = 0
        ) {
            self.world = world
            self.radius = radius
            self.height = height
            self.maxSpeed = maxSpeed
            self.maxAcceleration = maxAcceleration
            self.arrivalDistance = arrivalDistance
            self.priority = priority
            self.areaMask = areaMask
            self.capabilities = capabilities
        }

        var raw: CyNavAgentParams {
            var raw = CyNavAgentParams()
            raw.struct_size = UInt32(MemoryLayout<CyNavAgentParams>.size)
            raw.world = world
            raw.radius = radius
            raw.height = height
            raw.max_speed = maxSpeed
            raw.max_acceleration = maxAcceleration
            raw.arrival_distance = arrivalDistance
            raw.priority = priority
            raw.area_mask = areaMask
            raw.capabilities = capabilities
            return raw
        }
    }

    /// Where an agent is on its way.
    public struct State: Equatable, Sendable {
        public var status: NavPathStatus
        /// True for the one tick after the agent arrived or failed.
        public var event: Bool
        public var position: Vec3
        public var velocity: Vec3
        public var target: Vec3
        /// Metres along the path still to go; zero when idle.
        public var remainingDistance: Float

        /// It arrived this tick — the flag test "has it arrived" should be.
        public var justArrived: Bool { event && status == .arrived }
        /// It gave up this tick.
        public var justFailed: Bool { event && status == .failed }

        init(_ raw: CyNavAgentState) {
            status = NavPathStatus(rawValue: raw.status) ?? .idle
            event = raw.flags & CY_NAV_AGENT_EVENT != 0
            position = Vec3(raw.position)
            velocity = Vec3(raw.velocity)
            target = Vec3(raw.target)
            remainingDistance = raw.remaining_distance
        }
    }

    /// Make the entity an agent, or change its parameters. At load or in a fixed update; the first
    /// call adds the agent component, so a component borrowed before it is stale after it.
    public func configure(_ parameters: Parameters = Parameters()) throws {
        var raw = parameters.raw
        try GameServices.engine().navAgentConfigure(entity: entity.bits, params: &raw)
    }

    /// Send the agent to `point`. Fixed update only; takes effect in this tick's navigation update.
    public func move(to point: Vec3) throws {
        let target = [point.x, point.y, point.z]
        let engine = try GameServices.engine()
        try target.withUnsafeBufferPointer { target in
            try engine.navAgentMoveTo(entity: entity.bits, target: target.baseAddress)
        }
    }

    /// Stop where it is. Fixed update only.
    public func stop() throws {
        try GameServices.engine().navAgentStop(entity: entity.bits)
    }

    /// The agent's status, motion and target. Any phase.
    public var state: State {
        get throws {
            var raw = CyNavAgentState()
            raw.struct_size = UInt32(MemoryLayout<CyNavAgentState>.size)
            try GameServices.engine().navAgentState(entity: entity.bits, into: &raw)
            return State(raw)
        }
    }
}
