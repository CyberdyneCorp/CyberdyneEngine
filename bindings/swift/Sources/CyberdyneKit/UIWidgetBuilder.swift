// SPDX-License-Identifier: MIT
// UIWidgetBuilder.swift — a declarative way to write a toolkit interface over ABI 1.11: widget
// nodes in a result builder, their handlers, and values bound both ways to the game's state.
//
//     let menu = try mountWidgets {
//         WidgetNode.column(id: "menu") {
//             WidgetNode.label("Options").classes("title")
//             WidgetNode.slider(in: 0...10, step: 1)
//                 .bind(.value(self, \.volume))                  // the player's moves come back
//             WidgetNode.checkbox("Subtitles").bind(.checked(self, \.subtitles))
//             WidgetNode.dropdown(["Low", "Medium", "High"]).bind(.selected(self, \.quality))
//             WidgetNode.button("Play") { [unowned self] _ in self.start() }
//                 .appear(.pop())
//         }
//     }
//     try menu.refresh()     // after the game changed `volume` itself: pushes what moved, only that
//
// MOUNT ONCE, THEN BY HANDLE OR BY BINDING — the same argument as UIBuilder.swift: the toolkit
// already restyles and relays out only what a write dirtied. A binding pushes the game's value when
// `refresh` sees it change, and writes the player's change back from the CHANGED event — before the
// node's own `onChange` runs, so a handler reads the state already updated.

import CyberdyneABI
import CyberdyneCore

/// A widget's bound value, as it crosses either way.
public enum UIBoundValue: Equatable, Sendable {
    case number(Float)
    case flag(Bool)
    case index(Int)
    case text(String)
}

/// A widget property bound to the game's state: read on `refresh`, written by the player's change.
public struct UIWidgetBinding {
    /// What the binding drives.
    public enum Property: Sendable {
        /// A slider's or progress bar's value.
        case value
        /// A toggle's or checkbox's state.
        case checked
        /// A radio group's, drop-down's, list's or tabs' selection.
        case selected
        /// A text field's text — or a label's, one-way.
        case text
    }

    /// What it drives.
    public let property: Property
    /// Nil when what it reads is gone: nothing is pushed.
    let read: () -> UIBoundValue?
    let write: ((UIBoundValue) -> Void)?

    /// A binding from closures; a nil `set` is one-way, the game's value only.
    public init(
        _ property: Property, get: @escaping () -> UIBoundValue,
        set: ((UIBoundValue) -> Void)? = nil
    ) {
        self.init(property, reading: get, set: set)
    }

    private init(
        _ property: Property, reading: @escaping () -> UIBoundValue?,
        set: ((UIBoundValue) -> Void)?
    ) {
        self.property = property
        read = reading
        write = set
    }

    /// A value bound both ways to `root[keyPath:]`. The binding holds `root` weakly: a behaviour
    /// binding its own state does not keep itself alive through its widgets.
    public static func value<Root: AnyObject>(
        _ root: Root, _ keyPath: ReferenceWritableKeyPath<Root, Float>
    ) -> UIWidgetBinding {
        keyed(.value, root, keyPath, wrap: UIBoundValue.number) {
            if case .number(let value) = $0 { return value }
            return nil
        }
    }

    /// A state bound both ways.
    public static func checked<Root: AnyObject>(
        _ root: Root, _ keyPath: ReferenceWritableKeyPath<Root, Bool>
    ) -> UIWidgetBinding {
        keyed(.checked, root, keyPath, wrap: UIBoundValue.flag) {
            if case .flag(let value) = $0 { return value }
            return nil
        }
    }

    /// A selection bound both ways.
    public static func selected<Root: AnyObject>(
        _ root: Root, _ keyPath: ReferenceWritableKeyPath<Root, Int>
    ) -> UIWidgetBinding {
        keyed(.selected, root, keyPath, wrap: UIBoundValue.index) {
            if case .index(let value) = $0 { return value }
            return nil
        }
    }

