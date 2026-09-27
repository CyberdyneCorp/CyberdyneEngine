// SPDX-License-Identifier: MIT
// InputTests.swift — the `Input` facade through `FakeEngine`. `add-swift-game-api`.
//
// OWNER: implementer A. Each case installs the entries it exercises, calls the facade, and checks
// three things: the entry got the arguments the facade was given, the answer came back converted,
// and a refusal came back as `CyberdyneError.status` with the engine's message.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries were last asked. C function pointers cannot capture, so they write here.
private enum Seen {
    nonisolated(unsafe) static var name = ""
    nonisolated(unsafe) static var user: UInt32 = 0
    nonisolated(unsafe) static var action: CyInputAction = 0
    nonisolated(unsafe) static var context: CyInputContext = 0
    nonisolated(unsafe) static var priority: Int32 = 0
    nonisolated(unsafe) static var structSize: UInt32 = 0

    static func reset() {
        name = ""
        user = 0
        action = 0
        context = 0
        priority = 0
        structSize = 0
    }
}

final class InputTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.reset()
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testActionByNameReachesTheEntryAndConvertsTheState() throws {
        FakeEngine.install { table in
            table.input_action_state_by_name = { _, user, name, state in
                Seen.user = user
                Seen.name = String(cString: name!)
                Seen.structSize = state!.pointee.struct_size
                state!.pointee.flags = CY_INPUT_ACTION_PRESSED | CY_INPUT_ACTION_JUST_PRESSED
                state!.pointee.value = (0.5, -1, 0)
                state!.pointee.press_count = 1
                state!.pointee.release_count = 1
                state!.pointee.tick = 42
                return CY_RESULT_OK
            }
        }
        let state = try Input.action("select", user: 2)
        XCTAssertEqual(Seen.name, "select")
        XCTAssertEqual(Seen.user, 2)
        XCTAssertEqual(Seen.structSize, UInt32(MemoryLayout<CyInputActionState>.size))
        XCTAssertTrue(state.pressed)
        XCTAssertTrue(state.justPressed)
        XCTAssertFalse(state.justReleased)
        XCTAssertFalse(state.synthetic)
        XCTAssertEqual(state.pressCount, 1)
        XCTAssertEqual(state.releaseCount, 1)
        XCTAssertEqual(state.tick, 42)
        XCTAssertEqual(state.scalar, 0.5)
        XCTAssertEqual(state.axis2, Vec2(x: 0.5, y: -1))
    }

    func testAResolvedActionReadsByIndex() throws {
        FakeEngine.install { table in
            table.input_find_action = { _, name, action in
                Seen.name = String(cString: name!)
                action!.pointee = 9
                return CY_RESULT_OK
            }
            table.input_action_state = { _, user, action, state in
                Seen.user = user
                Seen.action = action
                state!.pointee.flags = CY_INPUT_ACTION_TRIGGERED | CY_INPUT_ACTION_SYNTHETIC
                return CY_RESULT_OK
            }
        }
        let select = try Input.find(action: "select")
        XCTAssertEqual(Seen.name, "select")
        XCTAssertEqual(select, InputAction(raw: 9))
        let state = try select.state(user: 1)
        XCTAssertEqual(Seen.action, 9)
        XCTAssertEqual(Seen.user, 1)
        XCTAssertTrue(state.triggered)
        XCTAssertTrue(state.synthetic)
        XCTAssertFalse(state.pressed)
    }

    func testAnUnknownActionThrowsNotFound() {
        FakeEngine.install { table in
            table.input_find_action = { _, _, _ in
                FakeEngine.fail(CY_RESULT_NOT_FOUND, "input: no action is declared as 'jump'")
            }
        }
        XCTAssertThrowsError(try Input.find(action: "jump")) { error in
            XCTAssertEqual(
                error as? CyberdyneError,
                .status(.notFound, message: "input: no action is declared as 'jump'"))
        }
    }

    func testThePointerConvertsEveryField() throws {
        FakeEngine.install { table in
            table.input_pointer = { _, user, pointer in
                Seen.user = user
                Seen.structSize = pointer!.pointee.struct_size
                pointer!.pointee.flags = CY_INPUT_POINTER_PRESENT | CY_INPUT_POINTER_IN_WINDOW
                pointer!.pointee.buttons = CY_INPUT_BUTTON_LEFT
                pointer!.pointee.buttons_pressed = CY_INPUT_BUTTON_LEFT | CY_INPUT_BUTTON_RIGHT
                pointer!.pointee.buttons_released = CY_INPUT_BUTTON_RIGHT
                pointer!.pointee.position = (640, 360)
                pointer!.pointee.delta = (4, -2)
                pointer!.pointee.wheel = (0, -3)
                return CY_RESULT_OK
            }
        }
        let pointer = try Input.pointer(user: 1)
        XCTAssertEqual(Seen.user, 1)
        XCTAssertEqual(Seen.structSize, UInt32(MemoryLayout<CyInputPointer>.size))
        XCTAssertTrue(pointer.isPresent)
        XCTAssertTrue(pointer.isInWindow)
        XCTAssertFalse(pointer.isOverUI)
        XCTAssertEqual(pointer.buttons, .left)
        XCTAssertEqual(pointer.pressed, [.left, .right])
        XCTAssertEqual(pointer.released, .right)
        XCTAssertEqual(pointer.position, Vec2(x: 640, y: 360))
        XCTAssertEqual(pointer.delta, Vec2(x: 4, y: -2))
        XCTAssertEqual(pointer.wheel, Vec2(x: 0, y: -3))
    }

    func testThePointerInAFixedStepThrowsPermissionDenied() {
        FakeEngine.install { table in
            table.input_pointer = { _, _, _ in
                FakeEngine.fail(
                    CY_RESULT_PERMISSION_DENIED,
                    "input_pointer may not be called during fixed update")
            }
            table.input_modifiers = { _, _, _ in
                FakeEngine.fail(
                    CY_RESULT_PERMISSION_DENIED,
                    "input_modifiers may not be called during fixed update")
            }
        }
        XCTAssertThrowsError(try Input.pointer()) { error in
            guard case CyberdyneError.status(let status, let message) = error else {
                return XCTFail("expected a status error, got \(error)")
            }
            XCTAssertEqual(status, .permissionDenied)
            XCTAssertTrue(message.contains("input_pointer"))
        }
        XCTAssertThrowsError(try Input.modifiers()) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("expected a status error, got \(error)")
            }
            XCTAssertEqual(status, .permissionDenied)
        }
    }

    func testModifiersAreAnOptionSet() throws {
        FakeEngine.install { table in
            table.input_modifiers = { _, user, modifiers in
                Seen.user = user
                modifiers!.pointee = CY_INPUT_MOD_SHIFT | CY_INPUT_MOD_ALT
                return CY_RESULT_OK
            }
        }
        let modifiers = try Input.modifiers(user: 3)
        XCTAssertEqual(Seen.user, 3)
        XCTAssertTrue(modifiers.contains(.shift))
        XCTAssertTrue(modifiers.contains(.alt))
        XCTAssertFalse(modifiers.contains(.ctrl))
    }

    func testContextsAreFoundPushedAndPopped() throws {
        FakeEngine.install { table in
            table.input_find_context = { _, name, context in
                Seen.name = String(cString: name!)
                context!.pointee = 0x1_0000_0004
                return CY_RESULT_OK
            }
            table.input_push_context = { _, user, context, priority in
                Seen.user = user
                Seen.context = context
                Seen.priority = priority
                return CY_RESULT_OK
            }
            table.input_pop_context = { _, user, context in
                Seen.user = user
                Seen.context = context
                return FakeEngine.fail(CY_RESULT_NOT_FOUND, "not on the stack")
            }
        }
        let menu = try Input.context("menu")
        XCTAssertEqual(Seen.name, "menu")
        XCTAssertEqual(menu.raw, 0x1_0000_0004)
        try Input.push(menu, priority: 10, user: 1)
        XCTAssertEqual(Seen.context, menu.raw)
        XCTAssertEqual(Seen.priority, 10)
        XCTAssertEqual(Seen.user, 1)
        XCTAssertThrowsError(try Input.pop(menu)) { error in
            XCTAssertEqual(
                error as? CyberdyneError, .status(.notFound, message: "not on the stack"))
        }
        XCTAssertEqual(Seen.user, 0)
    }

    func testWithNoEngineInputThrowsUnavailable() {
        FakeEngine.uninstall()
        XCTAssertThrowsError(try Input.action("select")) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("expected a status error, got \(error)")
            }
            XCTAssertEqual(status, .unavailable)
        }
    }
}
