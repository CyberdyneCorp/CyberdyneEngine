// Macros.swift — the declarative registration surface. Task 3.4.
//
// `swift-scripting` names five: `@Behaviour`, `@Component`, `@System(stage:)`, `@Export` and
// `@Node(path)`. Four of them are here; `@Export` and `@Node` are PROPERTY WRAPPERS in
// Export.swift, and that is a deliberate deviation with a reason.
//
// WHY `@Export` IS A WRAPPER AND NOT A MACRO. It needs storage and behaviour, not source rewriting:
// a value that lives in the instance, reads and writes like a plain `var`, and can be reached
// type-erased by the serializer. A macro cannot hold storage — it would have to synthesise a
// backing property and an accessor pair, which is what `@propertyWrapper` already is, written by
// the compiler and understood by every tool. The use site is identical either way
// (`@Export var speed: Float = 6.0`), and the registration half — which properties exist, under
// which names — is what `@Behaviour` reads out of the class body. So the split is: the wrapper
// carries the value, the macro carries the names.
//
// Every macro below emits COMPILE-TIME diagnostics for invalid use, which `swift-scripting`
// requires: a non-representable exported type, a component with a reference type, a system with
// conflicting access.

import CyberdyneCore

/// Registers a class as a behaviour type: its name, its schema, its exported properties, and which
/// lifecycle callbacks it implements.
///
/// The callback set is computed HERE, at compile time, from the functions the class declares. See
/// Behaviour.swift for why it cannot be computed at run time on this platform, and why asking the
/// Swift runtime would be wrong in exactly the two-resident-images configuration hot reload creates.
@attached(extension, conformances: BehaviourClass)
@attached(member, names: named(behaviourName), named(behaviourSchema), named(behaviourCallbacks),
          named(exportedNames), named(exportedStorage(named:)))
public macro Behaviour(name: String? = nil, schema: UInt32 = 1) =
    #externalMacro(module: "CyberdyneMacros", type: "BehaviourMacro")

/// Registers a struct as an ECS component with field reflection.
///
/// Rejects a stored property whose type is not one the engine can store in chunk memory — which
/// includes every class reference, because a component's bytes are moved by the ECS and a moved
/// refcount is a leak or a double free.
@attached(extension, conformances: Component)
@attached(member, names: named(componentName), named(componentFields), named(init()))
public macro Component(name: String? = nil) =
    #externalMacro(module: "CyberdyneMacros", type: "ComponentMacro")

/// Registers a function as a system, with its access derived from its signature.
///
/// The supported shape is `func name(_ query: Query<...>, _ chunks: ChunkSource)`. A resource
/// parameter — `time: Res<Time>` in the specification's example — is diagnosed rather than accepted,
/// because ABI 1.0's table has no resource entry and accepting it would compile into a call that
/// cannot be made.
// `prefixed(__CySystem_)` rather than `arbitrary`: a peer macro at global scope must say what it
// introduces, and what this one introduces is exactly the attached function's name behind a prefix.
// Saying so is also what lets a reader find the registration enum without expanding the macro.
@attached(peer, names: prefixed(__CySystem_))
public macro System(stage: SystemStage) =
    #externalMacro(module: "CyberdyneMacros", type: "SystemMacro")

/// Emits the two C entry points a game module exports.
///
/// Applied to a type conforming to `GameModule`. See Module.swift for why these cannot live in this
/// package: a linker drops an unreferenced object out of a static archive, and the loader would
/// then report a module that "did not export its declared entry symbol" with nothing pointing at
/// the cause.
// The two names are fixed by the ABI — `native-abi` names the entry symbol — so they are declared
// here rather than left arbitrary, and a game that also wrote one by hand gets a redeclaration
// error instead of a silently ignored duplicate.
@attached(peer, names: named(cy_module_entry), named(cy_module_shutdown))
public macro GameModule() = #externalMacro(module: "CyberdyneMacros", type: "GameModuleMacro")
