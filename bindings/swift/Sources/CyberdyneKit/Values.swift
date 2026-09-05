// Values.swift — `CyVar` as a Swift value, and what may cross the boundary. Tasks 3.2, 3.5.
//
// `CyVar` is the ABI's dynamic value: a type tag, a flags word, a length, and sixteen bytes of
// payload. It is correct for tools and for anything reflective, and it is *not* the path a
// behaviour updating a transform every tick takes — that is `component_get_f32` and friends, which
// the generated overlay exposes and `Components.swift` wraps.
//
// TWO THINGS THIS FILE IS RESPONSIBLE FOR, AND THEY PULL IN OPPOSITE DIRECTIONS.
//
//   * `Value` is a Swift enum, so a game never writes a tag and a payload by hand and never gets
//     the pairing wrong. That is the whole reason for it: the failure the spike measured — reading
//     a `String`'s bits as a `Double` and getting 3.5e18 with no diagnostic — is what an untagged
//     union does to code that guesses.
//   * `CyVar` ownership is manual. A heap-backed value carries `CY_VAR_FLAG_OWNED` and must be
//     released exactly once. So every conversion in this file that ALLOCATES says so in its name
//     (`makeCyVar`) and hands back something the caller must release, and every conversion that
//     only reads (`init(reading:)`) copies out and leaves ownership alone.

import CyberdyneABI
import CyberdyneCore

/// A value crossing the ABI, with its type tag and its payload paired by the compiler.
public enum Value: Equatable, Sendable {
    case none
    case bool(Bool)
    case i64(Int64)
    case f32(Float)
    case f64(Double)
    case vec2(Vec2)
    case vec3(Vec3)
    case vec4(Vec4)
    case quat(Quat)
    case string(String)
    case bytes([UInt8])
    case entity(Entity)

    /// The ABI tag this case carries. Derived from the generated `VarType`, so a kind appended to
    /// `CyVarType` shows up here as a missing case rather than as a silent zero.
    public var type: VarType {
        switch self {
        case .none: return .nil
        case .bool: return .bool
        case .i64: return .i64
        case .f32: return .f32
        case .f64: return .f64
        case .vec2: return .vec2
        case .vec3: return .vec3
        case .vec4: return .vec4
        case .quat: return .quat
        case .string: return .string
        case .bytes: return .bytes
        case .entity: return .entity
        }
    }
}

// --- Reading a CyVar ------------------------------------------------------------------------------

extension Value {
    /// Copy a `CyVar` out into Swift. Ownership is untouched: the caller still releases what it
    /// owns, and this makes its own copy of any string or byte payload.
    ///
    /// Returns nil for a tag this overlay does not know, which is the "newer engine, older module"
    /// case: the engine may append a `CyVarType`, and a module compiled before it must not read the
    /// payload of a tag whose shape it cannot know.
    public init?(reading variable: CyVar) {
        guard let kind = VarType(rawValue: variable.type) else { return nil }
        switch kind {
        case .nil: self = .none
        case .bool: self = .bool(variable.payload.as_bool)
        case .i64: self = .i64(variable.payload.as_i64)
        case .f32: self = .f32(variable.payload.as_f32)
        case .f64: self = .f64(variable.payload.as_f64)
        case .vec2: self = .vec2(Value.vector(variable, Vec2.init(lanes:)))
        case .vec3: self = .vec3(Value.vector(variable, Vec3.init(lanes:)))
        case .vec4: self = .vec4(Value.vector(variable, Vec4.init(lanes:)))
        case .quat: self = .quat(Value.vector(variable, Quat.init(lanes:)))
        case .entity: self = .entity(Entity(bits: variable.payload.as_entity))
        case .string:
            self = .string(Value.string(variable))
        case .bytes:
            self = .bytes(Value.bytes(variable))
        }
    }

    private static func vector<T>(_ variable: CyVar, _ make: ([Float]) -> T) -> T {
        var lanes = variable.payload.as_f32x4
        return withUnsafeBytes(of: &lanes) { raw in
            make(Array(raw.bindMemory(to: Float.self)))
        }
    }

