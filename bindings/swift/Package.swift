// swift-tools-version: 6.0
//
// CyberdyneKit — the Swift package a game depends on. `swift-scripting`.
//
// FOUR TARGETS, AND THE SPLIT IS THE SPECIFICATION'S:
//
//   CyberdyneABI     a C target exposing the ABI header. Its contents are GENERATED — the header is
//                    copied out of src/abi/ by tools/gen/swift/overlay_gen.py so that this package
//                    is self-contained, and `just generate-swift --check` fails when the copy is
//                    stale. Its one C source compiles that header AS C on every build, which is how
//                    "the ABI consists only of C constructs" stays a build failure rather than a
//                    claim.
//   CyberdyneCore    the generated overlay: enums, the math types, the interface table, the handle
//                    wrappers. Nothing in it is hand-written; every file carries the banner saying
//                    so. design.md §2 gives the reason, and `core-type-system` gave it first: a
//                    declaration that can drift from the thing it describes will.
//   CyberdyneKit     the hand-written ergonomic layer. Behaviours, exports, serialization, the
//                    module entry, systems, concurrency.
//   CyberdyneMacros  the compiler plugin behind @Behaviour, @Component, @System and @GameModule.
//
// SWIFT 6 LANGUAGE MODE IS DELIBERATE, NOT A DEFAULT WE INHERITED. `swift-scripting` requires that
// "Swift code [that] mutates engine state from a detached task without game-actor isolation" be a
// COMPILE-TIME error. That is strict concurrency checking, and it only holds in Swift 6 mode. It is
// also what forces every piece of global state in this package to justify itself in writing — see
// Runtime.swift, which is the only place that answer is "nonisolated(unsafe)", with the ABI's own
// quiescing rule as the argument.
//
// THE ONE EXTERNAL DEPENDENCY, AND WHY IT IS ACCEPTABLE. swift-syntax is what a macro
// implementation is written against; there is no other way to write one.
//
// It is pinned with `exact:` rather than `from:`, and that is load-bearing rather than cautious:
// `.gitignore` excludes `Package.resolved` repository-wide, so there is no lock file to fall back
// on and the manifest is the only thing that fixes the version. A resolution that could drift would
// build the macro plugin against a different syntax tree than the compiler ships, which fails in
// ways that look like the macros being wrong.
//
// It is a *build-time* dependency. Nothing from swift-syntax is linked into a game module, and
// nothing in the engine ever sees it — `integration.swift_no_runtime` is the check that says so
// about the engine side.

import CompilerPluginSupport
import PackageDescription

let package = Package(
    name: "CyberdyneKit",
    products: [
        .library(name: "CyberdyneKit", targets: ["CyberdyneKit"]),
    ],
    dependencies: [
        .package(url: "https://github.com/swiftlang/swift-syntax.git", exact: "603.0.2"),
    ],
    targets: [
        .target(name: "CyberdyneABI"),
        .target(name: "CyberdyneCore", dependencies: ["CyberdyneABI"]),
        .macro(name: "CyberdyneMacros", dependencies: [
            .product(name: "SwiftSyntaxMacros", package: "swift-syntax"),
            .product(name: "SwiftCompilerPlugin", package: "swift-syntax"),
        ]),
        .target(name: "CyberdyneKit", dependencies: ["CyberdyneCore", "CyberdyneMacros"]),
        .testTarget(name: "CyberdyneCoreTests", dependencies: ["CyberdyneCore"]),
        .testTarget(name: "CyberdyneKitTests", dependencies: ["CyberdyneKit"]),
        .testTarget(name: "CyberdyneMacrosTests", dependencies: [
            "CyberdyneMacros",
            .product(name: "SwiftSyntaxMacroExpansion", package: "swift-syntax"),
            .product(name: "SwiftSyntaxMacrosTestSupport", package: "swift-syntax"),
        ]),
    ]
)
