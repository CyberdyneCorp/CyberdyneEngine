//! Gizmo intent up, gizmo geometry down. M6 task 2.7.
//!
//! --- THE DIVISION THIS MODULE ENFORCES ------------------------------------------------------------
//!
//! `editor-viewport-and-gizmos` is unambiguous about where a gizmo comes from:
//!
//! > Gizmo geometry, depth behaviour, occlusion handling, and screen-constant sizing SHALL be
//! > produced by **the engine**; the editor SHALL supply intent and manipulation state.
//!
//! M5.5 built both ends of that and nothing between them. `cy_editor_viewport::layout::GizmoLayout`
//! is a *reader* — its own module says "there is no function in this module that produces a handle's
//! position, and there is deliberately nowhere to put one" — and no message carried one, so the
//! published layout the editor hit-tests against never arrived. A click therefore had nothing to
//! land on however correct the reader was.
//!
//! This module is the wire. [`Request`] is what the editor sends: which manipulator, in which space,
//! about which pivot, for which objects, against which frame. [`accept`] is what it does with the
//! answer — and the one thing it does beyond decoding is **refuse a layout for the wrong frame**.
//!
//! --- WHY A STALE LAYOUT IS REFUSED RATHER THAN USED ------------------------------------------------
//!
//! A layout describes where the handles were *in one frame*. The editor's camera is free-running and
//! the runtime's frames arrive whenever they arrive; on a remote transport the two are tens of
//! milliseconds apart. Hit-testing a click against a layout from a different frame is the same defect
//! as resolving a pick against a newer camera — the user grabs the arrow they can see and drags the
//! one the editor thinks is there — and it is unattributable, because both halves are individually
//! correct.
//!
//! So [`accept`] compares the layout's frame against the frame the viewport is showing, and answers
//! a [`Problem`] when they differ. That is a refusal the caller can act on: ask again.

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_protocol::{FrameId, RequestId};
use cy_editor_viewport::gizmo::{GizmoMode, GizmoSpace, Pivot};
use cy_editor_viewport::layout::GizmoLayout;
use cy_editor_viewport::math::Quat;
use cy_editor_viewport::viewport::Viewport;

use crate::runtime::RuntimeSession;

/// What the editor wants drawn, and about what.
///
/// Every field is **intent**. There is no position, no extent and no handle in it, because those are
/// what the engine answers with — a request that carried them would be the editor computing the
/// geometry and asking the engine to agree.
#[derive(Clone, PartialEq, Debug)]
pub struct Request {
    /// The frame the editor is showing, which the layout must come back naming.
    pub frame: FrameId,
    /// Which manipulator.
    pub mode: GizmoMode,
    /// The frame its axes are in.
    pub space: GizmoSpace,
    /// What the manipulation happens about.
    pub pivot: Pivot,
    /// The engine's stable identities for what is selected. Empty means "draw no gizmo", which is a
    /// legitimate request and not an absent one: a selection that became empty must take the gizmo
    /// off the screen.
    pub identities: Vec<u64>,
    /// The viewport's size in its own pixels, which is **the space the layout must come back in**.
    ///
    /// --- WHY THIS IS INTENT AND NOT GEOMETRY ------------------------------------------------
    ///
    /// It is the size the editor is ASKING to see, which `editor-viewport-and-gizmos` already has
    /// the transport carrying alongside the image; it is not where anything is. The engine still
    /// decides where every handle goes.
    ///
    /// --- WHY IT HAS TO BE HERE ----------------------------------------------------------------
    ///
    /// Because a runtime renders at whatever size it renders at, and the editor stretches that
    /// frame to fill its panel. A layout published in the frame's pixels would be hit-tested
    /// against a pointer in the panel's, and at 1280x720 into a 934x570 panel that is a handle
    /// missed by a third of the viewport — a drag that lands on nothing while both sides are
    /// individually correct, which is the exact shape of defect this whole path exists to avoid.
    ///
    /// Zero means "answer in your own frame's pixels", which is what a runtime older than this
    /// field will do anyway.
    pub width: u32,
    /// The same, vertically.
    pub height: u32,
    /// The camera the editor wants the frame rendered from: position, rotation, vertical field of
    /// view in radians, and near plane.
    ///
    /// **INTENT, and the most important intent there is.** Navigation is the editor's — a person
    /// orbits, pans and dollies — and until this field existed the runtime rendered whatever camera
    /// it felt like while the editor did its manipulation arithmetic against its own. Both halves
    /// were correct and the drag moved nothing, because the pivot was at the editor's camera
    /// position and a screen-space axis of zero length has no direction.
    ///
    /// The engine still decides where the handles go, what the frame contains and how it is lit.
    /// This says where to stand.
    pub camera_position: [f32; 3],
    /// The camera's rotation. It looks down its local −Z.
    pub camera_rotation: [f32; 4],
    /// The vertical field of view, in radians.
    pub fov_y_radians: f32,
    /// The near clip distance, in world units.
    pub near: f32,
}

