// SPDX-License-Identifier: MIT
// UIToolkit.swift — ABI 1.11's widget toolkit: real widgets (sliders, text fields, drop-downs,
// lists, tabs, dialogs), `.cyui` documents, `.cyss` sheets, animations and focus navigation. The
// declarative layer over it is UIWidgetBuilder.swift.
//
//     let menu = try loadUI(document: menuText)                 // a behaviour's: it owns the events
//     try menu["volume"].setValue(settings.volume)
//     on(.changed, of: menu["volume"]) { [unowned self] in self.settings.volume = $0.value }
//     on(.click, of: menu["play"]) { [unowned self] _ in self.start() }
//     try menu["title"].animate(.fade(from: 0, to: 1, duration: 0.3))
//     _ = try UI.navigate(.navigateDown)                         // a game that maps its own input
//
// A WIDGET IS AN ELEMENT. `Widget.element` is the same handle as a `UIElement`, and 1.6's
// `destroy`, `setVisibility`, `rect` and `focus` take it; the 1.6 writes that assume an element's
// kind (`setText(_:colour:scale:)`, `setStyle`) do not, and answer `.notFound`.
//
// EVENTS. A widget's events go to the behaviours of the entity it was made for — `owner`, or for a
// behaviour's `loadUI` and `mountWidgets`, its own entity — through `CyBehaviourVTable.ui_event`:
// to the handler `on(_:of:run:)` registered, then to `Behaviour.onUIEvent`. A CHANGED event
// carries the new value in `UIEvent.value` and a selection in `UIEvent.index`; a widget's text is
// read back with `Widget.text`.

import CyberdyneABI
import CyberdyneCore

/// A widget of the toolkit. A handle, not an owner: destroying it is explicit.
public struct Widget: Hashable, Sendable {
    /// The same handle as a `UIElement`.
    public let raw: CyUiElement

    /// Wrap a handle the engine returned.
    public init(raw: CyUiElement) {
        self.raw = raw
    }

    /// No widget: every call through it throws `.notFound`.
    public static let none = Widget(raw: CY_UI_ELEMENT_NULL)
    /// True for no widget.
    public var isNone: Bool { raw == CY_UI_ELEMENT_NULL }
    /// The widget as a 1.6 element: `destroy`, `setVisibility`, `rect`, `focus`.
    public var element: UIElement { UIElement(raw: raw) }

    /// A caption, a text field's text, a rich text's markup.
    public var text: String {
        get throws {
            let engine = try GameServices.engine()
            var size: UInt32 = 0
            try engine.uiWidgetText(widget: raw, buffer: nil, capacity: 0, size: &size)
            var buffer = [CChar](repeating: 0, count: Int(size) + 1)
            try buffer.withUnsafeMutableBufferPointer {
                try engine.uiWidgetText(
                    widget: raw, buffer: $0.baseAddress, capacity: UInt32($0.count), size: &size)
            }
            return String(
                decoding: buffer.prefix(Int(size)).map { UInt8(bitPattern: $0) },
                as: UTF8.self)
        }
    }

    /// Replace the caption, a text field's text or a rich text's markup.
    public func setText(_ text: String) throws {
        let engine = try GameServices.engine()
        try text.withCString { try engine.uiWidgetSetText(widget: raw, text: $0) }
    }

    /// A slider's or progress bar's value; a toggle's or checkbox's 0 or 1.
    public var value: Float {
        get throws {
            var out: Float = 0
            try GameServices.engine().uiWidgetValue(widget: raw, into: &out)
            return out
        }
    }

    /// Set the value, clamped to the widget's range. Raises no CHANGED event: the game set it.
    public func setValue(_ value: Float) throws {
        try GameServices.engine().uiWidgetSetValue(widget: raw, value: value)
    }

    /// A toggle's or checkbox's state.
    public var isChecked: Bool {
        get throws { try value >= 0.5 }
    }

    /// Turn a toggle or a checkbox on or off.
    public func setChecked(_ checked: Bool) throws {
        try setValue(checked ? 1 : 0)
    }

    /// A radio group's, drop-down's, list's or tabs' selected index.
    public var selected: Int {
        get throws {
            var out: UInt32 = 0
            try GameServices.engine().uiWidgetSelected(widget: raw, into: &out)
            return Int(out)
        }
    }

    /// Select an index, clamped to the items.
    public func select(_ index: Int) throws {
        try GameServices.engine().uiWidgetSetSelected(widget: raw, index: UInt32(clamping: index))
    }

    /// A radio group's, drop-down's or list's items.
    public func setItems(_ items: [String]) throws {
        let engine = try GameServices.engine()
        try withCStrings(items) {
            try engine.uiWidgetSetItems(widget: raw, items: $0, count: UInt32(items.count))
        }
    }

