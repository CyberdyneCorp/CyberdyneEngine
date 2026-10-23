// SPDX-License-Identifier: MIT
// UIToolkitTests.swift — ABI 1.12's widget toolkit through `FakeEngine`: `Widget`, `UI`'s documents
// and navigation, the declarative `mountWidgets` with its bindings, and widget events reaching the
// handlers registered for them through `ui_event`.
//
// The facades' half: the right descriptions, strings and enumerators reach the entries, answers are
// converted, a refusal is a thrown `CyberdyneError`, a binding pushes the game's value and writes
// the player's back. The engine's half — a real toolkit behind the same entries — is
// integration.game_backend_ui.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake toolkit was sent, and the widgets it keeps: widget n is `n + 1`, generation 2.
private enum Kit {
    struct Made {
        var parent: CyUiElement
        var kind: UInt32
        var id: String?
        var text: String?
        var placeholder: String?
        var items: [String]
        var flags: UInt32
        var value: Float
        var minimum: Float
        var maximum: Float
        var step: Float
        var rowHeight: Float
        var owner: CyEntity
        var structSize: UInt32
    }

    nonisolated(unsafe) static var made: [Made] = []
    nonisolated(unsafe) static var texts: [CyUiElement: String] = [:]
    nonisolated(unsafe) static var values: [CyUiElement: Float] = [:]
    nonisolated(unsafe) static var selected: [CyUiElement: UInt32] = [:]
    nonisolated(unsafe) static var items: [CyUiElement: [String]] = [:]
    nonisolated(unsafe) static var enabled: [CyUiElement: UInt32] = [:]
    nonisolated(unsafe) static var classes: [CyUiElement: String] = [:]
    nonisolated(unsafe) static var opened: [CyUiElement: UInt32] = [:]
    nonisolated(unsafe) static var animations: [CyUiElement: CyUiAnimation] = [:]
    nonisolated(unsafe) static var documents: [(text: String?, path: String?, owner: CyEntity)] =
        []
    nonisolated(unsafe) static var sheet = ""
    nonisolated(unsafe) static var finds: [(scope: CyUiElement, id: String)] = []
    nonisolated(unsafe) static var actions: [UInt32] = []
    nonisolated(unsafe) static var writes = 0

    static func handle(_ index: Int) -> CyUiElement { 0x2_0000_0000 | CyUiElement(index + 1) }

    static func reset() {
        made = []
        texts = [:]
        values = [:]
        selected = [:]
        items = [:]
        enabled = [:]
        classes = [:]
        opened = [:]
        animations = [:]
        documents = []
        sheet = ""
        finds = []
        actions = []
        writes = 0
    }

    static func known(_ widget: CyUiElement) -> CyResult {
        let index = Int(widget & 0xFFFF_FFFF) - 1
        guard index >= 0, index < made.count else {
            return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such widget")
        }
        return CY_RESULT_OK
    }

    static func string(_ text: UnsafePointer<CChar>?) -> String? {
        text.map { String(cString: $0) }
    }

    static func strings(_ list: UnsafePointer<UnsafePointer<CChar>?>?, _ count: UInt32) -> [String]
    {
        guard let list else { return [] }
        return (0..<Int(count)).map { string(list[$0]) ?? "" }
    }

    /// Install the toolkit's entries on `table`.
    static func install(_ table: inout CyInterface) {
        table.ui_create_widget = { _, parent, desc, out in
            guard let desc = desc?.pointee else { return CY_RESULT_INVALID_ARGUMENT }
            Kit.made.append(
                Made(
                    parent: parent, kind: desc.kind, id: Kit.string(desc.id),
                    text: Kit.string(desc.text), placeholder: Kit.string(desc.placeholder),
                    items: Kit.strings(desc.items, desc.item_count), flags: desc.flags,
                    value: desc.value, minimum: desc.minimum, maximum: desc.maximum,
                    step: desc.step, rowHeight: desc.row_height, owner: desc.owner,
                    structSize: desc.struct_size))
            let made = Kit.handle(Kit.made.count - 1)
            Kit.texts[made] = Kit.string(desc.text) ?? ""
            out?.pointee = made
            return CY_RESULT_OK
        }
        installWrites(&table)
        installReads(&table)
        installDocuments(&table)
    }

