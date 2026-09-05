// MacroExpansionTests.swift — what each macro emits, and what it refuses. Task 3.4.
//
// `swift-scripting` requires compile-time diagnostics for invalid usage, and names three cases: a
// non-representable exported type, a component with a reference type, and a system with conflicting
// access. A diagnostic that has never been seen to fire is a diagnostic nobody knows works — the
// same argument design.md §1 makes for the ABI gate — so every one of them is asserted here against
// the source that should produce it.

import SwiftSyntaxMacroExpansion
import SwiftSyntaxMacros
import SwiftSyntaxMacrosTestSupport
import XCTest

@testable import CyberdyneMacros

// `MacroSpec` rather than a bare type, because two of these are ExtensionMacros: the conformance a
// macro adds comes from its DECLARATION (`@attached(extension, conformances: ...)`), which lives in
// CyberdyneKit and which this target does not import. Naming the conformances here is how the
// harness reproduces what the compiler would pass — and it is why an extension shows up in the
// expected source below at all.
private let macros: [String: MacroSpec] = [
    "Behaviour": MacroSpec(type: BehaviourMacro.self, conformances: ["BehaviourClass"]),
    "Component": MacroSpec(type: ComponentMacro.self, conformances: ["Component"]),
    "System": MacroSpec(type: SystemMacro.self),
    "GameModule": MacroSpec(type: GameModuleMacro.self),
]

final class BehaviourMacroTests: XCTestCase {
    func testItNamesTheClassItsExportsAndItsCallbacks() {
        assertMacroExpansion(
            """
            @Behaviour
            final class Player: Behaviour {
                @Export var speed: Float = 6.0
                override func onFixedUpdate(_ dt: Double) {}
            }
            """,
            expandedSource: """
            final class Player: Behaviour {
                @Export var speed: Float = 6.0
                override func onFixedUpdate(_ dt: Double) {}

                public static let behaviourName: String = "Player"

                public static let behaviourSchema: UInt32 = 1

                public static let behaviourCallbacks: CallbackSet = [.fixedUpdate]

                public static let exportedNames: [String] = ["speed"]

                public func exportedStorage(named name: String) -> (any ExportedStorage)? {
                    switch name {
                        case "speed":
                        return _speed
                    default:
                        return nil
                    }
                }
            }

            extension Player: BehaviourClass {
            }
            """,
            macroSpecs: macros)
    }

    /// The callback set is what makes "Unimplemented callback costs nothing" true. A class with no
    /// lifecycle functions declares an empty set, and the engine never puts it in a dispatch list.
    func testAClassWithNoCallbacksDeclaresAnEmptySet() {
        assertMacroExpansion(
            """
            @Behaviour(name: "Renamed", schema: 4)
            final class Silent: Behaviour {
            }
            """,
            expandedSource: """
            final class Silent: Behaviour {

                public static let behaviourName: String = "Renamed"

                public static let behaviourSchema: UInt32 = 4

                public static let behaviourCallbacks: CallbackSet = []

                public static let exportedNames: [String] = []

                public func exportedStorage(named name: String) -> (any ExportedStorage)? {
                    switch name {

                    default:
                        return nil
                    }
                }
            }

            extension Silent: BehaviourClass {
            }
            """,
            macroSpecs: macros)
    }

    func testItRefusesAStruct() {
        assertMacroExpansion(
            """
            @Behaviour
            struct Player {}
            """,
            expandedSource: """
            struct Player {}
            """,
            diagnostics: [
                DiagnosticSpec(message: MacroDiagnostic.behaviourNeedsClass.message, line: 1,
                               column: 1),
            ],
            macroSpecs: macros)
    }

    func testItRefusesAnExportedLet() {
        assertMacroExpansion(
            """
            @Behaviour
            final class Player: Behaviour {
                @Export let speed: Float = 6.0
            }
            """,
            expandedSource: """
            final class Player: Behaviour {
                @Export let speed: Float = 6.0

                public static let behaviourName: String = "Player"

                public static let behaviourSchema: UInt32 = 1

                public static let behaviourCallbacks: CallbackSet = []

                public static let exportedNames: [String] = []

                public func exportedStorage(named name: String) -> (any ExportedStorage)? {
                    switch name {

                    default:
                        return nil
                    }
                }
            }

            extension Player: BehaviourClass {
            }
            """,
            diagnostics: [
                DiagnosticSpec(message: MacroDiagnostic.behaviourExportMustBeVar.message, line: 3,
                               column: 5),
            ],
            macroSpecs: macros)
    }
}

final class ComponentMacroTests: XCTestCase {
    func testItDerivesTheFieldTableFromTheStoredProperties() {
        assertMacroExpansion(
            """
            @Component
            struct Velocity {
                var linear: Vec3 = .zero
                var speed: Float = 0
            }
            """,
            expandedSource: """
            struct Velocity {
                var linear: Vec3 = .zero
                var speed: Float = 0

                public static let componentName: String = "Velocity"

                public static let componentFields: [FieldDescriptor] = [
                        FieldDescriptor(name: "linear", type: .vec3, offset: MemoryLayout<Self>.offset(of: \\Self.linear) ?? 0, size: MemoryLayout<Vec3>.size),
                        FieldDescriptor(name: "speed", type: .f32, offset: MemoryLayout<Self>.offset(of: \\Self.speed) ?? 0, size: MemoryLayout<Float>.size),
                ]
            }

            extension Velocity: Component {
            }
            """,
            macroSpecs: macros)
    }

