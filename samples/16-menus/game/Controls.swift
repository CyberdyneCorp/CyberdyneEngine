// SPDX-License-Identifier: MIT
// Controls.swift — the controls tab: every rebindable action's keyboard and gamepad control, and a
// rebind through the input system's own flow.
//
// A binding button opens the rebind dialog and starts `InputAction.rebind` for that action and
// scheme. The ENGINE listens: the next control the player presses on that scheme is offered to the
// input system's `RebindOperation`, and a control another action holds is swapped onto that action
// (`.swap`) — so Jump on E gives Interact Jump's old Space, and nothing is ever bound twice. Escape
// or a gamepad's Back cancels. When it settles the dialog closes, the buttons show the bindings in
// effect, and the player's profile is written to `bindings.cyinputprofile`, which the next start
// loads back.

import CyberdyneKit

struct Controls {
    let document: UIDocument
    private var actions: [(stem: String, action: InputAction)] = []
    /// The rebind in progress: its button, its action and its scheme.
    private(set) var listening: (button: Widget, stem: String, scheme: InputScheme)?
    /// A rebind ended this frame. The Escape that cancelled it is still `justPressed` for the
    /// game's own Cancel, which must not also leave the screen.
    private(set) var endedThisFrame = false

    init(document: UIDocument) throws {
        self.document = document
        actions = try Actions.rebindable.map { ($0.stem, try Input.find(action: $0.action)) }
    }

    /// Every binding button and the scheme it rebinds, for the click handlers.
    var buttons: [(button: Widget, stem: String, scheme: InputScheme)] {
        actions.flatMap { entry in
            [
                (document["bind-\(entry.stem)-keyboard"], entry.stem, .keyboardMouse),
                (document["bind-\(entry.stem)-gamepad"], entry.stem, .gamepad),
            ]
        }
    }

    /// Show the control each binding has now: the player's, or the game's own.
    func refresh() throws {
        for entry in actions {
            for (scheme, suffix) in [
                (InputScheme.keyboardMouse, "keyboard"), (.gamepad, "gamepad"),
            ] {
                let path = try entry.action.binding(scheme: scheme) ?? "none"
                try document["bind-\(entry.stem)-\(suffix)"].setText(Self.caption(path))
            }
        }
    }

    /// Start listening for `stem`'s control on `scheme`.
    mutating func begin(_ button: Widget, stem: String, scheme: InputScheme) throws {
        guard let action = actions.first(where: { $0.stem == stem })?.action else { return }
        let name = Actions.rebindable.first { $0.stem == stem }?.action ?? stem
        let device = scheme == .gamepad ? "a button" : "a key"
        let shown = name.hasPrefix("Game.") ? String(name.dropFirst("Game.".count)) : name
        try document["rebind-prompt"].setText("Press \(device) for \(shown)")
        try document["rebind-dialog"].open()
        try action.rebind(scheme: scheme, conflict: .swap)
        listening = (button, stem, scheme)
    }

    /// Once a frame while listening: the state the engine's rebind is in. Nil while it listens.
    mutating func poll() throws -> RebindState? {
        guard listening != nil else { return nil }
        let state = try Rebinding.poll()
        if state.status == .listening {
            return nil
        }
        try finish()
        return state
    }

    /// The dialog closed under the rebind — Escape reached the toolkit before the input system.
    mutating func dialogClosed() throws {
        guard listening != nil else { return }
        try Rebinding.cancel()
        try finish()
    }

    /// Once a frame, after the game read its keys.
    mutating func endFrame() {
        endedThisFrame = false
    }

    private mutating func finish() throws {
        let button = listening?.button
        listening = nil
        endedThisFrame = true
        let dialog = document["rebind-dialog"]
        try? dialog.close()
        try refresh()
        try button?.focus()
    }

    /// What a control's path reads as on its button: the key's name, or the gamepad's button.
    static func caption(_ path: String) -> String {
        let parts = path.split(separator: "/").map(String.init)
        guard parts.count == 2 else { return path == "none" ? "-" : path }
        let gamepad = [
            "south": "A", "east": "B", "west": "X", "north": "Y", "start": "Start",
            "back": "Back", "leftShoulder": "LB", "rightShoulder": "RB", "leftStick": "L3",
            "rightStick": "R3", "dpadUp": "D-pad up", "dpadDown": "D-pad down",
        ]
        if parts[0] == "gamepad" {
            return gamepad[parts[1]] ?? parts[1]
        }
        // `leftShift` reads as `Left shift`: a capital, and a space before every later capital.
        var caption = ""
        for character in parts[1] {
            if caption.isEmpty {
                caption += character.uppercased()
            } else if character.isUppercase {
                caption += " " + character.lowercased()
            } else {
                caption.append(character)
            }
        }
        return caption
    }
}
