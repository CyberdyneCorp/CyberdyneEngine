//! Picking, from the editor's side: a pixel, the frame it was clicked on, and what to do with the
//! answer.
//!
//! `editor-viewport-and-gizmos` — "Selection and picking":
//!
//! > Picking SHALL be **engine-side**, so that what is picked matches what is rendered — including
//! > virtual geometry, instanced content, foliage, terrain, and skinned meshes. Picking SHALL support
//! > click selection, rectangle and lasso selection, cycling through overlapping candidates,
//! > selecting through transparent surfaces by intent, and selecting the prefab root or the inner
//! > instance explicitly. Selection SHALL be expressed in **stable identity** ... and SHALL survive
//! > streaming and reload.
//!
//! --- WHAT THIS MODULE MAY AND MAY NOT COMPUTE ------------------------------------------------------
//!
//! It may not resolve a pick. There is no ray here, no intersection and no traversal, and
//! [`crate::state::ViewState::ray_through_pixel`] is documented as being for manipulation only. A
//! [`PickRequest`] carries **the pixel and the identifier of the frame it was clicked on**; the
//! runtime builds the ray from that frame's own view state and resolves it against the draw list
//! that frame produced (`src/servers/render/picking.h`).
//!
//! That is not fastidiousness. "Editor-side picking that does not match what the engine rendered" is
//! a named forbidden pattern, and the way editors acquire one is exactly this: the editor already has
//! a camera, so building a ray is three lines, and once the ray exists intersecting it against
//! something the editor knows about is three more. Sending the pixel removes the first three lines
//! and with them the temptation.
//!
//! It also answers the scenario directly. "WHEN the user clicks in a streamed viewport THEN the hit
//! SHALL be resolved against the view state of the frame shown, not a newer one": the request names
//! the frame, and [`PickRequest::for_frame`] is the only constructor, so a request with no frame
//! cannot be built.
//!
//! --- WHAT IT MAY COMPUTE ---------------------------------------------------------------------------
//!
//! Everything about *intent* and everything about *authoring*:
//!
//!   * which of the returned candidates a click selects, given how many times the same spot has been
//!     clicked ([`ClickCycle`]);
//!   * whether the click replaces, adds to, removes from or toggles the selection
//!     ([`SelectionMode`]);
//!   * whether the prefab root or the inner instance is selected ([`Granularity`]) — the engine has
//!     no prefabs, only instances with stable identities, so this is a walk up the document's own
//!     hierarchy;
//!   * type and tag filters, which are document properties the engine has never heard of.
//!
//! --- IDENTITY, AND THE TWO KINDS OF IT --------------------------------------------------------------
//!
//! The engine returns `u64` stable identities: what it drew. The document addresses `NodeId`: what a
//! transaction targets. [`IdentityMap`] is the correspondence, and it lives no longer than a runtime
//! session — `cy_editor_core::ids` says so of `RuntimeEntity` for the same reason. Selection is held
//! in `NodeId`, which is why it survives a reload: nothing is remapped, because nothing was ever
//! addressed by where it was.

use std::collections::BTreeMap;

use cy_editor_core::codec::{Reader, Writer};
use cy_editor_core::ids::{NodeId, TypeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;
use cy_editor_protocol::FrameId;

use crate::state::VisibilityFilter;
use crate::viewport::ViewportId;

/// What the user did with the pointer.
#[derive(Clone, PartialEq, Debug)]
pub enum PickIntent {
    /// A click at a pixel of the presented frame.
    Click {
        /// Pixels from the viewport's left edge.
        x: f32,
        /// Pixels from the viewport's top edge.
        y: f32,
    },
    /// A dragged rectangle, in the same pixel convention.
    Rectangle {
        /// Left edge.
        min_x: f32,
        /// Top edge.
        min_y: f32,
        /// Right edge.
        max_x: f32,
        /// Bottom edge.
        max_y: f32,
    },
    /// A freehand lasso: at least three points, in the same pixel convention.
    Lasso {
        /// The outline, in the order it was drawn.
        points: Vec<(f32, f32)>,
    },
}

impl PickIntent {
    /// A rectangle from two corners in any order, which is what a drag actually produces.
    #[must_use]
    pub fn rectangle(from: (f32, f32), to: (f32, f32)) -> Self {
        Self::Rectangle {
            min_x: from.0.min(to.0),
            min_y: from.1.min(to.1),
            max_x: from.0.max(to.0),
            max_y: from.1.max(to.1),
        }
    }

    /// Whether this intent selects an area rather than a point. Area picks have no depth to order
    /// by, so they never cycle.
    #[must_use]
    pub const fn is_area(&self) -> bool {
        !matches!(self, PickIntent::Click { .. })
    }
}

/// What the click does to the selection that is already there.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum SelectionMode {
    /// The usual click: what was selected is not any more.
    #[default]
    Replace,
    /// Shift-click.
    Add,
    /// Control-click on something selected.
    Remove,
    /// Control-click: in if it was out, out if it was in.
    Toggle,
}