    /// Taking input, or refusing it and drawn `:disabled`.
    public func setEnabled(_ enabled: Bool) throws {
        try GameServices.engine().uiWidgetSetEnabled(widget: raw, enabled: enabled ? 1 : 0)
    }

    /// Replace the widget's classes: space-separated, at most four.
    public func setClasses(_ classes: String) throws {
        let engine = try GameServices.engine()
        try classes.withCString { try engine.uiWidgetSetClasses(widget: raw, classes: $0) }
    }

    /// Open a drop-down's list or a dialog. A dialog is modal until it closes.
    public func open() throws {
        try GameServices.engine().uiWidgetOpen(widget: raw, open: 1)
    }

    /// Close a drop-down's list or a dialog.
    public func close() throws {
        try GameServices.engine().uiWidgetOpen(widget: raw, open: 0)
    }

    /// Start an animation, replacing one of the same property.
    public func animate(_ animation: UIAnimation) throws {
        var value = animation.raw
        try GameServices.engine().uiWidgetAnimate(widget: raw, animation: &value)
    }

    /// The first widget at or under this one whose id is `id`, or `.none`.
    public func find(_ id: String) throws -> Widget {
        try UI.find(id, in: self)
    }

    /// Shown, or collapsed out of layout.
    public func setShown(_ shown: Bool) throws {
        try element.setShown(shown)
    }

    /// Give the widget focus.
    public func focus() throws {
        try element.focus()
    }

    /// Destroy the widget and everything under it.
    public func destroy() throws {
        try element.destroy()
    }
}

/// One animation of one paint property: opacity, a render offset, a scale, or the background.
/// Layout never sees it; the engine evaluates it every frame, and nothing crosses the boundary
/// until the next is started.
public struct UIAnimation: Equatable, Sendable {
    /// What it drives.
    public var property: UIAnimatedProperty
    /// The start and the end, for every property but the background.
    public var from: Float = 0
    public var to: Float = 1
    /// The background's start and end, premultiplied.
    public var fromColour: UIColour = 0
    public var toColour: UIColour = 0
    /// Seconds.
    public var duration: Float = 0.25
    public var delay: Float = 0
    public var easing: UIEasing = .easeOut
    /// State feedback still plays under the reduced-motion preference; decoration jumps to its end.
    public var stateFeedback = false

    /// An animation of `property` from `from` to `to`.
    public init(
        _ property: UIAnimatedProperty, from: Float = 0, to: Float = 1, duration: Float = 0.25,
        delay: Float = 0, easing: UIEasing = .easeOut
    ) {
        self.property = property
        self.from = from
        self.to = to
        self.duration = duration
        self.delay = delay
        self.easing = easing
    }

    /// Opacity from `from` to `to`.
    public static func fade(from: Float, to: Float, duration: Float = 0.25) -> UIAnimation {
        UIAnimation(.opacity, from: from, to: to, duration: duration)
    }

    /// In from `offset` units to the left (negative) or right along x, settling at its place.
    public static func slideX(from offset: Float, duration: Float = 0.25) -> UIAnimation {
        UIAnimation(.translateX, from: offset, to: 0, duration: duration)
    }

    /// In from `offset` units above (negative) or below along y.
    public static func slideY(from offset: Float, duration: Float = 0.25) -> UIAnimation {
        UIAnimation(.translateY, from: offset, to: 0, duration: duration)
    }

    /// Grown from `scale` to its size, overshooting a little.
    public static func pop(from scale: Float = 0.8, duration: Float = 0.25) -> UIAnimation {
        UIAnimation(.scale, from: scale, to: 1, duration: duration, easing: .back)
    }

    /// The background from one colour to another.
    public static func background(
        from: UIColour, to: UIColour, duration: Float = 0.25
    ) -> UIAnimation {
        var animation = UIAnimation(.background, duration: duration)
        animation.fromColour = from
        animation.toColour = to
        return animation
    }

    /// In the ABI's spelling.
    public var raw: CyUiAnimation {
        var value = CyUiAnimation()
        value.struct_size = UInt32(MemoryLayout<CyUiAnimation>.size)
        value.property = property.rawValue
        value.from = from
        value.to = to
        value.from_colour = fromColour
        value.to_colour = toColour
        value.duration = duration
        value.delay = delay
        value.easing = easing.rawValue
        value.flags = stateFeedback ? CY_UI_ANIMATION_STATE_FEEDBACK : 0
        return value
    }
}

