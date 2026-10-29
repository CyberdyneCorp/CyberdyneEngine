// SPDX-License-Identifier: MIT
// PauseMenu.swift — the pause menu of a game a desktop play host runs (samples/play), as a game
// writes it:
//
//     private var menu: PauseMenu?
//
//     override func onReady() throws {
//         // Only where the host offers an interface: a scripted or headless run has none.
//         menu = try? PauseMenu(for: self, title: "Paused") { [unowned self] settings in
//             self.lookScale = settings.lookSensitivity
//         }
//     }
//     override func onUpdate(_ delta: Double) throws {
//         try menu?.update(paused: Time.now.isPaused)
//     }
//
// WHAT THE HOST READS, AND ALL IT READS. The host pauses the game — Esc, a pad's Start, the window
// losing the keyboard's focus — and says so in `Time.now.isPaused`. The menu shows itself while
// that is true. Its two buttons are the host's: a click on the widget whose id is `play-resume`
// resumes the game and one on `play-quit` ends the run (`PauseMenu.resumeId`, `.quitId`). Nothing
// else crosses: the look sensitivity and the inverted Y are the GAME's settings, handed to the
// closure, and the game applies them where it applies its look — once.

import CyberdyneABI
import CyberdyneCore

/// A player's settings, as the pause menu edits them.
public struct PlaySettings: Equatable, Sendable {
    /// A multiplier on the game's own look rate: 1 is the game's default.
    public var lookSensitivity: Float = 1
    /// Pushing the mouse (or the stick) forward looks down.
    public var invertY = false

    /// The defaults.
    public init(lookSensitivity: Float = 1, invertY: Bool = false) {
        self.lookSensitivity = lookSensitivity
        self.invertY = invertY
    }
}

/// The pause menu: a dimmed screen, a card with the look sensitivity, the inverted Y, Resume and
/// Quit, shown while the game is paused.
public final class PauseMenu {
    /// The ids the play host listens for.
    public static let resumeId = "play-resume"
    public static let quitId = "play-quit"
    /// The sensitivity slider's range.
    public static let sensitivityRange: ClosedRange<Float> = 0.25...4

    /// The settings as the player last left them.
    public private(set) var settings: PlaySettings
    /// Whether the menu is on screen.
    public private(set) var isShown = false

    private let document: UIDocument
    private let changed: (PlaySettings) -> Void

    /// Load the menu for `owner`, whose entity receives its events, hidden. Throws where the host
    /// has no interface (`.unavailable`).
    public init(
        for owner: Behaviour, title: String = "Paused", settings: PlaySettings = PlaySettings(),
        onChange: @escaping (PlaySettings) -> Void
    ) throws {
        self.settings = settings
        changed = onChange
        document = try owner.loadUI(document: PauseMenu.text(title: title, settings: settings))
        try document.root.element.setShown(false)
        owner.on(.changed, of: document["play-sensitivity"]) { [unowned self] event in
            self.settings.lookSensitivity = event.value
            self.changed(self.settings)
        }
        owner.on(.changed, of: document["play-invert"]) { [unowned self] event in
            self.settings.invertY = event.value > 0.5
            self.changed(self.settings)
        }
    }

    /// Show the menu while `paused`, hide it otherwise.
    public func update(paused: Bool) throws {
        guard paused != isShown else { return }
        isShown = paused
        try document.root.element.setShown(paused)
    }

    /// The document: a full-screen panel that dims the game, and a card at its centre.
    static func text(title: String, settings: PlaySettings) -> String {
        let range = sensitivityRange
        let invert = settings.invertY ? " checked=true" : ""
        return """
            cyui 1
            panel #play-menu anchor=[0, 0, 1, 1]
              column #play-card .panel anchor=[0.5, 0.5, 0.5, 0.5] offset=[-190, -92, 190, 92]
                label .heading text="\(title)"
                label .caption text="Look sensitivity"
                slider #play-sensitivity min=\(range.lowerBound) max=\(range.upperBound) \
            step=0.05 value=\(settings.lookSensitivity)
                checkbox #play-invert text="Invert the look's Y"\(invert)
                label .caption text="Esc resumes"
                row #play-buttons
                  button #\(resumeId) .primary text="Resume"
                  button #\(quitId) .danger text="Quit"
            """
    }
}
