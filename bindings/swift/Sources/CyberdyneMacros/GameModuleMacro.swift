// GameModuleMacro.swift — `@GameModule`. Tasks 3.4, 3.7.
//
// Emits the two C entry points a game module's shared library exports. Three lines each, and they
// have to be in the GAME's own module: a linker pulls an object out of a static archive only when
// something references a symbol in it, so an `@_cdecl` sitting in CyberdyneKit would be dropped from
// the game's `.so` and the loader would report a module that "did not export its declared entry
// symbol" with nothing pointing at why. Module.swift carries the same note at the other end.

import SwiftSyntax
import SwiftSyntaxMacros

public struct GameModuleMacro: PeerMacro {
    public static func expansion(of node: AttributeSyntax,
                                 providingPeersOf declaration: some DeclSyntaxProtocol,
                                 in context: some MacroExpansionContext) throws -> [DeclSyntax] {
        guard let name = typeName(of: declaration) else {
            context.fail(node, .gameModuleNeedsType)
            return []
        }
        return ["""
        @_cdecl("cy_module_entry")
        public func cy_module_entry(_ interface: UnsafePointer<CyInterface>?,
                                    _ engine: CyEngine?,
                                    _ out: UnsafeMutablePointer<CyModuleInit>?) -> Bool {
            ModuleBootstrap.entry(interface, engine, out, module: \(raw: name).self)
        }
        """, """
        @_cdecl("cy_module_shutdown")
        public func cy_module_shutdown() {
            ModuleBootstrap.shutdown()
        }
        """]
    }

    static func typeName(of declaration: some DeclSyntaxProtocol) -> String? {
        if let enumDecl = declaration.as(EnumDeclSyntax.self) { return enumDecl.name.text }
        if let structDecl = declaration.as(StructDeclSyntax.self) { return structDecl.name.text }
        if let classDecl = declaration.as(ClassDeclSyntax.self) { return classDecl.name.text }
        return nil
    }
}
