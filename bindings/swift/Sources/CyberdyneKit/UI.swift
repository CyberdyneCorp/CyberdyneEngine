// SPDX-License-Identifier: MIT
// UI.swift — ABI 1.6's runtime interface: elements, their layout and style, and what the pointer
// is over. The declarative layer a game usually writes is UIBuilder.swift, built on this.
//
//     let root = try UI.root()
//     let gold = try UI.create(.label, in: root, name: "gold")
//     try gold.setText("1250", colour: 0xFFFF_D34D)
//     try gold.setLayout(.anchored(min: Vec2(x: 0, y: 0), max: Vec2(x: 0, y: 0),
//                                  offsetMin: Vec2(x: 6, y: 2), offsetMax: Vec2(x: 60, y: 15)))
//     if let over = try UI.hitTest(pointer.position) { … }      // a click on the HUD, not the world
//
// PHASES. The interface is presentation: everything here is allowed at initialisation and in
// `onUpdate`, and refused in `onFixedUpdate` with `CyberdyneError.status(.permissionDenied, …)`.
//
// COLOURS are premultiplied 0xAARRGGBB, as CyberUI's are. LENGTHS are reference units — window
// pixels under the embedder's fixed-pixel scale.
//
// EVENTS. A button's click is delivered to the behaviours of the entity the button was created
// for, through `CyBehaviourVTable.ui_event`: to the `onClick` closure it was built with (see
// UIBuilder.swift) and to `Behaviour.onUIEvent`.

import CyberdyneABI
import CyberdyneCore

/// A premultiplied 0xAARRGGBB colour.
public typealias UIColour = UInt32

/// An element of the runtime interface. A handle, not an owner: destroying it is explicit.
public struct UIElement: Hashable, Sendable {
    /// `CyUiElement`: the store index low, its generation high.
    public let raw: CyUiElement

    /// Wrap a handle the engine returned.
    public init(raw: CyUiElement) {
        self.raw = raw
    }

    /// `CY_UI_ELEMENT_NULL`.
    public static let none = UIElement(raw: CY_UI_ELEMENT_NULL)
    /// True for the null element.
    public var isNone: Bool { raw == CY_UI_ELEMENT_NULL }

    /// Replace the element's layout input.
    public func setLayout(_ layout: UILayout) throws {
        var value = layout.raw
        try GameServices.engine().uiSetLayout(element: raw, layout: &value)
    }

    /// Replace what the element draws.
    public func setStyle(_ style: UIStyle) throws {
        var value = style.raw
        try GameServices.engine().uiSetStyle(element: raw, style: &value)
    }

    /// A label's or a button's text: one line in the built-in font at a whole `scale`.
    public func setText(_ text: String, colour: UIColour = 0xFFFF_FFFF, scale: UInt32 = 1) throws {
        let engine = try GameServices.engine()
        try text.withCString {
            try engine.uiSetText(element: raw, text: $0, colour: colour, pixelScale: scale)
        }
    }

    /// An image's atlas page and its rectangle on it, in [0, 1]; nil is the whole page.
    public func setImage(page: UInt32, uv: UIRect? = nil) throws {
        let engine = try GameServices.engine()
        try withOptionalBuffer(uv.map { [$0.x, $0.y, $0.width, $0.height] }) {
            try engine.uiSetImage(element: raw, page: page, uv: $0)
        }
    }

    /// A progress bar's fraction, clamped to [0, 1] by the engine.
    public func setProgress(_ value: Float) throws {
        try GameServices.engine().uiSetProgress(element: raw, value: value)
    }

    /// Shown, hidden, or collapsed out of layout.
    public func setVisibility(_ visibility: UIVisibility) throws {
        try GameServices.engine().uiSetVisibility(element: raw, visibility: visibility.rawValue)
    }

    /// Shown, or collapsed out of layout: the common case of `setVisibility`.
    public func setShown(_ shown: Bool) throws {
        try setVisibility(shown ? .visible : .collapsed)
    }

    /// The element's opacity in [0, 1], multiplied down the tree.
    public func setOpacity(_ opacity: Float) throws {
        try GameServices.engine().uiSetOpacity(element: raw, opacity: opacity)
    }