    private static func installWrites(_ table: inout CyInterface) {
        table.ui_widget_set_text = { _, widget, text in
            Kit.texts[widget] = Kit.string(text) ?? ""
            Kit.writes += 1
            return Kit.known(widget)
        }
        table.ui_widget_set_value = { _, widget, value in
            Kit.values[widget] = value
            Kit.writes += 1
            return Kit.known(widget)
        }
        table.ui_widget_set_selected = { _, widget, index in
            Kit.selected[widget] = index
            Kit.writes += 1
            return Kit.known(widget)
        }
        table.ui_widget_set_items = { _, widget, list, count in
            Kit.items[widget] = Kit.strings(list, count)
            return Kit.known(widget)
        }
        table.ui_widget_set_enabled = { _, widget, enabled in
            Kit.enabled[widget] = enabled
            return Kit.known(widget)
        }
        table.ui_widget_set_classes = { _, widget, classes in
            Kit.classes[widget] = Kit.string(classes) ?? ""
            return Kit.known(widget)
        }
        table.ui_widget_open = { _, widget, open in
            Kit.opened[widget] = open
            return Kit.known(widget)
        }
        table.ui_widget_animate = { _, widget, animation in
            guard let animation = animation?.pointee, animation.easing <= 6 else {
                return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "ui_widget_animate: no easing")
            }
            Kit.animations[widget] = animation
            return Kit.known(widget)
        }
        table.ui_destroy = { _, widget in Kit.known(widget) }
        table.ui_navigate = { _, action, consumed in
            Kit.actions.append(action)
            consumed?.pointee = action == CY_UI_ACTION_CANCEL.rawValue ? 0 : 1
            return CY_RESULT_OK
        }
    }

    private static func installReads(_ table: inout CyInterface) {
        table.ui_widget_text = { _, widget, buffer, capacity, size in
            let bytes = Array((Kit.texts[widget] ?? "").utf8)
            size?.pointee = UInt32(bytes.count)
            if let buffer, capacity > 0 {
                let count = min(bytes.count, Int(capacity) - 1)
                for index in 0..<count { buffer[index] = CChar(bitPattern: bytes[index]) }
                buffer[count] = 0
            }
            return Kit.known(widget)
        }
        table.ui_widget_value = { _, widget, out in
            out?.pointee = Kit.values[widget] ?? 0
            return Kit.known(widget)
        }
        table.ui_widget_selected = { _, widget, out in
            out?.pointee = Kit.selected[widget] ?? 0
            return Kit.known(widget)
        }
        table.ui_find = { _, scope, id, out in
            let wanted = Kit.string(id) ?? ""
            Kit.finds.append((scope, wanted))
            let index = Kit.made.firstIndex { $0.id == wanted }
            out?.pointee = index.map(Kit.handle) ?? CY_UI_ELEMENT_NULL
            return CY_RESULT_OK
        }
    }

    private static func installDocuments(_ table: inout CyInterface) {
        table.ui_load_document = { _, _, desc, out in
            guard let desc = desc?.pointee else { return CY_RESULT_INVALID_ARGUMENT }
            let text = desc.text.map {
                String(
                    decoding: UnsafeRawBufferPointer(start: $0, count: Int(desc.text_size)),
                    as: UTF8.self)
            }
            if let text, !text.hasPrefix("cyui 1") {
                return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "ui_load_document: line 1")
            }
            Kit.documents.append((text, Kit.string(desc.path), desc.owner))
            out?.pointee = 0x2_0000_0100
            return CY_RESULT_OK
        }
        table.ui_load_stylesheet = { _, text, size in
            Kit.sheet =
                text.map {
                    String(
                        decoding: UnsafeRawBufferPointer(start: $0, count: Int(size)), as: UTF8.self
                    )
                } ?? ""
            return CY_RESULT_OK
        }
    }
}