/// Whether a click selects the object drawn or the authoring object that owns it.
///
/// "selecting the prefab root or the inner instance explicitly". Explicitly, so it is a field the
/// caller sets from a modifier rather than a heuristic about how deep to stop.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Granularity {
    /// The outermost authoring object that contains what was drawn — a prefab's root.
    #[default]
    Root,
    /// Exactly what was drawn.
    Instance,
}

/// What the editor will accept as a hit, sent with the request.
///
/// Every field is intent. Nothing here restates a visibility rule the renderer applied: an object
/// the renderer did not draw is not in the draw list the pick resolves against, so it cannot be a
/// candidate whatever this filter says.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct PickFilter {
    /// The engine layers the viewport draws.
    pub layers: u32,
    /// Identities the editor will not select: locked objects, and anything isolation excludes.
    pub excluded: Vec<u64>,
    /// Whether a transparent surface can be hit. "Selecting through transparent surfaces by intent"
    /// is a modifier the user holds, so it is a field.
    pub include_transparent: bool,
    /// How many candidates to return. A click on a forest overlaps thousands of bounding volumes
    /// and an editor that cycles through the nearest few should not carry the rest across the
    /// bridge.
    pub max_candidates: u32,
}

impl Default for PickFilter {
    fn default() -> Self {
        Self {
            layers: u32::MAX,
            excluded: Vec::new(),
            include_transparent: true,
            max_candidates: 16,
        }
    }
}

impl PickFilter {
    /// The filter a viewport's visibility settings imply.
    ///
    /// Isolation and locking are authoring state, and this is where they become something the engine
    /// can act on: a list of identities it will not report. The engine never learns what "locked"
    /// means, which is correct — it is not a rendering property.
    #[must_use]
    pub fn from_visibility(filter: &VisibilityFilter) -> Self {
        let mut excluded = filter.locked.clone();
        excluded.sort_unstable();
        excluded.dedup();
        Self {
            layers: filter.layers,
            excluded,
            ..Self::default()
        }
    }
}

/// A pick, addressed to one frame of one viewport.
#[derive(Clone, PartialEq, Debug)]
pub struct PickRequest {
    /// The viewport that was clicked.
    pub viewport: ViewportId,
    /// **The frame that was on screen when it was clicked.** The runtime resolves against this
    /// frame's view state, not against whatever the editor's camera has since become.
    pub frame: FrameId,
    /// What the pointer did.
    pub intent: PickIntent,
    /// What the editor will accept.
    pub filter: PickFilter,
}

impl PickRequest {
    /// A request against a presented frame. The only constructor, so a request without a frame
    /// cannot be built.
    #[must_use]
    pub fn for_frame(viewport: ViewportId, frame: FrameId, intent: PickIntent) -> Self {
        Self {
            viewport,
            frame,
            intent,
            filter: PickFilter::default(),
        }
    }

    /// Restrict what will be accepted.
    #[must_use]
    pub fn with_filter(mut self, filter: PickFilter) -> Self {
        self.filter = filter;
        self
    }

    /// Encode the request for the bridge.
    ///
    /// The same codec the journal and the control path use. The viewport's messages are not in
    /// `cy_editor_protocol::Message` because that enum is another crate's; the encoding is here and
    /// folding it in is one variant when somebody owns both. See this crate's README.
    #[must_use]
    pub fn encode(&self) -> Vec<u8> {
        let mut writer = Writer::new();
        writer.u64(self.viewport.as_u64());
        writer.u64(self.frame.as_u64());
        write_intent(&mut writer, &self.intent);
        writer.u32(self.filter.layers);
        writer.u8(u8::from(self.filter.include_transparent));
        writer.u32(self.filter.max_candidates);
        writer.u32(u32::try_from(self.filter.excluded.len()).unwrap_or(u32::MAX));
        for identity in &self.filter.excluded {
            writer.u64(*identity);
        }
        writer.finish()
    }

