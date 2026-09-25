//! The specialised editors, and the region `chrome.rs` reserved for them. Tasks 3.3 and 3.4.
//!
//! --- WHAT WAS HERE BEFORE, AND WHY IT IS THE POINT --------------------------------------------------
//!
//! `cy_editor_visual::chrome::Region::CentreLower` has read *"the active specialised editor: script
//! graph, animation, materials, sequencing"* since M5.5, and until this module existed **one file
//! in the workspace mentioned it — the one that reserves it**. The criterion that was supposed to
//! establish otherwise, `m11b:specialised-editors`, was `grep -rniIl CentreLower editor/crates/`
//! with a count of three, and said so in its own body: *"three files naming a region is satisfied
//! by three comments"*. That is the ninth instance of the defect `tools/roadmap/falsify.py`
//! enumerates and it was left red on purpose, with the note *"whoever writes section 3.3 owns the
//! replacement"*.
//!
//! So this module is written to be checkable by something a person cannot satisfy by typing:
//!
//! | Claim | What can disagree with it |
//! |---|---|
//! | the editors are the ones the requirement names | [`Domain::spec_term`] against the requirement's own enumeration |
//! | each names the row that owns its subject | [`Domain::owning_row`] against `openspec/specs/` |
//! | every graph editor shares ONE canvas | every [`Session::graph`] carries the same [`graph::CanvasId`] |
//! | every keyed-time editor shares ONE surface | every [`Session::timeline`] carries the same [`timeline::SurfaceId`] |
//! | every spatial brush editor shares ONE surface | every [`Session::painting`] carries the same [`painting::SurfaceId`] |
//! | the palette offers what the engine can lower | [`Domain::node_types`] against `src/graph/src/lower_*.cpp` |
//! | the timeline offers what the engine can dispatch | [`timeline::TrackKind`] against `cy::sequencing::TrackKind` |
//! | the active editor is drawn in the reserved region | [`SpecialisedEditors::REGION`] against `chrome.rs` |
//!
//! The first, fifth, sixth and seventh are `tools/editor/play_contract.py specialised-editors`,
//! which needs no build. The rest are the cases below.
//!
//! --- A DOMAIN THIS TREE CANNOT OPEN REFUSES BY NAME -------------------------------------------------
//!
//! Sixteen editors are named by the requirement and this tree can open the four whose authoring
//! vocabulary this milestone declares. **The other twelve are registered and refuse**, naming
//! themselves and the capability row that owes the vocabulary — because the alternative is the one
//! outcome this project has decided is worse than a refutation. M11.b's own gate wrote it down:
//!
//! > And the one outcome that is worse than a refutation: a silent fallback. A mode that is selected
//! > and not implemented must refuse by name.
//!
//! An empty canvas opened for `terrain` would be a specialised editor that exists in a screenshot.

pub mod graph;
pub mod material;
pub mod painting;
pub mod timeline;
pub mod vfx;

use std::collections::BTreeMap;

use cy_editor_core::problem::{Problem, Result};
use cy_editor_visual::chrome::Region;

use crate::panels::PanelKey;

use graph::{Catalogue, GraphCanvas, NodeType};
use painting::PaintingSurface;
use timeline::{TimelineSurface, TrackKind};

/// Which shared surface an editor is built on.
///
/// The requirement forbids two things by name — a sixth bespoke graph editor, and a second curve
/// surface — so the two shared ones are variants here rather than traits a domain could implement
/// its own version of.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Surface {
    /// The one node-graph canvas, [`graph::GraphCanvas`].
    Graph,
    /// The one timeline and curve surface, [`timeline::TimelineSurface`].
    Timeline,
    /// A brush over a field or a heightmap: terrain, foliage, water, environment fields.
    Painting,
    /// A two-dimensional arrangement: tilemaps, interface layout.
    Canvas2D,
    /// Rows and columns: audio buses, localisation tables.
    Table,
    /// Named settings and a button that starts a bake.
    Form,
}

/// One of the specialised editors `editor-architecture` enumerates.
///
/// The list is the requirement's, in the requirement's order, and
/// `tools/editor/play_contract.py specialised-editors` reads both and requires them to agree — so
/// an editor added to the specification and not here is RED rather than forgotten.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Domain {
    /// Materials, including the node graph.
    Materials,
    /// Animation graphs and clips: a timeline with curve editing.
    AnimationGraphsAndClips,
    /// The VFX graph.
    VfxGraph,
    /// Terrain sculpting and material painting.
    Terrain,
    /// Foliage painting and rule authoring.
    Foliage,
    /// Lakes, oceans and rivers.
    Water,
    /// The fields that drive procedural placement and terrain materials.
    EnvironmentFields,
    /// Tilemaps.
    Tilemaps,
    /// Interface layout.
    UiLayout,
    /// Audio buses and mixing.
    AudioBusesAndMixing,
    /// Navigation baking.
    NavigationBaking,
    /// Lighting and lightmap baking.
    LightingAndLightmapBaking,
    /// Abilities and effects.
    AbilitiesAndEffects,
    /// Gameplay and utility graphs.
    GameplayAndUtilityGraphs,
    /// Sequences and cinematics.
    SequencesAndCinematics,
    /// Localisation tables.
    LocalisationTables,
}