    /// Where the last layout put the element, in window pixels.
    public var rect: UIRect {
        get throws {
            var values = [Float](repeating: 0, count: 4)
            try values.withUnsafeMutableBufferPointer {
                try GameServices.engine().uiElementRect(element: raw, into: $0.baseAddress)
            }
            return UIRect(x: values[0], y: values[1], width: values[2], height: values[3])
        }
    }

    /// Give a button keyboard focus.
    public func focus() throws {
        try GameServices.engine().uiSetFocus(element: raw)
    }

    /// Destroy the element and everything under it.
    public func destroy() throws {
        try GameServices.engine().uiDestroy(element: raw)
    }
}

/// A rectangle in reference units, or a uv rectangle in [0, 1].
public struct UIRect: Equatable, Sendable {
    /// Left edge.
    public var x: Float
    /// Top edge.
    public var y: Float
    /// Width; zero or less is empty.
    public var width: Float
    /// Height; zero or less is empty.
    public var height: Float

    /// A rectangle at `x, y`, `width` by `height`.
    public init(x: Float = 0, y: Float = 0, width: Float = 0, height: Float = 0) {
        self.x = x
        self.y = y
        self.width = width
        self.height = height
    }

    /// The right edge, `x + width`.
    public var right: Float { x + width }
    /// The bottom edge, `y + height`.
    public var bottom: Float { y + height }

    /// True for a point inside, right and bottom edges excluded — as the engine's hit test reads it.
    public func contains(_ point: Vec2) -> Bool {
        point.x >= x && point.x < right && point.y >= y && point.y < bottom
    }
}

/// Margins and padding, in the order a CSS author expects.
public struct UIInsets: Equatable, Sendable {
    /// Inset from the left edge.
    public var left: Float
    /// Inset from the top edge.
    public var top: Float
    /// Inset from the right edge.
    public var right: Float
    /// Inset from the bottom edge.
    public var bottom: Float

    /// Each side's inset.
    public init(left: Float = 0, top: Float = 0, right: Float = 0, bottom: Float = 0) {
        self.left = left
        self.top = top
        self.right = right
        self.bottom = bottom
    }

    /// The same inset on all four sides.
    public init(_ all: Float) {
        self.init(left: all, top: all, right: all, bottom: all)
    }

    /// `horizontal` on the left and right, `vertical` on the top and bottom.
    public init(horizontal: Float, vertical: Float) {
        self.init(left: horizontal, top: vertical, right: horizontal, bottom: vertical)
    }

    static let zero = UIInsets()
    var tuple: (Float, Float, Float, Float) { (left, top, right, bottom) }
}

/// An element's layout input: how it lays out its children, and where its parent puts it.
///
/// Every default is the engine's: a stretching flex row whose elements are sized by their content.
/// A nil preferred axis is measured from the content; a nil maximum is unbounded.
public struct UILayout: Equatable, Sendable {
    /// How this element lays out its children.
    public var model: UILayoutModel = .flex
    /// A flex container's main axis.
    public var direction: UIDirection = .row
    /// Distribution of a flex container's children along the main axis.
    public var justify: UIJustify = .start
    /// Alignment of a flex container's children across the cross axis.
    public var align: UIAlign = .stretch
    /// This element's own cross-axis alignment; stretch defers to the parent's `align`.
    public var selfAlign: UIAlign = .stretch
    /// Flex children wrap onto further lines.
    public var wrap = false
    /// Space between flex children and grid tracks.
    public var gap: Float = 0
    /// Preferred width; nil is measured from the content.
    public var width: Float?
    /// Preferred height; nil is measured from the content.
    public var height: Float?
    /// The smallest size layout may give the element.
    public var minimum = Vec2.zero
    /// The largest size layout may give the element; nil is unbounded.
    public var maximum: Vec2?
    /// Space outside the element, in a flex parent.
    public var margin = UIInsets()
    /// Space between the element's edge and its children.
    public var padding = UIInsets()
    /// Share of a flex container's free space this element takes.
    public var flexGrow: Float = 0
    /// 1 by default; 0 never shrinks.
    public var flexShrink: Float = 1
    /// Width over height; zero is unconstrained.
    public var aspectRatio: Float = 0
    /// In an absolute parent: the corners as fractions of the parent's rect, then offset in units.
    public var anchorMin = Vec2.zero
    /// The bottom-right corner, as fractions of the parent's rect.
    public var anchorMax = Vec2.zero
    /// The top-left corner's offset from its anchor, in units.
    public var offsetMin = Vec2.zero
    /// The bottom-right corner's offset from its anchor, in units.
    public var offsetMax = Vec2.zero
    /// In a grid parent: the cell and its spans. On a grid container: its column count.
    public var gridColumn: UInt16 = 0
    /// The grid row this element occupies.
    public var gridRow: UInt16 = 0
    /// Grid columns this element spans.
    public var gridColumnSpan: UInt16 = 1
    /// Grid rows this element spans.
    public var gridRowSpan: UInt16 = 1
    /// On a grid container: its explicit column count; zero is one.
    public var gridColumns: UInt16 = 0

