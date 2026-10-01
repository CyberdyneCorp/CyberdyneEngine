// Systems.swift — the data-oriented programming model, over the ECS's access declarations.
// Task 3.3; scheduled by the engine since ABI 1.5 (`add-swift-m12-gaps`).
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
// --- HOW A SYSTEM REACHES THE ENGINE'S SCHEDULER (ABI 1.5) ----------------------------------------
//
// `Systems.register`, in a module bound to an engine, also calls `register_system`: the access set
// becomes `CySystemAccess` terms (each component resolved by name in the bound world, so the
// component must be registered first — `GameModule.components` is registered before `systems`), the
// stage is the one the attribute named, and the body is a C thunk over a retained record.
// `cy::abi::ScriptSystems` puts it in the stage beside the native systems and orders it by the same
// conflict rules. When the stage runs, the thunk hands the body an `EngineChunkSource` over
// `world_chunks` — the chunks of every archetype holding every component the query reads or
// writes, and none that holds a `Without` — and invalidates every view when the body returns.
//
// A body runs in its stage's phase (F for the simulation stages, U for the frame ones), possibly on a
// job worker beside other systems it does not conflict with, and the world is iterating while it
// runs: a structural call (create, destroy, add, remove) throws `.unavailable`.

import CyberdyneABI
import CyberdyneCore

// `SystemStage` USED TO BE DECLARED HERE as a copy of `cy::ecs::Stage`, and its own comment said
// what was wrong with that: "there is no `CyStage` in `cy_abi.h`, so nothing checks that this list
// still matches. That is exactly the drift the generated overlay exists to prevent, and the fix is
// an appended enum rather than more care here."
//
// ABI 1.1 appended it. `SystemStage` is now generated into CyberdyneCore/Generated/Enums.swift from
// `CyStage`, and src/abi/src/interface.cpp asserts each enumerator — and the stage COUNT — against
// the engine's own, so adding a stage without extending the ABI is a compile error in the engine
// rather than a Swift system scheduled into the wrong phase.
//
// The one thing the enum cannot carry is the fixed-step split, because it is a property of the
// order rather than of a value. It lives here as an extension, next to the systems that ask.
extension SystemStage {
    /// True for the four stages that run on the fixed simulation step. `cy::ecs::stage_is_fixed_step`
    /// spells the same comparison, and cy_abi.h states the rule where `CyStage` is declared.
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

    public init(
        entities: UnsafeBufferPointer<CyEntity>,
        bases: [String: UnsafeMutableRawPointer],
        strides: [String: Int],
        guardToken: EscapeGuard
    ) {
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
        Log.error(
            "\(systemName) used a borrowed \(component) array after its iteration ended. A "
                + "borrowed component pointer is scoped to the callback that produced it; chunk storage "
                + "moves when an entity changes archetype.")
    }
}

/// Where a system's chunks come from: the engine's world (`EngineChunkSource`) when the scheduler
/// runs it, or a test's own source when the package's suite does.
public protocol ChunkSource {
    func forEachChunk(matching access: AccessSet, _ body: (ChunkView) -> Void)
}

/// The engine's chunks, through `world_chunks`. ABI 1.5.
///
/// One `world_chunks` call per component the query names, then a join by archetype: chunks are
/// listed "in archetype then chunk order", so the k-th chunk of an archetype is the same chunk for
/// every component it holds — the same entity array — and a view over it carries one column per
/// component. An archetype that lacks a component the query reads or writes is skipped, and so is
/// one that holds a component it excludes. No per-entity call, no `CyVar`.
public struct EngineChunkSource: ChunkSource {
    public let world: World
    private let guardToken: EscapeGuard

    public init(world: World, guardToken: EscapeGuard) {
        self.world = world
        self.guardToken = guardToken
    }

    public func forEachChunk(matching access: AccessSet, _ body: (ChunkView) -> Void) {
        let required = access.reads.union(access.writes).sorted()
        guard let lead = required.first else { return }
        var columns: [String: [UInt32: [CyChunk]]] = [:]
        for name in required {
            guard let chunks = chunks(of: name) else { return }  // not registered: nothing matches
            columns[name] = Dictionary(grouping: chunks, by: \.archetype)
        }
        let excluded = Set(access.excludes.flatMap { chunks(of: $0)?.map(\.archetype) ?? [] })
        var seen: Set<UInt32> = []
        for chunk in chunks(of: lead) ?? [] {
            guard seen.insert(chunk.archetype).inserted, !excluded.contains(chunk.archetype) else {
                continue
            }
            visit(archetype: chunk.archetype, columns: columns, required: required, body)
        }
    }

    /// Every chunk of one archetype, as views carrying every required column.
    private func visit(
        archetype: UInt32, columns: [String: [UInt32: [CyChunk]]], required: [String],
        _ body: (ChunkView) -> Void
    ) {
        guard let leading = columns[required[0]]?[archetype] else { return }
        for (index, chunk) in leading.enumerated() {
            var bases: [String: UnsafeMutableRawPointer] = [:]
            var strides: [String: Int] = [:]
            for name in required {
                guard let column = columns[name]?[archetype], index < column.count else { return }
                if let data = column[index].data {
                    bases[name] = data
                    strides[name] = Int(column[index].stride)
                }
            }
            let entities = UnsafeBufferPointer(start: chunk.entities, count: Int(chunk.entity_count))
            body(ChunkView(entities: entities, bases: bases, strides: strides, guardToken: guardToken))
        }
    }