    /// `swift-scripting`, "Invalid component is rejected at compile time": a struct marked
    /// `@Component` containing a Swift class reference is a compile error explaining that components
    /// must be trivially relocatable value types.
    func testItRefusesAReferenceTypeField() {
        assertMacroExpansion(
            """
            @Component
            struct Broken {
                var owner: SomeClass = SomeClass()
            }
            """,
            expandedSource: """
            struct Broken {
                var owner: SomeClass = SomeClass()

                public static let componentName: String = "Broken"

                public static let componentFields: [FieldDescriptor] = [

                ]
            }

            extension Broken: Component {
            }
            """,
            diagnostics: [
                DiagnosticSpec(message: MacroDiagnostic.componentFieldNotStorable.message, line: 3,
                               column: 9),
            ],
            macroSpecs: macros)
    }

    func testItRefusesAClass() {
        assertMacroExpansion(
            """
            @Component
            final class Broken {}
            """,
            expandedSource: """
            final class Broken {}
            """,
            diagnostics: [
                DiagnosticSpec(message: MacroDiagnostic.componentNeedsStruct.message, line: 1,
                               column: 1),
            ],
            macroSpecs: macros)
    }
}

final class SystemMacroTests: XCTestCase {
    func testItRegistersTheSignatureItWasWritten() {
        assertMacroExpansion(
            """
            @System(stage: .simulation)
            func applyGravity(_ query: Query<Write<Velocity>, Read<Mass>>, _ chunks: ChunkSource) {}
            """,
            expandedSource: """
            func applyGravity(_ query: Query<Write<Velocity>, Read<Mass>>, _ chunks: ChunkSource) {}

            public enum __CySystem_applyGravity {
                public static let descriptor = SystemDescriptor(
                    name: "applyGravity", stage: .simulation, access: Query<Write<Velocity>, Read<Mass>>.access)

                /// Register this system. A game calls it from `GameModule.initialize(at:)`.
                public static func register() throws {
                    try Systems.register(descriptor) { chunks in
                        applyGravity(Query<Write<Velocity>, Read<Mass>>(), chunks)
                    }
                }
            }
            """,
            macroSpecs: macros)
    }

    func testItRefusesAQueryThatReadsAndWritesTheSameComponent() {
        assertMacroExpansion(
            """
            @System(stage: .simulation)
            func bad(_ query: Query<Write<Health>, Read<Health>>, _ chunks: ChunkSource) {}
            """,
            expandedSource: """
            func bad(_ query: Query<Write<Health>, Read<Health>>, _ chunks: ChunkSource) {}
            """,
            diagnostics: [
                DiagnosticSpec(message: MacroDiagnostic.systemConflictingAccess.message, line: 2,
                               column: 19),
            ],
            macroSpecs: macros)
    }

    func testItRefusesAFunctionWhoseFirstParameterIsNotAQuery() {
        assertMacroExpansion(
            """
            @System(stage: .simulation)
            func bad(_ chunks: ChunkSource) {}
            """,
            expandedSource: """
            func bad(_ chunks: ChunkSource) {}
            """,
            diagnostics: [
                DiagnosticSpec(message: MacroDiagnostic.systemNeedsQuery.message, line: 2,
                               column: 9),
            ],
            macroSpecs: macros)
    }

    /// ABI 1.0 has no resource entry, so the specification's `time: Res<Time>` parameter is
    /// diagnosed rather than compiled into a call that cannot be made.
    func testItRefusesAResourceParameterAtThisABIVersion() {
        assertMacroExpansion(
            """
            @System(stage: .simulation)
            func bad(_ query: Query<Read<Mass>>, time: Res<Time>) {}
            """,
            expandedSource: """
            func bad(_ query: Query<Read<Mass>>, time: Res<Time>) {}
            """,
            diagnostics: [
                DiagnosticSpec(message: MacroDiagnostic.systemUnsupportedParameter.message, line: 2,
                               column: 38),
            ],
            macroSpecs: macros)
    }
}

final class GameModuleMacroTests: XCTestCase {
    func testItEmitsTheTwoExportedEntryPoints() {
        assertMacroExpansion(
            """
            @GameModule
            enum Game: GameModule {
                static let behaviours: [any BehaviourClass.Type] = [Player.self]
            }
            """,
            expandedSource: """
            enum Game: GameModule {
                static let behaviours: [any BehaviourClass.Type] = [Player.self]
            }

            @_cdecl("cy_module_entry")
            public func cy_module_entry(_ interface: UnsafePointer<CyInterface>?,
                                        _ engine: CyEngine?,
                                        _ out: UnsafeMutablePointer<CyModuleInit>?) -> Bool {
                ModuleBootstrap.entry(interface, engine, out, module: Game.self)
            }

            @_cdecl("cy_module_shutdown")
            public func cy_module_shutdown() {
                ModuleBootstrap.shutdown()
            }
            """,
            macroSpecs: macros)
    }
}