    private static func string(_ variable: CyVar) -> String {
        guard let start = variable.payload.as_bytes, variable.length > 0 else { return "" }
        let buffer = UnsafeRawBufferPointer(start: start, count: Int(variable.length))
        // The header says a string payload is UTF-8 and is NOT required to be NUL-terminated, so
        // this decodes a counted buffer. `String(cString:)` here would read past the payload.
        return String(decoding: buffer, as: UTF8.self)
    }

    private static func bytes(_ variable: CyVar) -> [UInt8] {
        guard let start = variable.payload.as_bytes, variable.length > 0 else { return [] }
        return Array(UnsafeRawBufferPointer(start: start, count: Int(variable.length))
            .bindMemory(to: UInt8.self))
    }
}

// --- Writing a CyVar ------------------------------------------------------------------------------

extension Value {
    /// Build a `CyVar` for this value and pass it to `body`, releasing it afterwards if the engine
    /// allocated one.
    ///
    /// A `withX` shape rather than a returned `CyVar` on purpose: a returned owned value is a
    /// release the caller can forget, and `var_live_count` exists precisely because that leak is
    /// the one `native-abi` asks development builds to detect. Scoping it makes forgetting
    /// impossible.
    public func withCyVar<Result>(_ body: (CyVar) throws -> Result) throws -> Result {
        guard let interface = Runtime.interface, let engine = Runtime.engineHandle else {
            throw CyberdyneError.invalidHandle
        }
        switch self {
        case let .string(text):
            var variable = text.withCString { pointer in
                interface.varMakeString(engine: engine, utf8: pointer,
                                        length: UInt64(text.utf8.count))
            }
            defer { interface.varRelease(value: &variable) }
            return try body(variable)
        case let .bytes(data):
            // An EMPTY array's `baseAddress` is nil, and `var_make_bytes(engine, NULL, 0)` is an
            // invalid argument rather than an empty value. A one-byte local gives the call an
            // address it will never read — the size is zero — so an empty `.bytes` crosses as an
            // empty value instead of as a failure.
            var empty: UInt8 = 0
            var variable = withUnsafeBytes(of: &empty) { fallback in
                data.withUnsafeBytes { raw in
                    interface.varMakeBytes(engine: engine,
                                           data: raw.baseAddress ?? fallback.baseAddress!,
                                           size: UInt64(raw.count))
                }
            }
            defer { interface.varRelease(value: &variable) }
            return try body(variable)
        default:
            return try body(inlineCyVar)
        }
    }

    /// The cases whose payload fits in the sixteen inline bytes, which is every case but the two
    /// the engine has to allocate for. No ownership, nothing to release.
    var inlineCyVar: CyVar {
        var variable = CyVar()
        variable.type = type.rawValue
        variable.flags = 0
        variable.length = 0
        switch self {
        case .none: break
        case let .bool(value): variable.payload.as_bool = value
        case let .i64(value): variable.payload.as_i64 = value
        case let .f32(value): variable.payload.as_f32 = value
        case let .f64(value): variable.payload.as_f64 = value
        case let .vec2(value): Value.store([value.x, value.y], into: &variable)
        case let .vec3(value): Value.store([value.x, value.y, value.z], into: &variable)
        case let .vec4(value): Value.store([value.x, value.y, value.z, value.w], into: &variable)
        case let .quat(value): Value.store([value.x, value.y, value.z, value.w], into: &variable)
        case let .entity(value): variable.payload.as_entity = value.bits
        case .string, .bytes:
            // Unreachable through `withCyVar`, which handles both before it gets here. Left as a
            // nil value rather than a trap: a module that traps takes the engine's process with it,
            // and `swift-scripting` requires a misbehaving behaviour to be disabled instead.
            variable.type = VarType.nil.rawValue
        }
        return variable
    }

    private static func store(_ lanes: [Float], into variable: inout CyVar) {
        withUnsafeMutableBytes(of: &variable.payload.as_f32x4) { raw in
            for (index, lane) in lanes.enumerated() {
                raw.storeBytes(of: lane, toByteOffset: index * MemoryLayout<Float>.size,
                               as: Float.self)
            }
        }
    }
}