    /// Decode a request. What a runtime does with the bytes.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(bytes);
        let viewport = ViewportId::from_raw(reader.u64()?);
        let frame = FrameId::from_raw(reader.u64()?);
        let intent = read_intent(&mut reader)?;
        let layers = reader.u32()?;
        let include_transparent = reader.u8()? != 0;
        let max_candidates = reader.u32()?;
        let count = reader.u32()? as usize;
        let mut excluded = Vec::new();
        for _ in 0..count {
            excluded.push(reader.u64()?);
        }
        Ok(Self {
            viewport,
            frame,
            intent,
            filter: PickFilter {
                layers,
                excluded,
                include_transparent,
                max_candidates,
            },
        })
    }
}

fn write_intent(writer: &mut Writer, intent: &PickIntent) {
    match intent {
        PickIntent::Click { x, y } => {
            writer.u8(0);
            writer.f32(*x);
            writer.f32(*y);
        }
        PickIntent::Rectangle {
            min_x,
            min_y,
            max_x,
            max_y,
        } => {
            writer.u8(1);
            for value in [*min_x, *min_y, *max_x, *max_y] {
                writer.f32(value);
            }
        }
        PickIntent::Lasso { points } => {
            writer.u8(2);
            writer.u32(u32::try_from(points.len()).unwrap_or(u32::MAX));
            for (x, y) in points {
                writer.f32(*x);
                writer.f32(*y);
            }
        }
    }
}

fn read_intent(reader: &mut Reader<'_>) -> Result<PickIntent> {
    match reader.u8()? {
        0 => Ok(PickIntent::Click {
            x: reader.f32()?,
            y: reader.f32()?,
        }),
        1 => Ok(PickIntent::Rectangle {
            min_x: reader.f32()?,
            min_y: reader.f32()?,
            max_x: reader.f32()?,
            max_y: reader.f32()?,
        }),
        2 => {
            let count = reader.u32()? as usize;
            let mut points = Vec::new();
            for _ in 0..count {
                points.push((reader.f32()?, reader.f32()?));
            }
            Ok(PickIntent::Lasso { points })
        }
        other => Err(Problem::new(
            "decode a pick",
            format!("intent kind {other} is not one this build knows"),
        )),
    }
}

/// One thing the runtime found, in the order it found them: nearest first for a click.
#[derive(Clone, Copy, PartialEq, Debug)]
pub struct PickCandidate {
    /// The stable identity of what was drawn.
    pub identity: u64,
    /// Distance along the ray, world units. Zero for an area pick.
    pub distance: f32,
    /// Whether the renderer drew it in the transparent layer.
    pub transparent: bool,
}

/// What the runtime answered.
#[derive(Clone, PartialEq, Debug)]
pub struct PickResponse {
    /// The frame the pick was resolved against — the one the request named, echoed so the editor can
    /// tell an answer to an old click from an answer to the current one.
    pub frame: FrameId,
    /// What was found, nearest first.
    pub candidates: Vec<PickCandidate>,
}

impl PickResponse {
    /// An answer with nothing in it: the user clicked the sky, which is not an error.
    #[must_use]
    pub const fn empty(frame: FrameId) -> Self {
        Self {
            frame,
            candidates: Vec::new(),
        }
    }

    /// Encode the answer.
    #[must_use]
    pub fn encode(&self) -> Vec<u8> {
        let mut writer = Writer::new();
        writer.u64(self.frame.as_u64());
        writer.u32(u32::try_from(self.candidates.len()).unwrap_or(u32::MAX));
        for candidate in &self.candidates {
            writer.u64(candidate.identity);
            writer.f32(candidate.distance);
            writer.u8(u8::from(candidate.transparent));
        }
        writer.finish()
    }

    /// Decode an answer.
    pub fn decode(bytes: &[u8]) -> Result<Self> {
        let mut reader = Reader::new(bytes);
        let frame = FrameId::from_raw(reader.u64()?);
        let count = reader.u32()? as usize;
        let mut candidates = Vec::new();
        for _ in 0..count {
            candidates.push(PickCandidate {
                identity: reader.u64()?,
                distance: reader.f32()?,
                transparent: reader.u8()? != 0,
            });
        }
        Ok(Self { frame, candidates })
    }
}

