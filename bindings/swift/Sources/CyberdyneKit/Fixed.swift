// SPDX-License-Identifier: MIT
// Fixed.swift — the deterministic math scalar, angle and vectors, in Swift. ABI 1.8,
// openspec/changes/add-deterministic-math stage 8 (design §13).
//
//     let speed = Fixed(4)                                  // an integer, exactly
//     let step = speed * Fixed(raw: Fixed.oneRaw / 60)      // the engine's `*`, bit for bit
//     let heading = Angle.quarter + Angle(raw: 1 << 28)     // wraps, exactly
//     let target = FixedVec2(x: Fixed(cooking: hit.x), y: Fixed(cooking: hit.z))  // command creation
//
// THE RULES ARE THE ENGINE'S (src/core/detmath/include/cy/core/detmath/fixed.h, design §4.3), and
// this file is their second statement — a third, counting tools/detmath/model.py. Each operator
// produces the bits the C++ operator produces, and `FixedTests` holds them to the committed golden
// vectors under tools/detmath/vectors/ and to the C++ sweep digests in digests.txt:
//
//   + - and unary -   two's-complement wrapping: `&+`, `&-`. Swift's `+` TRAPS on overflow, which
//                     the engine's rule does not; a trap is a different answer, not a safer one.
//   *                 the exact 128-bit product (`multipliedFullWidth(by:)`), plus 2^31, shifted
//                     right arithmetically by 32, wrapped to 64 bits: round to nearest, ties toward
//                     +infinity.
//   /                 the exact quotient of (a << 32) / b, truncated toward zero, wrapped. Division
//                     by zero gives `max` for a >= 0 and `min` for a < 0. The long division is done
//                     on magnitudes with `UInt64.dividingFullWidth` one limb at a time, because
//                     `Int64.dividingFullWidth` traps when the quotient does not fit and the engine
//                     wraps.
//   comparison        of the raw value: total, no NaN.
//
// THE TRANSCENDENTALS ARE THE ENGINE'S, NOT PORTED. `Detmath.sin`, `atan2`, `sqrt`, … call ABI 1.8's
// `detmath_*` entries, so there is one implementation of each polynomial (design §13: "a second
// copy in Swift would be a second implementation to keep bit-identical").
//
// THE FLOAT BOUNDARY. `Fixed(cooking:)` is `detmath::from_f64_cooked`: the value times 2^32,
// rounded to nearest-even, saturating — permitted at cook, at session configuration and when the
// issuing peer creates a command (design §7.1), and never inside a fixed step's arithmetic.

import CyberdyneABI
import CyberdyneCore

// --- Fixed ---------------------------------------------------------------------------------------

/// A Q32.32 fixed-point number, `raw / 2^32`: `cy::detmath::Fixed`. The type of every authoritative
/// quantity in a `CrossPlatform` or `Lockstep` session.
@frozen
public struct Fixed: Hashable, Comparable, Sendable, CustomStringConvertible {
    /// The fractional bits.
    public static let fractionBits = 32
    /// The raw value of 1.0.
    public static let oneRaw: Int64 = 1 << 32

    /// The value times 2^32: what is hashed, serialised and sent across the ABI.
    public var raw: Int64

    /// The value whose raw representation is `raw`.
    @inlinable
    public init(raw: Int64) {
        self.raw = raw
    }

    /// An integer. Every `Int32` is exact.
    @inlinable
    public init(_ value: Int32) {
        raw = Int64(value) << 32
    }

    /// `value` as the engine cooks it (`detmath::from_f64_cooked`): times 2^32, rounded to nearest
    /// with ties to even, saturating beyond the range; NaN is zero. For cook time, session
    /// configuration and command creation only.
    public init(cooking value: Double) {
        if value.isNaN {
            raw = 0
            return
        }
        let scaled = (value * 4_294_967_296.0).rounded(.toNearestOrEven)
        // 2^63 is the first double past the range; every double below it converts exactly.
        if scaled >= 9_223_372_036_854_775_808.0 {
            raw = .max
        } else if scaled < -9_223_372_036_854_775_808.0 {
            raw = .min
        } else {
            raw = Int64(scaled)
        }
    }

    /// `value` as the engine cooks it, through the `Double` it widens to exactly.
    public init(cooking value: Float) {
        self.init(cooking: Double(value))
    }

