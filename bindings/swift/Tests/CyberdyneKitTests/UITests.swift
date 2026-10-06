// SPDX-License-Identifier: MIT
// UITests.swift — ABI 1.6's interface through `FakeEngine`: `UIElement`, `UI`, the declarative
// `mountUI`, and a button's click reaching the behaviour that mounted it through `ui_event`.
//
// The facades' half: the right handles, structs and strings reach the entries, every default
// crosses as the zero the ABI reads as that default, answers are converted, a refusal is a thrown
// `CyberdyneError`, and a click delivered to the vtable runs the button's action and `onUIEvent`.
// The engine's half — the store, the layout, routing a press and a release into a click — is
// integration.game_backend_ui; the whole path in a running game is integration.rts_api_sample.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake interface was sent, and the store it keeps: element n is `n + 1` with generation 1.
private enum Fake {
    struct Made: Equatable {
        var parent: CyUiElement
        var kind: UInt32
        var name: String?
        var owner: CyEntity
    }

    static let root: CyUiElement = 0x1_0000_0000
    nonisolated(unsafe) static var made: [Made] = []
    nonisolated(unsafe) static var layouts: [CyUiElement: CyUiLayout] = [:]
    nonisolated(unsafe) static var styles: [CyUiElement: CyUiStyle] = [:]
    nonisolated(unsafe) static var texts: [CyUiElement: (String, UInt32, UInt32)] = [:]
    nonisolated(unsafe) static var images: [CyUiElement: (UInt32, [Float]?)] = [:]
    nonisolated(unsafe) static var progress: [CyUiElement: Float] = [:]
    nonisolated(unsafe) static var visibility: [CyUiElement: UInt32] = [:]
    nonisolated(unsafe) static var opacity: [CyUiElement: Float] = [:]
    nonisolated(unsafe) static var destroyed: [CyUiElement] = []
    nonisolated(unsafe) static var focus: CyUiElement = 0
    nonisolated(unsafe) static var hitAt: [Float] = []
    nonisolated(unsafe) static var order: [String] = []

    static func handle(_ index: Int) -> CyUiElement { 0x1_0000_0000 | CyUiElement(index + 1) }

    static func reset() {
        made = []
        layouts = [:]
        styles = [:]
        texts = [:]
        images = [:]
        progress = [:]
        visibility = [:]
        opacity = [:]
        destroyed = []
        focus = 0
        hitAt = []
        order = []
    }

    static func known(_ element: CyUiElement) -> CyResult {
        let index = Int(element & 0xFFFF_FFFF) - 1
        guard element != root, index >= 0, index < made.count, !destroyed.contains(element) else {
            return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such interface element")
        }
        return CY_RESULT_OK
    }
}

/// A behaviour whose HUD has a button, and which counts what reached it.
@Behaviour
final class HudProbe: Behaviour {
    var clicks: [UIEvent] = []
    var seen: [UIEventKind] = []
    var hud = UITree()
    var throwOnEvent = false

    override func onCreate() throws {
        hud = try mountUI {
            Panel("bar") {
                Label("0", colour: 0xFFFF_D34D).id("gold")
                Button("Build", name: "build") { [unowned self] event in
                    self.clicks.append(event)
                }
                .id("build")
            }
            .id("bar")
        }
    }

    override func onUIEvent(_ event: UIEvent) throws {
        seen.append(event.kind)
        if throwOnEvent {
            throw ProbeError.refused
        }
    }
}

private enum ProbeError: Error { case refused }

/// No `onUIEvent`: a button's action still runs.
@Behaviour
final class QuietHud: Behaviour {
    var clicks = 0
    var hud = UITree()

    override func onCreate() throws {
        hud = try mountUI {
            Button("Go") { [unowned self] _ in self.clicks += 1 }.id("go")
        }
    }
}

private enum Registered {
    nonisolated(unsafe) static var vtable = CyBehaviourVTable()
}

