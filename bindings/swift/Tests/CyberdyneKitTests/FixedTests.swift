// SPDX-License-Identifier: MIT
// FixedTests.swift — Swift's fixed-point arithmetic against the engine's, bit for bit. ABI 1.8,
// openspec/changes/add-deterministic-math task 8.3.
//
// TWO PIECES OF EVIDENCE, AND EACH FAILS ON ITS OWN.
//
//   the golden vectors    every line of tools/detmath/vectors/{add,sub,mul,div,narrow16,
//                         angle_scale}.txt — the edge cases and the first seeded inputs — through
//                         the Swift operator, against the committed output `unit.detmath` checks
//                         the C++ operator against.
//   the sweep digests     the whole 16 384-input seeded sweep of each of those functions, drawn by
//                         the same SplitMix64 and folded by the same `digest_fold`
//                         (src/core/detmath/src/digest.cpp), against tools/detmath/vectors/
//                         digests.txt — the number `integration.detmath_vectors` and
//                         `determinism.cross_leg` hold every C++ leg to. So the Swift `*` and the
//                         C++ `*` are held to ONE answer over every input either side has tried.
//
// The transcendentals are the engine's, not Swift's: `Detmath` forwards to `detmath_*`, and the
// case below checks only that it forwards raw bits both ways, through a fake table answering from
// the sin and atan2 vector files.

import CyberdyneABI
import CyberdyneCore
import Foundation
import XCTest

@testable import CyberdyneKit

/// tools/detmath/vectors/, found from this file: bindings/swift/Tests/CyberdyneKitTests/.
private let vectorsDirectory = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent()
    .appendingPathComponent("tools/detmath/vectors")

/// One vector line: its raw inputs and its expected raw output, as 64-bit patterns.
private struct VectorLine {
    var inputs: [UInt64]
    var expected: UInt64
}

private func readVectors(_ name: String) throws -> [VectorLine] {
    let url = vectorsDirectory.appendingPathComponent("\(name).txt")
    let text = try String(contentsOf: url, encoding: .utf8)
    var lines: [VectorLine] = []
    for line in text.split(separator: "\n") where !line.hasPrefix("#") {
        let words = line.split(separator: " ")
        guard words.count >= 3, words[0] == "edge" || words[0] == "seed" else { continue }
        let numbers = try words.dropFirst().map { word -> UInt64 in
            guard let value = UInt64(word, radix: 16) else {
                throw CyberdyneError.notRepresentable("\(name): \(line)")
            }
            return value
        }
        lines.append(VectorLine(inputs: Array(numbers.dropLast()), expected: numbers.last!))
    }
    return lines
}

private func readDigests() throws -> [String: UInt64] {
    let url = vectorsDirectory.appendingPathComponent("digests.txt")
    var digests: [String: UInt64] = [:]
    for line in try String(contentsOf: url, encoding: .utf8).split(separator: "\n")
    where !line.hasPrefix("#") {
        let words = line.split(separator: " ")
        if words.count == 2, let value = UInt64(words[1], radix: 16) {
            digests[String(words[0])] = value
        }
    }
    return digests
}

/// The functions Swift implements itself, in `cy::detmath::KernelFunction` order — the index is
/// part of each sweep's seed.
private enum SwiftFunction: CaseIterable {
    case add, sub, mul, div, narrow16, angleScale

    var name: String {
        switch self {
        case .add: return "add"
        case .sub: return "sub"
        case .mul: return "mul"
        case .div: return "div"
        case .narrow16: return "narrow16"
        case .angleScale: return "angle_scale"
        }
    }

    /// `KernelFunction`'s value: Add 0, Subtract 1, Multiply 2, Divide 3, NarrowFixed16 6,
    /// AngleScale 7.
    var kernelIndex: UInt64 {
        switch self {
        case .add: return 0
        case .sub: return 1
        case .mul: return 2
        case .div: return 3
        case .narrow16: return 6
        case .angleScale: return 7
        }
    }

    func evaluate(_ inputs: [UInt64]) -> UInt64 {
        let a = Fixed(raw: Int64(bitPattern: inputs[0]))
        switch self {
        case .add: return UInt64(bitPattern: (a + Fixed(raw: Int64(bitPattern: inputs[1]))).raw)
        case .sub: return UInt64(bitPattern: (a - Fixed(raw: Int64(bitPattern: inputs[1]))).raw)
        case .mul: return UInt64(bitPattern: (a * Fixed(raw: Int64(bitPattern: inputs[1]))).raw)
        case .div: return UInt64(bitPattern: (a / Fixed(raw: Int64(bitPattern: inputs[1]))).raw)
        case .narrow16:
            return UInt64(bitPattern: Int64(Fixed16(narrowing: a).raw))
        case .angleScale:
            let angle = Angle(raw: UInt32(truncatingIfNeeded: inputs[0]))
            return UInt64((angle * Fixed(raw: Int64(bitPattern: inputs[1]))).raw)
        }
    }
}

