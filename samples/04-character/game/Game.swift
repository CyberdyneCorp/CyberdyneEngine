// Game.swift — the module's two exported symbols, and the order its behaviours register in.
//
// `@GameModule` emits `cy_module_entry` and `cy_module_shutdown` INTO THIS MODULE. They cannot live
// in CyberdyneKit: a linker drops an unreferenced object out of a static archive, and the loader
// would then report a module that "did not export its declared entry symbol" with nothing pointing
// at the cause. See CyberdyneKit/Module.swift, which says so at greater length.
//
// This file, plus the four beside it, is the whole game. There is no build script, no code
// generator and no engine header: it is an ordinary Swift package target that depends on
// `CyberdyneKit`, which is what `swift-scripting`'s "Standard Swift tooling" requires.

import CyberdyneKit

@GameModule
enum CharacterGame: GameModule {
    /// Registration order, which is not the creation order — the host creates instances by name and
    /// bridge.cpp records why the director is created before the character.
    static let behaviours: [any BehaviourClass.Type] = [
        Level.self,
        CameraDirector.self,
        Character.self,
    ]
}
