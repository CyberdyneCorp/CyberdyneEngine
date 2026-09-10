#ifndef CY_CAMERA_AUTHORING_H
#define CY_CAMERA_AUTHORING_H
// CyberGraph as the camera's authoring front end. M8.b task 7.3.
//
// --- WHY THIS BRIDGE EXISTS, AND WHAT IT IS NOT --------------------------------------------------
//
// M8.b's finding is that there is ONE authoring layer and one back end per consumer: "Build one
// front end, one expression core, and six back ends. Not one IR." CyberGraph (`cy::graph`) is the
// front end every consumer adopts, and `camera-system`'s back end — "compiled at cook time into a
// compact rig program", "no per-node allocation or virtual dispatch" — is `cy::camera::RigProgram`,
// which M4 built and which `src/servers/camera/` evaluates.
//
// This file is the join: an authored `cy::graph::Graph` becomes a `cy::camera::RigDefinition`,
// which `CameraServer::create_definition()` compiles. That is what makes a camera rig editable in
// the same canvas, diffable by the same diff and merged by the same three-way merge as an ability,
// a behaviour tree and a material — which is the promise `visual-scripting`'s "Shared
// infrastructure" requirement actually makes, and the only part of it a camera needs.
//
// IT IS NOT A SECOND EVALUATOR. Nothing here executes a graph; `resolve()` returns a definition and
// stops.
//
// --- THE NODE VOCABULARY IS `rig.*`, AND THE COLLISION IS DELIBERATE -----------------------------
//
// `cy::graph::camera::register_camera_nodes()` (M8.b task 2.4) registers `camera.constant`,
// `camera.add`, `camera.smooth_half_life`, `camera.lens`, `camera.output` and nine more: the
// EXPRESSION vocabulary of the shared SSA core, which is what a smoothing transfer function is
// written in. The rig vocabulary here — target, follow, orbit, offset, look-at, lens, noise,
// constraint, collision, output — is a different language at a different altitude, and two of its
// names (`lens`, `output`) would collide in one `NodeRegistry`.
//
// So these are `rig.target`, `rig.follow` and so on. Both vocabularies can be registered in one
// registry, and an author can tell from a node's name which language they are writing in. THAT
// THERE ARE TWO CAMERA COMPILERS IN THE TREE IS A REAL FINDING and it is written down in this
// module's README rather than resolved quietly here: the M4 rig compiler is what evaluates cameras
// today, the M8.b expression lowering is what the milestone's design names, and choosing between
// them is a decision with cooked data behind it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/graph/cybergraph.h>
#include <cy/servers/camera/rig.h>

namespace cy::camera {

/// Register the rig node vocabulary — `rig.target`, `rig.follow`, `rig.orbit`, `rig.offset`,
/// `rig.look_at`, `rig.lens`, `rig.noise`, `rig.constraint`, `rig.collision`, `rig.output` — with a
/// CyberGraph registry, so `validate()` can check a rig graph before it is compiled.
[[nodiscard]] Status register_rig_nodes(graph::NodeRegistry& registry) noexcept;

/// What a graph could not be turned into a definition for. Node-precise, because a diagnostic that
/// names a graph and not a node sends an author hunting through a canvas.
[[nodiscard]] Expected<RigDefinition, Error> rig_definition_from_graph(
    const graph::Graph& source, const graph::NodeRegistry& registry, Allocator& allocator,
    graph::DiagnosticSink& sink) noexcept;

}  // namespace cy::camera

#endif  // CY_CAMERA_AUTHORING_H
