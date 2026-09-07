#ifndef CY_BUILD_CONTENT_PRODUCERS_H
#define CY_BUILD_CONTENT_PRODUCERS_H
// The real cooks, as nodes in the derivation graph. M7 task 1.4.
//
// --- WHAT M6 LEFT, AND WHY IT HAD A DEADLINE ----------------------------------------------------
//
// M6's closing gate: "**The real cooks are not nodes in the build graph.** `cy_cook` and
// `cy_import_cli` run outside it; the graph's producers are its own four builtins." Those four —
// `copy`, `normalise`, `concat`, `manifest` — are deliberately small, because the graph was the
// subject. The consequence is that everything `build-and-packaging` guarantees about a node
// (a derivation key over declared inputs, an immutable content-addressed artefact, precise
// invalidation, an undeclared read reported as a defect) applied to nothing a project actually
// runs.
//
// M7 cooks materials, virtual geometry and shadow data. All three are expensive, and design.md §3
// requires the virtual-geometry cook to land "as a node in the build graph rather than beside it".
// The two cooks that exist today go in first, so that the pattern the expensive ones follow is one
// that has been run.
//
// --- WHY THIS IS A SEPARATE TARGET FROM `cy::build-graph` ---------------------------------------
//
// `cy_build_graph` is the graph, the key, the store and the service, and it links `cy::core-assets`
// and `cy::core-memory` and nothing else. The importer links ufbx and xatlas; the cook links the
// ECS, the scene serializer and the reflection registry. Folding either into the graph library
// would make every consumer of a derivation key pay for a glTF parser. `cy_build_content` is the
// join, at the same layer, and `cy_build` links it — so the shipped tool has these producers and
// the graph's own tests do not need a mesh.
//
// --- WHAT MAKES THESE HONEST NODES RATHER THAN A SHELL-OUT --------------------------------------
//
// Neither producer touches a filesystem. A `NodeContext` is the only route to the world, so:
//
//   * the import producer reads its source through `context.read` (declared, in the key) and every
//     input the importer discovers — a glTF's `.bin`, a referenced texture — through
//     `context.discover`, which files the dependency in the same act as reading it;
//   * the cook producer reads every declared document through `context.read` into a `MemoryMount`
//     and hands the cook a virtual filesystem over that mount, so a document the node did not
//     declare does not exist as far as the cook is concerned.
//
// An undeclared read is therefore not a rule somebody remembers: it is a name that cannot be
// resolved. That is the same argument `ImportResolver` and `NodeContext` each make on their own,
// joined up.

#include <cy/build/producer.h>

namespace cy::ecs {
class World;
}

namespace cy::build {

/// Register `import` and `cook`. `cy_build` calls this after `add_builtins()`.
///
/// `world` is the component registry the cook emits blocks against, and may be null — in which case
/// the `cook` producer registers and fails by name if a node asks for it, rather than silently not
/// existing. `tools/cook/`'s own front end is explicit that a cook needs a world "that has
/// registered them ... so the caller supplies one", and a producer that guessed would produce a
/// package the runtime rejects at the build-schema check.
[[nodiscard]] Status add_content_producers(ProducerRegistry& registry,
                                           const ecs::World* world) noexcept;

/// The version every `import` node carries in its key. Moved when the producer's own behaviour
/// changes — the bundle framing, which options reach the importer — as distinct from an importer's
/// version, which covers only that importer's output.
inline constexpr u32 kImportProducerVersion = 1;
inline constexpr u32 kCookProducerVersion = 1;

}  // namespace cy::build

#endif  // CY_BUILD_CONTENT_PRODUCERS_H