/// How many times the same spot has been clicked, which is what cycling counts.
///
/// `editor-viewport-and-gizmos` asks for "cycling through overlapping candidates". The state that
/// needs is one pixel and one counter, and it belongs to the viewport rather than to the runtime:
/// the runtime returns the same ordered candidates every time, and which of them this click takes is
/// a question about the user's clicks.
#[derive(Clone, Copy, PartialEq, Debug, Default)]
pub struct ClickCycle {
    last: Option<(f32, f32)>,
    count: u32,
}

impl ClickCycle {
    /// How many pixels the pointer may move and still count as the same spot.
    pub const TOLERANCE: f32 = 2.0;

    /// Nothing clicked yet.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            last: None,
            count: 0,
        }
    }

    /// Record a click and return which candidate it should take.
    pub fn advance(&mut self, x: f32, y: f32) -> u32 {
        let same_spot = self.last.is_some_and(|(previous_x, previous_y)| {
            (previous_x - x).abs() <= Self::TOLERANCE && (previous_y - y).abs() <= Self::TOLERANCE
        });
        self.count = if same_spot { self.count + 1 } else { 0 };
        self.last = Some((x, y));
        self.count
    }

    /// Forget where the last click was. Called when the selection changes for any other reason, so
    /// that clicking the same spot after selecting from the hierarchy starts the cycle again rather
    /// than resuming one the user has lost track of.
    pub fn reset(&mut self) {
        *self = Self::new();
    }
}

/// The correspondence between what the engine drew and what a transaction addresses.
///
/// Session-scoped: a runtime restart issues new identities, and the map is rebuilt. Selection is not
/// — it is held in [`NodeId`], which outlives every runtime.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct IdentityMap {
    to_node: BTreeMap<u64, NodeId>,
}

impl IdentityMap {
    /// An empty map.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Record that the engine's `identity` is the document's `node`.
    pub fn insert(&mut self, identity: u64, node: NodeId) {
        self.to_node.insert(identity, node);
    }

    /// The node an identity names, if the editor knows it.
    #[must_use]
    pub fn node(&self, identity: u64) -> Option<NodeId> {
        self.to_node.get(&identity).copied()
    }

    /// Forget everything. What a runtime restart calls.
    pub fn clear(&mut self) {
        self.to_node.clear();
    }

    /// How many correspondences are known.
    #[must_use]
    pub fn len(&self) -> usize {
        self.to_node.len()
    }

    /// Whether the map knows nothing.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.to_node.is_empty()
    }
}

/// Filters that are document properties rather than rendering ones.
///
/// "Selection SHALL be filterable by type, layer, tag, and locked state". Layer and locked state
/// cross the bridge in [`PickFilter`]; type and tag cannot, because the engine has no notion of
/// either, so they are applied to the answer.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct DocumentFilter {
    /// Only nodes carrying this component are selectable.
    pub require_component: Option<TypeId>,
    /// Only nodes whose tag component holds this value are selectable.
    pub require_tag: Option<(TypeId, cy_editor_core::ids::FieldId, String)>,
}

impl DocumentFilter {
    /// Whether a node passes.
    #[must_use]
    pub fn accepts(&self, document: &Document, node: NodeId) -> bool {
        if self
            .require_component
            .is_some_and(|component| !document.content().has_component(node, component))
        {
            return false;
        }
        match &self.require_tag {
            Some((component, field, wanted)) => document
                .content()
                .field(node, *component, *field)
                .and_then(cy_editor_core::Value::as_text)
                .is_some_and(|tag| tag == wanted),
            None => true,
        }
    }
}

/// Turn an answer into a selection change.
///
/// Everything the runtime cannot decide happens here: which candidate the click's cycle takes,
/// whether the prefab root or the inner instance is meant, whether the document's own filters accept
/// it, and what the modifier does to the existing selection.
pub struct PickResolution<'a> {
    /// What the engine drew, mapped to what a transaction addresses.
    pub identities: &'a IdentityMap,
    /// The document the selection lives in.
    pub document: &'a Document,
    /// Type and tag filters the engine cannot apply.
    pub filter: &'a DocumentFilter,
    /// Whether a click means the prefab root or the inner instance.
    pub granularity: Granularity,
}

