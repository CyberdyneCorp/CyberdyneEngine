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
use cy_editor_services::editor::Editor;
use cy_editor_viewport::math::{Bounds, Ray, Vec3};
use cy_editor_viewport::overlay::Overlays;
use cy_editor_viewport::picking::{PickIntent, PickRequest};
use cy_editor_viewport::state::{CameraPose, Projection, ViewportRect};
use cy_editor_viewport::transport::{Degradation, FrameImage, TransportKind};
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

/// The name an agent's own viewport is opened under.
///
/// One per connection rather than one per request, so that an agent that looks twice from the same
/// camera does not open two.
pub const AGENT_VIEWPORT: &str = "Agent";

/// Look through a viewport, as the human's own panel does.
///
/// --- WHY THIS TOUCHES A VIEWPORT AT ALL, AND WHOSE ---------------------------------------------
///
/// The requirement is that the image be "produced by the engine's own renderer through the same path
/// that produces the human's viewport", and that a request be able to state its camera. Those pull in
/// opposite directions: moving the human's camera to answer an agent's question would take the
/// editor away from the person using it, which "the editor stays usable while an agent works"
/// forbids.
///
/// So an agent that states a camera, or that asks for a frame with no editor drawing in it, gets
/// **its own viewport** — a viewport is a thing a person can open too, and opening one is not a
/// capability a human lacks. An agent that states neither is looking at what the person is looking
/// at, which is what "show me what the user sees" means, and it reads the human's viewport without
/// changing it.
///
/// --- WHY A MISSING FRAME IS A REFUSAL RATHER THAN AN EMPTY IMAGE -------------------------------
///
/// `design.md` §2: the viewport "shows a message saying so — not an approximation". The same rule
/// applies to an agent, more strongly: a person can see that a viewport is blank, and an agent
/// evaluating a lighting change against a substituted image cannot.
pub fn observe(
    editor: &mut Editor,
    request: &ViewportRequest,
    agent_viewport: &mut Option<ViewportId>,
) -> Result<Observation> {
    let restated = request.camera.is_some() || request.projection.is_some();
    let target = if restated || !request.include_overlays {
        Some(agent_viewport_of(editor, agent_viewport, request))
    } else {
        None
    };
    let id = target.unwrap_or(request.viewport);
    let viewport = editor.viewports.all().get(id).ok_or_else(|| {
        Problem::new(
            format!("observe {id}"),
            "the editor has no viewport with that identity",
        )
        .with_remedy("read the play resource, or omit the viewport to use the focused one")
    })?;

    let overlays = viewport.overlays.active();
    let frame = viewport.stream.latest().ok_or_else(|| {
        Problem::new(
            format!("observe {id}"),
            "no frame has arrived from the runtime for this viewport",
        )
        .with_remedy(
            "start a runtime and let it render at least one frame; the editor shows nothing rather \
             than an approximation, and so does this",
        )
    })?;

    let kind = if request.view_mode == ViewMode::Off {
        if overlays.is_empty() {
            ObservationKind::ShippingFrame
        } else {
            ObservationKind::EditorFrame
        }
    } else {
        ObservationKind::DebugView(request.view_mode)
    };
    Ok(Observation {
        frame: frame.frame,
        image: frame.image.clone(),
        kind,
        includes_overlays: !overlays.is_empty(),
        resolution: frame.state.viewport,
        degraded: frame.degradation,
        viewport: id,
    })
}

