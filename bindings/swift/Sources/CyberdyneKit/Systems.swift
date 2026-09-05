// Systems.swift — the data-oriented programming model, over the ECS's access declarations.
// Task 3.3.
//
// `swift-scripting`: "Swift SHALL also be able to define **systems** for data-oriented work, with
// access declared in the signature so the scheduler can parallelise them exactly as it does native
// systems", and "Query iteration SHALL operate over chunk-contiguous storage through borrowed
// pointers, so a Swift system's inner loop does not marshal per entity."
//
// --- THE ACCESS IS THE QUERY, WHICH IS M1'S OWN FINDING -------------------------------------------
//
// `src/ecs/include/cy/ecs/system.h` states it in as many words: "the access model expresses what a
// real system needs, on one condition — that the query and the declaration are the same object. A
// system that writes down its access separately from the query it runs can drift, and nothing
// catches the drift, because a declaration is only checked against other declarations."
//
// So `Query<Write<Velocity>, Read<Mass>, Without<Grounded>>` IS the declaration here too. The
// `@System` macro reads it out of the function's signature and registers exactly it; there is no
// second place to write access down, and therefore nothing for a second place to disagree with.
//
// --- WHAT ABI 1.0 CANNOT DO YET, STATED PLAINLY ----------------------------------------------------
//
// `CyInterface` at 1.0 has thirty entries and NONE of them hands a module a chunk. It carries the
// world, entities, components (by id, by field, one entity at a time) and behaviours. So:
//
//   * the model below — terms, access derivation, conflict detection, registration, and a chunk
//     inner loop that never marshals — is complete and tested;
//   * the SOURCE of chunks is not, because the entry that would provide one does not exist. It is
//     an append to `CyInterface` (the only legal way to grow it) that belongs to whoever owns the
//     ECS's ABI surface, alongside a `CyStage` enum so that `SystemStage` below stops being a copy.
//
// `ChunkSource` is that seam, and it is a protocol rather than a `TODO` so that the inner loop is
// exercised by real tests today and the day the entry lands is a conformance and nothing else.

import CyberdyneABI
import CyberdyneCore

/// The stages, in execution order.
///
/// A COPY, AND IT SAYS SO. These are `cy::ecs::Stage`'s own values from
/// `src/ecs/include/cy/ecs/system.h`, and the ABI does not carry them: there is no `CyStage` in
/// `cy_abi.h`, so nothing checks that this list still matches. That is exactly the drift the
/// generated overlay exists to prevent, and the fix is an appended enum rather than more care here.
/// Until then, this file is the one place in the package that a reviewer has to read against the
/// engine.
public enum SystemStage: UInt32, Sendable, CaseIterable {
    case preSimulation = 0
    case physics = 1
    case simulation = 2
    case postSimulation = 3
    case frame = 4
    case animation = 5
    case ui = 6
    case render = 7

    /// True for the four stages that run on the fixed simulation step.
    public var isFixedStep: Bool { rawValue <= SystemStage.postSimulation.rawValue }
}

/// One access declaration: what a term says about one component or resource.
public struct AccessTerm: Hashable, Sendable {
    public enum Mode: Sendable, Hashable { case read, write, exclude }
    public let name: String
    public let mode: Mode

    public init(name: String, mode: Mode) {
        self.name = name
        self.mode = mode
    }
}

/// A term of a query. The four the specification names, and no more: a term that does not declare
/// access is a term the scheduler cannot order.
public protocol QueryTerm {
    static var accessTerm: AccessTerm { get }
}

/// Read access to a component. Two systems that both only read may run together.
public struct Read<T: Component>: QueryTerm {
    public static var accessTerm: AccessTerm { AccessTerm(name: T.componentName, mode: .read) }
}

/// Write access. Conflicts with every other access to the same component.
public struct Write<T: Component>: QueryTerm {
    public static var accessTerm: AccessTerm { AccessTerm(name: T.componentName, mode: .write) }
}

