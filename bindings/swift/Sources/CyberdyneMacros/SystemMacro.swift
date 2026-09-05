// SystemMacro.swift — `@System(stage:)`. Task 3.4.
//
// `swift-scripting`, "Access derived from the signature": a system taking `Query<Write<Health>>`
// SHALL register `Write` access for `Health` "without a separate declaration". So this macro reads
// the query type out of the parameter list and emits `Query<...>.access` — the SAME type, not a
// transcription of it. There is nothing here that can disagree with the signature, because the
// emitted code names the signature's own type.
//
// The macro also rejects a query that declares both `Read` and `Write` for one component, which is
// the "systems with conflicting access" diagnostic. That one IS a transcription — it reads the
// generic argument list textually — and it is a diagnostic rather than the registration, so a
// mistake in it can only ever fail to warn, never mis-register. `Systems.register` re-checks the
// same property at run time over the real `AccessSet`, which is the check that matters.

import SwiftSyntax
import SwiftSyntaxMacros

public struct SystemMacro: PeerMacro {
    public static func expansion(of node: AttributeSyntax,
                                 providingPeersOf declaration: some DeclSyntaxProtocol,
                                 in context: some MacroExpansionContext) throws -> [DeclSyntax] {
        guard let function = declaration.as(FunctionDeclSyntax.self) else {
            context.fail(node, .systemNeedsFunction)
            return []
        }
        let parameters = Array(function.signature.parameterClause.parameters)
        guard let first = parameters.first,
              let query = queryType(first.type) else {
            context.fail(function.signature.parameterClause, .systemNeedsQuery)
            return []
        }
        for parameter in parameters.dropFirst()
        where parameter.type.trimmedDescription != "ChunkSource"
            && parameter.type.trimmedDescription != "any ChunkSource" {
            context.fail(parameter, .systemUnsupportedParameter)
            return []
        }
        if let conflict = conflictingComponent(in: query) {
            context.fail(first.type, .systemConflictingAccess)
            _ = conflict
            return []
        }

        let stage = arguments(of: node)["stage"]?.trimmedDescription ?? ".simulation"
        let name = function.name.text
        let call = parameters.count > 1 ? "\(name)(\(query)(), chunks)" : "\(name)(\(query)())"

        return ["""
        public enum __CySystem_\(raw: name) {
            public static let descriptor = SystemDescriptor(
                name: "\(raw: name)", stage: \(raw: stage), access: \(raw: query).access)

            /// Register this system. A game calls it from `GameModule.initialize(at:)`.
            public static func register() throws {
                try Systems.register(descriptor) { chunks in
                    \(raw: call)
                }
            }
        }
        """]
    }

    /// `Query<Write<Velocity>, Read<Mass>>` as written, or nil for any other parameter type.
    static func queryType(_ type: TypeSyntax) -> String? {
        guard let identifier = type.as(IdentifierTypeSyntax.self),
              identifier.name.text == "Query" else { return nil }
        return identifier.trimmedDescription
    }

    /// A component named by both a `Read` and a `Write` term of the same query.
    static func conflictingComponent(in query: String) -> String? {
        let reads = terms(named: "Read", in: query)
        let writes = terms(named: "Write", in: query)
        return reads.intersection(writes).sorted().first
    }

    /// The component names inside `Term<...>` occurrences of one term kind.
    ///
    /// A scan over the written text rather than a parse of the generic argument list. Being wrong
    /// here loses a WARNING and nothing else — the registration names the signature's own type, and
    /// `Systems.register` re-checks the same property over the real `AccessSet` — which is why a
    /// scan is allowed at all. It is written without Foundation so that this plugin builds on a
    /// platform where Foundation is not part of the host toolchain.
    static func terms(named kind: String, in query: String) -> Set<String> {
        let needle = Array("\(kind)<")
        let characters = Array(query)
        var found: Set<String> = []
        var index = 0
        while index + needle.count <= characters.count {
            guard Array(characters[index ..< index + needle.count]) == needle else {
                index += 1
                continue
            }
            var end = index + needle.count
            while end < characters.count, characters[end] != ">" { end += 1 }
            found.insert(String(characters[(index + needle.count) ..< end])
                .filter { !$0.isWhitespace })
            index = end
        }
        return found
    }
}
