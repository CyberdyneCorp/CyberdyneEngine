//! What an agent can see: the viewport, what is under a point, and where things are.
//!
//! `editor-agent-interface`, "The agent sees what the human sees":
//!
//! > The agent interface SHALL be able to return a **rendered image of a viewport**, produced by the
//! > engine's own renderer through the same path that produces the human's viewport. There SHALL be
//! > no separate agent rendering path and no simplified representation substituted for the image ...
//! > A request SHALL be able to state its camera, its resolution, and whether overlays, gizmos and
//! > selection outlines are included. An image intended to represent the shipping frame SHALL exclude
//! > them, and **every returned image SHALL state which it was**.
//!
//! # Why this module produces requests rather than images
//!
//! A viewport image comes from the runtime, over the transport
//! `cy_editor_viewport::transport` already carries the human's frames on. So an agent's observation
//! is a [`ViewportRequest`] the runtime answers with a `PresentedFrame`, and the agent's image is
//! literally one of the human's — the same `FrameImage`, the same `ViewState`, the same `FrameId`.
//! There is no second path here because there is no rendering here.
//!
//! That is what makes "no separate agent rendering path" structural rather than a promise: this
//! crate depends on `cy-editor-viewport` and on nothing that could draw, and a contributor who
//! wanted to answer an agent's request with a substituted representation would have to add a
//! dependency that `cy-editor-app`'s layering and safety tests would then have to be argued with.
//!
//! # The honesty flag
//!
//! [`Observation::includes_overlays`] is not a copy of what was asked for; it is what the answer
//! actually is. A runtime that could not exclude gizmos returns `true` and the agent knows the image
//! is not the shipping frame. An interface that echoed the request would let an agent evaluate a
//! lighting change against an image with a selection outline in it and never find out.

use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::FrameId;
use cy_editor_viewport::math::{Bounds, Ray, Vec3};
use cy_editor_viewport::picking::{PickIntent, PickRequest};
use cy_editor_viewport::state::{CameraPose, Projection, ViewportRect};
use cy_editor_viewport::transport::FrameImage;
use cy_editor_viewport::viewmode::ViewMode;
use cy_editor_viewport::viewport::{Viewport, ViewportId};

/// What an agent asks to see.
#[derive(Clone, PartialEq, Debug)]
pub struct ViewportRequest {
    /// Which viewport. An agent looks through one of the editor's, rather than conjuring its own,
    /// so that what it evaluates is what a person would see.
    pub viewport: ViewportId,
    /// The camera to render from. `None` takes the viewport's own, which is what "show me what the
    /// user is looking at" means.
    pub camera: Option<CameraPose>,
    /// The projection. `None` takes the viewport's own.
    pub projection: Option<Projection>,
    /// The resolution asked for. Bounded by the budget, and a runtime may return less — which is
    /// the degradation `editor-viewport-and-gizmos` already specifies rather than a failure.
    pub resolution: ViewportRect,
    /// Whether gizmos, selection outlines and overlays are wanted.
    ///
    /// False is what "represent the shipping frame" means, and it is the default because an agent
    /// evaluating its own edit almost always wants the frame without the editor drawn on top of it.
    pub include_overlays: bool,
    /// Which debug view to render. `Off` is the shipping image; anything else answers "why is this
    /// dark" from a buffer rather than from a colour image, which the requirement asks for
    /// explicitly.
    pub view_mode: ViewMode,
}

impl ViewportRequest {
    /// A request for the shipping frame of a viewport, at its own camera and size.
    #[must_use]
    pub fn shipping_frame(viewport: &Viewport) -> Self {
        Self {
            viewport: viewport.id,
            camera: None,
            projection: None,
            resolution: viewport.state.viewport,
            include_overlays: false,
            view_mode: ViewMode::Off,
        }
    }

    /// A request for a debug visualisation.
    #[must_use]
    pub fn debug_view(viewport: &Viewport, mode: ViewMode) -> Self {
        let mut request = Self::shipping_frame(viewport);
        request.view_mode = mode;
        request
    }

    /// Bound the resolution, refusing a request that would cost more than the connection may spend.
    ///
    /// Refused rather than silently clamped: an agent that asked for 4096 pixels and got 512 would
    /// measure the wrong thing and never find out, whereas one that is told the ceiling asks again.
    pub fn within(&self, maximum_pixels: u64) -> Result<()> {
        let pixels = u64::from(self.resolution.width) * u64::from(self.resolution.height);
        if pixels > maximum_pixels {
            return Err(Problem::new(
                "render a viewport for this agent",
                format!(
                    "the request is {} by {} pixels, which is more than this connection may spend",
                    self.resolution.width, self.resolution.height
                ),
            )
            .with_remedy(format!(
                "ask for at most {maximum_pixels} pixels — a smaller image, or a crop"
            )));
        }
        Ok(())
    }
}

/// What kind of image came back.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ObservationKind {
    /// The shipping frame: no gizmos, no selection outlines, no overlays.
    ShippingFrame,
    /// The editor's own image, with whatever the editor draws on top of it.
    EditorFrame,
    /// A debug visualisation — depth, normals, a light complexity view.
    DebugView(ViewMode),
}

