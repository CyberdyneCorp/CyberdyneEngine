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
        Ok(Self {
            frame,
            mode,
            space,
            pivot,
            identities,
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

/// Ask the runtime to draw a gizmo, and to say where it drew it.
pub fn request(
    runtime: &RuntimeSession,
    viewport: &Viewport,
    identities: Vec<u64>,
) -> Result<RequestId> {
    let request = Request::of_viewport(viewport, identities).ok_or_else(|| {
        Problem::new(
            "ask the runtime for a gizmo",
            "no frame has arrived from the runtime yet",
        )
        .with_remedy("wait for the first frame, or start a runtime that publishes one")
    })?;
    runtime.gizmo(viewport.id.as_u64(), request.encode())
}

/// Take a published layout, refusing one that belongs to a frame the viewport is not showing.
///
/// See the module note for why the frame check is the whole of this function's judgement.
pub fn accept(published: &[u8], viewport: &Viewport) -> Result<GizmoLayout> {
    let layout = GizmoLayout::decode(published)?;
    let showing = viewport.stream.latest().map(|frame| frame.frame);
    if showing != Some(layout.frame) {
        return Err(Problem::new(
            "use the gizmo the runtime published",
            format!(
                "it describes frame {} and the viewport is showing {}",
                layout.frame.as_u64(),
                showing.map_or_else(|| "nothing".to_string(), |frame| frame.as_u64().to_string())
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
