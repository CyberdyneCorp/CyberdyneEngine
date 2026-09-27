// SPDX-License-Identifier: MIT
// FakeEngine.swift — an interface table built in Swift, for testing the facades through
// `CyberdyneKit` with no engine in the process. `add-swift-game-api`.
//
// Each group's suite (`InputTests`, `PhysicsTests`, …) installs the entries it exercises as
// non-capturing closures — which is what a C function pointer can be — and asserts what the facade
// sent and what it made of the answer. The diagnostic entries are always installed, because a
// throwing overlay call reads `get_last_error` and a nil entry there would trap instead of throw.
//
// Shared by every group's suite, so it is owned by the change's declaring author, not by one of the
// three implementers; a group that needs more from it adds a helper in its own file.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

enum FakeEngine {
    /// A fake engine handle. Never dereferenced: every entry the facades reach is installed here.
    nonisolated(unsafe) static let handle = OpaquePointer(bitPattern: 0xE0)!

    /// The table, at a stable address for the life of the process — `Runtime` keeps the pointer.
    nonisolated(unsafe) static let table: UnsafeMutablePointer<CyInterface> = {
        let table = UnsafeMutablePointer<CyInterface>.allocate(capacity: 1)
        table.initialize(to: CyInterface())
        return table
    }()

    /// The last error a fake entry set, so `check` has a message to copy. The C string is owned
    /// here and replaced, never freed while `get_last_error` may still hand it out.
    nonisolated(unsafe) static var lastError: (CyResult, String) = (CY_RESULT_OK, "")
    nonisolated(unsafe) static var lastErrorText: UnsafeMutablePointer<CChar> = cString("")

    static func cString(_ text: String) -> UnsafeMutablePointer<CChar> {
        let bytes = Array(text.utf8CString)
        let buffer = UnsafeMutablePointer<CChar>.allocate(capacity: bytes.count)
        buffer.initialize(from: bytes, count: bytes.count)
        return buffer
    }

    /// Reset the table, install the diagnostics and `configure`'s entries, and bind it.
    static func install(_ configure: (inout CyInterface) -> Void) {
        var fresh = CyInterface()
        fresh.header = CyInterfaceHeader(
            abi_major: ABI.major, abi_minor: ABI.minor, abi_patch: ABI.patch,
            table_size: UInt32(MemoryLayout<CyInterface>.size))
        fresh.get_last_error = {
            FakeEngine.lastErrorText = FakeEngine.cString(FakeEngine.lastError.1)
            return UnsafePointer(FakeEngine.lastErrorText)
        }
        fresh.get_last_error_code = { FakeEngine.lastError.0 }
        fresh.set_last_error = { result, message in
            FakeEngine.lastError = (result, message.map { String(cString: $0) } ?? "")
        }
        fresh.log = { _, _, _ in }
        configure(&fresh)
        table.pointee = fresh
        lastError = (CY_RESULT_OK, "")
        Runtime.unbind()
        XCTAssertTrue(Runtime.bind(interface: UnsafePointer(table), engine: handle))
    }

    /// Answer `result` from a fake entry with `message` as the last error, as the engine does.
    static func fail(_ result: CyResult, _ message: String) -> CyResult {
        lastError = (result, message)
        return result
    }

    /// Unbind, so a later suite that expects no engine sees none.
    static func uninstall() {
        Runtime.unbind()
    }
}