final class UITests: XCTestCase {
    override func setUp() {
        super.setUp()
        Fake.reset()
        FakeEngine.install { table in
            table.register_behaviour = { _, _, vtable in
                Registered.vtable = vtable?.pointee ?? CyBehaviourVTable()
                return OpaquePointer(bitPattern: 0xB0)
            }
            table.ui_root = { _, out in
                out?.pointee = Fake.root
                return CY_RESULT_OK
            }
            table.ui_create = { _, parent, desc, out in
                guard let desc = desc?.pointee else { return CY_RESULT_INVALID_ARGUMENT }
                guard parent == Fake.root || Fake.known(parent) == CY_RESULT_OK else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such parent")
                }
                XCTAssertEqual(desc.struct_size, UInt32(MemoryLayout<CyUiElementDesc>.size))
                Fake.made.append(
                    Fake.Made(
                        parent: parent, kind: desc.kind,
                        name: desc.name.map { String(cString: $0) },
                        owner: desc.owner))
                out?.pointee = Fake.handle(Fake.made.count - 1)
                Fake.order.append("create")
                return CY_RESULT_OK
            }
            table.ui_destroy = { _, element in
                let known = Fake.known(element)
                if known == CY_RESULT_OK { Fake.destroyed.append(element) }
                return known
            }
            table.ui_set_layout = { _, element, layout in
                Fake.layouts[element] = layout?.pointee
                Fake.order.append("layout")
                return Fake.known(element)
            }
            table.ui_set_style = { _, element, style in
                Fake.styles[element] = style?.pointee
                return Fake.known(element)
            }
            table.ui_set_text = { _, element, text, colour, scale in
                Fake.texts[element] = (text.map { String(cString: $0) } ?? "", colour, scale)
                return Fake.known(element)
            }
            table.ui_set_image = { _, element, page, uv in
                Fake.images[element] = (page, uv.map { [$0[0], $0[1], $0[2], $0[3]] })
                return Fake.known(element)
            }
            table.ui_set_progress = { _, element, value in
                Fake.progress[element] = value
                return Fake.known(element)
            }
            table.ui_set_visibility = { _, element, visibility in
                Fake.visibility[element] = visibility
                return Fake.known(element)
            }
            table.ui_set_opacity = { _, element, opacity in
                guard opacity >= 0, opacity <= 1 else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "opacity outside [0, 1]")
                }
                Fake.opacity[element] = opacity
                return Fake.known(element)
            }
            table.ui_element_rect = { _, element, out in
                guard let out else { return CY_RESULT_INVALID_ARGUMENT }
                (out[0], out[1], out[2], out[3]) = (372, 190, 102, 74)
                return Fake.known(element)
            }
            table.ui_hit_test = { _, position, out in
                Fake.hitAt = position.map { [$0[0], $0[1]] } ?? []
                out?.pointee = (Fake.hitAt.first ?? 0) > 300 ? Fake.handle(0) : CY_UI_ELEMENT_NULL
                return CY_RESULT_OK
            }
            table.ui_focus = { _, out in
                out?.pointee = Fake.focus
                return CY_RESULT_OK
            }
            table.ui_set_focus = { _, element in
                Fake.focus = element
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    // --- The imperative layer ----------------------------------------------------------------

    func testCreateSendsTheKindTheNameAndTheOwner() throws {
        let root = try UI.root()
        XCTAssertEqual(root.raw, Fake.root)
        let panel = try UI.create(.panel, in: root, name: "selection", owner: Entity(bits: 9))
        let label = try UI.create(.label, in: panel)
        XCTAssertEqual(
            Fake.made,
            [
                Fake.Made(
                    parent: Fake.root, kind: CY_UI_PANEL.rawValue, name: "selection", owner: 9),
                Fake.Made(parent: panel.raw, kind: CY_UI_LABEL.rawValue, name: nil, owner: 0),
            ])
        XCTAssertEqual(label.raw, Fake.handle(1))
    }

    func testALayoutsDefaultsCrossAsTheZerosTheABIReadsAsDefaults() throws {
        let element = try UI.create(.panel, in: UI.root())
        try element.setLayout(UILayout())
        let zero = try XCTUnwrap(Fake.layouts[element.raw])
        XCTAssertEqual(zero.struct_size, UInt32(MemoryLayout<CyUiLayout>.size))
        XCTAssertEqual(zero.model, CY_UI_LAYOUT_FLEX.rawValue)
        XCTAssertEqual(zero.align, CY_UI_ALIGN_STRETCH.rawValue)
        XCTAssertEqual(zero.flags, 0)
        XCTAssertEqual(zero.preferred.0, 0, "a nil width is the content's: zero")
        XCTAssertEqual(zero.maximum.1, 0, "a nil maximum is unbounded: zero")
        XCTAssertEqual(zero.flex_shrink, 1)
        XCTAssertEqual(zero.grid_column_span, 1)

        // The minimap's anchoring, and a column with padding, a gap and a fixed height.
        let map = UILayout.anchored(
            min: Vec2(x: 1, y: 1), max: Vec2(x: 1, y: 1), offsetMin: Vec2(x: -108, y: -80),
            offsetMax: Vec2(x: -6, y: -6)
        ).laying(.absolute)
        try element.setLayout(map)
        var sent = try XCTUnwrap(Fake.layouts[element.raw])
        XCTAssertEqual(sent.model, CY_UI_LAYOUT_ABSOLUTE.rawValue)
        XCTAssertEqual(sent.anchor_min.0, 1)
        XCTAssertEqual(sent.offset_min.1, -80)
        XCTAssertEqual(sent.offset_max.0, -6)

        var column = UILayout().flex(
            .column, gap: 6, align: .centre, padding: UIInsets(horizontal: 5, vertical: 3)
        ).sized(height: 13)
        column.flexShrink = 0
        column.wrap = true
        try element.setLayout(column)
        sent = try XCTUnwrap(Fake.layouts[element.raw])
        XCTAssertEqual(sent.direction, CY_UI_DIRECTION_COLUMN.rawValue)
        XCTAssertEqual(sent.align, CY_UI_ALIGN_CENTRE.rawValue)
        XCTAssertEqual(sent.gap, 6)
        XCTAssertEqual(sent.padding.0, 5)
        XCTAssertEqual(sent.padding.1, 3)
        XCTAssertEqual(sent.preferred.0, 0)
        XCTAssertEqual(sent.preferred.1, 13)
        XCTAssertEqual(sent.flags, CY_UI_LAYOUT_WRAP | CY_UI_LAYOUT_NO_SHRINK)
    }

    func testStyleTextImageProgressVisibilityAndOpacityReachTheirEntries() throws {
        let element = try UI.create(.progress, in: UI.root())
        try element.setStyle(
            UIStyle(
                background: 0xC80E_141C, border: 0xFF3C_5064, cornerRadius: 3, accent: 0xFF4C_C34C,
                clipsChildren: true))
        let style = try XCTUnwrap(Fake.styles[element.raw])
        XCTAssertEqual(style.background, 0xC80E_141C)
        XCTAssertEqual(style.border_colour, 0xFF3C_5064)
        XCTAssertEqual(style.border_width, 1, "a border colour with no width is a one-unit frame")
        XCTAssertEqual(style.accent, 0xFF4C_C34C)
        XCTAssertEqual(style.flags, CY_UI_STYLE_CLIP_CHILDREN)
        XCTAssertEqual(UIStyle(background: 1).raw.border_width, 0)

        try element.setText("Selected: 3 units", colour: 0xFFE6_EBF0, scale: 2)
        XCTAssertEqual(Fake.texts[element.raw]?.0, "Selected: 3 units")
        XCTAssertEqual(Fake.texts[element.raw]?.1, 0xFFE6_EBF0)
        XCTAssertEqual(Fake.texts[element.raw]?.2, 2)

        try element.setImage(page: 2)
        XCTAssertEqual(Fake.images[element.raw]?.0, 2)
        XCTAssertNil(Fake.images[element.raw]?.1, "the whole page crosses as a null rectangle")
        try element.setImage(page: 3, uv: UIRect(x: 0.5, y: 0, width: 0.5, height: 1))
        XCTAssertEqual(Fake.images[element.raw]?.1, [0.5, 0, 0.5, 1])

        try element.setProgress(0.64)
        XCTAssertEqual(Fake.progress[element.raw], 0.64)
        try element.setShown(false)
        XCTAssertEqual(Fake.visibility[element.raw], CY_UI_COLLAPSED.rawValue)
        try element.setVisibility(.hidden)
        XCTAssertEqual(Fake.visibility[element.raw], CY_UI_HIDDEN.rawValue)
        try element.setOpacity(0.5)
        XCTAssertEqual(Fake.opacity[element.raw], 0.5)
    }

    func testARefusalThrowsTheStatusAndTheEnginesMessage() throws {
        let stale = UIElement(raw: 0x7_0000_0042)
        XCTAssertThrowsError(try stale.setProgress(1)) { error in
            guard case CyberdyneError.status(let status, let message) = error else {
                return XCTFail("not a status: \(error)")
            }
            XCTAssertEqual(status, .notFound)
            XCTAssertTrue(message.contains("no such interface element"))
        }
        let element = try UI.create(.panel, in: UI.root())
        XCTAssertThrowsError(try element.setOpacity(2))
        XCTAssertThrowsError(try UIElement.none.destroy())
    }

    func testRectsHitsAndFocusAreReadBack() throws {
        let element = try UI.create(.button, in: UI.root())
        XCTAssertEqual(try element.rect, UIRect(x: 372, y: 190, width: 102, height: 74))
        XCTAssertTrue(UIRect(x: 372, y: 190, width: 102, height: 74).contains(Vec2(x: 400, y: 200)))
        XCTAssertFalse(UIRect(x: 0, y: 0, width: 10, height: 10).contains(Vec2(x: 10, y: 5)))

        XCTAssertEqual(try UI.hitTest(Vec2(x: 400, y: 20)), element)
        XCTAssertEqual(Fake.hitAt, [400, 20])
        XCTAssertNil(try UI.hitTest(Vec2(x: 20, y: 20)), "the world is nil")

        XCTAssertNil(try UI.focus)
        try element.focus()
        XCTAssertEqual(try UI.focus, element)
        try UI.clearFocus()
        XCTAssertNil(try UI.focus)
    }

    func testWithNoEngineEveryCallThrowsUnavailable() {
        FakeEngine.uninstall()
        XCTAssertThrowsError(try UI.root()) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("not a status: \(error)")
            }
            XCTAssertEqual(status, .unavailable)
        }
    }

    // --- The declarative layer ---------------------------------------------------------------

    func testMountCreatesTheTreeDepthFirstAndFindsElementsById() throws {
        let rows = ["Rifleman", "Engineer"]
        let tree = try UI.mount {
            Panel("selection") {
                Label("Selected: 2 units").id("title")
                for (index, name) in rows.enumerated() {
                    Panel("unit-row") {
                        Label(name, name: "name")
                        ProgressBar(0.5, name: "health-bar")
                    }
                    .id("row\(index)")
                    .shown(index == 0)
                }
                if rows.isEmpty {
                    Label("Nothing selected")
                }
            }
            .layout(UILayout().flex(.column))
            .style(UIStyle(background: 0xC80E_141C))
            .id("panel")
            Image(page: 2).opacity(0.5).id("portrait")
        }
        XCTAssertEqual(
            Fake.made.map(\.name),
            [
                "selection", "label", "unit-row", "name", "health-bar", "unit-row", "name",
                "health-bar", "image",
            ])
        XCTAssertEqual(tree.roots, [tree["panel"], tree["portrait"]])
        XCTAssertEqual(tree.elements.count, 9)
        XCTAssertEqual(Fake.made[3].parent, tree["row0"].raw, "a row's label is the row's child")
        XCTAssertTrue(Fake.made.allSatisfy { $0.owner == 0 }, "UI.mount owns nothing")
        // Only what was said is written: no visibility for a shown row, collapsed for a hidden one.
        XCTAssertNil(Fake.visibility[tree["row0"].raw])
        XCTAssertEqual(Fake.visibility[tree["row1"].raw], CY_UI_COLLAPSED.rawValue)
        XCTAssertEqual(Fake.texts[tree["title"].raw]?.0, "Selected: 2 units")
        XCTAssertEqual(Fake.progress[Fake.handle(4)], 0.5)
        XCTAssertEqual(Fake.opacity[tree["portrait"].raw], 0.5)
        XCTAssertEqual(Fake.images[tree["portrait"].raw]?.0, 2)
        XCTAssertEqual(Fake.styles[tree["panel"].raw]?.background, 0xC80E_141C)
        // An element is created, then configured, before its children are made.
        XCTAssertEqual(Array(Fake.order.prefix(3)), ["create", "layout", "create"])
        XCTAssertTrue(tree.contains("title"))
        XCTAssertFalse(tree.contains("missing"))
        XCTAssertEqual(tree["missing"], .none)
    }

    // --- Clicks reach the behaviour ----------------------------------------------------------

    private func create(
        _ type: any BehaviourClass.Type, entity: CyEntity
    ) throws -> (
        CyBehaviourVTable, CyInstance
    ) {
        try Behaviours.register(type)
        let vtable = Registered.vtable
        let raw = try XCTUnwrap(vtable.create?(FakeEngine.handle, entity, vtable.user_data))
        return (vtable, raw)
    }

    private func object<T: Behaviour>(_ raw: CyInstance, as type: T.Type) -> T? {
        Unmanaged<Behaviour>.fromOpaque(UnsafeRawPointer(raw)).takeUnretainedValue() as? T
    }

    private func event(_ kind: CyUiEventKind, _ element: UIElement, owner: CyEntity) -> CyUiEvent {
        var event = CyUiEvent()
        event.struct_size = UInt32(MemoryLayout<CyUiEvent>.size)
        event.kind = kind.rawValue
        event.element = element.raw
        event.owner = owner
        event.position = (120, 110)
        event.button = CY_INPUT_BUTTON_LEFT
        return event
    }

    func testAClickRunsTheButtonsActionThenOnUIEventOnTheBehaviourThatMountedIt() throws {
        let (vtable, raw) = try create(HudProbe.self, entity: 7)
        let probe = try XCTUnwrap(object(raw, as: HudProbe.self))
        XCTAssertNotNil(vtable.ui_event, "every class takes interface events")
        XCTAssertTrue(HudProbe.behaviourCallbacks.contains(.uiEvent))
        XCTAssertTrue(Fake.made.allSatisfy { $0.owner == 7 }, "mountUI owns with the entity")

        let build = probe.hud["build"]
        var click = event(CY_UI_EVENT_CLICK, build, owner: 7)
        vtable.ui_event?(raw, &click, vtable.user_data)
        XCTAssertEqual(probe.clicks.count, 1)
        XCTAssertEqual(probe.clicks.first?.element, build)
        XCTAssertEqual(probe.clicks.first?.position, Vec2(x: 120, y: 110))
        XCTAssertEqual(probe.clicks.first?.button, .left)
        XCTAssertEqual(probe.seen, [.click])

        // A click on an element with no action reaches only `onUIEvent`; a focus never runs one.
        var other = event(CY_UI_EVENT_CLICK, probe.hud["gold"], owner: 7)
        vtable.ui_event?(raw, &other, vtable.user_data)
        var focus = event(CY_UI_EVENT_FOCUS, build, owner: 7)
        vtable.ui_event?(raw, &focus, vtable.user_data)
        XCTAssertEqual(probe.clicks.count, 1)
        XCTAssertEqual(probe.seen, [.click, .click, .focus])

        // An unknown kind, from a newer engine, is ignored rather than misread.
        var unknown = event(CY_UI_EVENT_CLICK, build, owner: 7)
        unknown.kind = 99
        vtable.ui_event?(raw, &unknown, vtable.user_data)
        XCTAssertEqual(probe.clicks.count, 1)

        // A throw disables the behaviour, which then takes nothing more.
        probe.throwOnEvent = true
        vtable.ui_event?(raw, &click, vtable.user_data)
        XCTAssertFalse(probe.isEnabled)
        vtable.ui_event?(raw, &click, vtable.user_data)
        XCTAssertEqual(probe.clicks.count, 2)
        vtable.destroy?(raw, vtable.user_data)
    }

    func testAButtonsActionRunsForAClassThatWroteNoOnUIEvent() throws {
        let (vtable, raw) = try create(QuietHud.self, entity: 3)
        let quiet = try XCTUnwrap(object(raw, as: QuietHud.self))
        XCTAssertFalse(QuietHud.behaviourCallbacks.contains(.uiEvent))
        var click = event(CY_UI_EVENT_CLICK, quiet.hud["go"], owner: 3)
        vtable.ui_event?(raw, &click, vtable.user_data)
        vtable.ui_event?(raw, &click, vtable.user_data)
        XCTAssertEqual(quiet.clicks, 2)

        // Unmounted: the elements are destroyed and the action forgotten.
        try quiet.unmountUI(quiet.hud)
        XCTAssertEqual(Fake.destroyed, [quiet.hud["go"].raw])
        vtable.ui_event?(raw, &click, vtable.user_data)
        XCTAssertEqual(quiet.clicks, 2)
        vtable.destroy?(raw, vtable.user_data)
    }
}