// --- The vector types' lane initialisers -----------------------------------------------------------

extension Vec2 { init(lanes: [Float]) { self.init(x: lanes[0], y: lanes[1]) } }
extension Vec3 { init(lanes: [Float]) { self.init(x: lanes[0], y: lanes[1], z: lanes[2]) } }
extension Vec4 {
    init(lanes: [Float]) { self.init(x: lanes[0], y: lanes[1], z: lanes[2], w: lanes[3]) }
}
extension Quat {
    init(lanes: [Float]) { self.init(x: lanes[0], y: lanes[1], z: lanes[2], w: lanes[3]) }
}

// --- What a Swift type has to be to cross ----------------------------------------------------------

/// A Swift type that can be an `@Export`ed property, a component field, or a serialized behaviour
/// field.
///
/// `swift-scripting` requires the macros to reject a non-representable exported type at COMPILE
/// time, and the `@Behaviour` macro does that syntactically. This protocol is the other half: it is
/// what makes the rejection meaningful, because conformance is the definition of representable and
/// it is closed — the conformances below are all of them, and a game cannot add one for a class.
public protocol Exportable: Sendable {
    /// The value that crosses the boundary.
    var cyValue: Value { get }
    /// Rebuild from a value, or nil when the blob carried a different kind — which is a migration
    /// that changed a field's type, and dropping the field is the right answer rather than
    /// reinterpreting the bits.
    init?(cyValue: Value)
}

extension Bool: Exportable {
    public var cyValue: Value { .bool(self) }
    public init?(cyValue: Value) {
        guard case let .bool(value) = cyValue else { return nil }
        self = value
    }
}

extension Int64: Exportable {
    public var cyValue: Value { .i64(self) }
    public init?(cyValue: Value) {
        guard case let .i64(value) = cyValue else { return nil }
        self = value
    }
}

extension Int32: Exportable {
    public var cyValue: Value { .i64(Int64(self)) }
    public init?(cyValue: Value) {
        guard case let .i64(value) = cyValue, let narrowed = Int32(exactly: value) else {
            return nil
        }
        self = narrowed
    }
}

extension Int: Exportable {
    public var cyValue: Value { .i64(Int64(self)) }
    public init?(cyValue: Value) {
        guard case let .i64(value) = cyValue, let narrowed = Int(exactly: value) else { return nil }
        self = narrowed
    }
}

extension Float: Exportable {
    public var cyValue: Value { .f32(self) }
    public init?(cyValue: Value) {
        guard case let .f32(value) = cyValue else { return nil }
        self = value
    }
}

extension Double: Exportable {
    public var cyValue: Value { .f64(self) }
    public init?(cyValue: Value) {
        guard case let .f64(value) = cyValue else { return nil }
        self = value
    }
}

extension String: Exportable {
    public var cyValue: Value { .string(self) }
    public init?(cyValue: Value) {
        guard case let .string(value) = cyValue else { return nil }
        self = value
    }
}

extension Vec2: Exportable {
    public var cyValue: Value { .vec2(self) }
    public init?(cyValue: Value) {
        guard case let .vec2(value) = cyValue else { return nil }
        self = value
    }
}

extension Vec3: Exportable {
    public var cyValue: Value { .vec3(self) }
    public init?(cyValue: Value) {
        guard case let .vec3(value) = cyValue else { return nil }
        self = value
    }
}

extension Vec4: Exportable {
    public var cyValue: Value { .vec4(self) }
    public init?(cyValue: Value) {
        guard case let .vec4(value) = cyValue else { return nil }
        self = value
    }
}

extension Quat: Exportable {
    public var cyValue: Value { .quat(self) }
    public init?(cyValue: Value) {
        guard case let .quat(value) = cyValue else { return nil }
        self = value
    }
}

extension Entity: Exportable {
    public var cyValue: Value { .entity(self) }
    public init?(cyValue: Value) {
        guard case let .entity(value) = cyValue else { return nil }
        self = value
    }
}
