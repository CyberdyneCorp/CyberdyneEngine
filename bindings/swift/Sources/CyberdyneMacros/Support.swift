// Support.swift — what every macro in this target needs: diagnostics, and reading a declaration.
// Task 3.4.
//
// A macro sees SYNTAX and nothing else. It does not know what a name resolves to, whether a type is
// a class, or what another file declares. Every check in this target is therefore syntactic, and
// that is stated rather than glossed: `@Component`'s rejection of a reference type is a rejection
// of a type NAME that is not in the allowed list, which catches every class anyone will actually
// write and would not catch a typealias for one. The type system catches that instead — a
// component whose field is a class fails to satisfy `Component`'s requirements — so the two checks
// together are complete even though neither is alone.

import SwiftDiagnostics
import SwiftSyntax
import SwiftSyntaxMacros

/// The diagnostics this target emits. One enum so that the messages are all in one place and read
/// like each other; `swift-scripting` asks for compile-time diagnostics that EXPLAIN, so each of
/// these says what is wrong and what to do instead.
enum MacroDiagnostic: String, DiagnosticMessage {
    case behaviourNeedsClass
    case behaviourExportMustBeVar
    case componentNeedsStruct
    case componentFieldNotStorable
    case componentFieldNeedsType
    case systemNeedsFunction
    case systemNeedsQuery
    case systemUnsupportedParameter
    case systemConflictingAccess
    case gameModuleNeedsType

    var severity: DiagnosticSeverity { .error }
    var diagnosticID: MessageID { MessageID(domain: "CyberdyneMacros", id: rawValue) }

    var message: String {
        switch self {
        case .behaviourNeedsClass:
            return "@Behaviour applies to a class that inherits from Behaviour. A behaviour is an "
                + "object with identity and a lifecycle; for data, use @Component and @System."
        case .behaviourExportMustBeVar:
            return "@Export applies to a var. An exported property is written by the inspector and "
                + "restored by a hot reload, so it cannot be a let."
        case .componentNeedsStruct:
            return "@Component applies to a struct. Components must be trivially relocatable value "
                + "types: the ECS moves their bytes when an entity changes archetype, and moving a "
                + "class reference that way leaks it or frees it twice."
        case .componentFieldNotStorable:
            return "this field's type cannot be stored in a component. Components must be trivially "
                + "relocatable value types; the storable types are Bool, Int64, Float, Double, "
                + "Vec2, Vec3, Vec4, Quat and Entity."
        case .componentFieldNeedsType:
            return "a component's stored property needs an explicit type annotation. The macro "
                + "reads the written type to build the field table and cannot infer one."
        case .systemNeedsFunction:
            return "@System applies to a function."
        case .systemNeedsQuery:
            return "a system's first parameter must be a Query<...>. The query is the access "
                + "declaration: writing access down separately from the query lets the two drift, "
                + "and nothing catches the drift."
        case .systemUnsupportedParameter:
            return "a system parameter other than the leading Query and a ChunkSource is not "
                + "supported at ABI 1.0: the interface table has no resource entry, so a Res<...> "
                + "parameter would compile into a call that cannot be made."
        case .systemConflictingAccess:
            return "this query declares both Read and Write for the same component. The scheduler "
                + "would order the system against itself; declare Write alone."
        case .gameModuleNeedsType:
            return "@GameModule applies to a type conforming to GameModule."
        }
    }
}

extension MacroExpansionContext {
    func fail(_ node: some SyntaxProtocol, _ diagnostic: MacroDiagnostic) {
        diagnose(Diagnostic(node: Syntax(node), message: diagnostic))
    }
}

/// The labelled arguments of an attached macro's attribute, by label.
func arguments(of node: AttributeSyntax) -> [String: ExprSyntax] {
    guard let list = node.arguments?.as(LabeledExprListSyntax.self) else { return [:] }
    var found: [String: ExprSyntax] = [:]
    for element in list {
        guard let label = element.label?.text else { continue }
        found[label] = element.expression
    }
    return found
}

/// The text of a string-literal argument, or nil when it is absent or is not a literal.
func stringLiteral(_ expression: ExprSyntax?) -> String? {
    guard let literal = expression?.as(StringLiteralExprSyntax.self) else { return nil }
    return literal.segments.compactMap { segment in
        segment.as(StringSegmentSyntax.self)?.content.text
    }.joined()
}

/// True when a declaration carries an attribute of this name, written either bare or with
/// arguments.
func hasAttribute(_ named: String, on attributes: AttributeListSyntax) -> Bool {
    attributes.contains { element in
        guard case let .attribute(attribute) = element else { return false }
        return attribute.attributeName.trimmedDescription == named
            || attribute.attributeName.trimmedDescription.hasPrefix("\(named)(")
    }
}