impl PickResolution<'_> {
    /// The nodes an answer selects, in the order the runtime reported them.
    ///
    /// An area pick takes every candidate; a click takes the one the cycle is on. A candidate the
    /// editor cannot map to a node is skipped rather than guessed at — it belongs to something the
    /// runtime is drawing that this document does not own, which is a legitimate thing for a hosted
    /// runtime to be doing.
    #[must_use]
    pub fn resolve(&self, response: &PickResponse, intent: &PickIntent, cycle: u32) -> Vec<NodeId> {
        let accepted: Vec<NodeId> = response
            .candidates
            .iter()
            .filter_map(|candidate| self.identities.node(candidate.identity))
            .map(|node| self.at_granularity(node))
            .filter(|node| self.filter.accepts(self.document, *node))
            .collect();

        if intent.is_area() {
            return deduplicated(accepted);
        }
        let deduplicated = deduplicated(accepted);
        if deduplicated.is_empty() {
            return Vec::new();
        }
        let index = cycle as usize % deduplicated.len();
        vec![deduplicated[index]]
    }

    /// The node a click means: the one drawn, or the outermost ancestor when the root is wanted.
    fn at_granularity(&self, node: NodeId) -> NodeId {
        if self.granularity == Granularity::Instance {
            return node;
        }
        let mut current = node;
        // Bounded by the depth of the hierarchy, and bounded again by the node count so that a
        // cycle in a corrupted document is a wrong answer rather than a hung editor.
        for _ in 0..=self.document.content().node_count() {
            match self
                .document
                .content()
                .node(current)
                .and_then(|state| state.parent)
            {
                Some(parent) => current = parent,
                None => break,
            }
        }
        current
    }
}

fn deduplicated(nodes: Vec<NodeId>) -> Vec<NodeId> {
    let mut seen = Vec::with_capacity(nodes.len());
    for node in nodes {
        if !seen.contains(&node) {
            seen.push(node);
        }
    }
    seen
}

/// Apply a resolved pick to a selection.
///
/// The selection is the document's, addressed by [`NodeId`], which is why it survives a reload: the
/// identities the engine issued are not stored anywhere a selection can see.
pub fn apply(selection: &mut Selection, mode: SelectionMode, nodes: &[NodeId]) {
    match mode {
        SelectionMode::Replace => selection.set_nodes(nodes.iter().copied()),
        SelectionMode::Add => {
            for node in nodes {
                selection.add_node(*node);
            }
        }
        SelectionMode::Remove => {
            for node in nodes {
                selection.remove_node(*node);
            }
        }
        SelectionMode::Toggle => toggle(selection, nodes),
    }
}

