// Serialization.swift — the state blob a hot reload carries across. Tasks 3.5, 3.7.
//
// --- THE FORMAT, AND WHY IT IS THIS ONE -----------------------------------------------------------
//
//     magic 'CYST' u32 | schema u32 | count u32 | count x (keyLength u32, key bytes, kind u32,
//                                                          payload)
//
// It is the hot-reload spike's own format, kept deliberately, because the spike is what makes the
// claims about it measurements rather than intentions: 40 consecutive edit/rebuild/reload cycles
// including a type-layout change, every field carried, one field migrated, one new field defaulted,
// a Swift `String` intact.
//
// THREE PROPERTIES DO ALL THE WORK, AND EACH ONE IS A NEGATIVE RESULT FROM THAT SPIKE.
//
//   1. RESTORE IS BY NAME. Not by offset, and not by declaration order. Reading v1 objects with v2
//      code at fixed offsets reported health = 17 (that was v1's `ammo`), shield = 1 (an entity id)
//      and mana = 3.5e18 (the raw bits of a `String`), with no trap and no diagnostic. By name, an
//      unknown key is skipped and a missing key keeps its default — so adding, removing and
//      reordering fields are all migrations rather than corruptions.
//
//   2. EVERY ENTRY CARRIES ITS KIND. The spike's C module got away with `i64` for everything
//      because its fields were all integers. A behaviour's exported properties are not: a `Float`
//      restored from a key that now holds a `String` must be REFUSED, and refusing needs the kind
//      in the blob. Without it the reader would have to trust the writer's declaration order, which
//      is item 1 again wearing a different hat.
//
//   3. THE SCHEMA IS THE WRITER'S. The reader compares it with its own and answers
//      `CY_RESULT_SCHEMA_TOO_NEW` when the blob is from the future. That is the one check
//      `cy::abi::BehaviourRuntime` relies on to keep the previous generation live rather than
//      restoring an instance into a shape nobody wrote — `native-abi`'s "Incompatible reload".
//
// Fixed-width, host-endian, written by copying. That is enough and is not a shortcut: a blob never
// leaves the process. It is produced by one image and consumed by the next image in the same
// process, minutes apart at most. A portable encoding would be paying for a property nothing uses.

import CyberdyneCore

/// 'CYST', little-endian, as it appears in the first four bytes.
let blobMagic: UInt32 = 0x5453_5943

enum BlobError: Error, Equatable {
    case truncated
    case badMagic
    case schemaTooNew(found: UInt32, supported: UInt32)
}

/// Writes the blob. Append-only by construction: there is no way to write an entry without its key
/// and its kind.
struct BlobWriter {
    private(set) var bytes: [UInt8] = []
    private var count: UInt32 = 0

    init(schema: UInt32) {
        append(blobMagic)
        append(schema)
        append(UInt32(0))  // patched by `finish()`, which is the only reader of `count`
    }

    mutating func write(_ key: String, _ value: Value) {
        let keyBytes = Array(key.utf8)
        append(UInt32(keyBytes.count))
        bytes.append(contentsOf: keyBytes)
        append(value.type.rawValue)
        appendPayload(value)
        count += 1
    }

    /// The finished blob, with the entry count filled in.
    mutating func finish() -> [UInt8] {
        withUnsafeBytes(of: count.littleEndian) { raw in
            for (index, byte) in raw.enumerated() { bytes[8 + index] = byte }
        }
        return bytes
    }

    private mutating func append(_ value: UInt32) {
        withUnsafeBytes(of: value.littleEndian) { bytes.append(contentsOf: $0) }
    }

    private mutating func append(_ value: UInt64) {
        withUnsafeBytes(of: value.littleEndian) { bytes.append(contentsOf: $0) }
    }

    private mutating func appendPayload(_ value: Value) {
        switch value {
        case .none: break
        case let .bool(inner): bytes.append(inner ? 1 : 0)
        case let .i64(inner): append(UInt64(bitPattern: inner))
        case let .f32(inner): append(inner.bitPattern)
        case let .f64(inner): append(inner.bitPattern)
        case let .vec2(inner): appendLanes([inner.x, inner.y])
        case let .vec3(inner): appendLanes([inner.x, inner.y, inner.z])
        case let .vec4(inner): appendLanes([inner.x, inner.y, inner.z, inner.w])
        case let .quat(inner): appendLanes([inner.x, inner.y, inner.z, inner.w])
        case let .entity(inner): append(inner.bits)
        case let .string(inner):
            let utf8 = Array(inner.utf8)
            append(UInt32(utf8.count))
            bytes.append(contentsOf: utf8)
        case let .bytes(inner):
            append(UInt32(inner.count))
            bytes.append(contentsOf: inner)
        }
    }