/// SplitMix64 and the draws of src/core/detmath/src/digest.cpp, for the functions above.
private struct SplitMix64 {
    var state: UInt64

    static let golden: UInt64 = 0x9E37_79B9_7F4A_7C15

    static func finalize(_ input: UInt64) -> UInt64 {
        var z = input
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return z ^ (z >> 31)
    }

    mutating func next() -> UInt64 {
        state = state &+ SplitMix64.golden
        return SplitMix64.finalize(state)
    }

    /// A raw value spread over every binade: a random i64 shifted right by a random amount.
    mutating func scaled() -> Int64 {
        let value = Int64(bitPattern: next())
        let shift = Int(next() & 63)
        return value >> shift
    }

    mutating func angle() -> UInt64 { next() >> 32 }

    mutating func draw(_ function: SwiftFunction) -> [UInt64] {
        switch function {
        case .add, .sub:
            let a = next()
            return [a, next()]
        case .mul, .div:
            let a = UInt64(bitPattern: scaled())
            return [a, UInt64(bitPattern: scaled())]
        case .narrow16:
            return [UInt64(bitPattern: scaled())]
        case .angleScale:
            let a = angle()
            return [a, UInt64(bitPattern: scaled() >> 16)]
        }
    }
}

/// `cy::detmath::digest_fold`.
private func digestFold(_ accumulator: UInt64, _ value: UInt64) -> UInt64 {
    SplitMix64.finalize((accumulator ^ value) &+ SplitMix64.golden)
}

/// `cy::detmath::function_digest` of `function` over the whole sweep.
private func sweepDigest(_ function: SwiftFunction, count: Int = 16384) -> UInt64 {
    let seed: UInt64 = 0xDE_73A7_0001 &+ 0x1000 &* (function.kernelIndex + 1)
    var rng = SplitMix64(state: seed)
    var accumulator = seed
    for _ in 0..<count {
        accumulator = digestFold(accumulator, function.evaluate(rng.draw(function)))
    }
    return accumulator
}

final class FixedTests: XCTestCase {
    func testEveryCommittedVectorOfSwiftsArithmeticMatches() throws {
        for function in SwiftFunction.allCases {
            let lines = try readVectors(function.name)
            XCTAssertGreaterThan(
                lines.count, 200, "\(function.name): the vector file is missing lines")
            for line in lines {
                XCTAssertEqual(
                    function.evaluate(line.inputs), line.expected,
                    "\(function.name)(\(line.inputs.map { String($0, radix: 16) }))")
            }
        }
    }

    func testSwiftReproducesTheCppSweepDigests() throws {
        let digests = try readDigests()
        for function in SwiftFunction.allCases {
            let committed = try XCTUnwrap(
                digests[function.name], "digests.txt has no \(function.name)")
            XCTAssertEqual(
                sweepDigest(function), committed,
                "\(function.name): Swift's sweep digest is not the one every C++ leg computes")
        }
    }

    func testOverflowWrapsAsTheEngineDoesRatherThanTrapping() {
        XCTAssertEqual(Fixed.max + .epsilon, .min)
        XCTAssertEqual(Fixed.min - .epsilon, .max)
        XCTAssertEqual(-Fixed.min, .min)
        XCTAssertEqual(Fixed.min.magnitude, .min)
        // 2^16 squared is 2^32, whose raw value 2^64 wraps to its low 64 bits: zero.
        XCTAssertEqual((Fixed(1 << 16) * Fixed(1 << 16)).raw, 0)
        // A quotient beyond the range wraps too: max / epsilon is (2^63 - 1) * 2^32.
        XCTAssertEqual((Fixed.max / .epsilon).raw, Int64(bitPattern: 0xFFFF_FFFF_0000_0000))
    }

    func testDivisionByZeroFollowsTheEnginesRule() {
        XCTAssertEqual(Fixed(3) / .zero, .max)
        XCTAssertEqual(Fixed.zero / .zero, .max)
        XCTAssertEqual(Fixed(-3) / .zero, .min)
    }

