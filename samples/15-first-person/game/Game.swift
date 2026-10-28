// SPDX-License-Identifier: MIT
// Game.swift — the module's entry points.
//
// `@GameModule` emits `cy_module_entry` and `cy_module_shutdown` into THIS module, not into
// CyberdyneKit, because a linker drops an unreferenced object out of a static archive
// (CyberdyneKit/Module.swift says why). The host attaches one `Player` to `/Arena/Player` and
// nothing else: the character, the look, the shots and everything a shot leaves behind come from
// Swift. The targets the player shoots are the engine's AI's.

import CyberdyneKit

@GameModule
enum FirstPersonGame: GameModule {
    static let components: [any Component.Type] = [PlayerReport.self, Health.self]
    static let behaviours: [any BehaviourClass.Type] = [Player.self]
    static let systems: [any SystemRegistration.Type] = []
}
