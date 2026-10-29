// SPDX-License-Identifier: MIT
// Game.swift — the module's entry points. `@GameModule` emits `cy_module_entry` and
// `cy_module_shutdown` into THIS module (samples/13-rts-api/game/Game.swift says why). The host
// attaches `Menus` to one node; every screen below it comes from Swift.

import CyberdyneKit

@GameModule
enum MenusGame: GameModule {
    static let components: [any Component.Type] = [MenusReport.self]
    static let behaviours: [any BehaviourClass.Type] = [Menus.self]
    static let systems: [any SystemRegistration.Type] = []
}