    func testProductsRoundToNearestWithTiesTowardPositiveInfinity() {
        // 2^-32 * 1/2 = 2^-33: a tie, rounded up to 2^-32.
        XCTAssertEqual((Fixed.epsilon * .half).raw, 1)
        // -2^-32 * 1/2 = -2^-33: a tie, rounded up to zero.
        XCTAssertEqual((-Fixed.epsilon * .half).raw, 0)
        XCTAssertEqual(Fixed(6) * Fixed(7), Fixed(42))
        XCTAssertEqual(Fixed(-6) / Fixed(4), Fixed(raw: -(Fixed.oneRaw * 3) / 2))
    }

    func testCookingIsTheEnginesConversion() {
        XCTAssertEqual(Fixed(cooking: 1.0).raw, Fixed.oneRaw)
        XCTAssertEqual(Fixed(cooking: Float(-0.5)).raw, -Fixed.oneRaw / 2)
        XCTAssertEqual(Fixed(cooking: Double.nan), .zero)
        XCTAssertEqual(Fixed(cooking: 1e300), .max)
        XCTAssertEqual(Fixed(cooking: -1e300), .min)
        // 2^-33 is half an ulp: nearest-even rounds the tie to zero, and 3 * 2^-33 up to 2^-31.
        XCTAssertEqual(Fixed(cooking: 0x1p-33).raw, 0)
        XCTAssertEqual(Fixed(cooking: 0x1.8p-32).raw, 2)
    }

    func testAnglesWrapExactly() {
        XCTAssertEqual(Angle.half + .half, .zero)
        XCTAssertEqual(-Angle.quarter, Angle(raw: 3 << 30))
        XCTAssertEqual(Angle(turns: Fixed(raw: Fixed.oneRaw + (1 << 30))), .quarter)
        XCTAssertEqual(Angle.quarter.signedTurns, Fixed(raw: 1 << 30))
        XCTAssertEqual((Angle.quarter * Fixed(2)), .half)
    }

    func testAFixedValueCrossesAsItsRawInteger() throws {
        let value = Fixed(raw: -0x1234_5678_9ABC)
        XCTAssertEqual(value.cyValue, .fixed(value))
        let variable = value.cyValue.inlineCyVar
        XCTAssertEqual(variable.type, VarType.fixed.rawValue)
        XCTAssertEqual(variable.payload.as_i64, value.raw)
        XCTAssertEqual(Value(reading: variable), .fixed(value))
        XCTAssertEqual(Fixed(cyValue: .fixed(value)), value)
        XCTAssertNil(Fixed(cyValue: .i64(value.raw)))
    }
}

/// The sin and atan2 vector files as lookups, for the forwarding check.
nonisolated(unsafe) private var sinAnswers: [UInt32: Int64] = [:]
nonisolated(unsafe) private var atan2Answers: [[Int64]: UInt32] = [:]

final class DetmathForwardingTests: XCTestCase {
    override func tearDown() {
        FakeEngine.uninstall()
    }

    func testTheTranscendentalsForwardRawBitsBothWays() throws {
        let sines = try readVectors("sin")
        let atans = try readVectors("atan2")
        sinAnswers = [:]
        atan2Answers = [:]
        for line in sines {
            sinAnswers[UInt32(truncatingIfNeeded: line.inputs[0])] = Int64(
                bitPattern: line.expected)
        }
        for line in atans {
            atan2Answers[line.inputs.map { Int64(bitPattern: $0) }] =
                UInt32(truncatingIfNeeded: line.expected)
        }
        FakeEngine.install { table in
            table.detmath_kernel_version = { 1 }
            table.detmath_sin = { angle in sinAnswers[angle] ?? 0 }
            table.detmath_atan2 = { y, x in atan2Answers[[y, x]] ?? 0 }
        }
        XCTAssertEqual(Detmath.kernelVersion, 1)
        for line in sines {
            XCTAssertEqual(
                Detmath.sin(Angle(raw: UInt32(truncatingIfNeeded: line.inputs[0]))).raw,
                Int64(bitPattern: line.expected))
        }
        for line in atans {
            XCTAssertEqual(
                Detmath.atan2(
                    y: Fixed(raw: Int64(bitPattern: line.inputs[0])),
                    x: Fixed(raw: Int64(bitPattern: line.inputs[1]))
                ).raw,
                UInt32(truncatingIfNeeded: line.expected))
        }
    }

    func testEvaluateWithNoEngineThrowsRatherThanTraps() {
        FakeEngine.uninstall()
        XCTAssertThrowsError(try Detmath.evaluate(.sqrt, [Fixed.oneRaw]))
    }
}