impl Domain {
    /// Every specialised editor, in the order the requirement enumerates them.
    pub const ALL: [Domain; 16] = [
        Domain::Materials,
        Domain::AnimationGraphsAndClips,
        Domain::VfxGraph,
        Domain::Terrain,
        Domain::Foliage,
        Domain::Water,
        Domain::EnvironmentFields,
        Domain::Tilemaps,
        Domain::UiLayout,
        Domain::AudioBusesAndMixing,
        Domain::NavigationBaking,
        Domain::LightingAndLightmapBaking,
        Domain::AbilitiesAndEffects,
        Domain::GameplayAndUtilityGraphs,
        Domain::SequencesAndCinematics,
        Domain::LocalisationTables,
    ];

    /// The requirement's own words for this editor, as it enumerates them.
    ///
    /// Not a display title — a **join key**. The contract gate parses the requirement's list and
    /// compares it with these, so drift between the specification and the editor is a red gate
    /// rather than a discrepancy nobody reads.
    pub const fn spec_term(self) -> &'static str {
        match self {
            Domain::Materials => "materials",
            Domain::AnimationGraphsAndClips => "animation graphs and clips",
            Domain::VfxGraph => "vfx graph",
            Domain::Terrain => "terrain",
            Domain::Foliage => "foliage",
            Domain::Water => "water",
            Domain::EnvironmentFields => "environment fields",
            Domain::Tilemaps => "tilemaps",
            Domain::UiLayout => "ui layout",
            Domain::AudioBusesAndMixing => "audio buses and mixing",
            Domain::NavigationBaking => "navigation baking",
            Domain::LightingAndLightmapBaking => "lighting and lightmap baking",
            Domain::AbilitiesAndEffects => "abilities and effects",
            Domain::GameplayAndUtilityGraphs => "gameplay and utility graphs",
            Domain::SequencesAndCinematics => "sequences and cinematics",
            Domain::LocalisationTables => "localisation tables",
        }
    }

    /// The capability row that owns this editor's subject.
    ///
    /// Checked against `openspec/specs/` rather than read: a row named here that no specification
    /// declares is a refusal message pointing at nothing.
    pub const fn owning_row(self) -> &'static str {
        match self {
            Domain::Materials => "material-compiler",
            Domain::AnimationGraphsAndClips => "animation-and-skinning",
            Domain::VfxGraph => "vfx-system",
            Domain::Terrain => "terrain",
            Domain::Foliage => "foliage",
            Domain::Water => "water",
            Domain::EnvironmentFields => "environment-fields",
            Domain::Tilemaps => "rendering-2d",
            Domain::UiLayout => "ui-system",
            Domain::AudioBusesAndMixing => "audio",
            Domain::NavigationBaking => "navigation",
            Domain::LightingAndLightmapBaking => "rendering-lighting-and-shadows",
            Domain::AbilitiesAndEffects => "gameplay-abilities-and-effects",
            Domain::GameplayAndUtilityGraphs => "visual-scripting",
            Domain::SequencesAndCinematics => "sequencing-and-cinematics",
            Domain::LocalisationTables => "text-and-fonts",
        }
    }

    /// Which shared surfaces this editor is built on.
    ///
    /// Two for `animation graphs and clips`, which the requirement itself describes as a graph AND
    /// "a timeline with curve editing"; one for the rest.
    pub const fn surfaces(self) -> &'static [Surface] {
        match self {
            Domain::Materials
            | Domain::VfxGraph
            | Domain::AbilitiesAndEffects
            | Domain::GameplayAndUtilityGraphs => &[Surface::Graph],
            Domain::AnimationGraphsAndClips => &[Surface::Graph, Surface::Timeline],
            Domain::SequencesAndCinematics => &[Surface::Timeline],
            Domain::Terrain | Domain::Foliage | Domain::Water | Domain::EnvironmentFields => {
                &[Surface::Painting]
            }
            Domain::Tilemaps | Domain::UiLayout => &[Surface::Canvas2D],
            Domain::AudioBusesAndMixing | Domain::LocalisationTables => &[Surface::Table],
            Domain::NavigationBaking | Domain::LightingAndLightmapBaking => &[Surface::Form],
        }
    }

    /// The panel kind a layout stores for this editor.
    ///
    /// Derived from the specification's own term rather than chosen, so a domain cannot acquire a
    /// panel identity that disagrees with what the requirement calls it.
    pub fn panel_kind(self) -> String {
        format!("editor-{}", self.spec_term().replace(' ', "-"))
    }

    /// The node types this editor's palette offers, which are the engine's own.
    ///
    /// Empty where this tree declares no vocabulary for the domain: those editors refuse rather
    /// than opening onto an empty canvas. The lists below are compared against
    /// `src/graph/src/lower_*.cpp` by the contract gate, so a node type the engine gains and the
    /// palette does not is RED.
    pub const fn node_types(self) -> &'static [&'static str] {
        match self {
            Domain::GameplayAndUtilityGraphs => SCRIPT_AND_AI_NODES,
            Domain::AbilitiesAndEffects => ABILITY_NODES,
            Domain::AnimationGraphsAndClips => POSE_NODES,
            Domain::Materials => MATERIAL_NODES,
            _ => &[],
        }
    }

    /// The track kinds this editor's timeline OFFERS — what its add-track menu lists, which is
    /// not the same thing as what a document holds.
    ///
    /// Every kind the engine can dispatch, for the sequence editor; the keyed subset for the
    /// animation timeline, which has no camera cuts and no nested sequences to offer.
    pub fn track_kinds(self) -> Vec<TrackKind> {
        match self {
            Domain::SequencesAndCinematics => TrackKind::ALL.to_vec(),
            Domain::AnimationGraphsAndClips => TrackKind::ALL
                .into_iter()
                .filter(|kind| kind.is_keyed() || *kind == TrackKind::Animation)
                .collect(),
            _ => Vec::new(),
        }
    }
}