    /// A text bound both ways.
    public static func text<Root: AnyObject>(
        _ root: Root, _ keyPath: ReferenceWritableKeyPath<Root, String>
    ) -> UIWidgetBinding {
        keyed(.text, root, keyPath, wrap: UIBoundValue.text) {
            if case .text(let value) = $0 { return value }
            return nil
        }
    }

    /// `root[keyPath:]` both ways, `root` held weakly. A root that is gone reads as nil and writes
    /// nothing.
    private static func keyed<Root: AnyObject, Value>(
        _ property: Property, _ root: Root, _ keyPath: ReferenceWritableKeyPath<Root, Value>,
        wrap: @escaping (Value) -> UIBoundValue, unwrap: @escaping (UIBoundValue) -> Value?
    ) -> UIWidgetBinding {
        weak var held = root
        return UIWidgetBinding(
            property, reading: { held.map { wrap($0[keyPath: keyPath]) } },
            set: { value in
                if let held, let unwrapped = unwrap(value) { held[keyPath: keyPath] = unwrapped }
            })
    }

    /// A text the game computes, shown and never written back: a score, a timer.
    public static func display(_ get: @escaping () -> String) -> UIWidgetBinding {
        UIWidgetBinding(.text, get: { .text(get()) })
    }

    /// Push `value` into `widget`.
    func push(_ value: UIBoundValue, to widget: Widget) throws {
        switch value {
        case .number(let number): try widget.setValue(number)
        case .flag(let flag): try widget.setChecked(flag)
        case .index(let index): try widget.select(index)
        case .text(let text): try widget.setText(text)
        }
    }

    /// What the player changed `widget` to, from its CHANGED or SUBMITTED event.
    func pulled(from event: UIEvent, widget: Widget) throws -> UIBoundValue {
        switch property {
        case .value: return .number(event.value)
        case .checked: return .flag(event.value >= 0.5)
        case .selected: return .index(event.index)
        case .text: return .text(try widget.text)
        }
    }
}

/// One widget of a declarative interface.
public struct WidgetNode {
    /// What the widget is made with.
    public var desc: UIWidgetDesc
    /// What runs when something happens to it, by kind.
    public var handlers: [UIEventKind: UIEventHandler] = [:]
    /// Its bound value, if any.
    public var binding: UIWidgetBinding?
    /// What plays once it is mounted.
    public var entrance: UIAnimation?
    /// The widgets under it, in order.
    public var children: [WidgetNode] = []

    /// A node of `kind`, holding `children`.
    public init(_ kind: UIWidgetKind, id: String? = nil, children: [WidgetNode] = []) {
        desc = UIWidgetDesc(kind, id: id)
        self.children = children
    }

    // --- Containers ---------------------------------------------------------------------------

    /// Children placed by anchors.
    public static func panel(
        id: String? = nil, @WidgetBuilder _ children: () -> [WidgetNode]
    )
        -> WidgetNode
    { WidgetNode(.panel, id: id, children: children()) }
    /// Children in a flex row.
    public static func row(
        id: String? = nil, @WidgetBuilder _ children: () -> [WidgetNode]
    )
        -> WidgetNode
    { WidgetNode(.row, id: id, children: children()) }
    /// Children in a flex column.
    public static func column(
        id: String? = nil, @WidgetBuilder _ children: () -> [WidgetNode]
    )
        -> WidgetNode
    { WidgetNode(.column, id: id, children: children()) }
    /// Children layered over one another.
    public static func stack(
        id: String? = nil, @WidgetBuilder _ children: () -> [WidgetNode]
    )
        -> WidgetNode
    { WidgetNode(.stack, id: id, children: children()) }
    /// Children scrolled in a clipped viewport.
    public static func scrollView(
        id: String? = nil, @WidgetBuilder _ children: () -> [WidgetNode]
    ) -> WidgetNode { WidgetNode(.scrollView, id: id, children: children()) }