/// An exclusion filter. It narrows the match and declares no access, so it never conflicts.
public struct Without<T: Component>: QueryTerm {
    public static var accessTerm: AccessTerm { AccessTerm(name: T.componentName, mode: .exclude) }
}

/// A resource, read. Named separately from `Read` because a resource is not a component and the
/// scheduler orders them in their own namespace.
public struct Res<T>: QueryTerm {
    public static var accessTerm: AccessTerm {
        AccessTerm(name: "res:\(String(describing: T.self))", mode: .read)
    }
}

/// What a system touches, and whether two systems may run at the same time.
public struct AccessSet: Sendable, Equatable {
    public private(set) var reads: Set<String> = []
    public private(set) var writes: Set<String> = []
    public private(set) var excludes: Set<String> = []

    public init(_ terms: [AccessTerm] = []) {
        for term in terms { insert(term) }
    }

    public mutating func insert(_ term: AccessTerm) {
        switch term.mode {
        case .read: reads.insert(term.name)
        case .write: writes.insert(term.name)
        case .exclude: excludes.insert(term.name)
        }
    }

    /// The same rule the engine's scheduler uses: a write conflicts with any other access to the
    /// same name, and two reads never conflict.
    public func conflicts(with other: AccessSet) -> Bool {
        !writes.isDisjoint(with: other.writes)
            || !writes.isDisjoint(with: other.reads)
            || !reads.isDisjoint(with: other.writes)
    }

    /// A query that both reads and writes one component has declared a conflict with itself, which
    /// is the `@System` macro's "systems with conflicting access" diagnostic.
    public var isSelfConflicting: Bool { !reads.isDisjoint(with: writes) }
}

/// A query over the world, whose type IS its access declaration.
public struct Query<each Term: QueryTerm>: Sendable {
    public init() {}

    /// The terms, in the order they were written.
    public static var terms: [AccessTerm] {
        var collected: [AccessTerm] = []
        _ = (repeat collected.append((each Term).accessTerm))
        return collected
    }

    public static var access: AccessSet { AccessSet(terms) }
}

// --- Chunk iteration --------------------------------------------------------------------------------

/// One archetype chunk: the entities in it, and the base address of each component array it holds.
///
/// The whole point is that a component array is CONTIGUOUS and borrowed. A system's inner loop
/// indexes it directly — no `CyVar`, no per-entity ABI call — which is `swift-scripting`'s "Bulk
/// iteration does not marshal".
///
/// `~Escapable` would be the language's own way to say a borrow may not outlive its iteration, and
/// it is not usable here yet: a non-escapable type cannot be handed to a closure that Swift 6.3
/// will accept in this position. So the rule is enforced the other way the specification allows —
/// "development-build checks otherwise" — by `EscapeGuard` below, which invalidates the view when
/// the iteration ends and traps a use afterwards in a way that names the system.
public struct ChunkView {
    public let entities: UnsafeBufferPointer<CyEntity>
    private let bases: [String: UnsafeMutableRawPointer]
    private let strides: [String: Int]
    private let guardToken: EscapeGuard

    public init(entities: UnsafeBufferPointer<CyEntity>,
                bases: [String: UnsafeMutableRawPointer],
                strides: [String: Int],
                guardToken: EscapeGuard) {
        self.entities = entities
        self.bases = bases
        self.strides = strides
        self.guardToken = guardToken
    }

    public var count: Int { entities.count }

    /// The contiguous array for one component, as a typed buffer.
    ///
    /// Returns nil rather than trapping when the chunk does not hold that component: a system whose
    /// query is right never sees nil, and one whose query is wrong gets an answer it can report.
    public func array<T: Component>(_ type: T.Type) -> UnsafeMutableBufferPointer<T>? {
        guardToken.check(T.componentName)
        guard let base = bases[T.componentName], strides[T.componentName] == MemoryLayout<T>.stride
        else { return nil }
        return UnsafeMutableBufferPointer(start: base.assumingMemoryBound(to: T.self), count: count)
    }
}