/// `script.*` and `ai.*`: the vocabulary `lower_script.cpp` and `lower_behaviour.cpp` register.
const SCRIPT_AND_AI_NODES: &[&str] = &[
    "ai.condition",
    "ai.inverter",
    "ai.operator",
    "ai.parallel",
    "ai.plan",
    "ai.root",
    "ai.selector",
    "ai.sequence",
    "ai.state",
    "ai.task",
    "ai.utility",
    "ai.wait",
    "script.add_float",
    "script.add_int",
    "script.branch",
    "script.call",
    "script.const_bool",
    "script.const_float",
    "script.const_int",
    "script.emit_command",
    "script.emit_event",
    "script.entry",
    "script.get_field",
    "script.less_float",
    "script.loop",
    "script.mul_float",
    "script.not",
    "script.query",
    "script.return",
    "script.set_field",
    "script.sub_float",
    "script.wait",
];

/// `ability.*` beside the script vocabulary an ability graph also compiles through.
const ABILITY_NODES: &[&str] = &[
    "ability.refuse",
    "ability.stage",
    "script.add_float",
    "script.add_int",
    "script.branch",
    "script.call",
    "script.const_bool",
    "script.const_float",
    "script.const_int",
    "script.emit_command",
    "script.emit_event",
    "script.entry",
    "script.get_field",
    "script.less_float",
    "script.loop",
    "script.mul_float",
    "script.not",
    "script.query",
    "script.return",
    "script.set_field",
    "script.sub_float",
    "script.wait",
];

/// `material.*`: the vocabulary `src/graph/material/src/lower_material.cpp` registers.
///
/// M11.C TASK 6.1A, AND THE REASON THIS LIST EXISTS AT ALL. Until it did, `Domain::Materials`
/// answered the empty slice, no catalogue was built, and `open(Domain::Materials)` refused with
/// *"this build declares no authoring vocabulary for materials — `material-compiler` owes it"*.
/// M11.c's spike ran the rung's whole authoring path and that refusal is what junction 1 came back
/// with, so the missing half was the ENGINE'S: `src/graph/src/` had four lowerings and no material
/// one.
///
/// Every name below is `"material." + graph_op_name(op)` for one of the material compiler's own
/// `GraphOp`s — which `graph.h` already describes as "the editor's palette, not the IR's opcodes" —
/// plus `material.output`, the root that becomes `set_surface_output` and `set_opacity_output`.
/// `unit.graph_material` asserts that derivation on the engine's side; the contract gate compares
/// this list against those literals.
const MATERIAL_NODES: &[&str] = &[
    "material.add",
    "material.add_closures",
    "material.attribute",
    "material.coat",
    "material.combine",
    "material.constant",
    "material.custom",
    "material.diffuse",
    "material.divide",
    "material.emission",
    "material.field",
    "material.layer_closures",
    "material.lerp",
    "material.multiply",
    "material.one_minus",
    "material.output",
    "material.parameter",
    "material.saturate",
    "material.sheen",
    "material.specular",
    "material.subsurface",
    "material.subtract",
    "material.swizzle",
    "material.texture_sample",
    "material.transmission",
];

/// `pose.*`: the vocabulary `lower_pose.cpp` and `locomotion.cpp` register.
const POSE_NODES: &[&str] = &[
    "pose.additive",
    "pose.blend",
    "pose.blend_mask",
    "pose.clip",
    "pose.ik",
    "pose.layer",
    "pose.ref",
    "pose.state",
    "pose.transition",
];

/// An editor that is open: which domain, and the shared surfaces it is editing on.
///
/// The borrows are the mechanism. A caller cannot obtain a graph canvas except from here, and there
/// is one canvas, so "all node-graph editors share one canvas" is a property of the type rather
/// than a convention a sixth editor could decline.
#[derive(Debug)]
pub struct Session<'a> {
    /// Which editor is open.
    pub domain: Domain,
    /// The one canvas, where this editor is a graph editor.
    pub graph: Option<&'a mut GraphCanvas>,
    /// The one timeline surface, where this editor is a keyed-time editor.
    pub timeline: Option<&'a mut TimelineSurface>,
    /// The one brush surface, where this editor authors spatial strokes.
    pub painting: Option<&'a mut PaintingSurface>,
}

/// The host: one graph canvas, one timeline, one painting surface, and the active editor.
#[derive(Debug)]
pub struct SpecialisedEditors {
    canvas: GraphCanvas,
    graph_drafts: BTreeMap<Domain, GraphCanvas>,
    vfx_document: Option<vfx::VfxDocument>,
    vfx_stage: Option<(usize, vfx::Stage)>,
    timeline: TimelineSurface,
    painting: PaintingSurface,
    active: Option<Domain>,
    catalogues: BTreeMap<Domain, Catalogue>,
}

impl SpecialisedEditors {
    /// The region of the workspace the active specialised editor is drawn in.
    ///
    /// `chrome.rs` has reserved it since M5.5 — *"the active specialised editor: script graph,
    /// animation, materials, sequencing"*. This constant is what fills it.
    pub const REGION: Region = Region::CentreLower;

