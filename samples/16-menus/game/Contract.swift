// SPDX-License-Identifier: MIT
// Contract.swift — what the menus game and the host agree on: the documents' paths, the actions'
// names, the player's files, and one report the host reads for its test.
//
// The documents are the ones the UI layout editor wrote (project/ui/, from tools/session.py); the
// game loads them by path, so a document saved in the editor while the game runs is reloaded in
// it, every widget the game holds keeping its handle and its state.

import CyberdyneKit

enum Documents {
    static let mainMenu = "ui/main_menu.cyui"
    static let settings = "ui/settings.cyui"
    static let pause = "ui/pause.cyui"
    static let inventory = "ui/inventory.cyui"
    static let playing = "ui/playing.cyui"
}

/// The game's actions, which the host declares in its `Game` context, and the interface's own.
enum Actions {
    static let jump = "Game.Jump"
    static let interact = "Game.Interact"
    static let inventory = "Game.Inventory"
    static let sprint = "Game.Sprint"
    static let pause = "Game.Pause"
    /// What closes a screen: Escape, a gamepad's east button.
    static let cancel = "UI.Cancel"
    /// The rows of the controls tab, in order: the action and the id stem of its two buttons.
    static let rebindable: [(action: String, stem: String)] = [
        (jump, "jump"), (interact, "interact"), (inventory, "inventory"), (sprint, "sprint"),
    ]
}

/// The player's files, in the user-data directory the host binds.
enum PlayerFiles {
    static let settings = "settings.cfg"
    static let bindings = "bindings.cyinputprofile"
}

/// The screens, numbered for the report.
enum Screen: Int {
    case mainMenu = 1
    case settings = 2
    case playing = 3
    case paused = 4
    case inventory = 5
    case quit = 6
}

/// What the game did, for the host's report and its test; nothing in the host decides by it.
@Component
struct MenusReport {
    /// The screen on show: a `Screen`.
    var screen: Float = 0
    /// New games started, times settings were applied, rebinds the input system applied, and
    /// conflicts it reported.
    var started: Float = 0
    var applied: Float = 0
    var rebinds: Float = 0
    var conflicts: Float = 0
    /// Whether the player's files were found and loaded at start: settings, then bindings.
    var loadedSettings: Float = 0
    var loadedBindings: Float = 0
    /// Inventory moves: picked up with Accept, put down, dragged with the pointer.
    var picks: Float = 0
    var drops: Float = 0
    var drags: Float = 0
    /// The item in the inventory's first slot, by its index in `Inventory.items` (-1 for none),
    /// and the slot the potion is in: what the test reads a move by.
    var firstSlotItem: Float = -1
    var potionSlot: Float = -1
    /// Screens opened by a cancel rather than a button: the gamepad's and Escape's way back.
    var backs: Float = 0
    /// Jumps while playing: the game's own action, on whatever control the player bound it to.
    var jumps: Float = 0
}

extension Component {
    /// A field's index, by name. Resolved once in `onCreate`.
    static func field(_ name: String) -> UInt32 {
        guard let index = componentFields.firstIndex(where: { $0.name == name }) else {
            Log.error("\(componentName) has no field named '\(name)'")
            return 0
        }
        return UInt32(index)
    }
}
