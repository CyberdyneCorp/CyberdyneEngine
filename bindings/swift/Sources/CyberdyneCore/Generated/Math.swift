// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/swift/overlay_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just generate-swift`, and `just generate-swift --check` fails when this file is stale.

// The math types, generated from `CyVarType`'s vector kinds.
//
// They have no C struct to mirror: the ABI carries a vector in `CyVarPayload.as_f32x4`, four floats
// with the used lanes at the front. So the layout claim these make is against that array, and
// `Tests/CyberdyneCoreTests/Generated/LayoutTests.swift` asserts it — a `Vec3` must be three
// contiguous floats or `withUnsafeBytes` into the payload writes the wrong thing.
//
// `@frozen` because the layout IS the ABI: a future field would change what crosses the boundary,
// which is a change to `CyVarPayload` and not to this file.

@frozen
public struct Quat: Equatable, Sendable {
    public var x: Float
    public var y: Float
    public var z: Float
    public var w: Float

    @inlinable
    public init(x: Float = 0, y: Float = 0, z: Float = 0, w: Float = 0) {
        self.x = x
        self.y = y
        self.z = z
        self.w = w
    }

    public static let zero = Quat(x: 0, y: 0, z: 0, w: 0)

    /// The lane count, so that generic code over the math types does not repeat the number.
    public static var lanes: Int { 4 }
}

@frozen
public struct Vec2: Equatable, Sendable {
    public var x: Float
    public var y: Float

    @inlinable
    public init(x: Float = 0, y: Float = 0) {
        self.x = x
        self.y = y
    }

    public static let zero = Vec2(x: 0, y: 0)

    /// The lane count, so that generic code over the math types does not repeat the number.
    public static var lanes: Int { 2 }
}

@frozen
public struct Vec3: Equatable, Sendable {
    public var x: Float
    public var y: Float
    public var z: Float

    @inlinable
    public init(x: Float = 0, y: Float = 0, z: Float = 0) {
        self.x = x
        self.y = y
        self.z = z
    }

    public static let zero = Vec3(x: 0, y: 0, z: 0)

    /// The lane count, so that generic code over the math types does not repeat the number.
    public static var lanes: Int { 3 }
}

@frozen
public struct Vec4: Equatable, Sendable {
    public var x: Float
    public var y: Float
    public var z: Float
    public var w: Float

    @inlinable
    public init(x: Float = 0, y: Float = 0, z: Float = 0, w: Float = 0) {
        self.x = x
        self.y = y
        self.z = z
        self.w = w
    }

    public static let zero = Vec4(x: 0, y: 0, z: 0, w: 0)

    /// The lane count, so that generic code over the math types does not repeat the number.
    public static var lanes: Int { 4 }
}
