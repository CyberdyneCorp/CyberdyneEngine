// SPDX-License-Identifier: MIT
// UIBuilder.swift — a declarative way to write an interface over ABI 1.6. No SwiftUI: a result
// builder, five node types and their modifiers, mounted into the engine's store once and then
// updated by handle.
//
//     let hud = try mountUI {
//         Panel("resource-bar") {
//             Label("0", colour: gold).id("gold")
//             Button("Build", colour: white) { _ in self.buildRequested = true }
//                 .style(UIStyle(background: panel, border: frame))
//         }
//         .layout(.anchored(min: Vec2(x: 0, y: 0), max: Vec2(x: 1, y: 0),
//                           offsetMax: Vec2(x: 0, y: 17)).flex(.row, gap: 4))
//         .style(UIStyle(background: panel))
//     }
//     try hud["gold"].setText("1250", colour: gold)       // every frame after: by handle
//
// WHY MOUNT ONCE AND UPDATE BY HANDLE, rather than re-describing the tree every frame and diffing:
// CyberUI's store already has three dirty states, and a write through a handle marks exactly the
// one that changed. A HUD whose gold count moves repaints one label and relays out nothing — the
// property `ui-system` asks for, kept without a reconciler on this side of the boundary.
//
// CLICKS go to the behaviour that mounted the button: `mountUI` records each `onClick` on the
// behaviour, the button is created with the behaviour's entity as its owner, and the engine
// delivers the click to that entity's behaviours through `CyBehaviourVTable.ui_event`.

import CyberdyneABI
import CyberdyneCore

/// What a click, or any other event on an element, runs.
public typealias UIEventHandler = (UIEvent) throws -> Void

/// Everything a node says about its element. Built by the node types and their modifiers.
public struct UINodeSpec {
    /// What the element is.
    public var kind: UIKind
    /// Its type name in the store, for styles and diagnostics; the kind's when nil.
    public var name: String?
    /// The name `UITree` finds the element by.
    public var id: String?
    /// The layout to write at mount; nil leaves the engine's default.
    public var layout: UILayout?
    /// The style to write at mount; nil draws nothing.
    public var style: UIStyle?
    /// A label's or button's text.
    public var text: String?
    /// The text's colour.
    public var textColour: UIColour = 0xFFFF_FFFF
    /// The text's whole-number pixel scale.
    public var textScale: UInt32 = 1
    /// An image's atlas page.
    public var imagePage: UInt32?
    /// An image's rectangle on its page; nil is the whole page.
    public var imageUV: UIRect?
    /// A progress bar's fraction.
    public var progress: Float?
    /// Shown, hidden or collapsed at mount.
    public var visibility: UIVisibility = .visible
    /// Opacity at mount; nil is opaque.
    public var opacity: Float?
    /// What a click on a button runs, on the behaviour that mounted it.
    public var onClick: UIEventHandler?
    /// The nodes mounted under this one, in order.
    public var children: [any UINode] = []

    /// An empty description of an element of `kind`.
    public init(kind: UIKind, name: String? = nil) {
        self.kind = kind
        self.name = name
    }
}

/// One element of a declarative interface.
public protocol UINode {
    /// Everything the node says about its element.
    var spec: UINodeSpec { get set }
}

extension UINode {
    /// The name `UITree` finds the element by after mounting.
    public func id(_ id: String) -> Self { modified { $0.id = id } }
    /// The node with `layout`.
    public func layout(_ layout: UILayout) -> Self { modified { $0.layout = layout } }
    /// The node with `style`.
    public func style(_ style: UIStyle) -> Self { modified { $0.style = style } }
    /// The node shown, hidden or collapsed.
    public func visibility(_ visibility: UIVisibility) -> Self {
        modified { $0.visibility = visibility }
    }
    /// Shown, or collapsed out of layout.
    public func shown(_ shown: Bool) -> Self { visibility(shown ? .visible : .collapsed) }
    /// The node at `opacity`.
    public func opacity(_ opacity: Float) -> Self { modified { $0.opacity = opacity } }

    private func modified(_ change: (inout UINodeSpec) -> Void) -> Self {
        var copy = self
        change(&copy.spec)
        return copy
    }
}

/// Builds the children of a node from a block of nodes, `if`s and `for` loops.
@resultBuilder
public enum UIBuilder {
    /// One node.
    public static func buildExpression(_ node: any UINode) -> [any UINode] { [node] }
    /// Several nodes, from an array.
    public static func buildExpression(_ nodes: [any UINode]) -> [any UINode] { nodes }
    /// A block's nodes, in order.
    public static func buildBlock(_ parts: [any UINode]...) -> [any UINode] { parts.flatMap { $0 } }
    /// An `if` without an `else`.
    public static func buildOptional(_ part: [any UINode]?) -> [any UINode] { part ?? [] }
    /// The `if` branch.
    public static func buildEither(first part: [any UINode]) -> [any UINode] { part }
    /// The `else` branch.
    public static func buildEither(second part: [any UINode]) -> [any UINode] { part }
    /// A `for` loop's nodes, in order.
    public static func buildArray(_ parts: [[any UINode]]) -> [any UINode] { parts.flatMap { $0 } }
}

/// A box: a background, a border, children.
public struct Panel: UINode {
    /// Everything this node says about its element.
    public var spec: UINodeSpec

    /// A panel named `name` holding `children`.
    public init(_ name: String = "panel", @UIBuilder _ children: () -> [any UINode] = { [] }) {
        spec = UINodeSpec(kind: .panel, name: name)
        spec.children = children()
    }
}