/// What a widget is made with. Every default crosses as the zero the ABI reads as that default.
public struct UIWidgetDesc: Equatable, Sendable {
    /// What the widget is.
    public var kind: UIWidgetKind
    /// The name `find` and a sheet's `#id` know it by.
    public var id: String?
    /// Space-separated classes, at most four.
    public var classes: String?
    /// A caption, a text field's text, a rich text's markup.
    public var text: String?
    /// A text field's hint.
    public var placeholder: String?
    /// A tab page's or a dialog's title.
    public var title: String?
    /// A radio group's, drop-down's or list's items.
    public var items: [String] = []
    public var value: Float = 0
    public var minimum: Float = 0
    public var maximum: Float = 1
    /// A slider's step; zero is a hundredth of its range.
    public var step: Float = 0
    /// A grid's column count.
    public var columns: UInt32 = 0
    /// A list's row height; nil is the engine's.
    public var rowHeight: Float?
    public var checked = false
    public var enabled = true

    /// A widget of `kind`.
    public init(_ kind: UIWidgetKind, id: String? = nil) {
        self.kind = kind
        self.id = id
    }
}

/// A loaded `.cyui` document: its first top-level widget, and its widgets by id.
public struct UIDocument: Equatable, Sendable {
    /// The first top-level widget.
    public let root: Widget

    /// The widget whose id is `id`, at or under the root; `.none` when there is none, so a write
    /// through it throws `.notFound` rather than the lookup trapping.
    public subscript(_ id: String) -> Widget {
        (try? root.find(id)) ?? .none
    }
}

extension UI {
    /// A widget, the last child of `parent` (the toolkit's root when nil), whose events go to
    /// `owner`'s behaviours.
    public static func createWidget(
        _ desc: UIWidgetDesc, in parent: Widget? = nil, owner: Entity = .null
    ) throws -> Widget {
        let engine = try GameServices.engine()
        var out = CY_UI_ELEMENT_NULL
        try withWidgetDesc(desc, owner: owner) { raw in
            try engine.uiCreateWidget(
                parent: parent?.raw ?? CY_UI_ELEMENT_NULL, desc: &raw, into: &out)
        }
        return Widget(raw: out)
    }

    /// Load a `.cyui` document's text under `parent` (the toolkit's root when nil). Its `use`d
    /// documents and sheets are read by the embedder.
    public static func loadDocument(
        _ text: String, in parent: Widget? = nil, owner: Entity = .null
    ) throws -> UIDocument {
        var bytes = Array(text.utf8)
        return try bytes.withUnsafeMutableBufferPointer { buffer in
            try buffer.withMemoryRebound(to: CChar.self) {
                try load(text: $0.baseAddress, size: UInt64($0.count), path: nil, parent, owner)
            }
        }
    }

    /// Load the document at `path` in the project; the embedder reloads it when it is saved.
    public static func loadDocument(
        path: String, in parent: Widget? = nil, owner: Entity = .null
    ) throws -> UIDocument {
        try path.withCString { try load(text: nil, size: 0, path: $0, parent, owner) }
    }

    /// Replace the style sheet, over the built-in theme. A sheet with a mistake throws, naming it;
    /// what parsed still applies.
    public static func loadStylesheet(_ sheet: String) throws {
        let engine = try GameServices.engine()
        var bytes = Array(sheet.utf8)
        try bytes.withUnsafeMutableBufferPointer { buffer in
            try buffer.withMemoryRebound(to: CChar.self) {
                try engine.uiLoadStylesheet(text: $0.baseAddress, size: UInt64($0.count))
            }
        }
    }

    /// The first widget whose id is `id` at or under `scope` — or anywhere this module made, when
    /// nil — or `.none`.
    public static func find(_ id: String, in scope: Widget? = nil) throws -> Widget {
        let engine = try GameServices.engine()
        var out = CY_UI_ELEMENT_NULL
        try id.withCString {
            try engine.uiFind(scope: scope?.raw ?? CY_UI_ELEMENT_NULL, id: $0, into: &out)
        }
        return Widget(raw: out)
    }

    /// Deliver a navigation action. True when the interface took it, false when it is the game's.
    public static func navigate(_ action: UIAction) throws -> Bool {
        var consumed: UInt32 = 0
        try GameServices.engine().uiNavigate(action: action.rawValue, consumed: &consumed)
        return consumed != 0
    }