    /// The host, with local presentation vocabularies loaded.
    ///
    /// Material definitions deliberately are not local: the desktop installs them from the
    /// engine-owned backend catalogue after connecting to a runtime.
    pub fn new() -> Result<Self> {
        let mut catalogues = BTreeMap::new();
        for domain in Domain::ALL {
            // Material definitions are compiler-owned and arrive through editor-backend-services.
            // The desktop editor must not silently accept the historical Rust copy when the
            // runtime is absent or incompatible.
            if domain == Domain::Materials {
                continue;
            }
            let types = domain.node_types();
            if types.is_empty() {
                continue;
            }
            // Script, ability and pose still carry names only; whoever makes one of those editors
            // authorable owes it pins through its own engine-owned catalogue.
            let catalogue = Catalogue::new(
                types
                    .iter()
                    .map(|name| NodeType::new(*name, Vec::new()))
                    .collect(),
            )?;
            catalogues.insert(domain, catalogue);
        }
        Ok(Self {
            canvas: GraphCanvas::new(1),
            graph_drafts: BTreeMap::new(),
            vfx_document: None,
            vfx_stage: None,
            timeline: TimelineSurface::new(1, 30.0)?,
            painting: PaintingSurface::new(1),
            active: None,
            catalogues,
        })
    }

    /// Construct the legacy offline content generator's host.
    ///
    /// The shipped desktop uses [`Self::new`] and installs the engine catalogue. This explicit
    /// exception keeps `cy-author-material`, which generates the committed M11.c beauty content
    /// without a runtime, reproducible until that build tool itself becomes a service client.
    #[doc(hidden)]
    pub fn with_legacy_material_catalogue() -> Result<Self> {
        let mut editors = Self::new()?;
        editors
            .catalogues
            .insert(Domain::Materials, material::catalogue()?);
        Ok(editors)
    }

    /// Replace the bootstrap material vocabulary with the versioned catalogue returned by the
    /// backend. Presentation state remains local; type, pin, and schema identity come from here.
    pub fn install_material_catalogue(&mut self, payload: &[u8]) -> Result<()> {
        let catalogue = Catalogue::new(material::catalogue_from_service(payload)?)?;
        self.catalogues.insert(Domain::Materials, catalogue.clone());
        if self.active == Some(Domain::Materials) {
            self.canvas.replace_catalogue(catalogue);
        }
        Ok(())
    }

    /// Install VFX node definitions supplied by the engine registry through the backend service.
    /// An empty or incompatible catalogue leaves the VFX editor unavailable.
    pub fn install_vfx_catalogue(&mut self, payload: &[u8]) -> Result<()> {
        let nodes = material::catalogue_from_service(payload)?;
        if nodes.is_empty() || nodes.iter().any(|node| !node.name.starts_with("vfx.")) {
            return Err(Problem::new(
                "load the VFX node catalogue",
                "the backend supplied no VFX nodes or a node outside the VFX domain",
            ));
        }
        let catalogue = Catalogue::new(nodes)?;
        self.catalogues.insert(Domain::VfxGraph, catalogue.clone());
        if self.active == Some(Domain::VfxGraph) {
            self.canvas.replace_catalogue(catalogue);
        }
        Ok(())
    }

    /// Begin editing one VFX system on the existing shared graph canvas.
    pub fn start_vfx_document(&mut self, document: vfx::VfxDocument) -> Result<()> {
        document.encode()?;
        let catalogue = self.catalogues.get(&Domain::VfxGraph).ok_or_else(|| {
            Problem::new(
                "open a VFX document",
                "the engine VFX catalogue is unavailable",
            )
        })?;
        let mut probe = GraphCanvas::new(1);
        probe.load(catalogue.clone());
        for (emitter_index, emitter) in document.emitters.iter().enumerate() {
            for stage in &emitter.stages {
                document.open_stage(emitter_index, stage.stage, &mut probe)?;
            }
        }
        self.open(Domain::VfxGraph)?;
        self.vfx_document = Some(document);
        self.vfx_stage = None;
        self.canvas.load(self.canvas.catalogue().clone());
        Ok(())
    }

    /// Close the opened VFX draft after its project asset is removed by undo.
    pub fn close_vfx_document(&mut self) {
        self.vfx_document = None;
        self.vfx_stage = None;
        self.graph_drafts.remove(&Domain::VfxGraph);
        if self.active == Some(Domain::VfxGraph) {
            self.canvas.load(self.canvas.catalogue().clone());
        }
    }

    /// Select one emitter stage, preserving the previously visible stage first.
    pub fn select_vfx_stage(&mut self, emitter: usize, stage: vfx::Stage) -> Result<()> {
        if self.active != Some(Domain::VfxGraph) {
            self.open(Domain::VfxGraph)?;
        }
        let document = self
            .vfx_document
            .as_ref()
            .ok_or_else(|| Problem::new("select a VFX stage", "no VFX document is open"))?;
        let mut next_canvas = self.canvas.clone();
        document.open_stage(emitter, stage, &mut next_canvas)?;
        if let Some((previous_emitter, previous_stage)) = self.vfx_stage {
            self.vfx_document
                .as_mut()
                .expect("checked above")
                .capture_stage(previous_emitter, previous_stage, &self.canvas)?;
        }
        self.canvas = next_canvas;
        self.vfx_stage = Some((emitter, stage));
        Ok(())
    }