/// What an agent got back.
#[derive(Clone, PartialEq, Debug)]
pub struct Observation {
    /// The frame that answered. The SAME identifier the control path reconciles against, so an agent
    /// that made an edit and then observed can tell whether the image it is looking at is the one
    /// its edit reached.
    pub frame: FrameId,
    /// The image, exactly as the human's viewport receives it.
    pub image: FrameImage,
    /// What the image actually is — not what was asked for. See the module note.
    pub kind: ObservationKind,
    /// Whether the image contains the editor's own drawing.
    pub includes_overlays: bool,
    /// The size that came back, which may be smaller than the size asked for when the runtime
    /// degraded rather than stalled.
    pub resolution: ViewportRect,
}

impl Observation {
    /// Whether this image can be used to judge what the project will look like.
    ///
    /// The one question an agent should ask before evaluating a lighting change, and the reason the
    /// honesty flag exists at all.
    #[must_use]
    pub const fn represents_the_shipping_frame(&self) -> bool {
        matches!(self.kind, ObservationKind::ShippingFrame) && !self.includes_overlays
    }
}

/// A spatial question the editor can answer without rendering.
///
/// `editor-agent-interface`: "An agent SHALL be able to query spatial relationships the editor itself
/// can answer: bounds, distance, containment, what a ray intersects, and what occupies a volume."
#[derive(Clone, PartialEq, Debug)]
pub enum SpatialQuery {
    /// The bounds of what is selected, or of a named set.
    Bounds,
    /// What a ray hits. The ray is in world space; a ray through a pixel comes from a pick.
    Ray(Ray),
    /// What lies inside a volume.
    Volume(Bounds),
    /// How far apart two points are. Trivial, and here because an agent that has to do arithmetic
    /// on coordinates it read as text will eventually do it in the wrong space.
    Distance(Vec3, Vec3),
}

/// What a spatial query answered.
#[derive(Clone, PartialEq, Debug)]
pub enum SpatialResult {
    /// A bounding volume.
    Bounds(Bounds),
    /// Stable identities, in the editor's own terms — never indices into anything, so the answer
    /// survives streaming and reload as the requirement demands.
    Identities(Vec<u64>),
    /// A scalar.
    Distance(f32),
}

/// A pick an agent asks for, against the frame it is looking at.
///
/// It is `cy_editor_viewport::PickRequest`: the agent's pick IS the human's pick, resolved by the
/// engine against the frame's own draw list, which is what makes "the agent picks what was drawn"
/// true rather than intended. This function exists so that an agent cannot form a pick against a
/// frame it never saw.
pub fn pick_from(
    observation: &Observation,
    viewport: ViewportId,
    intent: PickIntent,
) -> PickRequest {
    PickRequest::for_frame(viewport, observation.frame, intent)
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_viewport::viewport::Viewport;

    fn viewport() -> Viewport {
        Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            cy_editor_viewport::transport::TransportKind::LocalSurface,
        )
    }

    #[test]
    fn a_shipping_frame_request_asks_for_no_overlays() {
        let request = ViewportRequest::shipping_frame(&viewport());
        assert!(!request.include_overlays);
        assert_eq!(request.view_mode, ViewMode::Off);
    }

    #[test]
    fn an_image_states_what_it_is_rather_than_what_was_asked_for() {
        // The failure this prevents: an agent evaluates a lighting change against an image with a
        // selection outline in it and never finds out.
        let honest = Observation {
            frame: FrameId::from_raw(7),
            image: FrameImage::Surface(1),
            kind: ObservationKind::ShippingFrame,
            includes_overlays: false,
            resolution: ViewportRect::default(),
        };
        assert!(honest.represents_the_shipping_frame());

        let mut could_not_exclude = honest.clone();
        could_not_exclude.includes_overlays = true;
        assert!(!could_not_exclude.represents_the_shipping_frame());

        let debug = Observation {
            kind: ObservationKind::DebugView(ViewMode::Normals),
            ..honest
        };
        assert!(!debug.represents_the_shipping_frame());
    }

    #[test]
    fn a_render_larger_than_the_budget_is_refused_with_the_ceiling() {
        let mut request = ViewportRequest::shipping_frame(&viewport());
        request.resolution = ViewportRect {
            x: 0,
            y: 0,
            width: 4096,
            height: 4096,
        };
        let refused = request.within(1_920 * 1_080).unwrap_err();
        // Refused rather than clamped: an agent that asked for one size and got another would
        // measure the wrong thing.
        assert!(refused.remedy.unwrap().contains("at most"));
        request.resolution.width = 640;
        request.resolution.height = 480;
        assert!(request.within(1_920 * 1_080).is_ok());
    }

    #[test]
    fn a_pick_can_only_be_formed_against_a_frame_that_was_observed() {
        let observation = Observation {
            frame: FrameId::from_raw(11),
            image: FrameImage::Surface(1),
            kind: ObservationKind::EditorFrame,
            includes_overlays: true,
            resolution: ViewportRect::default(),
        };
        let request = pick_from(
            &observation,
            ViewportId::from_raw(1),
            PickIntent::Click { x: 10.0, y: 20.0 },
        );
        // The same frame the agent looked at, which is what makes the engine resolve the pick
        // against the view state that produced the image rather than the camera's current one.
        assert_eq!(request.frame, observation.frame);
    }
}
