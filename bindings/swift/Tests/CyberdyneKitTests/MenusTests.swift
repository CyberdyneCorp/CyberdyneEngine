// SPDX-License-Identifier: MIT
// MenusTests.swift — rebinding, the profile, the display settings, the player's files and tooltips
// through `FakeEngine`. ABI 1.17.
//
// Each case checks that a facade call reaches its entry with the right arguments, converts the
// answer — a fixed C string, a two-call sized read — and turns a refusal into the Swift answer the
// facade promises: nil for a binding or a file that is not there, a thrown status otherwise.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw, and what they hold.
private enum Seen {
    nonisolated(unsafe) static var rebound: (UInt32, CyInputAction, UInt32, UInt32, UInt32) =
        (0, 0, 0, 0, 0)
    nonisolated(unsafe) static var cancelled = false
    nonisolated(unsafe) static var profile = ""
    nonisolated(unsafe) static var video = CyVideoSettings()
    nonisolated(unsafe) static var files: [String: [UInt8]] = [:]
    nonisolated(unsafe) static var tooltip = ""
}

/// Copy `text` into a C buffer the sized-read way: the whole size always, the bytes when it fits.
private func answer(
    _ text: String, _ buffer: UnsafeMutablePointer<CChar>?, _ capacity: UInt32,
    _ size: UnsafeMutablePointer<UInt32>?
) {
    let bytes = Array(text.utf8)
    size?.pointee = UInt32(bytes.count)
    guard let buffer, capacity > 0 else { return }
    let fits = min(bytes.count, Int(capacity) - 1)
    for index in 0..<fits { buffer[index] = CChar(bitPattern: bytes[index]) }
    buffer[fits] = 0
}

private func writeFixedString<T>(_ text: String, into tuple: inout T) {
    withUnsafeMutableBytes(of: &tuple) { raw in
        let bytes = Array(text.utf8.prefix(raw.count - 1))
        raw.copyBytes(from: bytes)
        raw[bytes.count] = 0
    }
}

