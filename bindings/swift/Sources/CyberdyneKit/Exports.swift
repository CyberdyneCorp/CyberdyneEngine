// Exports.swift — one import for a game. Task 3.2.
//
// `swift-scripting`: "Swift game code SHALL depend on a single Swift package, **`CyberdyneKit`**".
// A game that had to write three imports — the C target for `CyInterface`, the generated overlay
// for `Vec3`, and this one for `Behaviour` — would be depending on three things and calling it one.
//
// `@_exported` is underscored, and it is the right tool anyway: the alternative is to re-declare
// every generated type here as a typealias, which is a hand-written copy of a generated surface and
// is precisely the drift this package is built to avoid.
@_exported import CyberdyneABI
@_exported import CyberdyneCore