    private static func load(
        text: UnsafePointer<CChar>?, size: UInt64, path: UnsafePointer<CChar>?,
        _ parent: Widget?, _ owner: Entity
    ) throws -> UIDocument {
        var desc = CyUiDocumentDesc()
        desc.struct_size = UInt32(MemoryLayout<CyUiDocumentDesc>.size)
        desc.text = text
        desc.text_size = size
        desc.path = path
        desc.owner = owner.bits
        var out = CY_UI_ELEMENT_NULL
        try GameServices.engine().uiLoadDocument(
            parent: parent?.raw ?? CY_UI_ELEMENT_NULL, desc: &desc, into: &out)
        return UIDocument(root: Widget(raw: out))
    }
}

extension UIEvent {
    /// A CHANGED or SUBMITTED event's value: a slider's, a toggle's 0 or 1.
    public var value: Float { position.x }
    /// A CHANGED or SUBMITTED event's selected index.
    public var index: Int { Int(button.rawValue) }
    /// The widget it happened to.
    public var widget: Widget { Widget(raw: element.raw) }
}

extension Behaviour {
    /// Load a `.cyui` document for this behaviour: its entity owns every widget, so their events
    /// come back here.
    public func loadUI(document text: String, in parent: Widget? = nil) throws -> UIDocument {
        try UI.loadDocument(text, in: parent, owner: entity)
    }

    /// Load the document at `path` for this behaviour; it reloads when it is saved, keeping state.
    public func loadUI(path: String, in parent: Widget? = nil) throws -> UIDocument {
        try UI.loadDocument(path: path, in: parent, owner: entity)
    }

    /// Run `handler` when `kind` happens to `widget` — before `onUIEvent`. A second registration
    /// for the same widget and kind replaces the first.
    public func on(_ kind: UIEventKind, of widget: Widget, run handler: @escaping UIEventHandler) {
        uiEventHandlers[widget.raw, default: [:]][kind] = handler
    }

    /// Forget every handler of `widget`.
    public func forgetHandlers(of widget: Widget) {
        uiEventHandlers[widget.raw] = nil
        uiHandlers[widget.raw] = nil
    }
}

/// `desc` in the ABI's spelling, its strings borrowed for `body`.
func withWidgetDesc<Result>(
    _ desc: UIWidgetDesc, owner: Entity, _ body: (inout CyUiWidgetDesc) throws -> Result
) throws -> Result {
    var raw = CyUiWidgetDesc()
    raw.struct_size = UInt32(MemoryLayout<CyUiWidgetDesc>.size)
    raw.kind = desc.kind.rawValue
    raw.item_count = UInt32(desc.items.count)
    raw.flags =
        (desc.checked ? CY_UI_WIDGET_CHECKED : 0) | (desc.enabled ? 0 : CY_UI_WIDGET_DISABLED)
    raw.value = desc.value
    raw.minimum = desc.minimum
    // The ABI reads a zero maximum as one; a range that truly ends at zero is spelt by its minimum.
    raw.maximum = desc.maximum
    raw.step = desc.step
    raw.columns = desc.columns
    raw.row_height = desc.rowHeight ?? 0
    raw.owner = owner.bits
    let strings = [desc.id, desc.classes, desc.text, desc.placeholder, desc.title]
    return try withOptionalCStrings(strings) { pointers in
        raw.id = pointers[0]
        raw.classes = pointers[1]
        raw.text = pointers[2]
        raw.placeholder = pointers[3]
        raw.title = pointers[4]
        return try withCStrings(desc.items) { items in
            raw.items = items
            return try body(&raw)
        }
    }
}

/// A copy of `string` as a C string, freed by `releaseCString`.
private func copyCString(_ string: String) -> UnsafePointer<CChar> {
    let bytes = Array(string.utf8CString)
    let storage = UnsafeMutablePointer<CChar>.allocate(capacity: bytes.count)
    storage.initialize(from: bytes, count: bytes.count)
    return UnsafePointer(storage)
}

private func releaseCString(_ pointer: UnsafePointer<CChar>?) {
    pointer.map { UnsafeMutablePointer(mutating: $0).deallocate() }
}

/// `strings` as C strings, nil for nil, borrowed for `body`.
func withOptionalCStrings<Result>(
    _ strings: [String?], _ body: ([UnsafePointer<CChar>?]) throws -> Result
) rethrows -> Result {
    let copies = strings.map { $0.map(copyCString) }
    defer { copies.forEach(releaseCString) }
    return try body(copies)
}

/// `strings` as an array of C strings, borrowed for `body`; nil for none.
func withCStrings<Result>(
    _ strings: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>?) throws -> Result
) rethrows -> Result {
    if strings.isEmpty {
        return try body(nil)
    }
    let copies: [UnsafePointer<CChar>?] = strings.map(copyCString)
    defer { copies.forEach(releaseCString) }
    return try copies.withUnsafeBufferPointer { try body($0.baseAddress) }
}
