// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/swift/overlay_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just generate-swift`, and `just generate-swift --check` fails when this file is stale.

/// `CyResult`, as a Swift enum. `swift-scripting`: "Swift `enum`s for ABI enums".
///
/// The raw values are the C enumerators' own, so `Status(rawValue:)` over a `CyResult` cannot
/// reorder them. The first fifteen are `cy::ErrorCode`'s, in its order — see cy_abi.h, which
/// explains why that is a cast in the engine rather than a switch.
public enum Status: Int32, Sendable, CaseIterable {
    case ok = 0
    case unknown = 1
    case invalidArgument = 2
    case outOfRange = 3
    case notFound = 4
    case alreadyExists = 5
    case permissionDenied = 6
    case unsupported = 7
    case notImplemented = 8
    case unavailable = 9
    case timeout = 10
    case outOfMemory = 11
    case bufferTooSmall = 12
    case io = 13
    case `internal` = 14
    case versionMismatch = 100
    case schemaTooNew = 101
    case schemaUnmigratable = 102
    case moduleLoadFailed = 103
}

/// `CyVarType`: the kinds a value may carry across the boundary.
public enum VarType: UInt32, Sendable, CaseIterable {
    case `nil` = 0
    case bool = 1
    case i64 = 2
    case f32 = 3
    case f64 = 4
    case vec2 = 5
    case vec3 = 6
    case vec4 = 7
    case quat = 8
    case string = 9
    case bytes = 10
    case entity = 11
    case i8 = 12
    case i16 = 13
    case i32 = 14
    case u8 = 15
    case u16 = 16
    case u32 = 17
    case u64 = 18
    case fixed = 19
}

/// `CyInitLevel`: when a module registers what. Types are registered at `.scene`.
public enum InitLevel: UInt32, Sendable, CaseIterable {
    case core = 0
    case servers = 1
    case scene = 2
    case editor = 3
}

/// `CySeverity`: the levels the engine's diagnostic system carries, and the wire values `log` takes.
///
/// GENERATED, BECAUSE THE COPY WAS WRONG. CyberdyneKit hand-wrote this enum with six cases —
/// trace, debug, info, warning, error, fatal — against the engine's three, so `Log.info` put 2 on
/// the wire and every informational line from a behaviour arrived in the engine's log as `[error]`.
/// It ran green for a milestone. There is nothing to copy now: cy_abi.h declares `CySeverity`,
/// src/abi/src/interface.cpp asserts each value against `cy::DiagnosticSeverity`, and this file is
/// generated from the same description the compatibility gate diffs.
public enum Severity: UInt32, Sendable, CaseIterable {
    case info = 0
    case warning = 1
    case error = 2
}

/// `CyStage`: the stages of one frame, in execution order.
///
/// Also generated, and also replacing a copy. `SystemStage` was hand-written in CyberdyneKit with a
/// comment saying "there is no `CyStage` in `cy_abi.h`, so nothing checks that this list still
/// matches"; the fix was the appended enum rather than more care.
public enum SystemStage: UInt32, Sendable, CaseIterable {
    case preSimulation = 0
    case physics = 1
    case simulation = 2
    case postSimulation = 3
    case frame = 4
    case animation = 5
    case ui = 6
    case render = 7
}

/// `CyPhase`: the update phase an ABI 1.3 entry is called in.
public enum Phase: UInt32, Sendable, CaseIterable {
    case none = 0
    case fixedUpdate = 1
    case frameUpdate = 2
}

/// `CyShapeKind`: the shapes a physics query sweeps or overlaps.
public enum ShapeKind: UInt32, Sendable, CaseIterable {
    case sphere = 0
    case capsule = 1
    case box = 2
}

/// `CyNavPathStatus`: where a crowd agent is on its way to a target.
public enum NavPathStatus: UInt32, Sendable, CaseIterable {
    case idle = 0
    case computing = 1
    case following = 2
    case arrived = 3
    case failed = 4
}

/// `CyNavQueryState`: an asynchronous path search's state.
public enum NavQueryState: UInt32, Sendable, CaseIterable {
    case pending = 0
    case ready = 1
    case consumed = 2
    case cancelled = 3
}

