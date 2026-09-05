// Plugin.swift — the compiler plugin this target is.
//
// A macro implementation is a program the Swift compiler launches and talks to over a pipe. This
// file is its `main`; everything it provides is in the four files beside it.

import SwiftCompilerPlugin
import SwiftSyntaxMacros

@main
struct CyberdyneMacrosPlugin: CompilerPlugin {
    let providingMacros: [Macro.Type] = [
        BehaviourMacro.self,
        ComponentMacro.self,
        SystemMacro.self,
        GameModuleMacro.self,
    ]
}
