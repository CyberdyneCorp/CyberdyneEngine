// SPDX-License-Identifier: MIT
// Game.swift — the module's entry points.
//
// `@GameModule` emits `cy_module_entry` and `cy_module_shutdown` into THIS module rather than into
// CyberdyneKit (CyberdyneKit/Module.swift says why). The host attaches `Hero`, `CameraDirector` and
// `Ambience` to three level nodes and nothing else: every step, jump, footfall, camera move and
// bird below comes from Swift.

import CyberdyneKit

@GameModule
enum ThirdPersonGame: GameModule {
    static let components: [any Component.Type] = [
        HeroPose.self, HeroReport.self, CameraAim.self,
    ]
    static let behaviours: [any BehaviourClass.Type] = [
        Hero.self, CameraDirector.self, Ambience.self,
    ]
}
