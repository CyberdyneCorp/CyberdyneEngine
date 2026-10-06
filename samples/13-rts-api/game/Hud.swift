// SPDX-License-Identifier: MIT
// Hud.swift — the strategy HUD, built in Swift through ABI 1.6: a resource bar, a selection panel
// with health bars, and a minimap with the camera's frame. The same HUD as
// samples/13-rts-selection/hud.cpp, element for element, so the two draw the same bytes —
// `render.rts_api_hud` photographs both and compares them.
//
// Mounted once with `mountUI`, then updated by handle: `show(_:)` writes only what changed since
// the last frame, so a gold count that moved repaints one label, and a frame where nothing moved
// writes nothing at all. The Build button is the one thing the C++ HUD does not have; its click
// comes back to the behaviour that mounted it.

import CyberdyneKit

/// What the HUD shows. The commander builds one from the game each frame.
struct HudModel: Equatable {
    struct Resources: Equatable {
        var gold: UInt32 = 0
        var wood: UInt32 = 0
        var food: UInt32 = 0
        var foodCap: UInt32 = 0
    }

    struct Unit: Equatable {
        var name: String
        var health: UInt32
        var maxHealth: UInt32
    }

    enum Team: Int {
        case player, enemy, neutral
    }

    /// A unit on the minimap, normalised to the map: (0, 0) top-left, (1, 1) bottom-right.
    struct Dot: Equatable {
        var x: Float
        var y: Float
        var team: Team
    }

    var resources = Resources()
    var units: [Unit] = []
    var dots: [Dot] = []
    /// The part of the map the camera sees, in map coordinates.
    var camera = UIRect()
}

/// The HUD's elements and the last model it showed.
final class RtsHud {
    /// The selection panel lists at most this many units and says how many more.
    static let selectionRows = 3
    static let minimapDots = 16

    // Premultiplied 0xAARRGGBB — hud.cpp's palette, value for value.
    static let panel: UIColour = 0xC80E_141C
    static let panelBorder: UIColour = 0xFF3C_5064
    static let text: UIColour = 0xFFE6_EBF0
    static let dim: UIColour = 0xFF8C_98A6
    static let gold: UIColour = 0xFFFF_D34D
    static let wood: UIColour = 0xFF9B_D46A
    static let food: UIColour = 0xFFF0_A070
    static let mapGround: UIColour = 0xFF1E_3A24
    static let mapWater: UIColour = 0xFF1F_4E79
    static let mapBorder: UIColour = 0xFF8F_A3B5
    static let cameraFrame: UIColour = 0xFFFF_FFFF
    static let healthBack: UIColour = 0xFF3A_1E1E
    static let health: UIColour = 0xFF4C_C34C
    static let teams: [UIColour] = [0xFF28_8CFF, 0xFFFF_6040, 0xFFC8_C8C8]

    let tree: UITree
    private var shown: HudModel?

    /// Mount the HUD for `owner`. With `onBuild`, a Build button below the resource bar runs it.
    init(for owner: Behaviour, onBuild: (() -> Void)? = nil) throws {
        tree = try owner.mountUI {
            Self.resourceBar()
            Self.minimap()
            Self.selection()
            if let onBuild {
                Self.buildButton(onBuild)
            }
        }
    }

    /// Show `model`, writing only the parts that differ from what is on screen.
    func show(_ model: HudModel) throws {
        if shown?.resources != model.resources {
            try showResources(model.resources)
        }
        if shown?.units != model.units {
            try showSelection(model.units)
        }
        if shown?.dots != model.dots || shown?.camera != model.camera {
            try showMinimap(model.dots, camera: model.camera)
        }
        shown = model
    }

    // --- Building ----------------------------------------------------------------------------

