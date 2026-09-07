//! A click, carried to the engine and back. M6 task 2.6.
//!
//! --- THE GAP M5.5 LEFT, IN ONE SENTENCE -------------------------------------------------------------
//!
//! *"Picking has no wire: `cy-editor-protocol` carries no pick message and the SDK has no call, so
//! `pick` produces a `PickRequest` nothing answers."* Both ends existed and were tested — the
//! viewport built a request naming the frame the user was looking at, and the engine's
//! `src/servers/render/picking.h` resolves one — and there was nothing between them.
//!
//! --- WHY THE EDITOR MAY NOT ANSWER ITS OWN PICK -----------------------------------------------------
//!
//! `editor-viewport-and-gizmos` names "editor-side picking that does not match what the engine
//! rendered" as a forbidden pattern, and the way an editor acquires one is not by deciding to: the
//! editor already has a camera, so building a ray is three lines and intersecting it against
//! something the editor knows about is three more. The result is an editor that picks the authoring
//! transform while the screen shows an instanced, skinned, LOD-selected, virtual-geometry version of
//! it — and the two agree in every test a developer writes and disagree in the project.
//!
//! So nothing here computes a hit. [`request`] carries a pixel and a frame identifier; [`resolve`]
//! turns the identities the engine answers with into the document's own [`NodeId`]s and applies the
//! result to the selection. Between those two calls the editor does nothing at all, which is the
//! point.
//!
//! --- AND WHY IT IS NOT A BLOCKING CALL --------------------------------------------------------------
//!
//! *"The editor SHALL never block its interface thread on runtime rendering."* [`request`] enqueues
//! and returns a [`RequestId`]; the answer arrives through the session's ordinary event stream and is
//! handed to [`resolve`]. A click that waited for the runtime would freeze the editor for exactly as
//! long as the runtime was in trouble — which is when a user most needs the editor to work.

use cy_editor_core::ids::NodeId;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_documents::selection::Selection;
use cy_editor_protocol::RequestId;
use cy_editor_viewport::picking::{PickIntent, PickResolution, PickResponse, SelectionMode};
use cy_editor_viewport::viewport::Viewport;

use crate::runtime::RuntimeSession;

/// Ask the runtime what is under the pointer of `viewport`.
///
/// Answers a [`Problem`] rather than a request when no frame has arrived: a pick that named no frame
/// would have to be resolved against the runtime's *current* state, which is the mismatch this whole
/// path exists to prevent. That refusal is the sentence a user reads, so it says what to do.
pub fn request(
    runtime: &RuntimeSession,
    viewport: &Viewport,
    intent: PickIntent,
) -> Result<RequestId> {
    let pick = viewport.pick(intent).ok_or_else(|| {
        Problem::new(
            "resolve a click in the viewport",
            "no frame has arrived from the runtime yet",
        )
        .with_remedy("wait for the first frame, or start a runtime that publishes one")
    })?;
    runtime.pick(pick.frame, pick.encode())
}

/// What a resolved pick did, so a caller can say something rather than only having done it.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct Resolved {
    /// How many candidates the runtime returned.
    pub candidates: usize,
    /// How many of them the editor could name as nodes of this document.
    pub known: usize,
    /// The nodes that ended up selected.
    pub selected: Vec<NodeId>,
}

/// Apply a runtime's answer to the selection.
///
/// Everything the runtime cannot decide belongs to [`PickResolution`], which `cy-editor-viewport`
/// already owns and tests: which of several overlapping candidates this click takes, whether the
/// prefab root or the inner instance is meant, and which of the document's own filters accept it.
/// What this function adds is the two things that are the service layer's — decoding the wire and
/// changing the selection every panel observes.
///
/// An identity the editor cannot name is **counted and skipped**, not guessed at: a runtime may draw
/// things no document node stands for, and selecting the nearest node it does know would be an
/// editor that sometimes selects the wrong object.
pub fn resolve(
    reply: &[u8],
    resolution: &PickResolution<'_>,
    intent: &PickIntent,
    cycle: u32,
    selection: &mut Selection,
    mode: SelectionMode,
) -> Result<Resolved> {
    let response = PickResponse::decode(reply)?;
    let nodes = resolution.resolve(&response, intent, cycle);
    let resolved = Resolved {
        candidates: response.candidates.len(),
        known: nodes.len(),
        selected: nodes.clone(),
    };
    // Clicking the sky is not an error, and under Replace it clears the selection — which is what
    // every editor does and what a user expects. Under the other three modes an empty answer is not
    // a change: "add nothing" and "remove nothing" are not things to record.
    if !nodes.is_empty() || mode == SelectionMode::Replace {
        cy_editor_viewport::picking::apply(selection, mode, &nodes);
    }
    Ok(resolved)
}

