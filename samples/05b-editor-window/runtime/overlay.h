#pragma once
// The gizmo, drawn into the frame the engine publishes. M7 task 5b.3.
//
// ================================================================================================
// WHY THE RUNTIME DRAWS IT AND THE EDITOR DOES NOT
// ================================================================================================
//
// `editor-viewport-and-gizmos` assigns "gizmo geometry generation, depth behaviour, occlusion
// handling, and screen-constant sizing" to the engine, and names "editor-side picking that does not
// match what the engine rendered" as a forbidden pattern. The editor's own viewport panel says the
// same thing from its side: "Not drawn here: the transform gizmo … the gizmo appears when a runtime
// draws it into the frame it publishes."
//
// So the pixels a person aims at and the layout the editor hit-tests come from ONE computation:
// `cy::render::build_gizmo_layout` produces the handles, this file draws exactly those handles, and
// `encode_gizmo_layout` sends exactly those handles. A handle that is drawn and not published, or
// published and not drawn, is not expressible here — which is the property the division exists for.
//
// ================================================================================================
// IT IS A SOFTWARE RASTERISER, AND THAT IS A STATED COST
// ================================================================================================
//
// The frame already passes through host memory on its way to the shared image — see
// `cy/backends/viewport/publisher.h` for why, and for what it would take to remove that. Given that
// it does, compositing the overlay there costs one pass over a few thousand pixels and needs no
// pipeline, no shader and no second render pass.
//
// What that costs is stated rather than hidden: the overlay is not depth-tested against the scene,
// so a handle is drawn over geometry that is in front of it. `editor-viewport-and-gizmos` requires
// depth behaviour to be the engine's, and this is the engine deciding "always on top" — which is
// what a transform gizmo does in every editor — but it is a decision made by drawing order rather
// than by a depth test, and a gizmo pass in the render graph is where it belongs once there is one.

#include <cy/core/base/types.h>
#include <cy/servers/render/gizmo.h>

namespace cy::sample::editor_window {

/// The frame's pixels: 8-bit RGBA, row-major from the top-left, tightly packed.
///
/// The layout an image copy produces and the layout the publisher's staging buffer wants, so
/// nothing in this file flips anything.
struct Canvas {
    u8* pixels = nullptr;
    u32 width = 0;
    u32 height = 0;
};

/// Draw a gizmo layout into a frame.
///
/// Every handle in `layout` is drawn and nothing else is: there is no case here for a handle the
/// layout does not carry, which is what makes "what is drawn is what is published" structural.
void draw_gizmo(const Canvas& canvas, const render::GizmoLayout& layout,
                render::GizmoHandle hovered) noexcept;

/// Draw a small marker at the selected object's screen position, under the gizmo.
///
/// Not decoration: a gizmo with nothing under it is a gizmo a reader cannot tell is attached to
/// anything, and the exit criterion is a SCREENSHOT. One ring, in the interface's selection colour.
void draw_selection_marker(const Canvas& canvas, f32 x, f32 y, f32 radius) noexcept;

}  // namespace cy::sample::editor_window