    /// Children in the cells of `columns` columns.
    public static func grid(
        columns: UInt32, id: String? = nil, @WidgetBuilder _ children: () -> [WidgetNode]
    ) -> WidgetNode {
        var node = WidgetNode(.grid, id: id, children: children())
        node.desc.columns = columns
        return node
    }

    /// A bar of tabs over its pages; every child is a page, titled by its `title`.
    public static func tabs(
        id: String? = nil, @WidgetBuilder _ pages: () -> [WidgetNode]
    )
        -> WidgetNode
    { WidgetNode(.tabs, id: id, children: pages()) }

    /// A modal dialog, shown by `Widget.open()`.
    public static func dialog(
        _ title: String, id: String? = nil, @WidgetBuilder _ children: () -> [WidgetNode]
    ) -> WidgetNode {
        var node = WidgetNode(.dialog, id: id, children: children())
        node.desc.title = title
        return node
    }

    // --- Leaves -------------------------------------------------------------------------------

    /// A line of text.
    public static func label(_ text: String, id: String? = nil) -> WidgetNode {
        captioned(.label, text, id)
    }

    /// Text in runs: `[color=#rrggbb]`, `[size=N]`, `[img=...]`.
    public static func richText(_ markup: String, id: String? = nil) -> WidgetNode {
        captioned(.richText, markup, id)
    }

    /// A button whose click runs `action`.
    public static func button(
        _ title: String, id: String? = nil, action: @escaping UIEventHandler
    ) -> WidgetNode {
        captioned(.button, title, id).onClick(action)
    }

    /// A switch.
    public static func toggle(
        _ caption: String, isOn: Bool = false, id: String? = nil
    )
        -> WidgetNode
    {
        var node = captioned(.toggle, caption, id)
        node.desc.checked = isOn
        return node
    }

    /// A checkbox.
    public static func checkbox(
        _ caption: String, isOn: Bool = false, id: String? = nil
    )
        -> WidgetNode
    {
        var node = captioned(.checkbox, caption, id)
        node.desc.checked = isOn
        return node
    }

    /// One choice of `items`.
    public static func radioGroup(_ items: [String], id: String? = nil) -> WidgetNode {
        listed(.radioGroup, items, id)
    }

    /// One of `items`, picked from a list that opens over the interface.
    public static func dropdown(_ items: [String], id: String? = nil) -> WidgetNode {
        listed(.dropdown, items, id)
    }

    /// `items` in a virtualised list: only the rows in view exist.
    public static func list(
        _ items: [String], rowHeight: Float? = nil, id: String? = nil
    )
        -> WidgetNode
    {
        var node = listed(.listView, items, id)
        node.desc.rowHeight = rowHeight
        return node
    }

    /// A value in `range`, moved by `step`.
    public static func slider(
        value: Float = 0, in range: ClosedRange<Float> = 0...1, step: Float = 0, id: String? = nil
    ) -> WidgetNode {
        ranged(.slider, value, range, step, id)
    }

    /// A bar `value` full of `range`.
    public static func progressBar(
        value: Float = 0, in range: ClosedRange<Float> = 0...1, id: String? = nil
    ) -> WidgetNode {
        ranged(.progressBar, value, range, 0, id)
    }

    /// One line of editable text.
    public static func textField(
        _ text: String = "", placeholder: String? = nil, id: String? = nil
    ) -> WidgetNode {
        var node = captioned(.textField, text, id)
        node.desc.placeholder = placeholder
        return node
    }

    private static func captioned(
        _ kind: UIWidgetKind, _ text: String, _ id: String?
    )
        -> WidgetNode
    {
        var node = WidgetNode(kind, id: id)
        node.desc.text = text
        return node
    }