    /// 0.
    public static let zero = Fixed(raw: 0)
    /// 1.
    public static let one = Fixed(raw: oneRaw)
    /// 1/2.
    public static let half = Fixed(raw: oneRaw / 2)
    /// The smallest positive value, 2^-32.
    public static let epsilon = Fixed(raw: 1)
    /// The largest value, 2^31 - 2^-32.
    public static let max = Fixed(raw: .max)
    /// The smallest value, -2^31.
    public static let min = Fixed(raw: .min)

    /// The largest integer not above the value.
    @inlinable
    public var floor: Int64 { raw >> 32 }

    /// The value as a `Double`, for PRESENTATION only: exact within ±2^21, correctly rounded beyond.
    @inlinable
    public var presentation: Double { Double(raw) / 4_294_967_296.0 }

    /// Raw comparison: total, because there is no NaN.
    @inlinable
    public static func < (a: Fixed, b: Fixed) -> Bool { a.raw < b.raw }

    /// The raw value and the value it approximates.
    public var description: String { "Fixed(\(presentation), raw: \(raw))" }

    // --- Arithmetic: design §4.3 -----------------------------------------------------------------

    /// `a + b`, wrapped.
    @inlinable
    public static func + (a: Fixed, b: Fixed) -> Fixed { Fixed(raw: a.raw &+ b.raw) }

    /// `a - b`, wrapped.
    @inlinable
    public static func - (a: Fixed, b: Fixed) -> Fixed { Fixed(raw: a.raw &- b.raw) }

    /// `-a`, wrapped: `-Fixed.min` is `Fixed.min`.
    @inlinable
    public static prefix func - (a: Fixed) -> Fixed { Fixed(raw: 0 &- a.raw) }

    /// `a * b`: the exact product, rounded to nearest with ties toward +infinity, wrapped.
    @inlinable
    public static func * (a: Fixed, b: Fixed) -> Fixed {
        let product = a.raw.multipliedFullWidth(by: b.raw)
        // + 2^31, carrying into the high limb.
        let (low, carried) = product.low.addingReportingOverflow(1 << 31)
        let high = UInt64(bitPattern: product.high) &+ (carried ? 1 : 0)
        // The arithmetic shift right by 32 of (high, low), keeping its low 64 bits: the wrap.
        return Fixed(raw: Int64(bitPattern: (low >> 32) | (high << 32)))
    }

    /// `a / b`: `(a << 32) / b` truncated toward zero, wrapped. Division by zero gives `max` for
    /// `a >= 0` and `min` for `a < 0`.
    @inlinable
    public static func / (a: Fixed, b: Fixed) -> Fixed {
        if b.raw == 0 {
            return a.raw >= 0 ? .max : .min
        }
        // |a| << 32 as (high, low): the magnitude of a 96-bit numerator.
        let magnitude = a.raw.magnitude
        let numeratorHigh = magnitude >> 32
        let numeratorLow = magnitude << 32
        let divisor = b.raw.magnitude
        // Long division, one 64-bit limb at a time, so neither step can overflow: the remainder of
        // the high limb is below the divisor, which is `dividingFullWidth`'s precondition. The high
        // limb's quotient is not needed: the wrap keeps the low 64 bits of the 128-bit quotient.
        let remainder = numeratorHigh % divisor
        let quotientLow = divisor.dividingFullWidth((high: remainder, low: numeratorLow)).quotient
        let negative = (a.raw < 0) != (b.raw < 0)
        return Fixed(raw: Int64(bitPattern: negative ? 0 &- quotientLow : quotientLow))
    }

    /// `a = a + b`, wrapped.
    @inlinable
    public static func += (a: inout Fixed, b: Fixed) { a = a + b }
    /// `a = a - b`, wrapped.
    @inlinable
    public static func -= (a: inout Fixed, b: Fixed) { a = a - b }
    /// `a = a * b`, rounded and wrapped as `*`.
    @inlinable
    public static func *= (a: inout Fixed, b: Fixed) { a = a * b }
    /// `a = a / b`, truncated and wrapped as `/`.
    @inlinable
    public static func /= (a: inout Fixed, b: Fixed) { a = a / b }

    /// `a >> shift`: arithmetic, so it rounds toward -infinity.
    @inlinable
    public static func >> (a: Fixed, shift: Int) -> Fixed { Fixed(raw: a.raw >> shift) }