private enum Registered {
    nonisolated(unsafe) static var vtable = CyBehaviourVTable()
}

/// A settings menu whose state is bound to its widgets, counting what reached it.
@Behaviour
final class SettingsMenu: Behaviour {
    var volume: Float = 0.25
    var subtitles = true
    var quality = 1
    var name = "Ada"
    var changes: [Float] = []
    var plays = 0
    var submitted: [String] = []
    var menu: WidgetTree?

    override func onCreate() throws {
        menu = try mountWidgets {
            WidgetNode.column(id: "menu") {
                WidgetNode.label("Options", id: "title").classes("title")
                WidgetNode.slider(in: 0...1, step: 0.05, id: "volume")
                    .bind(.value(self, \.volume))
                    .onChange { [unowned self] event in
                        // The binding wrote first: the state is already the event's.
                        self.changes.append(self.volume)
                        XCTAssertEqual(self.volume, event.value)
                    }
                WidgetNode.checkbox("Subtitles", id: "subtitles").bind(.checked(self, \.subtitles))
                WidgetNode.dropdown(["Low", "Medium", "High"], id: "quality")
                    .bind(.selected(self, \.quality))
                WidgetNode.textField(placeholder: "Your name", id: "name")
                    .bind(.text(self, \.name))
                    .onSubmit { [unowned self] _ in self.submitted.append(self.name) }
                WidgetNode.button("Play", id: "play") { [unowned self] _ in self.plays += 1 }
                    .appear(.pop())
            }
        }
    }
}