impl Request {
    /// The request a viewport is currently asking for, or `None` before a frame has arrived.
    ///
    /// `None` rather than a request naming frame zero, for the reason `Viewport::pick` gives: a
    /// request naming no frame is one the runtime would have to answer against its current state.
    #[must_use]
    pub fn of_viewport(viewport: &Viewport, identities: Vec<u64>) -> Option<Self> {
        let frame = viewport.stream.latest()?.frame;
        Some(Self {
            frame,
            mode: viewport.gizmo_mode,
            space: viewport.gizmo_space,
            pivot: viewport.gizmo_pivot,
            identities,
            width: viewport.state.viewport.width,
            height: viewport.state.viewport.height,
            camera_position: viewport.state.camera.position.to_array(),
            camera_rotation: viewport.state.camera.rotation.to_array(),
            fov_y_radians: match viewport.state.projection {
                cy_editor_viewport::state::Projection::Perspective { fov_y } => fov_y,
                // An orthographic viewport has no field of view. Zero says so, and a runtime that
                // reads zero renders its own projection rather than an arbitrary one — which is
                // honest: this artefact's engine renders perspective, and an orthographic editor
                // viewport is a case M8's live editing will have to answer properly.
                cy_editor_viewport::state::Projection::Orthographic { .. } => 0.0,
            },
            near: viewport.state.near,
        })
    }

    /// Encode the request. The same codec the journal and every other message use.
    #[must_use]
    pub fn encode(&self) -> Vec<u8> {
        let mut writer = Writer::new();
        writer.u64(self.frame.as_u64());
        writer.u8(mode_code(self.mode));
        write_space(&mut writer, self.space);
        writer.u8(pivot_code(self.pivot));
        writer.u32(u32::try_from(self.identities.len()).unwrap_or(u32::MAX));
        for identity in &self.identities {
            writer.u64(*identity);
        }
        // AFTER the identities, so that a runtime built before this field existed reads a complete
        // message and stops — which is what its decoder does with the bytes it does not expect, and
        // is why this was appended rather than inserted.
        writer.u32(self.width);
        writer.u32(self.height);
        for lane in self.camera_position {
            writer.f32(lane);
        }
        for lane in self.camera_rotation {
            writer.f32(lane);
        }
        writer.f32(self.fov_y_radians);
        writer.f32(self.near);
        writer.finish()
    }

    /// Decode one. What a runtime implementing this protocol calls.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(bytes);
        let frame = FrameId::from_raw(reader.u64()?);
        let mode = mode_of(reader.u8()?)?;
        let space = read_space(&mut reader)?;
        let pivot = pivot_of(reader.u8()?)?;
        let count = reader.u32()? as usize;
        let mut identities = Vec::new();
        for _ in 0..count {
            identities.push(reader.u64()?);
        }
        // Optional, for the reason `Request::width` gives: a message from an editor that predates
        // the field ends here, and zero means "answer in your own frame's pixels".
        let width = reader.u32().unwrap_or(0);
        let height = reader.u32().unwrap_or(0);
        let mut camera_position = [0.0_f32; 3];
        let mut camera_rotation = [0.0, 0.0, 0.0, 1.0_f32];
        let mut fov_y_radians = 0.0_f32;
        let mut near = 0.0_f32;
        // Optional as a group, like the size above: a message that stops before the camera is one
        // from an editor that predates it, and a runtime reading it renders its own view.
        if let (Ok(x), Ok(y), Ok(z)) = (reader.f32(), reader.f32(), reader.f32()) {
            camera_position = [x, y, z];
            if let (Ok(i), Ok(j), Ok(k), Ok(w)) =
                (reader.f32(), reader.f32(), reader.f32(), reader.f32())
            {
                camera_rotation = [i, j, k, w];
            }
            fov_y_radians = reader.f32().unwrap_or(0.0);
            near = reader.f32().unwrap_or(0.0);
        }
        Ok(Self {
            frame,
            mode,
            space,
            pivot,
            identities,
            width,
            height,
            camera_position,
            camera_rotation,
            fov_y_radians,
            near,
        })
    }
}