    private static func listed(
        _ kind: UIWidgetKind, _ items: [String], _ id: String?
    )
        -> WidgetNode
    {
        var node = WidgetNode(kind, id: id)
        node.desc.items = items
        return node
    }

    private static func ranged(
        _ kind: UIWidgetKind, _ value: Float, _ range: ClosedRange<Float>, _ step: Float,
        _ id: String?
    ) -> WidgetNode {
        var node = WidgetNode(kind, id: id)
        node.desc.value = value
        node.desc.minimum = range.lowerBound
        node.desc.maximum = range.upperBound
        node.desc.step = step
        return node
    }

    // --- Modifiers ----------------------------------------------------------------------------

    /// The name `WidgetTree` and a sheet's `#id` know it by.
    public func id(_ id: String) -> WidgetNode { modified { $0.desc.id = id } }
    /// Its classes: space-separated, at most four.
    public func classes(_ classes: String) -> WidgetNode { modified { $0.desc.classes = classes } }
    /// A tab page's title.
    public func title(_ title: String) -> WidgetNode { modified { $0.desc.title = title } }
    /// Made refusing input.
    public func disabled(_ disabled: Bool = true) -> WidgetNode {
        modified { $0.desc.enabled = !disabled }
    }
    /// Run `handler` when `kind` happens to it.
    public func on(_ kind: UIEventKind, _ handler: @escaping UIEventHandler) -> WidgetNode {
        modified { $0.handlers[kind] = handler }
    }
    /// Run `handler` on a click or an Accept.
    public func onClick(_ handler: @escaping UIEventHandler) -> WidgetNode { on(.click, handler) }
    /// Run `handler` when the player changes its value.
    public func onChange(_ handler: @escaping UIEventHandler) -> WidgetNode {
        on(.changed, handler)
    }
    /// Run `handler` on a text field's Enter, or Accept on a list's row.
    public func onSubmit(_ handler: @escaping UIEventHandler) -> WidgetNode {
        on(.submitted, handler)
    }
    /// Bind its value to the game's state.
    public func bind(_ binding: UIWidgetBinding) -> WidgetNode {
        modified { $0.binding = binding }
    }
    /// Play `animation` once it is mounted.
    public func appear(_ animation: UIAnimation) -> WidgetNode {
        modified { $0.entrance = animation }
    }

    private func modified(_ change: (inout WidgetNode) -> Void) -> WidgetNode {
        var copy = self
        change(&copy)
        return copy
    }
}

/// Builds a node's children from a block of nodes, `if`s and `for` loops.
@resultBuilder
public enum WidgetBuilder {
    /// One node.
    public static func buildExpression(_ node: WidgetNode) -> [WidgetNode] { [node] }
    /// Several nodes, from an array.
    public static func buildExpression(_ nodes: [WidgetNode]) -> [WidgetNode] { nodes }
    /// A block's nodes, in order.
    public static func buildBlock(_ parts: [WidgetNode]...) -> [WidgetNode] { parts.flatMap { $0 } }
    /// An `if` without an `else`.
    public static func buildOptional(_ part: [WidgetNode]?) -> [WidgetNode] { part ?? [] }
    /// The `if` branch.
    public static func buildEither(first part: [WidgetNode]) -> [WidgetNode] { part }
    /// The `else` branch.
    public static func buildEither(second part: [WidgetNode]) -> [WidgetNode] { part }
    /// A `for` loop's nodes, in order.
    public static func buildArray(_ parts: [[WidgetNode]]) -> [WidgetNode] { parts.flatMap { $0 } }
}

/// The widgets a mount made, and the bindings `refresh` keeps current.
public final class WidgetTree {
    /// The top-level widgets, in order.
    public internal(set) var roots: [Widget] = []
    /// Every widget, depth first, parents before children.
    public internal(set) var widgets: [Widget] = []
    var named: [String: Widget] = [:]
    var bound: [(widget: Widget, binding: UIWidgetBinding, last: UIBoundValue)] = []