/// Development-build detection of a borrowed pointer that outlived its iteration.
///
/// `swift-scripting`: "development builds SHALL detect the escape at the next structural flush".
/// This is the cheaper half of that — it detects the escape at the next USE, which is strictly
/// earlier and needs no cooperation from the ECS. The structural-flush half needs the world's epoch
/// on the module side, which `world_epoch` and `borrow_valid` already provide and which
/// `ChunkSource` conformances should check.
public final class EscapeGuard {
    private var live = true
    private let systemName: String

    public init(systemName: String) {
        self.systemName = systemName
    }

    /// Called when the iteration that produced a view ends.
    public func invalidate() { live = false }

    func check(_ component: String) {
        guard !live else { return }
        Log.error("\(systemName) used a borrowed \(component) array after its iteration ended. A "
            + "borrowed component pointer is scoped to the callback that produced it; chunk storage "
            + "moves when an entity changes archetype.")
    }
}

/// Where a system's chunks come from.
///
/// A protocol because ABI 1.0 has no entry that hands a module a chunk (see the header comment). A
/// conformance over an appended `world_query_chunks` entry is the only thing missing; everything
/// above this line is complete and exercised.
public protocol ChunkSource {
    func forEachChunk(matching access: AccessSet, _ body: (ChunkView) -> Void)
}

// --- Registration -------------------------------------------------------------------------------------

/// One registered Swift system.
public struct SystemDescriptor: Sendable {
    public let name: String
    public let stage: SystemStage
    public let access: AccessSet

    public init(name: String, stage: SystemStage, access: AccessSet) {
        self.name = name
        self.stage = stage
        self.access = access
    }
}

/// The systems this module image declares.
///
/// Held on the module side because the ABI has no `register_system` entry to hand them across yet.
/// When one is appended, this is the list it reads — which is why registration is a real registry
/// rather than each `@System` macro emitting a call directly.
public enum Systems {
    public nonisolated(unsafe) private(set) static var registered: [SystemDescriptor] = []
    nonisolated(unsafe) private static var bodies: [String: (ChunkSource) -> Void] = [:]

    /// Register a system and its body.
    ///
    /// Rejects a self-conflicting access set — a query that both reads and writes one component —
    /// which the `@System` macro also catches at compile time. Both, because the macro sees only
    /// what is written in one signature and a hand-built descriptor does not go through it.
    public static func register(_ descriptor: SystemDescriptor,
                                body: @escaping (ChunkSource) -> Void) throws {
        guard !descriptor.access.isSelfConflicting else {
            throw CyberdyneError.notRepresentable(
                "\(descriptor.name): the query declares both Read and Write for "
                    + "\(descriptor.access.reads.intersection(descriptor.access.writes).sorted().joined(separator: ", "))")
        }
        registered.append(descriptor)
        bodies[descriptor.name] = body
    }

    /// Run one stage's systems against a chunk source. The engine's scheduler does this when the
    /// ABI can hand systems across; until then it is what the package's own tests run.
    public static func run(stage: SystemStage, over source: ChunkSource) {
        for descriptor in registered where descriptor.stage == stage {
            bodies[descriptor.name]?(source)
        }
    }

    /// Every pair of registered systems in one stage that may NOT run at the same time. The
    /// scheduler derives the same pairs from the same declarations, which is `swift-scripting`'s
    /// "the scheduler SHALL order them by the same conflict rules".
    public static func conflictingPairs(in stage: SystemStage) -> [(String, String)] {
        let systems = registered.filter { $0.stage == stage }
        var pairs: [(String, String)] = []
        for (index, first) in systems.enumerated() {
            for second in systems[(index + 1)...] where first.access.conflicts(with: second.access) {
                pairs.append((first.name, second.name))
            }
        }
        return pairs
    }

    static func forgetRegistrations() {
        registered.removeAll()
        bodies.removeAll()
    }
}