const fn mode_code(mode: GizmoMode) -> u8 {
    match mode {
        GizmoMode::Translate => 0,
        GizmoMode::Rotate => 1,
        GizmoMode::Scale => 2,
        GizmoMode::Universal => 3,
    }
}

fn mode_of(code: u8) -> Result<GizmoMode> {
    match code {
        0 => Ok(GizmoMode::Translate),
        1 => Ok(GizmoMode::Rotate),
        2 => Ok(GizmoMode::Scale),
        3 => Ok(GizmoMode::Universal),
        other => Err(unknown("a gizmo mode", other)),
    }
}

/// A custom space carries a frame, so its four floats are written after the tag rather than being
/// squeezed into it. Every other space is a tag and nothing else.
fn write_space(writer: &mut Writer, space: GizmoSpace) {
    match space {
        GizmoSpace::World => writer.u8(0),
        GizmoSpace::Local => writer.u8(1),
        GizmoSpace::Parent => writer.u8(2),
        GizmoSpace::View => writer.u8(3),
        GizmoSpace::Custom(frame) => {
            writer.u8(4);
            for lane in frame.to_array() {
                writer.f32(lane);
            }
        }
    }
}

fn read_space(reader: &mut Reader<'_>) -> Result<GizmoSpace> {
    match reader.u8()? {
        0 => Ok(GizmoSpace::World),
        1 => Ok(GizmoSpace::Local),
        2 => Ok(GizmoSpace::Parent),
        3 => Ok(GizmoSpace::View),
        4 => {
            let mut lanes = [0.0_f32; 4];
            for lane in &mut lanes {
                *lane = reader.f32()?;
            }
            Ok(GizmoSpace::Custom(Quat::from_array(lanes)))
        }
        other => Err(unknown("a gizmo space", other)),
    }
}

const fn pivot_code(pivot: Pivot) -> u8 {
    match pivot {
        Pivot::Pivot => 0,
        Pivot::Center => 1,
        Pivot::Bounds => 2,
        Pivot::Individual => 3,
    }
}

fn pivot_of(code: u8) -> Result<Pivot> {
    match code {
        0 => Ok(Pivot::Pivot),
        1 => Ok(Pivot::Center),
        2 => Ok(Pivot::Bounds),
        3 => Ok(Pivot::Individual),
        other => Err(unknown("a pivot", other)),
    }
}

fn unknown(what: &str, code: u8) -> Problem {
    Problem::new(
        format!("decode {what}"),
        format!("{code} is not one this build knows"),
    )
    .with_remedy("the peer is a newer editor or runtime; rebuild them together")
}

/// What an asked-for gizmo is waiting on: the request, and the frame it was asked about.
///
/// The frame is returned as well as the request because the answer has to be checked against it.
/// See [`accept_for`] for why that is the frame it was ASKED about rather than the newest one.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Asked {
    /// The request, to pair the answer with.
    pub request: RequestId,
    /// The frame the intent named, which is the frame the layout must come back describing.
    pub frame: FrameId,
}

/// Ask the runtime to draw a gizmo, and to say where it drew it.
pub fn request(
    runtime: &RuntimeSession,
    viewport: &Viewport,
    identities: Vec<u64>,
) -> Result<Asked> {
    let intent = Request::of_viewport(viewport, identities).ok_or_else(|| {
        Problem::new(
            "ask the runtime for a gizmo",
            "no frame has arrived from the runtime yet",
        )
        .with_remedy("wait for the first frame, or start a runtime that publishes one")
    })?;
    let frame = intent.frame;
    let request = runtime.gizmo(viewport.id.as_u64(), intent.encode())?;
    Ok(Asked { request, frame })
}

/// Take a published layout, refusing one that belongs to a frame the viewport is not showing.
///
/// See the module note for why the frame check is the whole of this function's judgement.
pub fn accept(published: &[u8], viewport: &Viewport) -> Result<GizmoLayout> {
    let showing = viewport.stream.latest().map(|frame| frame.frame);
    accept_for(published, showing)
}

