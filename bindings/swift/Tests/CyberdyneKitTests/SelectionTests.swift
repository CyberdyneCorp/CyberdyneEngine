// SPDX-License-Identifier: MIT
// SelectionTests.swift — the selection component's layout, with no engine.
//
// `SelectionHighlight` crosses the ABI as raw bytes (`world_add_component` copies them), so a Swift
// layout that disagreed with `cy::rendering::selection::SelectionHighlight` would mark the wrong
// kind in the wrong colour and compile anyway. These cases pin the layout and the packing; the
// engine side of the same path is `unit.abi_selection`.

import XCTest

@testable import CyberdyneKit

final class SelectionTests: XCTestCase {
    func testLayoutIsTheEnginesTwoWords() {
        XCTAssertEqual(MemoryLayout<SelectionHighlight>.size, 8)
        XCTAssertEqual(MemoryLayout<SelectionHighlight>.stride, 8)
        XCTAssertEqual(MemoryLayout<SelectionHighlight>.alignment, 4)
        XCTAssertEqual(MemoryLayout<SelectionHighlight>.offset(of: \.kind), 0)
        XCTAssertEqual(MemoryLayout<SelectionHighlight>.offset(of: \.colour), 4)
        XCTAssertEqual(
            SelectionHighlight.componentName, "cy::rendering::selection::SelectionHighlight")
        XCTAssertEqual(SelectionHighlight.componentFields.map(\.offset), [0, 4])
    }

    func testKindsAndColoursPackAsTheEngineReadsThem() {
        let selected = SelectionHighlight.selected(red: 40, green: 140, blue: 255)
        XCTAssertEqual(selected.kind, 1)
        XCTAssertEqual(selected.colour, 0xFFFF_8C28)
        let hovered = SelectionHighlight.hovered(red: 255, green: 244, blue: 214, alpha: 128)
        XCTAssertEqual(hovered.kind, 2)
        XCTAssertEqual(hovered.colour, 0x80D6_F4FF)
        XCTAssertEqual(SelectionHighlight().kind, 0)
    }

    func testTheBytesAddedAreKindThenColour() {
        var highlight = SelectionHighlight.selected(red: 1, green: 2, blue: 3, alpha: 4)
        let words = withUnsafeBytes(of: &highlight) { Array($0.bindMemory(to: UInt32.self)) }
        XCTAssertEqual(words, [1, 0x0403_0201])
    }
}
