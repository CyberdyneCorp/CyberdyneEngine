// SPDX-License-Identifier: MIT
// Menus.swift — a game's menus, written in Swift over documents made in the UI layout editor: a
// main menu, a settings screen, a pause menu and an inventory, every one of them navigable with a
// keyboard, a gamepad or the pointer.
//
// ONE SCREEN AT A TIME. Every document is loaded once, by path, in `onCreate`, and the screens are
// shown and collapsed as the player moves between them; the screen shown gets the focus on its
// first button, so a keyboard or a gamepad can drive it from the first frame. The interface's own
// actions — Accept, the four directions, the tabs — reach the toolkit through the host's input
// bridge; what is the GAME's is what a screen's buttons do and where Cancel goes back to.
//
//   Main menu  -> New game -> Playing <-> Paused -> Inventory
//              -> Continue (when a session was saved)        -> Settings
//              -> Settings -> Back
//              -> Quit
//
// A save of a document in the editor while this runs reloads it here: the widgets this behaviour
// holds keep their handles, and the screen keeps its state.

import CyberdyneKit

@Behaviour(name: "Menus", schema: 1)
final class Menus: Behaviour {
    private var screens: [Screen: UIDocument] = [:]
    private var screen: Screen = .mainMenu
    /// Where Settings goes back to: the main menu, or the pause menu.
    private var settingsReturn: Screen = .mainMenu
    private var settings = GameSettings()
    private var settingsScreen: SettingsScreen?
    private var controls: Controls?
    private var inventory: Inventory?
    private var cancel = InputAction(raw: 0)
    private var pause = InputAction(raw: 0)
    private var openInventory = InputAction(raw: 0)
    private var jump = InputAction(raw: 0)
    private var tally = Tally()
    private var report = ComponentType(id: 0)
    private let fields = ReportFields()

    // --- Lifecycle -----------------------------------------------------------------------------

    override func onCreate() throws {
        guard let world else { throw CyberdyneError.invalidHandle }
        report = try Components.register(MenusReport.self, in: world)
        if !world.has(report, on: entity) {
            try world.add(report, to: entity)
        }
        cancel = try Input.find(action: Actions.cancel)
        pause = try Input.find(action: Actions.pause)
        openInventory = try Input.find(action: Actions.inventory)
        jump = try Input.find(action: Actions.jump)
        for (screen, path) in [
            (Screen.mainMenu, Documents.mainMenu), (.settings, Documents.settings),
            (.paused, Documents.pause), (.inventory, Documents.inventory),
            (.playing, Documents.playing),
        ] {
            screens[screen] = try loadUI(path: path)
        }
        try loadPlayerFiles()
        try wireMainMenu()
        try wireSettings()
        try wirePause()
        try wireInventory()
        try show(.mainMenu)
        Log.info(
            "Menus: \(screens.count) screens; settings \(tally.loadedSettings), bindings \(tally.loadedBindings)"
        )
    }

    override func onUpdate(_ delta: Double) throws {
        try pollRebind()
        if screen == .inventory, var inventory {
            try inventory.pointer(try Input.pointer(), scale: try uiScale())
            self.inventory = inventory
        }
        try handleKeys()
        controls?.endFrame()
        try publish()
    }

    // --- Screens -------------------------------------------------------------------------------

    /// Show one screen, collapse the rest, and give it the focus. Playing keeps none: the game has
    /// the input, and the interface's bindings do nothing.
    private func show(_ next: Screen) throws {
        screen = next
        for (each, document) in screens {
            // The pause menu and the inventory are drawn over Playing's HUD.
            let overlaid = each == .playing && (next == .paused || next == .inventory)
            try document.root.setShown(each == next || overlaid)
        }
        switch next {
        case .mainMenu: try screens[.mainMenu]?["new-game"].focus()
        case .settings: try screens[.settings]?["resolution"].focus()
        case .paused: try screens[.paused]?["resume"].focus()
        case .inventory: try inventory?.widget(0).focus()
        case .playing, .quit: try UI.clearFocus()
        }
    }

    private func handleKeys() throws {
        let cancelled = try cancel.state().justPressed
        switch screen {
        case .playing:
            if try jump.state().justPressed {
                tally.jumps += 1
            }
            if try pause.state().justPressed {
                try show(.paused)
            } else if try openInventory.state().justPressed {
                try show(.inventory)
            }
        case .paused where cancelled:
            tally.backs += 1
            try show(.playing)
        case .settings
        where cancelled && controls?.listening == nil
            && controls?.endedThisFrame != true:
            tally.backs += 1
            try show(settingsReturn)
        case .inventory where cancelled:
            if try inventory?.cancel() != true {
                tally.backs += 1
                try show(.paused)
            }
        default:
            break
        }
    }

    private func button(_ screen: Screen, _ id: String, _ action: @escaping () throws -> Void) {
        guard let document = screens[screen] else { return }
        on(.click, of: document[id]) { _ in try action() }
    }

    private func wireMainMenu() throws {
        try screens[.mainMenu]?["continue"].setEnabled(tally.loadedSettings > 0)
        button(.mainMenu, "new-game") { [unowned self] in
            tally.started += 1
            try show(.playing)
        }
        button(.mainMenu, "continue") { [unowned self] in try show(.playing) }
        button(.mainMenu, "settings") { [unowned self] in
            settingsReturn = .mainMenu
            try openSettings()
        }
        button(.mainMenu, "quit") { [unowned self] in
            screen = .quit
            try publish()
        }
    }