    /// Snapshot the current stage into an editable document without changing the canvas.
    pub fn vfx_document_snapshot(&self) -> Result<Option<vfx::VfxDocument>> {
        let Some(mut document) = self.vfx_document.clone() else {
            return Ok(None);
        };
        if let Some((emitter, stage)) = self.vfx_stage {
            let canvas = if self.active == Some(Domain::VfxGraph) {
                &self.canvas
            } else {
                self.graph_drafts
                    .get(&Domain::VfxGraph)
                    .ok_or_else(|| Problem::new("save a VFX document", "stage canvas is missing"))?
            };
            document.capture_stage(emitter, stage, canvas)?;
        }
        Ok(Some(document))
    }

    /// Current VFX system metadata, if one has been opened.
    pub fn vfx_document(&self) -> Option<&vfx::VfxDocument> {
        self.vfx_document.as_ref()
    }

    /// The emitter and stage currently displayed on the shared canvas.
    pub fn active_vfx_stage(&self) -> Option<(usize, vfx::Stage)> {
        self.vfx_stage
    }

    /// Append one emitter to the open system after validating its authoring metadata.
    pub fn add_vfx_emitter(&mut self, emitter: vfx::Emitter) -> Result<usize> {
        let document = self
            .vfx_document
            .as_mut()
            .ok_or_else(|| Problem::new("add a VFX emitter", "no VFX document is open"))?;
        let index = document.emitters.len();
        document.emitters.push(emitter);
        if let Err(problem) = document.encode() {
            document.emitters.pop();
            return Err(problem);
        }
        Ok(index)
    }

    /// Update one emitter's renderer and simulation target without touching its stage graphs.
    pub fn set_vfx_emitter_settings(
        &mut self,
        index: usize,
        path: vfx::SimulationPath,
        renderer: String,
    ) -> Result<()> {
        let document = self
            .vfx_document
            .as_mut()
            .ok_or_else(|| Problem::new("configure a VFX emitter", "no VFX document is open"))?;
        let emitter = document
            .emitters
            .get_mut(index)
            .ok_or_else(|| Problem::new("configure a VFX emitter", "unknown emitter"))?;
        let previous = (
            emitter.path,
            std::mem::replace(&mut emitter.renderer, renderer),
        );
        emitter.path = path;
        if let Err(problem) = document.encode() {
            let emitter = &mut document.emitters[index];
            emitter.path = previous.0;
            emitter.renderer = previous.1;
            return Err(problem);
        }
        Ok(())
    }

    /// Apply a metadata edit atomically. Stage graphs remain on the shared canvas; the edited
    /// document is installed only after the full versioned payload passes validation.
    pub fn edit_vfx_metadata(
        &mut self,
        edit: impl FnOnce(&mut vfx::VfxDocument) -> Result<()>,
    ) -> Result<()> {
        let mut document = self
            .vfx_document
            .as_ref()
            .ok_or_else(|| Problem::new("edit VFX metadata", "no VFX document is open"))?
            .clone();
        edit(&mut document)?;
        document.encode()?;
        self.vfx_document = Some(document);
        Ok(())
    }

    /// Which editor is active, if any.
    pub fn active(&self) -> Option<Domain> {
        self.active
    }

    /// What the reserved region holds: the active editor's panel, or nothing.
    ///
    /// `None` is the honest answer before an editor is opened, and it is what the region held for
    /// the six milestones between reserving it and filling it.
    pub fn region_occupant(&self) -> Option<PanelKey> {
        self.active
            .map(|domain| PanelKey::new(domain.panel_kind()).expect("a hyphenated panel kind"))
    }

    /// Whether this tree can open an editor for the domain.
    pub fn can_open(&self, domain: Domain) -> bool {
        self.catalogues.contains_key(&domain)
            || !domain.track_kinds().is_empty()
            || domain == Domain::Terrain
    }

    /// Every editor this tree can open, in the requirement's order.
    pub fn openable(&self) -> Vec<Domain> {
        Domain::ALL
            .into_iter()
            .filter(|domain| self.can_open(*domain))
            .collect()
    }