    /// `|a|`, wrapped: `abs(Fixed.min)` is `Fixed.min`.
    @inlinable
    public var magnitude: Fixed { raw < 0 ? -self : self }
}

// --- Fixed16 -------------------------------------------------------------------------------------

/// A Q16.16 storage form, `cy::detmath::Fixed16`: never computed with, widened to `Fixed`.
@frozen
public struct Fixed16: Hashable, Comparable, Sendable {
    /// The value times 2^16.
    public var raw: Int32

    /// The value whose raw representation is `raw`.
    @inlinable
    public init(raw: Int32) {
        self.raw = raw
    }

    /// `value`, rounded to nearest with ties toward +infinity; out of range saturates.
    @inlinable
    public init(narrowing value: Fixed) {
        // floor((raw + 2^15) / 2^16) without forming raw + 2^15, which could overflow.
        let rounded = (value.raw >> 16) + ((value.raw >> 15) & 1)
        raw = Int32(clamping: rounded)
    }

    /// The same value as a `Fixed`. Exact.
    @inlinable
    public var widened: Fixed { Fixed(raw: Int64(raw) << 16) }

    /// Raw comparison: total, because there is no NaN.
    @inlinable
    public static func < (a: Fixed16, b: Fixed16) -> Bool { a.raw < b.raw }
}

// --- Angle ---------------------------------------------------------------------------------------

/// A binary angle, `raw / 2^32` of a turn counter-clockwise: `cy::detmath::Angle`. Arithmetic
/// wraps, exactly. Not ordered — on a circle "less than" does not survive wrapping.
@frozen
public struct Angle: Hashable, Sendable {
    /// The angle in units of 2^-32 turn.
    public var raw: UInt32

    /// The angle whose raw representation is `raw`.
    @inlinable
    public init(raw: UInt32) {
        self.raw = raw
    }

    /// `turns` modulo one turn. Exact: the fractional bits of a `Fixed` ARE a binary angle.
    @inlinable
    public init(turns: Fixed) {
        raw = UInt32(truncatingIfNeeded: turns.raw)
    }

    /// No turn.
    public static let zero = Angle(raw: 0)
    /// 45 degrees.
    public static let eighth = Angle(raw: 1 << 29)
    /// 90 degrees.
    public static let quarter = Angle(raw: 1 << 30)
    /// 180 degrees.
    public static let half = Angle(raw: 1 << 31)

    /// The angle in turns, in [0, 1). Exact.
    @inlinable
    public var turns: Fixed { Fixed(raw: Int64(raw)) }
    /// The angle in turns, in [-1/2, 1/2). Exact.
    @inlinable
    public var signedTurns: Fixed { Fixed(raw: Int64(Int32(bitPattern: raw))) }

    /// `a + b`, modulo one turn.
    @inlinable
    public static func + (a: Angle, b: Angle) -> Angle { Angle(raw: a.raw &+ b.raw) }
    /// `a - b`, modulo one turn.
    @inlinable
    public static func - (a: Angle, b: Angle) -> Angle { Angle(raw: a.raw &- b.raw) }
    /// The negation, wrapped.
    @inlinable
    public static prefix func - (a: Angle) -> Angle { Angle(raw: 0 &- a.raw) }

    /// `a * k` through the exact product, rounded to the nearest 2^-32 turn (ties toward +infinity),
    /// modulo one turn.
    @inlinable
    public static func * (a: Angle, k: Fixed) -> Angle {
        let product = Int64(a.raw).multipliedFullWidth(by: k.raw)
        let low = product.low &+ (1 << 31)
        return Angle(raw: UInt32(truncatingIfNeeded: low >> 32))
    }
}

// --- Vectors -------------------------------------------------------------------------------------

/// A planar vector of `Fixed`: `cy::detmath::FixedVec2`. The lockstep mover reads `x` as world X
/// and `y` as world Z.
@frozen
public struct FixedVec2: Hashable, Sendable {
    /// The plane's first axis: world X.
    public var x: Fixed
    /// The plane's second axis: world Z, for the lockstep mover.
    public var y: Fixed

    /// A vector from its components; the origin by default.
    @inlinable
    public init(x: Fixed = .zero, y: Fixed = .zero) {
        self.x = x
        self.y = y
    }

    /// The origin.
    public static let zero = FixedVec2()