#[cfg(test)]
mod tests {
    use cy_editor_core::value::ValueKind;
    use cy_editor_documents::Document;
    use cy_editor_viewport::picking::{
        DocumentFilter, Granularity, IdentityMap, PickCandidate, PickResolution, PickResponse,
    };
    use cy_editor_viewport::transport::TransportKind;
    use cy_editor_viewport::viewport::{Viewport, ViewportId};

    use super::*;

    fn a_document() -> (Document, NodeId) {
        let mut document = Document::new("worlds/city.cyworld");
        let marker = document.schema_mut().declare_type("Marker", false);
        document
            .schema_mut()
            .declare_field(marker, "name", ValueKind::Text, "what it is called")
            .unwrap();
        let node = document
            .with_transaction(
                "Build",
                cy_editor_core::Actor::human("designer"),
                |document| document.create_node(None),
            )
            .unwrap();
        (document, node)
    }

    fn a_viewport() -> Viewport {
        Viewport::new(
            ViewportId::from_raw(1),
            "Perspective",
            TransportKind::SharedTexture,
        )
    }

    fn answered(identity: u64) -> Vec<u8> {
        PickResponse {
            frame: cy_editor_protocol::FrameId::from_raw(1016),
            candidates: vec![PickCandidate {
                identity,
                distance: 3.0,
                transparent: false,
            }],
        }
        .encode()
    }

    #[test]
    fn a_pick_with_no_frame_is_refused_with_a_remedy_rather_than_resolved_against_nothing() {
        let runtime = RuntimeSession::none();
        let viewport = a_viewport();
        let problem = request(&runtime, &viewport, PickIntent::Click { x: 4.0, y: 4.0 })
            .expect_err("no frame has arrived");
        assert!(problem.because.contains("no frame"), "{problem:?}");
        assert!(problem.remedy.is_some(), "a refusal says what to do");
    }

    #[test]
    fn an_answer_the_editor_cannot_name_is_counted_rather_than_guessed_at() {
        let (document, _node) = a_document();
        let identities = IdentityMap::new();
        let filter = DocumentFilter::default();
        let resolution = PickResolution {
            identities: &identities,
            document: &document,
            filter: &filter,
            granularity: Granularity::Instance,
        };
        let mut selection = Selection::new();
        let intent = PickIntent::Click { x: 4.0, y: 4.0 };

        let resolved = resolve(
            &answered(77),
            &resolution,
            &intent,
            0,
            &mut selection,
            SelectionMode::Replace,
        )
        .unwrap();
        assert_eq!(resolved.candidates, 1);
        assert_eq!(
            resolved.known, 0,
            "the editor has never heard of identity 77"
        );
        assert!(
            selection.is_empty(),
            "and it selects nothing rather than the nearest thing it does know"
        );
    }

    #[test]
    fn a_known_identity_becomes_the_selection() {
        let (document, node) = a_document();
        let mut identities = IdentityMap::new();
        identities.insert(77, node);
        let filter = DocumentFilter::default();
        let resolution = PickResolution {
            identities: &identities,
            document: &document,
            filter: &filter,
            granularity: Granularity::Instance,
        };
        let mut selection = Selection::new();
        let intent = PickIntent::Click { x: 4.0, y: 4.0 };

        let resolved = resolve(
            &answered(77),
            &resolution,
            &intent,
            0,
            &mut selection,
            SelectionMode::Replace,
        )
        .unwrap();
        assert_eq!(resolved.selected, vec![node]);
        assert_eq!(selection.nodes().collect::<Vec<_>>(), vec![node]);
    }

    #[test]
    fn clicking_the_sky_clears_the_selection_and_is_not_an_error() {
        let (document, node) = a_document();
        let mut identities = IdentityMap::new();
        identities.insert(77, node);
        let filter = DocumentFilter::default();
        let resolution = PickResolution {
            identities: &identities,
            document: &document,
            filter: &filter,
            granularity: Granularity::Instance,
        };
        let mut selection = Selection::new();
        selection.add_node(node);
        let intent = PickIntent::Click { x: 4.0, y: 4.0 };
        let empty = PickResponse::empty(cy_editor_protocol::FrameId::from_raw(9)).encode();

        let resolved = resolve(
            &empty,
            &resolution,
            &intent,
            0,
            &mut selection,
            SelectionMode::Replace,
        )
        .unwrap();
        assert_eq!(resolved.candidates, 0);
        assert!(selection.is_empty());
    }
}