final class UIToolkitTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Kit.reset()
        FakeEngine.install { table in
            table.register_behaviour = { _, _, vtable in
                Registered.vtable = vtable?.pointee ?? CyBehaviourVTable()
                return OpaquePointer(bitPattern: 0xB1)
            }
            Kit.install(&table)
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    // --- The imperative layer ----------------------------------------------------------------

    func testCreateWidgetSendsTheWholeDescriptionAndItsDefaultsAsZeros() throws {
        var desc = UIWidgetDesc(.dropdown, id: "quality")
        desc.items = ["Low", "High"]
        desc.enabled = false
        let quality = try UI.createWidget(desc, owner: Entity(bits: 9))
        let panel = try UI.createWidget(UIWidgetDesc(.column))
        _ = try UI.createWidget(UIWidgetDesc(.checkbox), in: panel)
        XCTAssertEqual(quality.raw, Kit.handle(0))
        let first = Kit.made[0]
        XCTAssertEqual(first.structSize, UInt32(MemoryLayout<CyUiWidgetDesc>.size))
        XCTAssertEqual(first.kind, CY_UI_WIDGET_DROPDOWN.rawValue)
        XCTAssertEqual(first.id, "quality")
        XCTAssertNil(first.text, "an absent string crosses as null")
        XCTAssertEqual(first.items, ["Low", "High"])
        XCTAssertEqual(first.flags, CY_UI_WIDGET_DISABLED)
        XCTAssertEqual(first.owner, 9)
        XCTAssertEqual(first.parent, CY_UI_ELEMENT_NULL, "nil is the toolkit's root")
        XCTAssertEqual(first.rowHeight, 0, "a nil row height is the engine's: zero")
        XCTAssertEqual(Kit.made[2].parent, panel.raw)
        XCTAssertEqual(Kit.made[2].items, [])
    }

    func testAWidgetsTextValueSelectionAndItemsRoundTrip() throws {
        let field = try UI.createWidget(UIWidgetDesc(.textField))
        try field.setText("Grace Hopper — ünïcode")
        XCTAssertEqual(try field.text, "Grace Hopper — ünïcode")
        try field.setText("")
        XCTAssertEqual(try field.text, "")

        let slider = try UI.createWidget(UIWidgetDesc(.slider))
        try slider.setValue(0.75)
        XCTAssertEqual(try slider.value, 0.75)
        try slider.setChecked(true)
        XCTAssertTrue(try slider.isChecked)

        let list = try UI.createWidget(UIWidgetDesc(.listView))
        try list.setItems(["a", "b", "c"])
        XCTAssertEqual(Kit.items[list.raw], ["a", "b", "c"])
        try list.select(2)
        XCTAssertEqual(try list.selected, 2)
        try list.select(-4)
        XCTAssertEqual(Kit.selected[list.raw], 0, "a negative index is clamped to zero")

        try list.setEnabled(false)
        try list.setClasses("wide primary")
        try list.open()
        XCTAssertEqual(Kit.enabled[list.raw], 0)
        XCTAssertEqual(Kit.classes[list.raw], "wide primary")
        XCTAssertEqual(Kit.opened[list.raw], 1)
        try list.close()
        XCTAssertEqual(Kit.opened[list.raw], 0)
    }

    func testAnAnimationCrossesWithItsEasingAndItsStateFeedbackFlag() throws {
        let badge = try UI.createWidget(UIWidgetDesc(.label))
        try badge.animate(.pop(from: 0.5, duration: 0.4))
        var raw = try XCTUnwrap(Kit.animations[badge.raw])
        XCTAssertEqual(raw.struct_size, UInt32(MemoryLayout<CyUiAnimation>.size))
        XCTAssertEqual(raw.property, CY_UI_ANIMATE_SCALE.rawValue)
        XCTAssertEqual(raw.from, 0.5)
        XCTAssertEqual(raw.to, 1)
        XCTAssertEqual(raw.easing, CY_UI_EASING_BACK.rawValue)
        XCTAssertEqual(raw.flags, 0, "decorative by default")

        var press = UIAnimation.background(from: 0xFF20_2020, to: 0xFF40_4040, duration: 0.1)
        press.stateFeedback = true
        try badge.animate(press)
        raw = try XCTUnwrap(Kit.animations[badge.raw])
        XCTAssertEqual(raw.property, CY_UI_ANIMATE_BACKGROUND.rawValue)
        XCTAssertEqual(raw.from_colour, 0xFF20_2020)
        XCTAssertEqual(raw.to_colour, 0xFF40_4040)
        XCTAssertEqual(raw.flags, CY_UI_ANIMATION_STATE_FEEDBACK)
        XCTAssertEqual(UIAnimation.slideX(from: -40).raw.from, -40)
        XCTAssertEqual(
            UIAnimation.fade(from: 1, to: 0).raw.property, CY_UI_ANIMATE_OPACITY.rawValue)
    }

    func testADocumentCrossesAsItsBytesOrItsPathAndIsSearchedFromItsRoot() throws {
        let text = "cyui 1\ncolumn #menu\n  label #title text=\"Ünïcode\"\n"
        let menu = try UI.loadDocument(text, owner: Entity(bits: 4))
        XCTAssertEqual(Kit.documents.first?.text, text, "the whole text, by its byte count")
        XCTAssertEqual(Kit.documents.first?.owner, 4)
        XCTAssertEqual(menu.root.raw, 0x2_0000_0100)
        _ = try UI.createWidget(UIWidgetDesc(.label, id: "title"))
        XCTAssertEqual(menu["title"], Widget(raw: Kit.handle(0)))
        XCTAssertEqual(Kit.finds.last?.scope, menu.root.raw)
        XCTAssertEqual(menu["missing"], .none)

        _ = try UI.loadDocument(path: "ui/menu.cyui")
        XCTAssertEqual(Kit.documents.last?.path, "ui/menu.cyui")
        XCTAssertNil(Kit.documents.last?.text)

        try UI.loadStylesheet(".title { color: #ffd34d; }")
        XCTAssertEqual(Kit.sheet, ".title { color: #ffd34d; }")
    }

    func testARefusalThrowsTheStatusAndTheEnginesMessage() throws {
        XCTAssertThrowsError(try UI.loadDocument("column #menu")) { error in
            XCTAssertEqual(
                error as? CyberdyneError,
                .status(.invalidArgument, message: "ui_load_document: line 1"))
        }
        XCTAssertThrowsError(try Widget(raw: Kit.handle(40)).setValue(1)) { error in
            XCTAssertEqual(error as? CyberdyneError, .status(.notFound, message: "no such widget"))
        }
    }

    func testNavigationSaysWhetherTheInterfaceTookTheAction() throws {
        XCTAssertTrue(try UI.navigate(.navigateDown))
        XCTAssertFalse(try UI.navigate(.cancel))
        XCTAssertEqual(
            Kit.actions, [CY_UI_ACTION_NAVIGATE_DOWN.rawValue, CY_UI_ACTION_CANCEL.rawValue])
    }

    func testWithNoEngineEveryCallThrowsUnavailable() {
        FakeEngine.uninstall()
        XCTAssertThrowsError(try UI.createWidget(UIWidgetDesc(.button)))
        XCTAssertThrowsError(try UI.loadDocument("cyui 1\n"))
        XCTAssertThrowsError(try UI.navigate(.accept))
        XCTAssertThrowsError(try Widget.none.text)
    }

    // --- The declarative layer and its events ------------------------------------------------

    private func create() throws -> (CyBehaviourVTable, CyInstance, SettingsMenu) {
        try Behaviours.register(SettingsMenu.self)
        let vtable = Registered.vtable
        let raw = try XCTUnwrap(vtable.create?(FakeEngine.handle, 7, vtable.user_data))
        let object = Unmanaged<Behaviour>.fromOpaque(UnsafeRawPointer(raw))
            .takeUnretainedValue()
        return (vtable, raw, try XCTUnwrap(object as? SettingsMenu))
    }

    private func event(
        _ kind: CyUiEventKind, _ widget: Widget, value: Float = 0, index: UInt32 = 0
    ) -> CyUiEvent {
        var event = CyUiEvent()
        event.struct_size = UInt32(MemoryLayout<CyUiEvent>.size)
        event.kind = kind.rawValue
        event.element = widget.raw
        event.owner = 7
        event.position = (value, 0)
        event.button = index
        return event
    }

    func testMountWidgetsMakesTheTreeDepthFirstOwnedByTheBehaviourWithItsBindingsPushed() throws {
        let (vtable, raw, menu) = try create()
        let tree = try XCTUnwrap(menu.menu)
        XCTAssertEqual(Kit.made.map(\.kind), [2, 6, 13, 11, 16, 15, 9].map { UInt32($0) })
        XCTAssertTrue(Kit.made.allSatisfy { $0.owner == 7 }, "mountWidgets owns with the entity")
        XCTAssertEqual(Kit.made[1].parent, tree["menu"].raw)
        XCTAssertEqual(Kit.made[2].maximum, 1)
        XCTAssertEqual(Kit.made[2].step, 0.05)
        XCTAssertEqual(Kit.made[4].items, ["Low", "Medium", "High"])
        XCTAssertEqual(Kit.made[5].placeholder, "Your name")
        XCTAssertEqual(tree.roots, [tree["menu"]])
        XCTAssertEqual(tree.widgets.count, 7)
        // Every binding's first push, and the button's entrance.
        XCTAssertEqual(Kit.values[tree["volume"].raw], 0.25)
        XCTAssertEqual(Kit.values[tree["subtitles"].raw], 1)
        XCTAssertEqual(Kit.selected[tree["quality"].raw], 1)
        XCTAssertEqual(Kit.texts[tree["name"].raw], "Ada")
        XCTAssertEqual(Kit.animations[tree["play"].raw]?.property, CY_UI_ANIMATE_SCALE.rawValue)
        XCTAssertEqual(try tree.refresh(), 0, "nothing changed since the first push")
        vtable.destroy?(raw, vtable.user_data)
    }

    func testThePlayersChangeIsWrittenBackBeforeTheNodesOwnHandlerRuns() throws {
        let (vtable, raw, menu) = try create()
        let tree = try XCTUnwrap(menu.menu)
        var changed = event(CY_UI_EVENT_CHANGED, tree["volume"], value: 0.6)
        vtable.ui_event?(raw, &changed, vtable.user_data)
        XCTAssertEqual(menu.volume, 0.6)
        XCTAssertEqual(menu.changes, [0.6])

        var off = event(CY_UI_EVENT_CHANGED, tree["subtitles"], value: 0)
        vtable.ui_event?(raw, &off, vtable.user_data)
        XCTAssertFalse(menu.subtitles)
        var picked = event(CY_UI_EVENT_CHANGED, tree["quality"], index: 2)
        vtable.ui_event?(raw, &picked, vtable.user_data)
        XCTAssertEqual(menu.quality, 2)

        // A text field's change is read back from the widget; its Enter runs `onSubmit`.
        Kit.texts[tree["name"].raw] = "Grace"
        var typed = event(CY_UI_EVENT_SUBMITTED, tree["name"])
        vtable.ui_event?(raw, &typed, vtable.user_data)
        XCTAssertEqual(menu.name, "Grace")
        XCTAssertEqual(menu.submitted, ["Grace"])

        var click = event(CY_UI_EVENT_CLICK, tree["play"])
        vtable.ui_event?(raw, &click, vtable.user_data)
        XCTAssertEqual(menu.plays, 1)
        XCTAssertTrue(menu.isEnabled)

        // The player's values are not pushed straight back.
        let before = Kit.writes
        XCTAssertEqual(try tree.refresh(), 0)
        XCTAssertEqual(Kit.writes, before)
        vtable.destroy?(raw, vtable.user_data)
    }

    func testRefreshPushesOnlyWhatTheGameChanged() throws {
        let (vtable, raw, menu) = try create()
        let tree = try XCTUnwrap(menu.menu)
        menu.volume = 0.9
        menu.quality = 0
        let before = Kit.writes
        XCTAssertEqual(try tree.refresh(), 2)
        XCTAssertEqual(Kit.writes, before + 2)
        XCTAssertEqual(Kit.values[tree["volume"].raw], 0.9)
        XCTAssertEqual(Kit.selected[tree["quality"].raw], 0)
        XCTAssertEqual(try tree.refresh(), 0)

        // Unmounted: the handlers are forgotten.
        try menu.unmountWidgets(tree)
        var click = event(CY_UI_EVENT_CLICK, tree["play"])
        vtable.ui_event?(raw, &click, vtable.user_data)
        XCTAssertEqual(menu.plays, 0)
        vtable.destroy?(raw, vtable.user_data)
    }

    func testAHandlerRegisteredByWidgetAndKindRunsForThatKindOnly() throws {
        let (vtable, raw, menu) = try create()
        let title = try XCTUnwrap(menu.menu)["title"]
        var opened = 0
        menu.on(.opened, of: title) { _ in opened += 1 }
        var open = event(CY_UI_EVENT_OPENED, title)
        var close = event(CY_UI_EVENT_CLOSED, title)
        vtable.ui_event?(raw, &open, vtable.user_data)
        vtable.ui_event?(raw, &close, vtable.user_data)
        XCTAssertEqual(opened, 1)
        menu.forgetHandlers(of: title)
        vtable.ui_event?(raw, &open, vtable.user_data)
        XCTAssertEqual(opened, 1)
        vtable.destroy?(raw, vtable.user_data)
    }
}