    /// The engine's defaults.
    public init() {}

    /// Placed in an absolute parent: corners at `min` and `max` of the parent, offset in units.
    /// `(1, 1)–(1, 1)` offset by `(-108, -80)–(-6, -6)` is a 102 by 74 box six units in from the
    /// parent's bottom-right corner.
    public static func anchored(
        min: Vec2, max: Vec2, offsetMin: Vec2 = .zero, offsetMax: Vec2 = .zero
    ) -> UILayout {
        var layout = UILayout()
        layout.anchorMin = min
        layout.anchorMax = max
        layout.offsetMin = offsetMin
        layout.offsetMax = offsetMax
        return layout
    }

    /// A box at `x, y`, `width` by `height`, in an absolute parent's top-left corner.
    public static func at(x: Float, y: Float, width: Float, height: Float) -> UILayout {
        anchored(
            min: .zero, max: .zero, offsetMin: Vec2(x: x, y: y),
            offsetMax: Vec2(x: x + width, y: y + height))
    }

    /// The same layout, laying its children out with `model`.
    public func laying(_ model: UILayoutModel) -> UILayout {
        var copy = self
        copy.model = model
        return copy
    }

    /// The same layout as a flex container along `direction`.
    public func flex(
        _ direction: UIDirection, gap: Float = 0, align: UIAlign = .stretch,
        padding: UIInsets = UIInsets()
    ) -> UILayout {
        var copy = self
        copy.model = .flex
        copy.direction = direction
        copy.gap = gap
        copy.align = align
        copy.padding = padding
        return copy
    }

    /// The same layout with a preferred size; nil keeps that axis content-sized.
    public func sized(width: Float? = nil, height: Float? = nil) -> UILayout {
        var copy = self
        copy.width = width
        copy.height = height
        return copy
    }

    /// In the ABI's spelling, where zero means the default.
    public var raw: CyUiLayout {
        var value = CyUiLayout()
        value.struct_size = UInt32(MemoryLayout<CyUiLayout>.size)
        value.model = model.rawValue
        value.direction = direction.rawValue
        value.justify = justify.rawValue
        value.align = align.rawValue
        value.self_align = selfAlign.rawValue
        value.flags =
            (wrap ? CY_UI_LAYOUT_WRAP : 0) | (flexShrink == 0 ? CY_UI_LAYOUT_NO_SHRINK : 0)
        value.gap = gap
        value.preferred = (width ?? 0, height ?? 0)
        value.minimum = minimum.tuple
        value.maximum = (maximum ?? .zero).tuple
        value.margin = margin.tuple
        value.padding = padding.tuple
        value.flex_grow = flexGrow
        value.flex_shrink = flexShrink
        value.aspect_ratio = aspectRatio
        value.anchor_min = anchorMin.tuple
        value.anchor_max = anchorMax.tuple
        value.offset_min = offsetMin.tuple
        value.offset_max = offsetMax.tuple
        value.grid_column = gridColumn
        value.grid_row = gridRow
        value.grid_column_span = gridColumnSpan
        value.grid_row_span = gridRowSpan
        value.grid_columns = gridColumns
        return value
    }
}

/// What an element draws. Zero colours draw nothing.
public struct UIStyle: Equatable, Sendable {
    /// The fill; an image's tint.
    public var background: UIColour = 0
    /// Drawn inside the bounds, `borderWidth` thick.
    public var border: UIColour = 0
    /// The border's thickness, in units.
    public var borderWidth: Float = 0
    /// Rounds the fill, the border, an image and a progress bar's fill alike.
    public var cornerRadius: Float = 0
    /// A progress bar's fill.
    public var accent: UIColour = 0
    /// Clip the children to this element's rect.
    public var clipsChildren = false