    /// Open a specialised editor into the reserved region.
    ///
    /// Refuses by name, naming the row that owes the vocabulary, where this tree has none. A
    /// refused open leaves the previously active editor alone: a failed open that cleared the
    /// region would lose an author's work to a mis-click.
    pub fn open(&mut self, domain: Domain) -> Result<Session<'_>> {
        if !self.can_open(domain) {
            return Err(Problem::new(
                format!("open the {} editor", domain.spec_term()),
                format!(
                    "this build declares no authoring vocabulary for {} — `{}` owes it, and an \
                     empty canvas opened under that name would be a specialised editor that \
                     exists only in a screenshot",
                    domain.spec_term(),
                    domain.owning_row()
                ),
            )
            .with_remedy(format!(
                "open one of: {}",
                self.openable()
                    .iter()
                    .map(|open| open.spec_term())
                    .collect::<Vec<_>>()
                    .join(", ")
            )));
        }
        let changed_domain = self.active != Some(domain);
        if changed_domain
            && let Some(previous) = self.active
            && previous.surfaces().contains(&Surface::Graph)
        {
            self.graph_drafts.insert(previous, self.canvas.clone());
        }
        self.active = Some(domain);
        let surfaces = domain.surfaces();
        if changed_domain && let Some(catalogue) = self.catalogues.get(&domain) {
            if let Some(mut draft) = self.graph_drafts.remove(&domain) {
                draft.replace_catalogue(catalogue.clone());
                self.canvas = draft;
            } else {
                self.canvas.load(catalogue.clone());
            }
        }
        if changed_domain && surfaces.contains(&Surface::Timeline) {
            // The surface is emptied and NOT populated. `domain.track_kinds()` is what the
            // add-track menu offers, not a set of tracks to fabricate: a sequence editor that
            // opened with seventeen tracks nobody authored would put the editor's own furniture
            // into an author's document, and a diff would then report it.
            self.timeline.load(0.0);
        }
        Ok(Session {
            domain,
            graph: surfaces
                .contains(&Surface::Graph)
                .then_some(&mut self.canvas),
            timeline: surfaces
                .contains(&Surface::Timeline)
                .then_some(&mut self.timeline),
            painting: surfaces
                .contains(&Surface::Painting)
                .then_some(&mut self.painting),
        })
    }

    /// Close whatever is open, emptying the region.
    pub fn close(&mut self) {
        self.active = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use cy_editor_core::codec::Writer;

    /// The repository root, from this crate's own manifest. The tests below read the specification
    /// rather than a copy of it, for the reason `tools/editor/selftest.py` gives: a copied fixture
    /// goes stale, and a stale fixture agrees with a broken check.
    fn repository() -> std::path::PathBuf {
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../..")
            .canonicalize()
            .expect("the workspace is inside the repository")
    }

    fn host() -> SpecialisedEditors {
        SpecialisedEditors::new().expect("the built-in catalogues are well formed")
    }

    #[test]
    fn every_editor_the_requirement_names_is_registered_and_names_a_row_that_exists() {
        let specs = repository().join("openspec/specs");
        assert_eq!(
            Domain::ALL.len(),
            16,
            "the requirement enumerates sixteen editors"
        );
        let mut terms: Vec<&str> = Domain::ALL
            .iter()
            .map(|domain| domain.spec_term())
            .collect();
        let before = terms.len();
        terms.sort_unstable();
        terms.dedup();
        assert_eq!(
            terms.len(),
            before,
            "two editors claim the same term in the requirement"
        );

        let mut kinds: Vec<String> = Domain::ALL
            .iter()
            .map(|domain| domain.panel_kind())
            .collect();
        kinds.sort();
        kinds.dedup();
        assert_eq!(
            kinds.len(),
            before,
            "two editors would key the same panel in a layout"
        );

        for domain in Domain::ALL {
            let row = specs.join(domain.owning_row()).join("spec.md");
            assert!(
                row.is_file(),
                "{} refuses by naming `{}`, and no such specification exists — the refusal points \
                 at nothing",
                domain.spec_term(),
                domain.owning_row()
            );
            assert!(
                !domain.surfaces().is_empty(),
                "{} is built on no shared surface at all",
                domain.spec_term()
            );
        }
    }

    #[test]
    fn every_graph_editor_opens_the_same_one_canvas() {
        let mut host = host();
        let graph_editors: Vec<Domain> = Domain::ALL
            .into_iter()
            .filter(|domain| domain.surfaces().contains(&Surface::Graph))
            .collect();
        assert!(
            graph_editors.len() >= 4,
            "the requirement names at least the material, VFX, ability and gameplay graphs"
        );
        let mut canvases = Vec::new();
        for domain in graph_editors {
            let Ok(session) = host.open(domain) else {
                continue;
            };
            let canvas = session.graph.expect("a graph editor opens the canvas");
            canvases.push(canvas.id());
        }
        assert!(
            canvases.len() >= 2,
            "fewer than two graph editors opened, so nothing was compared"
        );
        assert!(
            canvases.windows(2).all(|pair| pair[0] == pair[1]),
            "the graph editors opened onto different canvases: {canvases:?} — which is the sixth \
             bespoke graph editor the requirement forbids"
        );
    }

    #[test]
    fn reopening_the_active_material_editor_preserves_authored_nodes() {
        let mut host = host();
        let mut catalogue = Writer::new();
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(42);
        catalogue.u32(1);
        catalogue.text("material.future");
        catalogue.u32(1);
        catalogue.u32(9);
        catalogue.u8(1);
        catalogue.text("out");
        catalogue.text("value");
        catalogue.u32(0);
        host.install_material_catalogue(&catalogue.finish())
            .expect("engine catalogue installs");

        host.open(Domain::Materials)
            .expect("material editor opens")
            .graph
            .expect("material editor uses the shared graph")
            .add("material.future", graph::Layout { x: 20.0, y: 30.0 })
            .expect("catalogue node is placed");
        let reopened = host.open(Domain::Materials).expect("active editor reopens");
        assert_eq!(
            reopened.graph.expect("shared graph").nodes().count(),
            1,
            "drawing another frame reset the authored material graph"
        );
    }

    #[test]
    fn vfx_editor_opens_on_the_shared_canvas_from_backend_nodes() {
        let mut host = host();
        assert!(host.open(Domain::VfxGraph).is_err());
        let mut catalogue = Writer::new();
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(42);
        catalogue.u32(1);
        catalogue.text("vfx.backend_only");
        catalogue.u32(1);
        catalogue.u32(9);
        catalogue.u8(1);
        catalogue.text("out");
        catalogue.text("float");
        catalogue.u32(0);
        host.install_vfx_catalogue(&catalogue.finish())
            .expect("backend VFX catalogue installs");

        let session = host.open(Domain::VfxGraph).expect("VFX editor opens");
        let canvas = session.graph.expect("VFX uses the shared graph canvas");
        canvas
            .add("vfx.backend_only", graph::Layout { x: 20.0, y: 30.0 })
            .expect("backend-only node is available");
        assert_eq!(canvas.nodes().count(), 1);
        let vfx_canvas = canvas.id();
        let ability_canvas = host
            .open(Domain::AbilitiesAndEffects)
            .expect("ability editor opens")
            .graph
            .expect("ability uses the shared graph canvas")
            .id();
        assert_eq!(vfx_canvas, ability_canvas);
        let restored = host.open(Domain::VfxGraph).expect("VFX draft reopens");
        assert_eq!(
            restored.graph.expect("shared graph").nodes().count(),
            1,
            "switching graph domains must preserve unsaved VFX nodes"
        );
    }

    #[test]
    fn vfx_stage_selection_preserves_edits_across_domains_and_document_snapshots() {
        let mut host = host();
        let mut catalogue = Writer::new();
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(42);
        catalogue.u32(1);
        catalogue.text("vfx.backend_only");
        catalogue.u32(0);
        catalogue.u32(0);
        host.install_vfx_catalogue(&catalogue.finish()).unwrap();

        let mut document = vfx::VfxDocument::new("sparks").unwrap();
        document.emitters.push(vfx::Emitter {
            name: "smoke".into(),
            path: vfx::SimulationPath::GpuPreferred,
            renderer: "Sprite".into(),
            stages: Vec::new(),
            modules: Vec::new(),
            interfaces: Vec::new(),
            capacity: 1024,
            attributes: Vec::new(),
        });
        host.start_vfx_document(document).unwrap();
        host.select_vfx_stage(0, vfx::Stage::Spawn).unwrap();
        host.open(Domain::VfxGraph)
            .unwrap()
            .graph
            .unwrap()
            .add("vfx.backend_only", graph::Layout { x: 20.0, y: 30.0 })
            .unwrap();
        host.open(Domain::AbilitiesAndEffects).unwrap();
        let snapshot = host.vfx_document_snapshot().unwrap().unwrap();
        assert!(
            snapshot.emitters[0].stages[0]
                .canvas
                .contains("node 1 vfx.backend_only")
        );
        host.select_vfx_stage(0, vfx::Stage::Update).unwrap();
        assert_eq!(
            host.open(Domain::VfxGraph)
                .unwrap()
                .graph
                .unwrap()
                .nodes()
                .count(),
            0
        );
        host.select_vfx_stage(0, vfx::Stage::Spawn).unwrap();
        assert_eq!(
            host.open(Domain::VfxGraph)
                .unwrap()
                .graph
                .unwrap()
                .nodes()
                .count(),
            1
        );
        host.set_vfx_emitter_settings(0, vfx::SimulationPath::CpuRequired, "Mesh".into())
            .unwrap();
        let saved = host.vfx_document_snapshot().unwrap().unwrap();
        let reopened = vfx::VfxDocument::decode_text(&saved.encode_text().unwrap()).unwrap();
        assert_eq!(reopened.emitters[0].path, vfx::SimulationPath::CpuRequired);
        assert_eq!(reopened.emitters[0].renderer, "Mesh");
    }

    #[test]
    fn vfx_metadata_edit_keeps_stage_draft_and_rejects_invalid_changes_atomically() {
        let mut host = host();
        let mut catalogue = Writer::new();
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(42);
        catalogue.u32(1);
        catalogue.text("vfx.backend_only");
        catalogue.u32(0);
        catalogue.u32(0);
        host.install_vfx_catalogue(&catalogue.finish()).unwrap();
        let mut document = vfx::VfxDocument::new("sparks").unwrap();
        document.emitters.push(vfx::Emitter {
            name: "smoke".into(),
            path: vfx::SimulationPath::GpuPreferred,
            renderer: "Sprite".into(),
            stages: Vec::new(),
            modules: Vec::new(),
            interfaces: Vec::new(),
            capacity: 1024,
            attributes: Vec::new(),
        });
        host.start_vfx_document(document).unwrap();
        host.select_vfx_stage(0, vfx::Stage::Spawn).unwrap();
        host.open(Domain::VfxGraph)
            .unwrap()
            .graph
            .unwrap()
            .add("vfx.backend_only", graph::Layout { x: 5.0, y: 6.0 })
            .unwrap();
        host.edit_vfx_metadata(|draft| {
            draft.emitters[0].capacity = 2048;
            draft.parameters.push(vfx::Parameter {
                name: "speed".into(),
                kind: "float".into(),
                value: [1.0, 0.0, 0.0, 0.0],
                exposed: true,
            });
            Ok(())
        })
        .unwrap();
        let snapshot = host.vfx_document_snapshot().unwrap().unwrap();
        assert_eq!(snapshot.emitters[0].capacity, 2048);
        assert!(
            snapshot.emitters[0].stages[0]
                .canvas
                .contains("node 1 vfx.backend_only")
        );
        assert!(
            host.edit_vfx_metadata(|draft| {
                draft.emitters[0].capacity = 0;
                Ok(())
            })
            .is_err()
        );
        assert_eq!(host.vfx_document().unwrap().emitters[0].capacity, 2048);
    }

    #[test]
    fn invalid_saved_vfx_stage_does_not_replace_the_open_document() {
        let mut host = host();
        let mut catalogue = Writer::new();
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(42);
        catalogue.u32(1);
        catalogue.text("vfx.backend_only");
        catalogue.u32(0);
        catalogue.u32(0);
        host.install_vfx_catalogue(&catalogue.finish()).unwrap();
        host.start_vfx_document(vfx::VfxDocument::new("current").unwrap())
            .unwrap();
        let mut invalid = vfx::VfxDocument::new("invalid").unwrap();
        invalid.emitters.push(vfx::Emitter {
            name: "smoke".into(),
            path: vfx::SimulationPath::GpuPreferred,
            renderer: "Sprite".into(),
            stages: vec![vfx::StageGraph {
                stage: vfx::Stage::Spawn,
                canvas: "not a canvas".into(),
            }],
            modules: Vec::new(),
            interfaces: Vec::new(),
            capacity: 1024,
            attributes: Vec::new(),
        });
        assert!(host.start_vfx_document(invalid).is_err());
        assert_eq!(host.vfx_document().unwrap().name, "current");
    }

    #[test]
    fn invalid_vfx_catalogue_does_not_open_the_editor() {
        let mut host = host();
        let mut catalogue = Writer::new();
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(1);
        catalogue.u32(42);
        catalogue.u32(1);
        catalogue.text("material.diffuse");
        catalogue.u32(0);
        catalogue.u32(0);
        assert!(host.install_vfx_catalogue(&catalogue.finish()).is_err());
        assert!(host.open(Domain::VfxGraph).is_err());
    }

    #[test]
    fn every_keyed_time_editor_opens_the_same_one_surface() {
        let mut host = host();
        let mut surfaces = Vec::new();
        for domain in Domain::ALL {
            if !domain.surfaces().contains(&Surface::Timeline) {
                continue;
            }
            let session = host.open(domain).expect("a keyed-time editor opens");
            let timeline = session
                .timeline
                .expect("a keyed-time editor opens the surface");
            surfaces.push(timeline.id());
        }
        assert_eq!(
            surfaces.len(),
            2,
            "the requirement names the sequence editor and the animation timeline"
        );
        assert_eq!(
            surfaces[0], surfaces[1],
            "the sequence editor and the animation timeline opened onto different curve surfaces"
        );
    }

    #[test]
    fn a_domain_this_tree_cannot_open_refuses_by_name_and_opens_nothing() {
        let mut host = host();
        let open = host.open(Domain::Terrain).expect("terrain now opens");
        assert_eq!(open.domain, Domain::Terrain);

        let refused = host
            .open(Domain::Foliage)
            .expect_err("foliage has no authoring vocabulary here");
        assert!(
            refused.because.contains("foliage")
                && refused.because.contains(Domain::Foliage.owning_row()),
            "the refusal names neither the editor nor the row that owes it: {refused:?}"
        );
        assert_eq!(
            host.active(),
            Some(Domain::Terrain),
            "a refused open cleared the region and lost what was being edited"
        );
    }

    #[test]
    fn terrain_opens_the_shared_painting_surface() {
        let mut host = host();
        let terrain = host.open(Domain::Terrain).expect("terrain opens");
        assert!(terrain.graph.is_none());
        assert!(terrain.timeline.is_none());
        assert_eq!(terrain.painting.expect("painting surface").id().as_u64(), 1);
    }

    #[test]
    fn the_active_specialised_editor_fills_the_region_chrome_reserved_for_it() {
        let mut host = host();
        assert_eq!(
            SpecialisedEditors::REGION,
            Region::CentreLower,
            "the specialised editors are drawn somewhere other than the region reserved for them"
        );
        assert_eq!(
            Region::CentreLower.contents(),
            "the active specialised editor",
            "chrome.rs no longer reserves CentreLower for this"
        );
        assert_eq!(
            host.region_occupant(),
            None,
            "an unopened region holds a panel"
        );
        host.open(Domain::SequencesAndCinematics)
            .expect("the sequence editor opens");
        let occupant = host
            .region_occupant()
            .expect("the region holds the active editor");
        assert_eq!(occupant.kind(), "editor-sequences-and-cinematics");
        host.close();
        assert_eq!(
            host.region_occupant(),
            None,
            "a closed editor left its panel behind"
        );
    }

    #[test]
    fn opening_a_second_editor_replaces_the_first_in_the_one_region() {
        let mut host = host();
        host.open(Domain::AnimationGraphsAndClips)
            .expect("the animation editor opens");
        assert_eq!(
            host.region_occupant().map(|key| key.kind().to_owned()),
            Some("editor-animation-graphs-and-clips".to_owned())
        );
        host.open(Domain::AbilitiesAndEffects)
            .expect("the ability editor opens");
        assert_eq!(
            host.region_occupant().map(|key| key.kind().to_owned()),
            Some("editor-abilities-and-effects".to_owned()),
            "two specialised editors are drawn at once in a region that holds one"
        );
    }

    #[test]
    fn the_palette_offers_the_engines_own_node_types_and_no_others() {
        let host = host();
        for domain in host.openable() {
            let declared = domain.node_types();
            if declared.is_empty() {
                continue;
            }
            let mut sorted = declared.to_vec();
            sorted.sort_unstable();
            assert_eq!(
                sorted.as_slice(),
                declared,
                "{}'s palette is not in name order, so two builds could offer it differently",
                domain.spec_term()
            );
            for name in declared {
                assert!(
                    name.contains('.'),
                    "{name} is not an engine node type name — those are `domain.node`"
                );
            }
        }
    }
}
