// BehaviourMacro.swift — `@Behaviour`. Task 3.4.
//
// What it emits, and why each part cannot be written any other way:
//
//   behaviourName        the class's own name, so a game never has to keep a string in step with a
//                        type. It is the key a hot reload matches an instance's saved state by, so
//                        renaming the class IS renaming the behaviour, which is the right coupling.
//   behaviourSchema      1 unless the attribute says otherwise. Bumping it is a deliberate act: it
//                        is what makes an older module refuse a newer blob rather than misread it.
//   behaviourCallbacks   THE ONE THAT CANNOT BE DONE AT RUN TIME. Which lifecycle functions the
//                        class declares, read out of the class body here. See Behaviour.swift.
//   exportedNames        the `@Export`ed properties in declaration order, which is the order the
//                        blob is written in and the order an inspector shows.
//   exportedStorage      a switch from a name to the property wrapper behind it. A switch rather
//                        than reflection, deliberately: `Mirror` and `_typeByName` are process-
//                        global name lookups, and the hot-reload spike measured one of those
//                        returning a RETIRED generation's metadata with two images resident.

import SwiftSyntax
import SwiftSyntaxMacros

public struct BehaviourMacro: MemberMacro, ExtensionMacro {
    /// The lifecycle functions, and the `CallbackSet` member each one implies.
    static let callbacks: [(String, String)] = [
        ("onCreate", ".create"), ("onEnterTree", ".enterTree"), ("onReady", ".ready"),
        ("onEnable", ".enable"), ("onDisable", ".disable"), ("onFixedUpdate", ".fixedUpdate"),
        ("onUpdate", ".update"), ("onExitTree", ".exitTree"), ("onDestroy", ".destroy"),
        ("onAfterReload", ".afterReload"), ("onMigrate", ".migrate"),
    ]

    public static func expansion(of node: AttributeSyntax,
                                 providingMembersOf declaration: some DeclGroupSyntax,
                                 conformingTo protocols: [TypeSyntax],
                                 in context: some MacroExpansionContext) throws -> [DeclSyntax] {
        guard let classDecl = declaration.as(ClassDeclSyntax.self) else {
            context.fail(node, .behaviourNeedsClass)
            return []
        }
        let options = arguments(of: node)
        let name = stringLiteral(options["name"]) ?? classDecl.name.text
        let schema = options["schema"]?.trimmedDescription ?? "1"
        let exported = exportedProperties(of: classDecl, in: context)
        let implemented = implementedCallbacks(of: classDecl)

        return [
            "public static let behaviourName: String = \"\(raw: name)\"",
            "public static let behaviourSchema: UInt32 = \(raw: schema)",
            "public static let behaviourCallbacks: CallbackSet = [\(raw: implemented.joined(separator: ", "))]",
            "public static let exportedNames: [String] = [\(raw: exported.map { "\"\($0)\"" }.joined(separator: ", "))]",
            storageAccessor(exported),
        ]
    }

    public static func expansion(of node: AttributeSyntax,
                                 attachedTo declaration: some DeclGroupSyntax,
                                 providingExtensionsOf type: some TypeSyntaxProtocol,
                                 conformingTo protocols: [TypeSyntax],
                                 in context: some MacroExpansionContext) throws
        -> [ExtensionDeclSyntax] {
        guard declaration.is(ClassDeclSyntax.self) else { return [] }
        // Empty when the class already states the conformance: emitting it twice is an error, and a
        // game that writes `: Behaviour, BehaviourClass` is not wrong.
        guard !protocols.isEmpty else { return [] }
        return [try ExtensionDeclSyntax("extension \(type.trimmed): BehaviourClass {}")]
    }

    /// The `@Export`ed property names, in declaration order.
    static func exportedProperties(of declaration: ClassDeclSyntax,
                                   in context: some MacroExpansionContext) -> [String] {
        var names: [String] = []
        for member in declaration.memberBlock.members {
            guard let variable = member.decl.as(VariableDeclSyntax.self),
                  hasAttribute("Export", on: variable.attributes) else { continue }
            if variable.bindingSpecifier.tokenKind == .keyword(.let) {
                context.fail(variable, .behaviourExportMustBeVar)
                continue
            }
            for binding in variable.bindings {
                if let identifier = binding.pattern.as(IdentifierPatternSyntax.self) {
                    names.append(identifier.identifier.text)
                }
            }
        }
        return names
    }

    /// The `CallbackSet` members for the lifecycle functions this class declares.
    static func implementedCallbacks(of declaration: ClassDeclSyntax) -> [String] {
        let declared = Set(declaration.memberBlock.members.compactMap { member in
            member.decl.as(FunctionDeclSyntax.self)?.name.text
        })
        return callbacks.filter { declared.contains($0.0) }.map(\.1)
    }

    static func storageAccessor(_ names: [String]) -> DeclSyntax {
        let cases = names.map { "        case \"\($0)\": return _\($0)" }.joined(separator: "\n")
        return """
        public func exportedStorage(named name: String) -> (any ExportedStorage)? {
            switch name {
        \(raw: cases)
            default: return nil
            }
        }
        """
    }
}