/// A line of text in the built-in font.
public struct Label: UINode {
    /// Everything this node says about its element.
    public var spec: UINodeSpec

    /// `text` in `colour` at a whole-number `scale`.
    public init(
        _ text: String, colour: UIColour = 0xFFFF_FFFF, scale: UInt32 = 1, name: String = "label"
    ) {
        spec = UINodeSpec(kind: .label, name: name)
        spec.text = text
        spec.textColour = colour
        spec.textScale = scale
    }
}

/// An atlas page the embedder uploaded, tinted by the style's background.
public struct Image: UINode {
    /// Everything this node says about its element.
    public var spec: UINodeSpec

    /// `uv` of atlas page `page`; nil is the whole page.
    public init(page: UInt32, uv: UIRect? = nil, name: String = "image") {
        spec = UINodeSpec(kind: .image, name: name)
        spec.imagePage = page
        spec.imageUV = uv
    }
}

/// A track (the style's background) with a fill (its accent) over `value` of it.
public struct ProgressBar: UINode {
    /// Everything this node says about its element.
    public var spec: UINodeSpec

    /// A bar `value` full, clamped to [0, 1] by the engine.
    public init(_ value: Float = 0, name: String = "progress") {
        spec = UINodeSpec(kind: .progress, name: name)
        spec.progress = value
    }
}

/// A focusable box with text whose clicks run `action` on the behaviour that mounted it.
public struct Button: UINode {
    /// Everything this node says about its element.
    public var spec: UINodeSpec

    /// A button reading `title` whose clicks run `action`.
    public init(
        _ title: String, colour: UIColour = 0xFFFF_FFFF, scale: UInt32 = 1,
        name: String = "button", action: @escaping UIEventHandler
    ) {
        spec = UINodeSpec(kind: .button, name: name)
        spec.text = title
        spec.textColour = colour
        spec.textScale = scale
        spec.onClick = action
    }
}

/// The elements a mount made: its top-level elements, and every element given an `id`.
public struct UITree {
    /// The top-level elements, in order.
    public internal(set) var roots: [UIElement] = []
    /// Every element, in creation order — depth first, parents before children.
    public internal(set) var elements: [UIElement] = []
    var named: [String: UIElement] = [:]

    /// The element mounted with `.id(id)`; `.none` when there is none, so a write through it
    /// throws `.notFound` rather than the lookup trapping.
    public subscript(_ id: String) -> UIElement { named[id] ?? .none }

    /// True when an element was mounted with `.id(id)`.
    public func contains(_ id: String) -> Bool { named[id] != nil }
}

/// Mounting nodes into the engine's store.
extension UI {
    /// Create `nodes` under `parent` (the root when nil), owned by nobody: a click on a button
    /// mounted this way reaches no behaviour. A behaviour mounts with `Behaviour.mountUI`.
    public static func mount(
        in parent: UIElement? = nil, @UIBuilder _ nodes: () -> [any UINode]
    ) throws -> UITree {
        var mounting = Mounting(owner: .null, handlers: nil)
        return try mounting.mount(nodes(), in: parent ?? root())
    }
}

extension Behaviour {
    /// Create `nodes` under `parent` (the root when nil) for this behaviour: its entity owns every
    /// element, so every button's click comes back here, to the button's `action`.
    public func mountUI(
        in parent: UIElement? = nil, @UIBuilder _ nodes: () -> [any UINode]
    ) throws -> UITree {
        var mounting = Mounting(owner: entity, handlers: self)
        return try mounting.mount(nodes(), in: parent ?? UI.root())
    }

    /// Destroy what `tree` mounted and forget its buttons' actions.
    public func unmountUI(_ tree: UITree) throws {
        for element in tree.elements {
            uiHandlers[element.raw] = nil
        }
        for root in tree.roots {
            try root.destroy()
        }
    }
}

/// One mount in progress.
private struct Mounting {
    let owner: Entity
    let handlers: Behaviour?
    var tree = UITree()

    mutating func mount(_ nodes: [any UINode], in parent: UIElement) throws -> UITree {
        for node in nodes {
            tree.roots.append(try make(node.spec, in: parent))
        }
        return tree
    }

    private mutating func make(_ spec: UINodeSpec, in parent: UIElement) throws -> UIElement {
        let element = try UI.create(spec.kind, in: parent, name: spec.name, owner: owner)
        tree.elements.append(element)
        if let id = spec.id {
            tree.named[id] = element
        }
        try apply(spec, to: element)
        if let action = spec.onClick {
            handlers?.uiHandlers[element.raw] = action
        }
        for child in spec.children {
            _ = try make(child.spec, in: element)
        }
        return element
    }

    private func apply(_ spec: UINodeSpec, to element: UIElement) throws {
        if let layout = spec.layout {
            try element.setLayout(layout)
        }
        if let style = spec.style {
            try element.setStyle(style)
        }
        if let text = spec.text {
            try element.setText(text, colour: spec.textColour, scale: spec.textScale)
        }
        if let page = spec.imagePage {
            try element.setImage(page: page, uv: spec.imageUV)
        }
        if let progress = spec.progress {
            try element.setProgress(progress)
        }
        if spec.visibility != .visible {
            try element.setVisibility(spec.visibility)
        }
        if let opacity = spec.opacity {
            try element.setOpacity(opacity)
        }
    }
}