final class MenusTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.rebound = (0, 0, 0, 0, 0)
        Seen.cancelled = false
        Seen.profile = ""
        Seen.video = CyVideoSettings()
        Seen.video.width = 1920
        Seen.video.height = 1080
        Seen.video.quality = CY_VIDEO_QUALITY_HIGH.rawValue
        Seen.files = [:]
        Seen.tooltip = ""
        FakeEngine.install { table in
            table.input_rebind_begin = { _, user, action, scheme, slot, conflict in
                Seen.rebound = (user, action, scheme, slot, conflict)
                return CY_RESULT_OK
            }
            table.input_rebind_poll = { _, _, out in
                guard let out,
                    out.pointee.struct_size == UInt32(MemoryLayout<CyInputRebind>.size)
                else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "a struct size") }
                out.pointee.status = CY_INPUT_REBIND_APPLIED.rawValue
                out.pointee.action = 4
                out.pointee.scheme = CY_INPUT_SCHEME_GAMEPAD.rawValue
                writeFixedString("gamepad/north", into: &out.pointee.control)
                writeFixedString("Game.Interact", into: &out.pointee.conflict)
                return CY_RESULT_OK
            }
            table.input_rebind_cancel = { _, _ in
                Seen.cancelled = true
                return CY_RESULT_OK
            }
            table.input_binding = { _, _, action, _, _, buffer, capacity, size in
                guard action == 4 else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such binding")
                }
                answer("keyboard/space", buffer, capacity, size)
                return CY_RESULT_OK
            }
            table.input_profile_save = { _, _, buffer, capacity, size in
                answer(Seen.profile, buffer, capacity, size)
                return CY_RESULT_OK
            }
            table.input_profile_load = { _, _, text, size in
                guard let text else { return CY_RESULT_OK }
                let bytes = UnsafeRawBufferPointer(start: text, count: Int(size))
                Seen.profile = String(decoding: bytes, as: UTF8.self)
                return CY_RESULT_OK
            }
            table.input_profile_reset = { _, _ in
                Seen.profile = ""
                return CY_RESULT_OK
            }
            table.video_get_settings = { _, out in
                out?.pointee = Seen.video
                return CY_RESULT_OK
            }
            table.video_set_settings = { _, settings in
                guard let settings, settings.pointee.width != 1 else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "the display does not offer it")
                }
                Seen.video = settings.pointee
                return CY_RESULT_OK
            }
            table.video_modes = { _, out, capacity, count in
                let modes = [
                    CyVideoMode(width: 1280, height: 720),
                    CyVideoMode(width: 1920, height: 1080),
                ]
                count?.pointee = UInt32(modes.count)
                if let out {
                    for index in 0..<min(Int(capacity), modes.count) { out[index] = modes[index] }
                }
                return CY_RESULT_OK
            }
            table.user_data_write = { _, name, bytes, size in
                guard let name else { return CY_RESULT_INVALID_ARGUMENT }
                let data =
                    bytes.map {
                        Array(UnsafeRawBufferPointer(start: $0, count: Int(size)))
                    } ?? []
                Seen.files[String(cString: name)] = data
                return CY_RESULT_OK
            }
            table.user_data_read = { _, name, buffer, capacity, size in
                guard let name, let data = Seen.files[String(cString: name)] else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such file")
                }
                size?.pointee = UInt64(data.count)
                if let buffer {
                    let count = min(Int(capacity), data.count)
                    data.withUnsafeBytes {
                        buffer.copyMemory(from: $0.baseAddress!, byteCount: count)
                    }
                }
                return CY_RESULT_OK
            }
            table.ui_widget_set_tooltip = { _, _, text in
                Seen.tooltip = text.map { String(cString: $0) } ?? ""
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testARebindReachesTheEntryAndItsStateReadsBack() throws {
        try InputAction(raw: 4).rebind(scheme: .gamepad, slot: 0, conflict: .reject, user: 1)
        XCTAssertEqual(Seen.rebound.0, 1)
        XCTAssertEqual(Seen.rebound.1, 4)
        XCTAssertEqual(Seen.rebound.2, CY_INPUT_SCHEME_GAMEPAD.rawValue)
        XCTAssertEqual(Seen.rebound.4, CY_INPUT_CONFLICT_REJECT.rawValue)
        let state = try Rebinding.poll()
        XCTAssertEqual(state.status, .applied)
        XCTAssertEqual(state.scheme, .gamepad)
        XCTAssertEqual(state.control, "gamepad/north")
        XCTAssertEqual(state.conflict, "Game.Interact")
        try Rebinding.cancel()
        XCTAssertTrue(Seen.cancelled)
    }

    func testABindingIsAPathAndAMissingOneIsNil() throws {
        XCTAssertEqual(try InputAction(raw: 4).binding(scheme: .keyboardMouse), "keyboard/space")
        XCTAssertNil(try InputAction(raw: 9).binding(scheme: .keyboardMouse))
    }

    func testAProfileRoundTripsThroughThePlayersFiles() throws {
        let text = "cyinputprofile 1\n4 gamepad 0 gamepad/north Game.Jump\n"
        try InputProfile.load(text)
        XCTAssertEqual(try InputProfile.save(), text)
        try UserData.write("bindings.cyinputprofile", text: try InputProfile.save())
        try InputProfile.reset()
        XCTAssertEqual(try InputProfile.save(), "")
        XCTAssertEqual(try UserData.readText("bindings.cyinputprofile"), text)
        try InputProfile.load(try UserData.readText("bindings.cyinputprofile") ?? "")
        XCTAssertEqual(Seen.profile, text)
        XCTAssertNil(try UserData.read("missing.txt"))
    }

    func testVideoSettingsAndModesConvertBothWays() throws {
        var settings = try Video.settings
        XCTAssertEqual(settings.width, 1920)
        XCTAssertEqual(settings.quality, .high)
        settings.width = 1280
        settings.height = 720
        settings.quality = .low
        settings.upscale = .performance
        settings.fullscreen = true
        try Video.apply(settings)
        XCTAssertEqual(Seen.video.width, 1280)
        XCTAssertEqual(Seen.video.quality, CY_VIDEO_QUALITY_LOW.rawValue)
        XCTAssertEqual(Seen.video.upscale, CY_VIDEO_UPSCALE_PERFORMANCE.rawValue)
        XCTAssertEqual(Seen.video.flags & CY_VIDEO_FULLSCREEN, CY_VIDEO_FULLSCREEN)
        XCTAssertEqual(Seen.video.struct_size, UInt32(MemoryLayout<CyVideoSettings>.size))
        let modes = try Video.modes
        XCTAssertEqual(modes.count, 2)
        XCTAssertEqual(modes[1].width, 1920)
        settings.width = 1
        XCTAssertThrowsError(try Video.apply(settings)) { error in
            guard case CyberdyneError.status(.notFound, _) = error else {
                return XCTFail("\(error)")
            }
        }
    }

    func testATooltipReachesItsEntry() throws {
        try Widget(raw: 0x1_0000_0003).setTooltip("Restores 25 health")
        XCTAssertEqual(Seen.tooltip, "Restores 25 health")
    }
}
