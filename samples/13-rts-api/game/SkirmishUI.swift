// SPDX-License-Identifier: MIT
// SkirmishUI.swift — the skirmish's own interface, beside the HUD (Hud.swift): a command bar, a
// line of status, a banner for what the game is waiting for, the box a drag draws, and — through
// the engine's widget toolkit (ABI 1.12) — the Esc menu and the result screen.
//
// The command bar is CyberUI's elements, as the HUD is; the two dialogs are toolkit widgets,
// modal over a dimmed backdrop. A click on any of them is recorded as an intent and acted on by
// the skirmish in the same frame, so a button and its key do exactly the same thing.

import CyberdyneKit

/// What the interface asked for since the skirmish last looked. A reference type, because the
/// buttons' handlers hold it.
final class SkirmishIntents {
    var attackMove = false
    var stop = false
    var army = false
    var menu = false
    var resume = false
    var quit = false
    var watch = false

    func clear() {
        attackMove = false
        stop = false
        army = false
        menu = false
        resume = false
        quit = false
        watch = false
    }
}

/// What the skirmish shows this frame.
struct SkirmishScreen: Equatable {
    var banner = ""
    var status = ""
    var box: UIRect?
    var armed = false
}

final class SkirmishUI {
    static let panel: UIColour = 0xC80E_141C
    static let border: UIColour = 0xFF3C_5064
    static let text: UIColour = 0xFFE6_EBF0
    static let warn: UIColour = 0xFFFF_D34D
    static let armedBorder: UIColour = 0xFFFF_6040
    static let boxBorder: UIColour = 0xFF7F_E07F
    static let boxFill: UIColour = 0x2014_3014

    let tree: UITree
    /// The menu and the two results, or nil where the engine has no widget toolkit.
    private let dialogs: WidgetTree?
    private var shown = SkirmishScreen()
    private var open: String?

    init(for owner: Behaviour, intents: SkirmishIntents) throws {
        tree = try owner.mountUI {
            SkirmishUI.commandBar(intents)
            Label("", colour: SkirmishUI.warn, scale: 2, name: "banner")
                .layout(
                    UILayout.anchored(
                        min: Vec2(x: 0.5, y: 0), max: Vec2(x: 0.5, y: 0),
                        offsetMin: Vec2(x: -200, y: 40), offsetMax: Vec2(x: 200, y: 60))
                )
                .id("banner")
            Label("", colour: SkirmishUI.text, name: "status")
                .layout(
                    UILayout.anchored(
                        min: Vec2(x: 0.5, y: 0), max: Vec2(x: 0.5, y: 0),
                        offsetMin: Vec2(x: -150, y: 21), offsetMax: Vec2(x: 150, y: 34))
                )
                .id("status")
            Panel("selection-box")
                .style(UIStyle(background: SkirmishUI.boxFill, border: SkirmishUI.boxBorder))
                .visibility(.collapsed)
                .id("box")
        }
        dialogs = try SkirmishUI.mountDialogs(owner, intents)
    }

    // --- Building --------------------------------------------------------------------------------

    private static func commandBar(_ intents: SkirmishIntents) -> any UINode {
        Panel("command-bar") {
            SkirmishUI.button("Attack (F)", id: "attack") { intents.attackMove = true }
            SkirmishUI.button("Stop (H)", id: "stop") { intents.stop = true }
            SkirmishUI.button("Army (E)", id: "army") { intents.army = true }
            SkirmishUI.button("Menu (Esc)", id: "menu") { intents.menu = true }
        }
        .layout(
            UILayout.anchored(
                min: Vec2(x: 0.5, y: 1), max: Vec2(x: 0.5, y: 1), offsetMin: Vec2(x: -170, y: -30),
                offsetMax: Vec2(x: 170, y: -6)
            ).flex(.row, gap: 6, align: .centre, padding: UIInsets(horizontal: 6, vertical: 3))
        )
        .style(UIStyle(background: panel, border: border, cornerRadius: 3))
        .id("command-bar")
    }

    private static func button(
        _ title: String, id: String, _ action: @escaping () -> Void
    ) -> any UINode {
        Button(title, colour: text, name: "command") { _ in action() }
            .layout(UILayout().flex(.row, padding: UIInsets(horizontal: 6, vertical: 2)))
            .style(UIStyle(background: panel, border: border, cornerRadius: 2))
            .id(id)
    }

    private static func mountDialogs(
        _ owner: Behaviour, _ intents: SkirmishIntents
    ) throws -> WidgetTree? {
        do {
            return try owner.mountWidgets {
                WidgetNode.dialog("Paused", id: "menu-dialog") {
                    WidgetNode.column {
                        WidgetNode.label("The battle waits for you.")
                        WidgetNode.button("Resume", id: "resume") { _ in intents.resume = true }
                        WidgetNode.button("Quit", id: "quit-menu") { _ in intents.quit = true }
                    }
                }
                for (id, title) in [("victory", "Victory"), ("defeat", "Defeat")] {
                    WidgetNode.dialog(title, id: id) {
                        WidgetNode.column {
                            WidgetNode.label("", id: "\(id)-summary")
                            WidgetNode.button("Keep watching", id: "\(id)-watch") { _ in
                                intents.watch = true
                            }
                            WidgetNode.button("Quit", id: "\(id)-quit") { _ in intents.quit = true }
                        }
                    }
                }
            }
        } catch CyberdyneError.status(.unavailable, _) {
            Log.info("Skirmish: the engine has no widget toolkit; Esc pauses without a menu")
            return nil
        }
    }

    // --- Showing ---------------------------------------------------------------------------------

    /// Write what changed since the last frame.
    func show(_ screen: SkirmishScreen) throws {
        if screen.banner != shown.banner {
            try tree["banner"].setText(screen.banner, colour: SkirmishUI.warn, scale: 2)
        }
        if screen.status != shown.status {
            try tree["status"].setText(screen.status, colour: SkirmishUI.text)
        }
        if screen.box != shown.box {
            try showBox(screen.box)
        }
        if screen.armed != shown.armed {
            let edge = screen.armed ? SkirmishUI.armedBorder : SkirmishUI.border
            try tree["attack"].setStyle(
                UIStyle(background: SkirmishUI.panel, border: edge, cornerRadius: 2))
        }
        shown = screen
    }

    private func showBox(_ box: UIRect?) throws {
        let element = tree["box"]
        guard let box else {
            try element.setShown(false)
            return
        }
        try element.setLayout(.at(x: box.x, y: box.y, width: box.width, height: box.height))
        try element.setShown(true)
    }

    /// Open the dialog named `id` (`menu-dialog`, `victory` or `defeat`) over the game, closing the
    /// one open; nil closes it. A result's summary is written before it opens.
    func present(_ id: String?, summary: String = "") throws {
        guard let dialogs, id != open else { return }
        if let open {
            try dialogs[open].close()
        }
        if let id {
            if dialogs.contains("\(id)-summary") {
                try dialogs["\(id)-summary"].setText(summary)
            }
            try dialogs[id].open()
        }
        open = id
    }

    /// The dialog open now, if any.
    var presented: String? { open }
}