/// `CyAccessMode`: what a scheduled system declares about one component.
public enum AccessMode: UInt32, Sendable, CaseIterable {
    case read = 0
    case write = 1
    case exclude = 2
}

/// `CyGroundState`: what a character controller is standing on.
public enum GroundState: UInt32, Sendable, CaseIterable {
    case grounded = 0
    case steepSlope = 1
    case inAir = 2
}

/// `CyUiEventKind`: what happened to an interface element.
public enum UIEventKind: UInt32, Sendable, CaseIterable {
    case click = 0
    case focus = 1
    case blur = 2
}

/// `CyUiKind`: what an interface element is, and so which writes it takes.
public enum UIKind: UInt32, Sendable, CaseIterable {
    case panel = 0
    case label = 1
    case image = 2
    case progress = 3
    case button = 4
}

/// `CyUiLayoutModel`: how an element lays out its children.
public enum UILayoutModel: UInt32, Sendable, CaseIterable {
    case flex = 0
    case grid = 1
    case absolute = 2
}

/// `CyUiDirection`: a flex container's main axis.
public enum UIDirection: UInt32, Sendable, CaseIterable {
    case row = 0
    case column = 1
    case rowReverse = 2
    case columnReverse = 3
}

/// `CyUiJustify`: distribution along a flex container's main axis.
public enum UIJustify: UInt32, Sendable, CaseIterable {
    case start = 0
    case centre = 1
    case end = 2
    case spaceBetween = 3
    case spaceAround = 4
    case spaceEvenly = 5
}

/// `CyUiAlign`: alignment across the cross axis; stretch is zero.
public enum UIAlign: UInt32, Sendable, CaseIterable {
    case stretch = 0
    case start = 1
    case centre = 2
    case end = 3
}

/// `CyUiVisibility`: shown, hidden, or out of layout too.
public enum UIVisibility: UInt32, Sendable, CaseIterable {
    case visible = 0
    case hidden = 1
    case collapsed = 2
}

/// `CyRootMotionMode`: where an animator's root motion goes.
public enum RootMotionMode: UInt32, Sendable, CaseIterable {
    case ignore = 0
    case transform = 1
    case accumulate = 2
    case extract = 3
    case character = 4
}

/// `CyAnimationTier`: an animator's level of detail.
public enum AnimationTier: UInt32, Sendable, CaseIterable {
    case full = 0
    case simplified = 1
    case cached = 2
    case baked = 3
}

/// `CyDetmathFunction`: a deterministic math function over a span.
public enum DetmathFunction: UInt32, Sendable, CaseIterable {
    case sqrt = 0
    case sin = 1
    case cos = 2
    case tan = 3
    case atan = 4
    case atan2 = 5
    case asin = 6
    case acos = 7
    case exp2 = 8
    case log2 = 9
    case exp = 10
    case log = 11
    case pow = 12
}

/// `CyLockstepOrderKind`: what an order tells a lockstep group.
public enum LockstepOrderKind: UInt32, Sendable, CaseIterable {
    case move = 0
    case stop = 1
}

/// The error every throwing overlay call raises.
///
/// `swift-scripting`: "the overlay SHALL throw a typed `CyberdyneError` carrying the status and the
/// engine's last-error message", and, separately, "accessing it SHALL return `nil` or throw a
/// `CyberdyneError.invalidHandle`". Both spellings are cases of one enum so that a single
/// `catch` covers the boundary.
public enum CyberdyneError: Error, Sendable, Equatable {
    /// An ABI call returned a failure status. The message is the engine's `get_last_error` at the
    /// moment of the failure, copied — the C pointer is only valid until this thread's next one.
    case status(Status, message: String)
    /// A handle whose target no longer exists, or was never valid.
    case invalidHandle
    /// A value that cannot cross the boundary in a `CyVar`: `swift-scripting`'s "non-representable
    /// exported types". Carries the Swift type's name.
    case notRepresentable(String)
}

extension CyberdyneError: CustomStringConvertible {
    public var description: String {
        switch self {
        case let .status(status, message):
            return message.isEmpty ? "\(status)" : "\(status): \(message)"
        case .invalidHandle:
            return "invalid handle"
        case let .notRepresentable(type):
            return "\(type) is not representable across the ABI"
        }
    }
}
