// ComponentMacro.swift — `@Component`. Task 3.4.
//
// `swift-scripting`, "Invalid component is rejected at compile time": a struct marked `@Component`
// containing a Swift class reference SHALL be a compile error "explaining that components must be
// trivially relocatable value types".
//
// The check is over the WRITTEN type name, because a macro sees syntax and never a resolved type.
// That is enough for the case the scenario names — nobody writes a class reference in a component
// by accident under a name this list happens to contain — and the type system catches what is left:
// a field whose type is not one of these cannot satisfy `componentFields`' offsets, so the
// generated code fails to compile rather than registering a component the engine cannot store.
//
// The OFFSETS are not computed here. They are emitted as `MemoryLayout<Self>.offset(of:)`, so the
// compiler that lays the struct out is the one that reports where each field is. A macro that
// computed offsets would be a third layout model, beside the C ABI's and the compiler's.

import SwiftSyntax
import SwiftSyntaxMacros

public struct ComponentMacro: MemberMacro, ExtensionMacro {
    /// The types the engine can store in chunk memory, and the `CyVarType` each maps to. Every one
    /// is trivially relocatable and has a fixed width; that is the whole membership rule.
    static let storable: [String: String] = [
        "Bool": ".bool", "Int64": ".i64", "Float": ".f32", "Double": ".f64",
        "Vec2": ".vec2", "Vec3": ".vec3", "Vec4": ".vec4", "Quat": ".quat", "Entity": ".entity",
    ]

    public static func expansion(of node: AttributeSyntax,
                                 providingMembersOf declaration: some DeclGroupSyntax,
                                 conformingTo protocols: [TypeSyntax],
                                 in context: some MacroExpansionContext) throws -> [DeclSyntax] {
        guard let structDecl = declaration.as(StructDeclSyntax.self) else {
            context.fail(node, .componentNeedsStruct)
            return []
        }
        let name = stringLiteral(arguments(of: node)["name"]) ?? structDecl.name.text
        let fields = storedFields(of: structDecl, in: context)
        let entries = fields.map { field in
            "        FieldDescriptor(name: \"\(field.name)\", type: \(field.kind), "
                + "offset: MemoryLayout<Self>.offset(of: \\Self.\(field.name)) ?? 0, "
                + "size: MemoryLayout<\(field.type)>.size),"
        }.joined(separator: "\n")

        return [
            "public static let componentName: String = \"\(raw: name)\"",
            """
            public static let componentFields: [FieldDescriptor] = [
            \(raw: entries)
            ]
            """,
        ]
    }

    public static func expansion(of node: AttributeSyntax,
                                 attachedTo declaration: some DeclGroupSyntax,
                                 providingExtensionsOf type: some TypeSyntaxProtocol,
                                 conformingTo protocols: [TypeSyntax],
                                 in context: some MacroExpansionContext) throws
        -> [ExtensionDeclSyntax] {
        guard declaration.is(StructDeclSyntax.self), !protocols.isEmpty else { return [] }
        return [try ExtensionDeclSyntax("extension \(type.trimmed): Component {}")]
    }

    struct Field {
        let name: String
        let type: String
        let kind: String
    }

    /// The stored properties, checked. A computed property is skipped — it has no storage and so no
    /// offset — and an unstorable one is diagnosed at its own declaration rather than at the
    /// attribute, so the error points at the field.
    static func storedFields(of declaration: StructDeclSyntax,
                             in context: some MacroExpansionContext) -> [Field] {
        var fields: [Field] = []
        for member in declaration.memberBlock.members {
            guard let variable = member.decl.as(VariableDeclSyntax.self),
                  !variable.modifiers.contains(where: { $0.name.tokenKind == .keyword(.static) })
            else { continue }
            for binding in variable.bindings {
                guard binding.accessorBlock == nil,
                      let identifier = binding.pattern.as(IdentifierPatternSyntax.self)
                else { continue }
                guard let written = binding.typeAnnotation?.type.trimmedDescription else {
                    context.fail(binding, .componentFieldNeedsType)
                    continue
                }
                guard let kind = storable[written] else {
                    context.fail(binding, .componentFieldNotStorable)
                    continue
                }
                fields.append(Field(name: identifier.identifier.text, type: written, kind: kind))
            }
        }
        return fields
    }
}