    /// A strip across the top: a row of icon-and-count pairs.
    private static func resourceBar() -> any UINode {
        let items: [(String, UIColour)] = [("gold", gold), ("wood", wood), ("food", food)]
        return Panel("resource-bar") {
            for (index, item) in items.enumerated() {
                Panel("icon")
                    .layout(icon(first: index == 0))
                    .style(UIStyle(background: item.1, cornerRadius: 2))
                Label("0", colour: item.1, name: item.0).id(item.0)
            }
        }
        .layout(
            UILayout.anchored(
                min: Vec2(x: 0, y: 0), max: Vec2(x: 1, y: 0), offsetMax: Vec2(x: 0, y: 17)
            ).flex(.row, gap: 4, align: .centre, padding: UIInsets(horizontal: 6, vertical: 2))
        )
        .style(UIStyle(background: panel))
        .id("resource-bar")
    }

    private static func icon(first: Bool) -> UILayout {
        var layout = UILayout().sized(width: 7, height: 7)
        layout.margin.left = first ? 0 : 10
        return layout
    }

    /// Bottom-right, clipping what it holds: the camera's frame runs off its edge when the camera
    /// looks past the map's.
    private static func minimap() -> any UINode {
        Panel("minimap") {
            Panel("water")
                .layout(.anchored(min: Vec2(x: 0.55, y: 0.12), max: Vec2(x: 0.9, y: 0.42)))
                .style(UIStyle(background: mapWater, cornerRadius: 6))
            for index in 0..<minimapDots {
                Panel("unit").visibility(.collapsed).id("dot\(index)")
            }
            // A frame and nothing inside it: no background, a one-unit border.
            Panel("camera").style(UIStyle(border: cameraFrame)).id("camera")
        }
        .layout(
            UILayout.anchored(
                min: Vec2(x: 1, y: 1), max: Vec2(x: 1, y: 1), offsetMin: Vec2(x: -108, y: -80),
                offsetMax: Vec2(x: -6, y: -6)
            ).laying(.absolute)
        )
        .style(UIStyle(background: mapGround, border: mapBorder, clipsChildren: true))
        .id("minimap")
    }

    /// Bottom-left: a title and one row per selected unit, each with a health bar.
    private static func selection() -> any UINode {
        Panel("selection") {
            Label("Nothing selected", colour: dim, name: "title").id("title")
            for index in 0..<selectionRows {
                Panel("unit-row") {
                    Label("", colour: text, name: "unit-name")
                        .layout(UILayout().sized(width: 54))
                        .id("name\(index)")
                    ProgressBar(name: "health-bar")
                        .layout(UILayout().sized(width: 50, height: 5))
                        .style(UIStyle(background: healthBack, cornerRadius: 1, accent: health))
                        .id("bar\(index)")
                    Label("", colour: dim, name: "unit-health").id("health\(index)")
                }
                .layout(UILayout().flex(.row, gap: 6, align: .centre).sized(height: 13))
                .visibility(.collapsed)
                .id("row\(index)")
            }
        }
        .layout(
            UILayout.anchored(
                min: Vec2(x: 0, y: 1), max: Vec2(x: 0, y: 1), offsetMin: Vec2(x: 6, y: -64),
                offsetMax: Vec2(x: 186, y: -6)
            ).flex(.column, padding: UIInsets(horizontal: 5, vertical: 3))
        )
        .style(
            UIStyle(background: panel, border: panelBorder, cornerRadius: 3, clipsChildren: true)
        )
        .id("selection")
    }

    /// Top-right, under the resource bar: what the B key does, for a pointer.
    private static func buildButton(_ onBuild: @escaping () -> Void) -> any UINode {
        Button("Build", colour: text, name: "build-button") { _ in onBuild() }
            .layout(
                UILayout.anchored(
                    min: Vec2(x: 1, y: 0), max: Vec2(x: 1, y: 0), offsetMin: Vec2(x: -48, y: 21),
                    offsetMax: Vec2(x: -6, y: 38)
                ).flex(.row, padding: UIInsets(horizontal: 6, vertical: 2))
            )
            .style(UIStyle(background: panel, border: panelBorder, cornerRadius: 3))
            .id("build")
    }

    // --- Updating ----------------------------------------------------------------------------

