#pragma once
// The shipped node catalogue's renderer half: its components, and its defaults. M8.b task 11.3.
//
// ================================================================================================
// WHY THIS IS A SECOND HEADER RATHER THAN FOUR LINES IN components.h
// ================================================================================================
//
// `components.h` is an ANNOTATED header: the reflection generator parses it with a real C++
// frontend, and `annotations.h` says what that costs — "a reflected header's cost is what it
// transitively includes: the M1 spike measured a ninefold increase in cold generation from two
// standard-library includes per header". This file names `cy::scene::SceneTree`, which pulls in the
// whole node façade, and none of that belongs on the generator's parse path.
//
// ================================================================================================
// THE DEFECT THIS CLOSES
// ================================================================================================
//
// `scene-graph-and-nodes` ships a catalogue of node types as DATA, and
// src/scene/src/node_template.cpp declares every one of them by component NAME. Two things follow
// that the scene layer cannot fix from where it sits:
//
//   * it cannot supply a template's `defaults` blob, because a blob is the component's own bytes
//     and layer 4's node façade may not include the renderer's header. A `MeshRenderer` node
//     created without one gets a ZEROED component — invisible, casting no shadow, empty bounds:
//     an object that exists and draws nothing, which is a quiet failure and the worst kind;
//   * a template binds its component names at registration, which for the catalogue is
//     `SceneTree::initialize()` — and every host builds the tree before it registers a renderer,
//     because the renderer extracts from a tree. So the renderer's templates were resolved against
//     a world that did not have the components yet, and stayed dead.
//
// `declare_render_templates()` closes both in one call, made where a host already calls
// `RenderComponents::register_all` and `declare_render_state`.

#include <cy/core/base/expected.h>
#include <cy/rendering/scene/components.h>
#include <cy/scene/tree.h>

namespace cy::rendering {

/// Redeclare the catalogue's five renderer templates with real defaults, and rebind the catalogue.
///
/// The five are `MeshRenderer`, `Camera`, `DirectionalLight`, `PointLight` and `SpotLight`.
/// `AreaLight` is deliberately not among them: `render::LightKind` is directional, point and spot,
/// so an area light is a node type this engine does not have yet and the catalogue goes on
/// reporting it as declared and not instantiable.
///
/// Refuses `InvalidArgument` when the renderer's components are not registered in `tree.world()`,
/// rather than binding the catalogue to nothing and reporting success.
///
/// Idempotent: redeclaring a template replaces its declaration, so a second call is a second bind
/// of the same data.
[[nodiscard]] Status declare_render_templates(scene::SceneTree& tree) noexcept;

}  // namespace cy::rendering
