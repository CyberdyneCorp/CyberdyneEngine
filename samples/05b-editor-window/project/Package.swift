// swift-tools-version: 6.0
// SPDX-License-Identifier: MIT
// SourceKit-LSP project for the editable game/ sources. Runtime generations are built separately.
import PackageDescription

let package = Package(
    name: "EditorDemoSources",
    platforms: [.macOS(.v14)],
    dependencies: [.package(path: "../../../bindings/swift")],
    targets: [.target(
        name: "EditorDemoSources",
        dependencies: [.product(name: "CyberdyneKit", package: "swift")],
        path: "game"
    )]
)