    private func chunks(of name: String) -> [CyChunk]? {
        let id = name.withCString { world.findComponent(name: $0) }
        guard id != CY_COMPONENT_TYPE_INVALID else { return nil }
        return try? Physics.sized { buffer, capacity, count in
            try world.chunks(component: id, into: buffer, capacity: capacity, count: count)
        }
    }
}

/// What the `@System` macro emits for each system: its descriptor and how to register it. A game
/// lists them in `GameModule.systems`.
public protocol SystemRegistration {
    static var descriptor: SystemDescriptor { get }
    static func register() throws
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

/// The systems this module image declares, and — when the image is bound to an engine — what the
/// engine's scheduler was handed for each.
public enum Systems {
    public nonisolated(unsafe) private(set) static var registered: [SystemDescriptor] = []
    nonisolated(unsafe) private static var bodies: [String: (ChunkSource) -> Void] = [:]

    /// Register a system and its body.
    ///
    /// Rejects a self-conflicting access set — a query that both reads and writes one component —
    /// which the `@System` macro also catches at compile time. Both, because the macro sees only
    /// what is written in one signature and a hand-built descriptor does not go through it.
    public static func register(
        _ descriptor: SystemDescriptor,
        body: @escaping (ChunkSource) -> Void
    ) throws {
        guard !descriptor.access.isSelfConflicting else {
            throw CyberdyneError.notRepresentable(
                "\(descriptor.name): the query declares both Read and Write for "
                    + "\(descriptor.access.reads.intersection(descriptor.access.writes).sorted().joined(separator: ", "))"
            )
        }
        if Runtime.isBoundToEngine {
            try EngineSystems.register(descriptor, body: body)
        }
        registered.append(descriptor)
        bodies[descriptor.name] = body
    }

    /// Run one stage's systems against a chunk source, in this process. The engine's scheduler runs
    /// a bound module's systems itself; this is what the package's own tests run.
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
            for second in systems[(index + 1)...] where first.access.conflicts(with: second.access)
            {
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

// --- Handing a system to the engine (ABI 1.5) ---------------------------------------------------------

/// One system as the engine holds it: the body the C thunk calls. Retained for the life of the
/// image, like a behaviour's registration — the engine keeps `user_data`, and a retired image is
/// never unloaded, so there is no later moment at which releasing it is safe.
final class SystemRecord {
    let name: String
    let body: (ChunkSource) -> Void

    init(name: String, body: @escaping (ChunkSource) -> Void) {
        self.name = name
        self.body = body
    }
}

enum EngineSystems {
    /// `register_system` for one descriptor. Throws when a component is not registered in the bound
    /// world, when the query names a resource (the ABI has no resource entry), or when the engine
    /// refuses — a reload that changed this system's stage or access among them.
    static func register(_ descriptor: SystemDescriptor, body: @escaping (ChunkSource) -> Void)
        throws
    {
        let engine = try GameServices.engine()
        guard let world = Runtime.world else {
            throw CyberdyneError.status(.unavailable, message: "no world is bound to this module")
        }
        let access = try terms(descriptor.access, in: world, system: descriptor.name)
        let record = SystemRecord(name: descriptor.name, body: body)
        try access.withUnsafeBufferPointer { terms in
            var desc = CySystemDesc()
            desc.struct_size = UInt32(MemoryLayout<CySystemDesc>.size)
            desc.stage = descriptor.stage.rawValue
            // Retained, not borrowed: the engine keeps the name for the registration's life.
            desc.name = RetainedCString.make(descriptor.name)
            desc.access = terms.baseAddress
            desc.access_count = UInt32(terms.count)
            desc.run = systemRun
            desc.user_data = Unmanaged.passRetained(record).toOpaque()
            try engine.registerSystem(desc: &desc)
        }
    }

    /// The access set as `CySystemAccess` terms, sorted by name so the declaration is the same bytes
    /// on every registration of the same query.
    static func terms(_ access: AccessSet, in world: World, system: String) throws
        -> [CySystemAccess]
    {
        let modes: [(Set<String>, AccessMode)] = [
            (access.reads, .read), (access.writes, .write), (access.excludes, .exclude),
        ]
        var terms: [CySystemAccess] = []
        for (names, mode) in modes {
            for name in names.sorted() {
                guard !name.hasPrefix("res:") else {
                    throw CyberdyneError.notRepresentable(
                        "\(system): a Res<...> term has no engine entry to read a resource through")
                }
                let id = name.withCString { world.findComponent(name: $0) }
                guard id != CY_COMPONENT_TYPE_INVALID else {
                    throw CyberdyneError.status(
                        .notFound,
                        message: "\(system): component \(name) is not registered in the world; "
                            + "list it in GameModule.components")
                }
                terms.append(CySystemAccess(component: id, mode: mode.rawValue))
            }
        }
        return terms
    }
}

/// `CySystemDesc.run`: the body, over the engine's chunks, with every view invalidated when it
/// returns. Possibly on a job worker; it reads only the record and the bound table.
private let systemRun:
    @convention(c) (CyEngine?, CyWorld?, UnsafeMutableRawPointer?) -> Void = {
        _, world, userData in
        guard let world, let userData, let interface = Runtime.interface else { return }
        let record = Unmanaged<SystemRecord>.fromOpaque(userData).takeUnretainedValue()
        let guardToken = EscapeGuard(systemName: record.name)
        record.body(EngineChunkSource(world: World(world, interface), guardToken: guardToken))
        guardToken.invalidate()
    }