    private func showResources(_ resources: HudModel.Resources) throws {
        try tree["gold"].setText("\(resources.gold)", colour: Self.gold)
        try tree["wood"].setText("\(resources.wood)", colour: Self.wood)
        try tree["food"].setText("\(resources.food)/\(resources.foodCap)", colour: Self.food)
    }

    private func showSelection(_ units: [HudModel.Unit]) throws {
        let rows = Self.selectionRows
        let title: String
        if units.isEmpty {
            title = "Nothing selected"
        } else if units.count > rows {
            title = "Selected: \(units.count) units (\(units.count - rows) more)"
        } else {
            title = "Selected: \(units.count) unit\(units.count == 1 ? "" : "s")"
        }
        try tree["title"].setText(title, colour: units.isEmpty ? Self.dim : Self.text)
        for index in 0..<rows {
            let shown = index < units.count
            try tree["row\(index)"].setShown(shown)
            guard shown else { continue }
            let unit = units[index]
            try tree["name\(index)"].setText(unit.name, colour: Self.text)
            try tree["health\(index)"].setText("\(unit.health)/\(unit.maxHealth)", colour: Self.dim)
            let ratio = unit.maxHealth == 0 ? 0 : min(1, Float(unit.health) / Float(unit.maxHealth))
            try tree["bar\(index)"].setProgress(ratio)
        }
    }

    private func showMinimap(_ dots: [HudModel.Dot], camera: UIRect) throws {
        for index in 0..<Self.minimapDots {
            let element = tree["dot\(index)"]
            guard index < dots.count else {
                try element.setShown(false)
                continue
            }
            let dot = dots[index]
            let at = Vec2(x: dot.x, y: dot.y)
            try element.setShown(true)
            try element.setLayout(
                .anchored(
                    min: at, max: at, offsetMin: Vec2(x: -1.5, y: -1.5),
                    offsetMax: Vec2(x: 1.5, y: 1.5)))
            try element.setStyle(UIStyle(background: Self.teams[dot.team.rawValue]))
        }
        try tree["camera"].setLayout(
            .anchored(
                min: Vec2(x: camera.x, y: camera.y), max: Vec2(x: camera.right, y: camera.bottom)))
    }
}

extension HudModel {
    /// What `render.ui` shows: samples/13-rts-selection's HUD, as its C++ build_hud fills it.
    static let showcase = HudModel(
        resources: Resources(gold: 1250, wood: 830, food: 42, foodCap: 60),
        units: [
            Unit(name: "Rifleman", health: 100, maxHealth: 100),
            Unit(name: "Rifleman", health: 64, maxHealth: 100),
            Unit(name: "Engineer", health: 30, maxHealth: 80),
        ],
        dots: [
            Dot(x: 0.30, y: 0.62, team: .player), Dot(x: 0.34, y: 0.66, team: .player),
            Dot(x: 0.27, y: 0.70, team: .player), Dot(x: 0.62, y: 0.58, team: .enemy),
            Dot(x: 0.70, y: 0.52, team: .enemy), Dot(x: 0.15, y: 0.25, team: .neutral),
        ],
        // The camera looks past the map's left edge: its frame is clipped by the minimap.
        camera: UIRect(x: -0.1, y: 0.45, width: 0.5, height: 0.4))
}

/// The HUD alone, showing `HudModel.showcase`: what `render.rts_api_hud` photographs beside the C++
/// HUD showing the same thing. No game runs; the host creates it on a bare entity.
@Behaviour(name: "HudShowcase", schema: 1)
final class HudShowcase: Behaviour {
    private var hud: RtsHud?

    override func onCreate() throws {
        let made = try RtsHud(for: self)
        try made.show(.showcase)
        hud = made
    }
}

/// The same, with the Build button the game's HUD has.
@Behaviour(name: "HudShowcaseWithButton", schema: 1)
final class HudShowcaseWithButton: Behaviour {
    private var hud: RtsHud?

    override func onCreate() throws {
        let made = try RtsHud(for: self, onBuild: {})
        try made.show(.showcase)
        hud = made
    }
}