    private func wirePause() throws {
        button(.paused, "resume") { [unowned self] in try show(.playing) }
        button(.paused, "open-inventory") { [unowned self] in try show(.inventory) }
        button(.paused, "pause-settings") { [unowned self] in
            settingsReturn = .paused
            try openSettings()
        }
        button(.paused, "to-main-menu") { [unowned self] in try show(.mainMenu) }
    }

    // --- Settings ------------------------------------------------------------------------------

    private func wireSettings() throws {
        guard let document = screens[.settings] else { return }
        var screen = SettingsScreen(document: document)
        try screen.listModes()
        settingsScreen = screen
        controls = try Controls(document: document)
        try controls?.refresh()
        for entry in controls?.buttons ?? [] {
            on(.click, of: entry.button) { [unowned self] _ in
                try controls?.begin(entry.button, stem: entry.stem, scheme: entry.scheme)
            }
        }
        on(.closed, of: document["rebind-dialog"]) { [unowned self] _ in
            try controls?.dialogClosed()
        }
        button(.settings, "reset-bindings") { [unowned self] in
            try InputProfile.reset()
            try UserData.write(PlayerFiles.bindings, text: "")
            try controls?.refresh()
        }
        button(.settings, "apply") { [unowned self] in try applySettings() }
        button(.settings, "back") { [unowned self] in try show(settingsReturn) }
    }

    private func openSettings() throws {
        try settingsScreen?.show(settings)
        try show(.settings)
    }

    private func applySettings() throws {
        guard let screen = settingsScreen else { return }
        settings = try screen.read(over: settings)
        try settings.applyToEngine()
        try UserData.write(PlayerFiles.settings, text: settings.text)
        tally.applied += 1
    }

    private func pollRebind() throws {
        guard let state = try controls?.poll() else { return }
        switch state.status {
        case .applied:
            tally.rebinds += 1
            try UserData.write(PlayerFiles.bindings, text: try InputProfile.save())
            Log.info("Menus: \(state.control) bound; \(state.conflict) took the old control")
        case .conflict:
            tally.conflicts += 1
        default:
            break
        }
    }

    /// The player's files, when a session left them: the settings applied to the engine and the
    /// bindings put back over the game's own.
    private func loadPlayerFiles() throws {
        if let text = try UserData.readText(PlayerFiles.settings) {
            settings = GameSettings(text: text)
            tally.loadedSettings = 1
        }
        try settings.applyToEngine()
        if let text = try UserData.readText(PlayerFiles.bindings), !text.isEmpty {
            try InputProfile.load(text)
            tally.loadedBindings = 1
        }
    }

    // --- Inventory -----------------------------------------------------------------------------

    private func wireInventory() throws {
        guard let document = screens[.inventory] else { return }
        let grid = Inventory(document: document)
        try grid.show()
        inventory = grid
        for slot in 0..<Inventory.slotCount {
            on(.click, of: grid.widget(slot)) { [unowned self] _ in try inventory?.accept(slot) }
            on(.focus, of: grid.widget(slot)) { [unowned self] _ in try inventory?.describe(slot) }
        }
        button(.inventory, "inventory-back") { [unowned self] in try show(.paused) }
    }

    /// Reference units per window pixel: the interface is designed at 1280x720 and scaled to the
    /// output's height.
    private func uiScale() throws -> Float {
        Float(try Video.settings.height) / 720
    }

    // --- The report ----------------------------------------------------------------------------

    private func publish() throws {
        guard let world else { return }
        let values: [(UInt32, Float)] = [
            (fields.screen, Float(screen.rawValue)), (fields.started, tally.started),
            (fields.applied, tally.applied), (fields.rebinds, tally.rebinds),
            (fields.conflicts, tally.conflicts), (fields.loadedSettings, tally.loadedSettings),
            (fields.loadedBindings, tally.loadedBindings),
            (fields.picks, Float(inventory?.picks ?? 0)),
            (fields.drops, Float(inventory?.drops ?? 0)),
            (fields.drags, Float(inventory?.drags ?? 0)),
            (fields.firstSlotItem, inventory?.firstSlotItem ?? -1),
            (fields.potionSlot, inventory?.potionSlot ?? -1), (fields.backs, tally.backs),
            (fields.jumps, tally.jumps),
        ]
        for (field, value) in values {
            try world.setFloat(value, entity, report, field: field)
        }
    }
}

private struct Tally {
    var started: Float = 0
    var applied: Float = 0
    var rebinds: Float = 0
    var conflicts: Float = 0
    var loadedSettings: Float = 0
    var loadedBindings: Float = 0
    var backs: Float = 0
    var jumps: Float = 0
}

/// `MenusReport`'s field indices, resolved by name once.
private struct ReportFields {
    let screen = MenusReport.field("screen")
    let started = MenusReport.field("started")
    let applied = MenusReport.field("applied")
    let rebinds = MenusReport.field("rebinds")
    let conflicts = MenusReport.field("conflicts")
    let loadedSettings = MenusReport.field("loadedSettings")
    let loadedBindings = MenusReport.field("loadedBindings")
    let picks = MenusReport.field("picks")
    let drops = MenusReport.field("drops")
    let drags = MenusReport.field("drags")
    let firstSlotItem = MenusReport.field("firstSlotItem")
    let potionSlot = MenusReport.field("potionSlot")
    let backs = MenusReport.field("backs")
    let jumps = MenusReport.field("jumps")
}