/// The same check, against the frame the intent NAMED rather than against the newest one.
///
/// --- WHY THERE ARE TWO, AND WHICH ONE A RUNTIME SESSION USES -------------------------------------
///
/// [`accept`] is the strict form and it is the right one when the two ends are in step. Over a real
/// socket they never are, and not because anything is wrong: the editor asks about frame N, the
/// runtime answers within a millisecond, and by the time the answer is read the viewport is showing
/// N+1 or N+2 — because both ends are free-running, which is the whole design of the transport.
/// Under [`accept`] that layout would be refused every single time, and the gizmo would never
/// appear.
///
/// So a session checks the answer against the frame it ASKED about. That is not a weaker claim, it
/// is the accurate one: the layout describes the frame the intent named, the intent named the frame
/// on screen when it was sent, and what makes the layout fresh is that the session asks again every
/// frame rather than that the two identifiers happen to be equal.
///
/// What is still refused is the thing the check exists for — a layout for a frame nobody asked
/// about, which is a late answer to a request two frames old, and which would put the handles where
/// the camera used to be.
pub fn accept_for(published: &[u8], expected: Option<FrameId>) -> Result<GizmoLayout> {
    let layout = GizmoLayout::decode(published)?;
    if expected != Some(layout.frame) {
        return Err(Problem::new(
            "use the gizmo the runtime published",
            format!(
                "it describes frame {} and the request named {}",
                layout.frame.as_u64(),
                expected.map_or_else(|| "nothing".to_string(), |frame| frame.as_u64().to_string())
            ),
        )
        .with_remedy("ask again against the frame on screen"));
    }
    Ok(layout)
}

#[cfg(test)]
mod tests {
    use cy_editor_viewport::layout::HandleSpot;
    use cy_editor_viewport::transport::{
        FrameImage, Mailbox, MailboxTransport, PresentedFrame, TransportKind,
    };
    use cy_editor_viewport::viewport::ViewportId;

    use super::*;

    fn showing(frame: u64) -> Viewport {
        let mut viewport = Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::SharedTexture,
        );
        let mailbox = Mailbox::new();
        mailbox.publish(PresentedFrame::new(
            FrameId::from_raw(frame),
            viewport.state.clone(),
            FrameImage::Surface(0),
            0,
        ));
        let mut transport = MailboxTransport::new(TransportKind::SharedTexture, mailbox);
        viewport.pump(&mut transport, 1_000);
        viewport
    }

    fn layout(frame: u64) -> Vec<u8> {
        GizmoLayout {
            frame: FrameId::from_raw(frame),
            mode: GizmoMode::Translate,
            centre: (640.0, 360.0),
            extent: 90.0,
            spots: vec![HandleSpot {
                handle: cy_editor_viewport::gizmo::Handle::AxisX,
                x: 730.0,
                y: 360.0,
                radius: 6.0,
                depth: 4.0,
            }],
        }
        .encode()
    }

    #[test]
    fn a_request_carries_intent_and_no_geometry() {
        let viewport = showing(1016);
        let request = Request::of_viewport(&viewport, vec![7, 9]).expect("a frame has arrived");
        let round_tripped = Request::decode(&request.encode()).expect("it round-trips");
        assert_eq!(round_tripped, request);
        assert_eq!(round_tripped.frame.as_u64(), 1016);
        assert_eq!(round_tripped.identities, vec![7, 9]);
    }

    #[test]
    fn a_gizmo_cannot_be_asked_for_before_a_frame_has_arrived() {
        let viewport = Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::SharedTexture,
        );
        assert!(Request::of_viewport(&viewport, vec![7]).is_none());
        let problem = request(&RuntimeSession::none(), &viewport, vec![7]).expect_err("no frame");
        assert!(problem.because.contains("no frame"), "{problem:?}");
    }

    #[test]
    fn a_published_layout_for_the_frame_on_screen_is_taken() {
        let viewport = showing(1016);
        let taken = accept(&layout(1016), &viewport).expect("the frames agree");
        assert_eq!(taken.frame.as_u64(), 1016);
        assert_eq!(
            taken.hit(730.0, 360.0),
            Some(cy_editor_viewport::gizmo::Handle::AxisX),
            "and it is what a click is hit-tested against"
        );
    }

    #[test]
    fn a_layout_for_another_frame_is_refused_rather_than_hit_tested() {
        // The defect this stands for: the user grabs the arrow they can see and drags the one the
        // editor thinks is there, because the layout is from a frame the camera has moved past.
        let viewport = showing(1016);
        let problem = accept(&layout(1015), &viewport).expect_err("a stale layout");
        assert!(problem.because.contains("1015"), "{problem:?}");
        assert!(problem.because.contains("1016"), "{problem:?}");
        assert!(problem.remedy.is_some());
    }

    #[test]
    fn an_empty_selection_is_a_request_and_not_an_absence() {
        // "Draw nothing" has to be sendable, or a selection that became empty leaves the last gizmo
        // on screen for ever.
        let viewport = showing(1016);
        let request = Request::of_viewport(&viewport, Vec::new()).expect("a frame has arrived");
        assert!(request.identities.is_empty());
        assert_eq!(Request::decode(&request.encode()).unwrap(), request);
    }
}