    /// The sum, wrapped component by component.
    @inlinable
    public static func + (a: FixedVec2, b: FixedVec2) -> FixedVec2 {
        FixedVec2(x: a.x + b.x, y: a.y + b.y)
    }
    /// The difference, wrapped component by component.
    @inlinable
    public static func - (a: FixedVec2, b: FixedVec2) -> FixedVec2 {
        FixedVec2(x: a.x - b.x, y: a.y - b.y)
    }
    /// The negation, wrapped.
    @inlinable
    public static prefix func - (a: FixedVec2) -> FixedVec2 { FixedVec2(x: -a.x, y: -a.y) }
    /// Each component times `k`, rounded as `Fixed`'s `*` rounds.
    @inlinable
    public static func * (v: FixedVec2, k: Fixed) -> FixedVec2 { FixedVec2(x: v.x * k, y: v.y * k) }

    /// As the ABI carries it.
    @inlinable
    public var abi: CyFixedVec2 { CyFixedVec2(x: x.raw, y: y.raw) }
    /// From the ABI's spelling.
    @inlinable
    public init(_ raw: CyFixedVec2) {
        x = Fixed(raw: raw.x)
        y = Fixed(raw: raw.y)
    }
}

/// A vector of `Fixed` in world space: `cy::detmath::FixedVec3`.
@frozen
public struct FixedVec3: Hashable, Sendable {
    /// World X.
    public var x: Fixed
    /// World Y.
    public var y: Fixed
    /// World Z.
    public var z: Fixed

    /// A vector from its components; the origin by default.
    @inlinable
    public init(x: Fixed = .zero, y: Fixed = .zero, z: Fixed = .zero) {
        self.x = x
        self.y = y
        self.z = z
    }

    /// The origin.
    public static let zero = FixedVec3()

    /// The sum, wrapped component by component.
    @inlinable
    public static func + (a: FixedVec3, b: FixedVec3) -> FixedVec3 {
        FixedVec3(x: a.x + b.x, y: a.y + b.y, z: a.z + b.z)
    }
    /// The difference, wrapped component by component.
    @inlinable
    public static func - (a: FixedVec3, b: FixedVec3) -> FixedVec3 {
        FixedVec3(x: a.x - b.x, y: a.y - b.y, z: a.z - b.z)
    }
    /// The negation, wrapped.
    @inlinable
    public static prefix func - (a: FixedVec3) -> FixedVec3 {
        FixedVec3(x: -a.x, y: -a.y, z: -a.z)
    }
    /// Each component times `k`, rounded as `Fixed`'s `*` rounds.
    @inlinable
    public static func * (v: FixedVec3, k: Fixed) -> FixedVec3 {
        FixedVec3(x: v.x * k, y: v.y * k, z: v.z * k)
    }

    /// As the ABI carries it.
    @inlinable
    public var abi: CyFixedVec3 { CyFixedVec3(x: x.raw, y: y.raw, z: z.raw) }
    /// From the ABI's spelling.
    @inlinable
    public init(_ raw: CyFixedVec3) {
        x = Fixed(raw: raw.x)
        y = Fixed(raw: raw.y)
        z = Fixed(raw: raw.z)
    }
}

/// A rotation quaternion of `Fixed`, x y z w: `cy::detmath::FixedQuat` as STORAGE. Composition
/// normalises through the engine's wide square root after every product, so it is not restated
/// here; a module that composes rotations does it in the engine.
@frozen
public struct FixedQuat: Hashable, Sendable {
    /// The vector part.
    public var x: Fixed
    /// The vector part.
    public var y: Fixed
    /// The vector part.
    public var z: Fixed
    /// The scalar part.
    public var w: Fixed

    /// A quaternion from its components; the identity by default.
    @inlinable
    public init(x: Fixed = .zero, y: Fixed = .zero, z: Fixed = .zero, w: Fixed = .one) {
        self.x = x
        self.y = y
        self.z = z
        self.w = w
    }

    /// No rotation.
    public static let identity = FixedQuat()

    /// As the ABI carries it.
    @inlinable
    public var abi: CyFixedQuat { CyFixedQuat(x: x.raw, y: y.raw, z: z.raw, w: w.raw) }
    /// From the ABI's spelling.
    @inlinable
    public init(_ raw: CyFixedQuat) {
        x = Fixed(raw: raw.x)
        y = Fixed(raw: raw.y)
        z = Fixed(raw: raw.z)
        w = Fixed(raw: raw.w)
    }
}