    init() {}

    /// The widget mounted with the id `id`; `.none` when there is none.
    public subscript(_ id: String) -> Widget { named[id] ?? .none }

    /// True when a widget was mounted with the id `id`.
    public func contains(_ id: String) -> Bool { named[id] != nil }

    /// Push every bound value the game changed since the last push. Returns how many it wrote.
    @discardableResult
    public func refresh() throws -> Int {
        var written = 0
        for index in bound.indices {
            guard let now = bound[index].binding.read(), now != bound[index].last else { continue }
            try bound[index].binding.push(now, to: bound[index].widget)
            bound[index].last = now
            written += 1
        }
        return written
    }

    /// Note the player's change, so `refresh` does not push it straight back.
    func noted(_ value: UIBoundValue, for widget: Widget) {
        if let index = bound.firstIndex(where: { $0.widget == widget }) {
            bound[index].last = value
        }
    }
}

extension UI {
    /// Make `nodes` under `parent` (the toolkit's root when nil), owned by nobody: no handler or
    /// binding of theirs runs. A behaviour mounts with `Behaviour.mountWidgets`.
    public static func mountWidgets(
        in parent: Widget? = nil, @WidgetBuilder _ nodes: () -> [WidgetNode]
    ) throws -> WidgetTree {
        let mounting = WidgetMounting(owner: .null, behaviour: nil)
        return try mounting.mount(nodes(), in: parent)
    }
}

extension Behaviour {
    /// Make `nodes` under `parent` for this behaviour: its entity owns every widget, so their
    /// handlers and bindings run here.
    public func mountWidgets(
        in parent: Widget? = nil, @WidgetBuilder _ nodes: () -> [WidgetNode]
    ) throws -> WidgetTree {
        let mounting = WidgetMounting(owner: entity, behaviour: self)
        return try mounting.mount(nodes(), in: parent)
    }

    /// Destroy what `tree` mounted and forget its handlers.
    public func unmountWidgets(_ tree: WidgetTree) throws {
        tree.widgets.forEach(forgetHandlers(of:))
        for root in tree.roots {
            try root.destroy()
        }
    }
}

/// One mount in progress.
private struct WidgetMounting {
    let owner: Entity
    let behaviour: Behaviour?
    let tree = WidgetTree()

    func mount(_ nodes: [WidgetNode], in parent: Widget?) throws -> WidgetTree {
        for node in nodes {
            tree.roots.append(try make(node, in: parent))
        }
        return tree
    }

    private func make(_ node: WidgetNode, in parent: Widget?) throws -> Widget {
        let widget = try UI.createWidget(node.desc, in: parent, owner: owner)
        tree.widgets.append(widget)
        if let id = node.desc.id {
            tree.named[id] = widget
        }
        try bind(node, widget)
        for (kind, handler) in node.handlers where kind != .changed && kind != .submitted {
            behaviour?.on(kind, of: widget, run: handler)
        }
        for child in node.children {
            _ = try make(child, in: widget)
        }
        if let entrance = node.entrance {
            try widget.animate(entrance)
        }
        return widget
    }

    /// The binding's first push, and the CHANGED and SUBMITTED handlers: the binding's write-back
    /// first, then the node's own.
    private func bind(_ node: WidgetNode, _ widget: Widget) throws {
        let binding = node.binding
        if let binding, let first = binding.read() {
            try binding.push(first, to: widget)
            tree.bound.append((widget, binding, first))
        }
        for kind in [UIEventKind.changed, .submitted] {
            let own = node.handlers[kind]
            guard own != nil || binding?.write != nil else { continue }
            behaviour?.on(kind, of: widget) { [weak tree] event in
                if let binding, let write = binding.write {
                    let value = try binding.pulled(from: event, widget: widget)
                    write(value)
                    tree?.noted(value, for: widget)
                }
                try own?(event)
            }
        }
    }
}