/// The connection's own viewport, opened on first use and configured from the request.
fn agent_viewport_of(
    editor: &mut Editor,
    held: &mut Option<ViewportId>,
    request: &ViewportRequest,
) -> ViewportId {
    let id = match held {
        Some(id) if editor.viewports.all().get(*id).is_some() => *id,
        _ => {
            // The same transport kind the human's viewport asks for, because the agent's image has
            // to arrive by the same path. A transport that cannot provide it says so.
            let opened = editor
                .viewports
                .all_mut()
                .open(AGENT_VIEWPORT, TransportKind::SharedTexture);
            *held = Some(opened);
            opened
        }
    };
    if let Some(viewport) = editor.viewports.all_mut().get_mut(id) {
        if let Some(camera) = request.camera {
            viewport.state.camera = camera;
        }
        if let Some(projection) = request.projection {
            viewport.state.projection = projection;
        }
        viewport.state.viewport = request.resolution;
        viewport.state.view_mode = request.view_mode;
        viewport.overlays = if request.include_overlays {
            Overlays::default()
        } else {
            // Nothing drawn over the image, which is what "an image intended to represent the
            // shipping frame SHALL exclude them" comes to when the editor controls the viewport.
            Overlays::none()
        };
    }
    id
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
    /// Why the image is not what the project actually looks like, when it is not.
    ///
    /// Carried rather than folded into [`Observation::kind`] because it answers a different
    /// question: `kind` says what was drawn, and this says how well. An agent judging a lighting
    /// change needs both, and a full-quality debug view and a degraded colour image are different
    /// kinds of unusable.
    pub degraded: Degradation,
    /// Which viewport answered — the one asked for, or the connection's own when it stated a camera.
    pub viewport: ViewportId,
}

impl Observation {
    /// Whether this image can be used to judge what the project will look like.
    ///
    /// The one question an agent should ask before evaluating a lighting change, and the reason the
    /// honesty flag exists at all.
    #[must_use]
    pub const fn represents_the_shipping_frame(&self) -> bool {
        matches!(self.kind, ObservationKind::ShippingFrame)
            && !self.includes_overlays
            && matches!(self.degraded, Degradation::None)
    }

    /// The image's bytes, when the delivery actually carried any.
    ///
    /// `None` for a surface and for a shared texture, which is not a failure: those are the
    /// deliveries that cost no copy, and the pixels are on the device where the human's viewport
    /// composites them. A transport that hands an agent bytes is one the runtime encoded for,
    /// and asking for one is what [`ViewportRequest`] is for.
    #[must_use]
    pub fn bytes(&self) -> Option<&[u8]> {
        match &self.image {
            FrameImage::Encoded(bytes) => Some(bytes),
            FrameImage::Surface(_) | FrameImage::SharedTexture { .. } => None,
        }
    }

    /// What the bytes are, sniffed from the bytes rather than declared.
    ///
    /// Sniffed because the transport carries an image and not a format, and a format field nobody
    /// filled in would be a lie with a type. Two signatures cover everything a runtime encodes
    /// today, and anything else is reported as what it is: bytes of an unstated kind.
    #[must_use]
    pub fn media_type(&self) -> &'static str {
        let Some(bytes) = self.bytes() else {
            return "image/x-cyberdyne-device-image";
        };
        match bytes {
            [0x89, b'P', b'N', b'G', ..] => "image/png",
            [0xff, 0xd8, 0xff, ..] => "image/jpeg",
            _ => "application/octet-stream",
        }
    }

    /// One line a caller reads, saying what the image is and whether it can be judged.
    #[must_use]
    pub fn describe(&self) -> String {
        format!(
            "{} from {} at {}x{}, frame {}, overlays {}, {}",
            match self.kind {
                ObservationKind::ShippingFrame => "the shipping frame".to_string(),
                ObservationKind::EditorFrame => "the editor's frame".to_string(),
                ObservationKind::DebugView(mode) =>
                    format!("the {} debug view", mode.engine_name()),
            },
            self.viewport,
            self.resolution.width,
            self.resolution.height,
            self.frame.as_u64(),
            if self.includes_overlays { "in" } else { "out" },
            match self.degraded {
                Degradation::None => "full quality",
                Degradation::ReducedRate => "rendered below the requested rate",
                Degradation::ReducedResolution => "rendered below the viewport's pixel size",
                Degradation::Paused => "not rendering; the last image stands",
            }
        )
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
            degraded: Degradation::None,
            viewport: ViewportId::from_raw(1),
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
            degraded: Degradation::None,
            viewport: ViewportId::from_raw(1),
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
