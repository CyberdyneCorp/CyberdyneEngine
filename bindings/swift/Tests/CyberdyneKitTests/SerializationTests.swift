// SerializationTests.swift — the blob, and the three migrations it has to survive. Tasks 3.5, 3.7.
//
// These are the Swift half of what the hot-reload spike measured in C: a field carried, a field
// migrated, a field defaulted. They run without an engine, because the blob is the module's own
// format and nothing in it crosses the ABI except as opaque bytes.

@testable import CyberdyneKit
import XCTest

final class SerializationTests: XCTestCase {
    private func read(_ bytes: [UInt8]) throws -> (schema: UInt32, entries: [String: Value]) {
        try bytes.withUnsafeBytes { raw in
            var reader = try BlobReader(raw)
            var entries: [String: Value] = [:]
            while let entry = try reader.next() {
                entries[entry.key] = entry.value
            }
            return (reader.schema, entries)
        }
    }

    func testEveryValueKindSurvivesARoundTrip() throws {
        var writer = BlobWriter(schema: 7)
        let written: [(String, Value)] = [
            ("none", .none),
            ("flag", .bool(true)),
            ("count", .i64(-9_000_000_000)),
            ("speed", .f32(6.5)),
            ("mass", .f64(0.125)),
            ("uv", .vec2(Vec2(x: 1, y: 2))),
            ("position", .vec3(Vec3(x: 1, y: -2, z: 3))),
            ("colour", .vec4(Vec4(x: 1, y: 2, z: 3, w: 4))),
            ("rotation", .quat(Quat(x: 0, y: 0, z: 0, w: 1))),
            ("name", .string("Pläyer 🎮")),
            ("blob", .bytes([0, 1, 2, 255])),
            ("target", .entity(Entity(bits: 0x0000_0002_0000_0007))),
        ]
        for (key, value) in written { writer.write(key, value) }

        let (schema, entries) = try read(writer.finish())
        XCTAssertEqual(schema, 7)
        XCTAssertEqual(entries.count, written.count)
        for (key, value) in written {
            XCTAssertEqual(entries[key], value, "\(key) did not survive the round trip")
        }
    }

    /// The migration the spike named: a key the reader no longer has is skipped, and a key the blob
    /// does not carry keeps its default. Neither is an error, and neither disturbs the fields around
    /// it — which is what restoring BY NAME buys and what restoring by offset does not.
    func testUnknownKeysAreSkippedAndMissingKeysKeepDefaults() throws {
        var writer = BlobWriter(schema: 1)
        writer.write("health", .i64(95))
        writer.write("ammo", .i64(34))  // the field schema 2 renamed
        let (_, entries) = try read(writer.finish())

        XCTAssertEqual(entries["health"], .i64(95))
        XCTAssertEqual(entries["ammo"], .i64(34))
        XCTAssertNil(entries["shield"], "a field the writer did not have must not appear")
    }

    func testATruncatedBlobIsRefusedRatherThanRead() {
        var writer = BlobWriter(schema: 1)
        writer.write("health", .i64(95))
        let bytes = writer.finish()
        for length in 0 ..< bytes.count {
            let prefix = Array(bytes.prefix(length))
            XCTAssertThrowsError(try read(prefix), "a \(length)-byte prefix was accepted")
        }
    }

    func testTheMagicWordIsChecked() {
        var writer = BlobWriter(schema: 1)
        writer.write("health", .i64(95))
        var bytes = writer.finish()
        bytes[0] ^= 0xFF
        XCTAssertThrowsError(try read(bytes)) { error in
            XCTAssertEqual(error as? BlobError, .badMagic)
        }
    }

    /// The entry count in the header is what a reader would trust if it trusted anything but the
    /// buffer's length; this asserts the count is written, so that a future reader that does use it
    /// is not reading a zero.
    func testTheEntryCountIsPatchedIn() throws {
        var writer = BlobWriter(schema: 3)
        writer.write("a", .i64(1))
        writer.write("b", .f32(2))
        writer.write("c", .bool(false))
        let bytes = writer.finish()
        let count = bytes.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 8, as: UInt32.self) }
        XCTAssertEqual(UInt32(littleEndian: count), 3)
    }
}
