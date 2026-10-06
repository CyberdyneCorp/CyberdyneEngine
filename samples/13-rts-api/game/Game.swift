// SPDX-License-Identifier: MIT
// Game.swift — the module's entry points and its one behaviour.
//
// `@GameModule` emits `cy_module_entry` and `cy_module_shutdown` into THIS module, not into
// CyberdyneKit, because a linker drops an unreferenced object out of a static archive
// (CyberdyneKit/Module.swift says why at length). The host attaches a `Commander` and a `Scout` to
// two level nodes and nothing else: every unit, order, sound, camera move, character step and push
// below comes from Swift — and `trainUnits` runs because the engine's scheduler runs it.

import CyberdyneKit

@GameModule
enum RtsGame: GameModule {
    static let components: [any Component.Type] = [
        RtsReport.self, ScoutReport.self, Veterancy.self,
    ]
    static let behaviours: [any BehaviourClass.Type] = [
        Commander.self, Scout.self, HudShowcase.self, HudShowcaseWithButton.self,
    ]
    static let systems: [any SystemRegistration.Type] = [
        __CySystem_trainUnits.self
    ]
}

/// Every unit with a `Veterancy` serves one more tick. A SYSTEM, not a behaviour: one call over
/// every unit's column per tick, scheduled by the engine in the simulation stage and ordered against
/// any native system touching `Veterancy` by this signature alone.
@System(stage: .simulation)
func trainUnits(_ query: Query<Write<Veterancy>>, _ chunks: ChunkSource) {
    chunks.forEachChunk(matching: type(of: query).access) { chunk in
        guard let veterancy = chunk.array(Veterancy.self) else { return }
        for index in 0..<chunk.count {
            veterancy[index].ticks += 1
        }
    }
}
