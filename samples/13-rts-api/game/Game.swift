// SPDX-License-Identifier: MIT
// Game.swift — the module's entry points and its one behaviour.
//
// `@GameModule` emits `cy_module_entry` and `cy_module_shutdown` into THIS module, not into
// CyberdyneKit, because a linker drops an unreferenced object out of a static archive
// (CyberdyneKit/Module.swift says why at length). The host creates one `Commander` on a player
// entity and nothing else: every unit, order, sound and camera move below comes from Swift.

import CyberdyneKit

@GameModule
enum RtsGame: GameModule {
    static let behaviours: [any BehaviourClass.Type] = [
        Commander.self
    ]
}