    /// A style; a border colour with no width is a one-unit frame.
    public init(
        background: UIColour = 0, border: UIColour = 0, borderWidth: Float? = nil,
        cornerRadius: Float = 0, accent: UIColour = 0, clipsChildren: Bool = false
    ) {
        self.background = background
        self.border = border
        // A border colour with no width is a one-unit border, which is what a frame usually is.
        self.borderWidth = borderWidth ?? (border != 0 ? 1 : 0)
        self.cornerRadius = cornerRadius
        self.accent = accent
        self.clipsChildren = clipsChildren
    }

    /// In the ABI's spelling.
    public var raw: CyUiStyle {
        var value = CyUiStyle()
        value.struct_size = UInt32(MemoryLayout<CyUiStyle>.size)
        value.flags = clipsChildren ? CY_UI_STYLE_CLIP_CHILDREN : 0
        value.background = background
        value.border_colour = border
        value.accent = accent
        value.border_width = borderWidth
        value.corner_radius = cornerRadius
        return value
    }
}

/// Something that happened to an element: a click, a focus change.
public struct UIEvent: Equatable, Sendable {
    /// What happened.
    public var kind: UIEventKind
    /// The element it happened to.
    public var element: UIElement
    /// The entity the element was created for: the receiving behaviour's own.
    public var owner: Entity
    /// The pointer, in window pixels, for a click.
    public var position: Vec2
    /// The button that clicked.
    public var button: PointerButtons

    /// An event, as a test or a tool builds one.
    public init(
        kind: UIEventKind, element: UIElement, owner: Entity = .null, position: Vec2 = .zero,
        button: PointerButtons = []
    ) {
        self.kind = kind
        self.element = element
        self.owner = owner
        self.position = position
        self.button = button
    }

    /// From the ABI's spelling. An unknown kind — from a newer engine — reads as nil.
    init?(_ raw: CyUiEvent) {
        guard let kind = UIEventKind(rawValue: raw.kind) else { return nil }
        self.init(
            kind: kind, element: UIElement(raw: raw.element), owner: Entity(bits: raw.owner),
            position: Vec2(raw.position), button: PointerButtons(rawValue: raw.button))
    }
}

/// The runtime interface's root and its pointer queries.
public enum UI {
    /// The screen's root: an absolute container covering the window. It takes no writes.
    public static func root() throws -> UIElement {
        var out = CY_UI_ELEMENT_NULL
        try GameServices.engine().uiRoot(into: &out)
        return UIElement(raw: out)
    }

    /// A new element, the last child of `parent`. `owner` receives its events; `name` is its type
    /// name for styles and diagnostics, the kind's when nil.
    public static func create(
        _ kind: UIKind, in parent: UIElement, name: String? = nil, owner: Entity = .null
    ) throws -> UIElement {
        let engine = try GameServices.engine()
        var out = CY_UI_ELEMENT_NULL
        func make(_ text: UnsafePointer<CChar>?) throws {
            var desc = CyUiElementDesc()
            desc.struct_size = UInt32(MemoryLayout<CyUiElementDesc>.size)
            desc.kind = kind.rawValue
            desc.name = text
            desc.owner = owner.bits
            try engine.uiCreate(parent: parent.raw, desc: &desc, into: &out)
        }
        if let name {
            try name.withCString { try make($0) }
        } else {
            try make(nil)
        }
        return UIElement(raw: out)
    }

    /// This module's element under a window point, or nil when the point is over the world.
    public static func hitTest(_ point: Vec2) throws -> UIElement? {
        var out = CY_UI_ELEMENT_NULL
        let position = [point.x, point.y]
        try position.withUnsafeBufferPointer {
            try GameServices.engine().uiHitTest(position: $0.baseAddress, into: &out)
        }
        return out == CY_UI_ELEMENT_NULL ? nil : UIElement(raw: out)
    }

    /// This module's element with keyboard focus, or nil.
    public static var focus: UIElement? {
        get throws {
            var out = CY_UI_ELEMENT_NULL
            try GameServices.engine().uiFocus(into: &out)
            return out == CY_UI_ELEMENT_NULL ? nil : UIElement(raw: out)
        }
    }

    /// Take keyboard focus away from every element.
    public static func clearFocus() throws {
        try GameServices.engine().uiSetFocus(element: CY_UI_ELEMENT_NULL)
    }
}