fn toggle(selection: &mut Selection, nodes: &[NodeId]) {
    let selected: Vec<NodeId> = selection.nodes().collect();
    for node in nodes {
        if selected.contains(node) {
            selection.remove_node(*node);
        } else {
            selection.add_node(*node);
        }
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::value::ValueKind;

    use super::*;

    fn document_with_a_prefab() -> (Document, NodeId, NodeId, TypeId) {
        let mut document = Document::new("worlds/city.cyworld");
        let marker = document.schema_mut().declare_type("Marker", false);
        document
            .schema_mut()
            .declare_field(marker, "name", ValueKind::Text, "what it is called")
            .expect("a fresh schema");
        let (root, child) = document
            .with_transaction("Populate", Actor::human("designer"), |document| {
                let root = document.create_node(None)?;
                let child = document.create_node(Some(root))?;
                Ok((root, child))
            })
            .expect("a transaction that only creates nodes");
        (document, root, child, marker)
    }

    fn resolution<'a>(
        document: &'a Document,
        identities: &'a IdentityMap,
        filter: &'a DocumentFilter,
        granularity: Granularity,
    ) -> PickResolution<'a> {
        PickResolution {
            identities,
            document,
            filter,
            granularity,
        }
    }

    #[test]
    fn a_request_names_the_frame_it_was_clicked_on() {
        // The scenario, as a property of the type: `for_frame` is the only constructor, so there is
        // no way to build a request that does not say which frame the user was looking at.
        let request = PickRequest::for_frame(
            ViewportId::from_raw(1),
            FrameId::from_raw(42),
            PickIntent::Click { x: 100.0, y: 50.0 },
        );
        assert_eq!(request.frame, FrameId::from_raw(42));
        let decoded = PickRequest::decode(&request.encode()).expect("our own encoding");
        assert_eq!(decoded, request);
    }

    #[test]
    fn every_intent_and_its_filter_round_trip() {
        let intents = [
            PickIntent::Click { x: 1.5, y: 2.5 },
            PickIntent::rectangle((30.0, 40.0), (10.0, 20.0)),
            PickIntent::Lasso {
                points: vec![(0.0, 0.0), (10.0, 0.0), (5.0, 9.0)],
            },
        ];
        for intent in intents {
            let request =
                PickRequest::for_frame(ViewportId::from_raw(3), FrameId::from_raw(9), intent)
                    .with_filter(PickFilter {
                        layers: 0b101,
                        excluded: vec![7, 11],
                        include_transparent: false,
                        max_candidates: 4,
                    });
            assert_eq!(
                PickRequest::decode(&request.encode()).expect("our own encoding"),
                request
            );
        }
        // And a rectangle dragged upward and leftward is still a rectangle.
        match PickIntent::rectangle((30.0, 40.0), (10.0, 20.0)) {
            PickIntent::Rectangle { min_x, max_y, .. } => {
                assert!((min_x - 10.0).abs() < f32::EPSILON);
                assert!((max_y - 40.0).abs() < f32::EPSILON);
            }
            other => panic!("expected a rectangle, got {other:?}"),
        }
    }

    #[test]
    fn a_locked_object_is_excluded_before_the_request_is_sent() {
        let visibility = VisibilityFilter {
            locked: vec![9, 9, 4],
            ..VisibilityFilter::default()
        };
        let filter = PickFilter::from_visibility(&visibility);
        assert_eq!(
            filter.excluded,
            vec![4, 9],
            "sorted and without the duplicate"
        );
        assert_eq!(filter.layers, visibility.layers);
    }

    #[test]
    fn clicking_the_same_spot_cycles_and_moving_away_starts_again() {
        let mut cycle = ClickCycle::new();
        assert_eq!(cycle.advance(100.0, 100.0), 0);
        assert_eq!(cycle.advance(101.0, 100.5), 1, "within the tolerance");
        assert_eq!(cycle.advance(100.0, 100.0), 2);
        assert_eq!(cycle.advance(400.0, 100.0), 0, "a different spot");
        cycle.advance(400.0, 100.0);
        cycle.reset();
        assert_eq!(
            cycle.advance(400.0, 100.0),
            0,
            "after a selection from elsewhere"
        );
    }

    #[test]
    fn a_click_takes_the_candidate_the_cycle_is_on_and_an_area_takes_them_all() {
        let (document, root, child, _) = document_with_a_prefab();
        let mut identities = IdentityMap::new();
        identities.insert(10, root);
        identities.insert(20, child);
        let filter = DocumentFilter::default();
        let resolver = resolution(&document, &identities, &filter, Granularity::Instance);

        let response = PickResponse {
            frame: FrameId::from_raw(1),
            candidates: vec![
                PickCandidate {
                    identity: 10,
                    distance: 4.0,
                    transparent: false,
                },
                PickCandidate {
                    identity: 20,
                    distance: 9.0,
                    transparent: false,
                },
            ],
        };
        let click = PickIntent::Click { x: 0.0, y: 0.0 };
        assert_eq!(resolver.resolve(&response, &click, 0), vec![root]);
        assert_eq!(resolver.resolve(&response, &click, 1), vec![child]);
        assert_eq!(
            resolver.resolve(&response, &click, 2),
            vec![root],
            "it wraps"
        );

        let area = PickIntent::rectangle((0.0, 0.0), (100.0, 100.0));
        assert_eq!(resolver.resolve(&response, &area, 0), vec![root, child]);
    }

    #[test]
    fn granularity_chooses_the_prefab_root_or_the_instance_that_was_drawn() {
        // The engine has no prefabs; it returned the identity of the thing it drew. Which authoring
        // object that means is a question about the document's hierarchy, answered here.
        let (document, root, child, _) = document_with_a_prefab();
        let mut identities = IdentityMap::new();
        identities.insert(20, child);
        let filter = DocumentFilter::default();
        let response = PickResponse {
            frame: FrameId::from_raw(1),
            candidates: vec![PickCandidate {
                identity: 20,
                distance: 1.0,
                transparent: false,
            }],
        };
        let click = PickIntent::Click { x: 0.0, y: 0.0 };

        let inner = resolution(&document, &identities, &filter, Granularity::Instance);
        assert_eq!(inner.resolve(&response, &click, 0), vec![child]);

        let outer = resolution(&document, &identities, &filter, Granularity::Root);
        assert_eq!(outer.resolve(&response, &click, 0), vec![root]);
    }

    #[test]
    fn a_candidate_the_editor_cannot_map_is_skipped_rather_than_guessed_at() {
        let (document, root, _, _) = document_with_a_prefab();
        let mut identities = IdentityMap::new();
        identities.insert(10, root);
        let filter = DocumentFilter::default();
        let resolver = resolution(&document, &identities, &filter, Granularity::Instance);

        let response = PickResponse {
            frame: FrameId::from_raw(1),
            candidates: vec![
                PickCandidate {
                    identity: 999,
                    distance: 1.0,
                    transparent: false,
                },
                PickCandidate {
                    identity: 10,
                    distance: 2.0,
                    transparent: false,
                },
            ],
        };
        let click = PickIntent::Click { x: 0.0, y: 0.0 };
        assert_eq!(resolver.resolve(&response, &click, 0), vec![root]);
    }

    #[test]
    fn a_type_filter_is_applied_where_the_engine_could_not() {
        let (mut document, root, child, marker) = document_with_a_prefab();
        document
            .with_transaction("Mark", Actor::human("designer"), |document| {
                document.add_component(child, marker, Vec::new())
            })
            .expect("adding a component");

        let mut identities = IdentityMap::new();
        identities.insert(10, root);
        identities.insert(20, child);
        let filter = DocumentFilter {
            require_component: Some(marker),
            require_tag: None,
        };
        let resolver = resolution(&document, &identities, &filter, Granularity::Instance);
        let response = PickResponse {
            frame: FrameId::from_raw(1),
            candidates: vec![
                PickCandidate {
                    identity: 10,
                    distance: 1.0,
                    transparent: false,
                },
                PickCandidate {
                    identity: 20,
                    distance: 2.0,
                    transparent: false,
                },
            ],
        };
        let area = PickIntent::rectangle((0.0, 0.0), (10.0, 10.0));
        assert_eq!(resolver.resolve(&response, &area, 0), vec![child]);
    }

    #[test]
    fn the_four_selection_modes_do_what_their_names_say() {
        let (document, root, child, _) = document_with_a_prefab();
        let _ = &document;
        let mut selection = Selection::new();

        apply(&mut selection, SelectionMode::Replace, &[root]);
        assert_eq!(selection.nodes().collect::<Vec<_>>(), vec![root]);

        apply(&mut selection, SelectionMode::Add, &[child]);
        assert_eq!(selection.node_count(), 2);

        apply(&mut selection, SelectionMode::Toggle, &[child]);
        assert_eq!(selection.node_count(), 1);
        apply(&mut selection, SelectionMode::Toggle, &[child]);
        assert_eq!(selection.node_count(), 2);

        apply(&mut selection, SelectionMode::Remove, &[root, child]);
        assert!(selection.is_empty());

        apply(&mut selection, SelectionMode::Replace, &[root, child]);
        apply(&mut selection, SelectionMode::Replace, &[]);
        assert!(selection.is_empty(), "a click on nothing clears");
    }

    #[test]
    fn a_selection_survives_a_runtime_restart_because_it_holds_no_engine_identity() {
        // "WHEN a region streams out and back in THEN the selection SHALL be preserved by identity."
        // The identity map is session state and is cleared; the selection is not, because it never
        // held anything the runtime issued.
        let (document, root, _, _) = document_with_a_prefab();
        let mut identities = IdentityMap::new();
        identities.insert(10, root);
        let mut selection = Selection::new();
        apply(&mut selection, SelectionMode::Replace, &[root]);

        identities.clear();
        assert!(identities.is_empty());
        selection.reresolve(&document);
        assert_eq!(selection.nodes().collect::<Vec<_>>(), vec![root]);
    }

    #[test]
    fn an_answer_round_trips_and_an_empty_one_is_not_an_error() {
        let response = PickResponse {
            frame: FrameId::from_raw(5),
            candidates: vec![PickCandidate {
                identity: 3,
                distance: 12.5,
                transparent: true,
            }],
        };
        assert_eq!(
            PickResponse::decode(&response.encode()).expect("our own encoding"),
            response
        );
        let empty = PickResponse::empty(FrameId::from_raw(5));
        assert!(empty.candidates.is_empty());
        assert_eq!(
            PickResponse::decode(&empty.encode()).expect("our own encoding"),
            empty
        );
    }
}
