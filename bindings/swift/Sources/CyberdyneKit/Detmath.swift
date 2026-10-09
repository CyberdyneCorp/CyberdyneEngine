// SPDX-License-Identifier: MIT
// Detmath.swift — the deterministic transcendentals, called in the engine. ABI 1.8,
// openspec/changes/add-deterministic-math stage 8 (design §13).
//
//     let facing = Detmath.atan2(y: offset.y, x: offset.x)       // an Angle
//     let step = FixedVec2(x: Detmath.cos(facing), y: Detmath.sin(facing)) * speed
//
// Arithmetic on `Fixed` is Swift's own (Fixed.swift); every function here is ONE CALL into
// `cy::detmath`, through `detmath_*`, so the polynomial a Swift game evaluates is the polynomial the
// engine evaluates, and nothing here could drift from it. Each is pure and legal in every phase.
// With no engine bound — a unit test that installed none — the scalar functions trap, as a missing
// table entry would, and `evaluate` throws `.unavailable`.

import CyberdyneABI
import CyberdyneCore

/// `cy::detmath`'s functions, through ABI 1.8. See src/core/detmath/include/cy/core/detmath/
/// functions.h for each bound and each out-of-domain answer.
public enum Detmath {
    @inline(__always)
    static var table: Interface {
        guard let interface = Runtime.interface else {
            preconditionFailure("Detmath needs a bound engine: its functions are the engine's")
        }
        return interface
    }

    /// `cy::detmath::kKernelVersion`. A peer or a replay with another number is another kernel.
    public static var kernelVersion: UInt32 { table.detmathKernelVersion() }

    /// `sqrt(x)`, correctly rounded; a negative `x` gives 0.
    public static func sqrt(_ x: Fixed) -> Fixed { Fixed(raw: table.detmathSqrt(x: x.raw)) }
    /// `sin(a)`, within 1 ulp.
    public static func sin(_ a: Angle) -> Fixed { Fixed(raw: table.detmathSin(angle: a.raw)) }
    /// `cos(a)`, within 1 ulp.
    public static func cos(_ a: Angle) -> Fixed { Fixed(raw: table.detmathCos(angle: a.raw)) }
    /// `tan(a)`, saturating at a pole.
    public static func tan(_ a: Angle) -> Fixed { Fixed(raw: table.detmathTan(angle: a.raw)) }
    /// `atan(x)`, as a signed angle.
    public static func atan(_ x: Fixed) -> Angle { Angle(raw: table.detmathAtan(x: x.raw)) }
    /// The angle of the vector (x, y), counter-clockwise from +x. `atan2(0, 0)` is zero.
    public static func atan2(y: Fixed, x: Fixed) -> Angle {
        Angle(raw: table.detmathAtan2(y: y.raw, x: x.raw))
    }
    /// `asin(x)`, `x` clamped to [-1, 1].
    public static func asin(_ x: Fixed) -> Angle { Angle(raw: table.detmathAsin(x: x.raw)) }
    /// `acos(x)`, `x` clamped to [-1, 1].
    public static func acos(_ x: Fixed) -> Angle { Angle(raw: table.detmathAcos(x: x.raw)) }
    /// `2^x`.
    public static func exp2(_ x: Fixed) -> Fixed { Fixed(raw: table.detmathExp2(x: x.raw)) }
    /// `log2(x)`; a non-positive `x` gives `Fixed.min`.
    public static func log2(_ x: Fixed) -> Fixed { Fixed(raw: table.detmathLog2(x: x.raw)) }
    /// `e^x`.
    public static func exp(_ x: Fixed) -> Fixed { Fixed(raw: table.detmathExp(x: x.raw)) }
    /// `ln(x)`; a non-positive `x` gives `Fixed.min`.
    public static func log(_ x: Fixed) -> Fixed { Fixed(raw: table.detmathLog(x: x.raw)) }
    /// `x^y` for `x > 0`; a non-positive base gives 0.
    public static func pow(_ x: Fixed, _ y: Fixed) -> Fixed {
        Fixed(raw: table.detmathPow(x: x.raw, y: y.raw))
    }

    /// `function` over raw inputs in one call: `out[i] = f(x[i])`, or `f(x[i], y[i])` for `.atan2`
    /// and `.pow`. Angles travel in the low 32 bits of their slot, as `CyDetmathFunction` states.
    public static func evaluate(
        _ function: DetmathFunction, _ x: [Int64], _ y: [Int64]? = nil
    ) throws -> [Int64] {
        guard let interface = Runtime.interface else {
            throw CyberdyneError.status(.unavailable, message: "no engine is bound to this module")
        }
        if let y, y.count != x.count {
            throw CyberdyneError.status(
                .invalidArgument, message: "Detmath.evaluate: x and y differ in length")
        }
        var out = [Int64](repeating: 0, count: x.count)
        try x.withUnsafeBufferPointer { xs in
            try out.withUnsafeMutableBufferPointer { results in
                if let y {
                    try y.withUnsafeBufferPointer { ys in
                        try interface.detmathEvaluate(
                            function: function.rawValue, x: xs.baseAddress, y: ys.baseAddress,
                            into: results.baseAddress, count: UInt64(x.count))
                    }
                } else {
                    try interface.detmathEvaluate(
                        function: function.rawValue, x: xs.baseAddress, y: nil,
                        into: results.baseAddress, count: UInt64(x.count))
                }
            }
        }
        return out
    }
}

extension World {
    /// One `Fixed` field, through `component_get_fixed`: the raw value, never through a float.
    public func fixed(_ entity: Entity, _ component: ComponentType, field: UInt32) throws -> Fixed {
        var raw: CyFixed = 0
        try componentGetFixed(
            entity: entity.bits, component: component.id, field: field, into: &raw)
        return Fixed(raw: raw)
    }

    /// Write one `Fixed` field's raw value. A FLOAT written to such a field is refused — with
    /// `.permissionDenied` in a `CrossPlatform` or `Lockstep` session.
    public func setFixed(
        _ value: Fixed, _ entity: Entity, _ component: ComponentType, field: UInt32
    ) throws {
        try componentSetFixed(
            entity: entity.bits, component: component.id, field: field, value: value.raw)
    }
}