    private mutating func appendLanes(_ lanes: [Float]) {
        for lane in lanes { append(lane.bitPattern) }
    }
}

/// Reads the blob back. Every read is bounds-checked against the buffer it was handed, because the
/// buffer comes from the engine and a module that reads past it corrupts the host.
struct BlobReader {
    private let bytes: UnsafeRawBufferPointer
    private var offset: Int = 0
    private var read: UInt32 = 0
    let schema: UInt32
    let count: UInt32

    init(_ buffer: UnsafeRawBufferPointer) throws {
        bytes = buffer
        offset = 0
        guard buffer.count >= 12 else { throw BlobError.truncated }
        var cursor = 0
        func word() -> UInt32 {
            defer { cursor += 4 }
            return UInt32(littleEndian: buffer.loadUnaligned(fromByteOffset: cursor, as: UInt32.self))
        }
        guard word() == blobMagic else { throw BlobError.badMagic }
        schema = word()
        count = word()
        offset = cursor
    }

    /// The next (key, value) pair, or nil at the end. A value whose kind this build does not know
    /// cannot be skipped — its payload width is unknown — so it ends the read rather than
    /// desynchronising it.
    mutating func next() throws -> (key: String, value: Value)? {
        if offset >= bytes.count {
            // THE HEADER'S COUNT IS CHECKED HERE AND NOWHERE ELSE. A blob truncated exactly at the
            // end of an entry — or to the twelve header bytes alone — has a well-formed prefix and
            // would otherwise read as "no more entries", which is a SHORTER instance restored
            // successfully rather than a failure. Comparing what was read against what the writer
            // said it wrote is what turns that into `BlobError.truncated`.
            guard read == count else { throw BlobError.truncated }
            return nil
        }
        let keyLength = Int(try word())
        let key = String(decoding: try take(keyLength), as: UTF8.self)
        let rawKind = try word()
        guard let kind = VarType(rawValue: rawKind) else { throw BlobError.truncated }
        let value = try payload(kind)
        read += 1
        return (key, value)
    }

    private mutating func word() throws -> UInt32 {
        guard offset + 4 <= bytes.count else { throw BlobError.truncated }
        defer { offset += 4 }
        return UInt32(littleEndian: bytes.loadUnaligned(fromByteOffset: offset, as: UInt32.self))
    }

    private mutating func doubleWord() throws -> UInt64 {
        guard offset + 8 <= bytes.count else { throw BlobError.truncated }
        defer { offset += 8 }
        return UInt64(littleEndian: bytes.loadUnaligned(fromByteOffset: offset, as: UInt64.self))
    }

    private mutating func take(_ length: Int) throws -> UnsafeRawBufferPointer {
        guard length >= 0, offset + length <= bytes.count else { throw BlobError.truncated }
        defer { offset += length }
        return UnsafeRawBufferPointer(rebasing: bytes[offset ..< offset + length])
    }

    private mutating func lanes(_ n: Int) throws -> [Float] {
        var out: [Float] = []
        out.reserveCapacity(n)
        for _ in 0 ..< n { out.append(Float(bitPattern: try word())) }
        return out
    }

    private mutating func payload(_ kind: VarType) throws -> Value {
        switch kind {
        case .nil: return .none
        case .bool: return .bool(try take(1)[0] != 0)
        case .i64: return .i64(Int64(bitPattern: try doubleWord()))
        case .f32: return .f32(Float(bitPattern: try word()))
        case .f64: return .f64(Double(bitPattern: try doubleWord()))
        case .vec2: return .vec2(Vec2(lanes: try lanes(2)))
        case .vec3: return .vec3(Vec3(lanes: try lanes(3)))
        case .vec4: return .vec4(Vec4(lanes: try lanes(4)))
        case .quat: return .quat(Quat(lanes: try lanes(4)))
        case .entity: return .entity(Entity(bits: try doubleWord()))
        case .string:
            let length = Int(try word())
            return .string(String(decoding: try take(length), as: UTF8.self))
        case .bytes:
            let length = Int(try word())
            return .bytes(Array(try take(length).bindMemory(to: UInt8.self)))
        }
    }
}
