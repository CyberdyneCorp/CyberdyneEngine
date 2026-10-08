//! The editor: every service in one place, and the [`CommandContext`] a command is given.
//!
//! This type is the "explicit application state model" `editor-rust-application` requires —
//! "documents, selection, workspaces, project state, runtime sessions, commands, and notifications —
//! rather than distributing authoritative state across widgets". It is a plain struct with a field
//! per service, which is what makes "inspectable and, where useful, serialisable" cheap: a defect
//! report can name what was open, what was selected, and what the runtime was doing.
//!
//! It implements [`CommandContext`] because the command registry is below it — see that crate's
//! `context` module for why the dependency is inverted — and that is the whole of the wiring between
//! the two layers.

use cy_editor_commands::{Arguments, CommandContext, Outcome, Registry, Scope};
use cy_editor_core::Actor;
use cy_editor_core::ids::{DocumentId, NodeId};
use cy_editor_core::observe::Revision;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;
use cy_editor_protocol::RequestId;
use cy_editor_protocol::message::Message;
use cy_editor_sdk::HostingMode;
use cy_editor_viewport::picking::{
    DocumentFilter, Granularity, PickIntent, PickRequest, PickResolution, PickResponse,
    SelectionMode,
};
use cy_editor_viewport::play::{PlayMode, PlayState};

use crate::asset_catalogue::AssetCatalogueService;
use crate::assets::{AssetImportService, ExternalImportCompletion};
use crate::documents::{CloseDecision, CloseOutcome, DocumentService};
use crate::lightmaps::LightmapBakeService;
use crate::manipulate;
use crate::mirror::{RuntimeMirror, engine_identity};
use crate::notifications::{Notification, NotificationService};
use crate::operations::OperationService;
use crate::picking;
use crate::primitives::{material_slots_of, mesh_of};
use crate::project::ProjectService;
use crate::runtime::RuntimeSession;
use crate::selection::SelectionService;
use crate::semantic_merge::SemanticMergeService;
use crate::settings::SettingsService;
use crate::source_control::SourceControlService;
use crate::source_language::SourceLanguageService;
use crate::source_workspace::SourceWorkspaceService;
use crate::viewports::ViewportService;
use crate::workspace::Workspace;
use crate::{BackendServices, MaterialOperation, MaterialRequestState};

/// Structured progress/result of the most recent script-module reload.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct ReloadReport {
    /// Runtime request identity.
    pub request: u64,
    /// Script module name.
    pub module: String,
    /// Hot-reload generation.
    pub generation: u32,
    /// `pending`, `succeeded`, or `failed`.
    pub state: String,
    /// State classes guaranteed preserved by the runtime protocol.
    pub preserved: Vec<String>,
    /// State classes explicitly dropped. Empty means none.
    pub dropped: Vec<String>,
    /// Actionable failure, when the runtime refused the reload.
    pub diagnostic: Option<String>,
}

struct PendingPick {
    document: DocumentId,
    frame: cy_editor_protocol::FrameId,
    intent: PickIntent,
    mode: SelectionMode,
    cycle: u32,
}

struct PendingGraphSave {
    request: u64,
    reference: String,
    source: String,
    document: DocumentId,
    actor: Actor,
}

fn material_geometry_payload(mut canvas: Vec<u8>, geometry: &[&str]) -> Result<Vec<u8>> {
    if geometry.is_empty() {
        return Ok(canvas);
    }
    if geometry
        .iter()
        .any(|name| name.is_empty() || !name.bytes().all(|byte| byte.is_ascii_alphanumeric()))
    {
        return Err(Problem::new(
            "submit a material geometry request",
            "geometry source names must be nonempty identifiers",
        ));
    }
    let mut request = format!("cymatrequest 1\ngeometry {}\n", geometry.join(",")).into_bytes();
    request.append(&mut canvas);
    Ok(request)
}

fn material_preview_payload(reference: &str, canvas: &str, geometry: &[&str]) -> Result<Vec<u8>> {
    let source = material_geometry_payload(canvas.as_bytes().to_vec(), geometry)?;
    let source = std::str::from_utf8(&source).map_err(|_| {
        Problem::new(
            "preview a material graph",
            "the material request is not UTF-8",
        )
    })?;
    let mut payload = cy_editor_core::codec::Writer::new();
    payload.text(reference);
    payload.text(source);
    Ok(payload.finish())
}

/// The editor's authoritative state.
pub struct Editor {
    /// Open documents.
    pub documents: DocumentService,
    /// What is selected.
    pub selection: SelectionService,
    /// What is open and how it looks. Never dirties a document; see [`crate::workspace`].
    pub workspace: Workspace,
    /// What the editor has to say.
    pub notifications: NotificationService,
    /// Long operations, off the interface thread.
    pub operations: OperationService,
    /// Engine-owned authoring catalogues and asynchronous service request state.
    pub backend: BackendServices,
    /// The engine's navigation service: bake progress, results and query answers. Issue #28.
    pub navmesh: crate::navmesh_service::NavmeshService,
    /// What the engine's terrain module evaluated for the terrain being edited.
    pub terrain: crate::terrain_engine::TerrainEngine,
    /// The engine, or the considered absence of one.
    pub runtime: RuntimeSession,
    /// What keeps the hosted runtime in step with the document, and what carries the engine's gizmo
    /// geometry back. M7 tasks 5b.3 and 5b.4 — see `crate::mirror` for why it watches the history
    /// rather than being called at the commit.
    pub mirror: RuntimeMirror,
    /// What the editor is showing: the viewports, their cameras and their tools.
    ///
    /// A service like the others, and for the same reason: a viewport's transform mode is state a
    /// command changes, a panel reads and an agent asks about, so it cannot live in the window.
    pub viewports: ViewportService,
    /// The project around the documents: its source tree, its build, and what has been built.
    pub project: ProjectService,
    /// The project's importers, run out of process. M8.a task 3.1.
    ///
    /// A service like the others rather than something `asset.import` reaches for privately, for
    /// the reason every other one is here: which importer ran, what it produced and what the cache
    /// said are things a panel shows and an agent asks about.
    pub imports: AssetImportService,
    /// The project's lightmap bakes, run out of process as `cy_build lightmap`. Issue #36.
    pub lightmaps: LightmapBakeService,
    /// Deterministic project files shown by the Content Browser.
    pub asset_catalogue: AssetCatalogueService,
    /// Project settings and per-user preferences, with distinct persistence surfaces.
    pub settings: SettingsService,
    /// Provider-neutral source control with background status refresh.
    pub source_control: SourceControlService,
    /// One pending identity-keyed semantic comparison and its explicit conflict decisions.
    pub semantic_merge: SemanticMergeService,
    /// Project-relative Swift sources and their on-disk fingerprints.
    pub sources: SourceWorkspaceService,
    /// Optional SourceKit-LSP enrichment for Swift buffers.
    pub source_language: SourceLanguageService,
    /// Most recent structured module-reload report.
    pub reload_report: Option<ReloadReport>,
    /// Where the runtime runs a play session. M11.b task 3.1.
    ///
    /// The runtime's, not a viewport's — two viewports showing frames from two machines would be
    /// two runtimes — so it is held once here beside the services rather than on `ViewportState`
    /// where `PlayState` lives. It is `pub` for the same reason the services are: a panel shows the
    /// badge and an agent asks about it.
    pub play_mode: PlayMode,
    /// Who the editor believes is acting. Attribution, not authorisation.
    actor: Actor,
    /// The directories the invocation in progress may touch, and the scope that says so.
    ///
    /// Set by [`Editor::invoke`] for the duration and restored afterwards, exactly as the actor is,
    /// and for the same reason: the value belongs to the invocation rather than to the editor. A
    /// command that touches a path reads it through
    /// [`cy_editor_commands::CommandContext::permitted_paths`], because only the command knows which
    /// of its arguments is a path.
    permitted: (String, Vec<String>),
    pending_reloads: std::collections::BTreeMap<u64, (String, u32)>,
    pending_picks: std::collections::BTreeMap<u64, PendingPick>,
    pending_graph_save: Option<PendingGraphSave>,
    graph_save_status: String,
    reload_revision: Revision,
}

impl Default for Editor {
    fn default() -> Self {
        Self::new(Actor::human("user"))
    }
}

impl Editor {
    /// A new editor with no documents, no selection and no runtime.
    #[must_use]
    pub fn new(actor: Actor) -> Self {
        let project = ProjectService::default();
        let mut documents = DocumentService::new();
        // Rooted from the start, not only when a project is named — otherwise an editor started the
        // way the artefacts start it would open every world empty. But rooted only when the working
        // directory SAYS it is a project: `ProjectService::default()` roots at wherever the process
        // was launched, and treating that as a project means an editor started in a source tree
        // saves worlds into it. That is not hypothetical; see `ProjectService::is_declared`.
        if project.is_declared() {
            documents.rooted_at(project.root());
        }
        Self {
            documents,
            // `InEditor` by default, which is the mode this editor has always been in without being
            // able to name it. See `PlayMode`: there is no "unset" and no nearest-match.
            play_mode: PlayMode::default(),
            selection: SelectionService::new(),
            workspace: Workspace::new(),
            notifications: NotificationService::new(),
            operations: OperationService::new(),
            backend: BackendServices::new(),
            navmesh: crate::navmesh_service::NavmeshService::new(),
            terrain: crate::terrain_engine::TerrainEngine::new(),
            runtime: RuntimeSession::none(),
            mirror: RuntimeMirror::new(),
            viewports: ViewportService::new(),
            imports: AssetImportService::new(project.root()),
            lightmaps: LightmapBakeService::new(project.root()),
            asset_catalogue: AssetCatalogueService::new(project.root()),
            settings: SettingsService::default(),
            source_control: SourceControlService::default(),
            semantic_merge: SemanticMergeService::default(),
            sources: SourceWorkspaceService::new(project.root()),
            source_language: SourceLanguageService::new(project.root()),
            reload_report: None,
            project,
            actor,
            permitted: unrestricted(),
            pending_reloads: std::collections::BTreeMap::new(),
            pending_picks: std::collections::BTreeMap::new(),
            pending_graph_save: None,
            graph_save_status: "idle".into(),
            reload_revision: Revision::INITIAL,
        }
    }

    /// Point the editor at a project on disk.
    ///
    /// The documents learn the root at the same moment, because a document service that did not
    /// know where the project was is precisely the state M5.5 shipped: `open` produced a name and
    /// an empty schema, and there was nowhere for a world to come from.
    ///
    /// Under the same rule [`Editor::new`] applies, and deliberately not a looser one: a project is
    /// a directory that says it is one. Two rules — a strict one for the implicit root and a lax one
    /// for a named root — would mean the editor wrote worlds in one configuration and not the other,
    /// which is exactly the kind of difference nobody finds until it matters.
    #[must_use]
    pub fn with_project(mut self, project: ProjectService) -> Self {
        if project.is_declared() {
            self.documents.rooted_at(project.root());
        }
        self.imports.rooted_at(project.root());
        self.lightmaps.rooted_at(project.root());
        self.asset_catalogue = AssetCatalogueService::new(project.root());
        self.sources = SourceWorkspaceService::new(project.root());
        self.source_language = SourceLanguageService::new(project.root());
        self.project = project;
        self
    }

    /// Import through something else — a test's recording double, or another way of reaching the
    /// importers.
    ///
    /// After [`Editor::with_project`] and not before: pointing the editor at a project finds that
    /// project's own importer, which would replace whatever was set here.
    #[must_use]
    pub fn with_importer(mut self, imports: AssetImportService) -> Self {
        self.imports = imports;
        self
    }

    /// Bake through something else — a test's recording double.
    ///
    /// After [`Editor::with_project`], for the reason [`Editor::with_importer`] gives.
    #[must_use]
    pub fn with_lightmap_baker(mut self, lightmaps: LightmapBakeService) -> Self {
        self.lightmaps = lightmaps;
        self
    }

    /// Start baking a level's lightmaps, returning the operation's stable request identity.
    pub fn bake_lightmaps(&mut self, description: &str, output: &str) -> Result<u64> {
        self.lightmaps
            .start(&mut self.operations, description, output)
    }

    /// Ask a lightmap bake to stop: `request`, or the most recently started.
    pub fn cancel_lightmap_bake(&mut self, request: Option<u64>) -> Result<u64> {
        let request = request.or(self.lightmaps.latest()).ok_or_else(|| {
            Problem::new(
                "cancel a lightmap bake",
                "no lightmap bake has been started",
            )
        })?;
        if self.operations.cancel(request) {
            Ok(request)
        } else {
            Err(Problem::new(
                "cancel a lightmap bake",
                format!("no operation #{request} is running"),
            ))
        }
    }

    /// Queue an external source for staging and import, returning its stable request identity.
    pub fn import_external(
        &mut self,
        source: std::path::PathBuf,
        destination: String,
    ) -> Result<u64> {
        self.imports
            .start_external(&mut self.operations, source, destination)
    }

    /// Drain completed visible imports without waiting.
    pub fn take_completed_imports(&mut self) -> Vec<ExternalImportCompletion> {
        self.imports.take_completed()
    }

    /// Act as somebody else — an agent, with its session and its stated intent.
    ///
    /// Every transaction produced while this is in force carries the given actor, which is how "an
    /// agent's move is a human's move" is true of the resulting history: the same commands, the same
    /// operations, a different name on the entry.
    pub fn acting_as(&mut self, actor: Actor) {
        self.actor = actor;
    }

    /// The mode the editor is hosting its engine in.
    #[must_use]
    pub const fn hosting_mode(&self) -> HostingMode {
        self.runtime.mode()
    }

    /// Submit the visible material canvas to the engine-owned asynchronous service.
    pub fn request_material(
        &mut self,
        operation: MaterialOperation,
        canvas: Vec<u8>,
    ) -> Result<cy_editor_protocol::RequestId> {
        if operation == MaterialOperation::Compile {
            // Authored meshes belong to AuthoredFrame, not the first-light preview renderer.
            self.backend.clear_material_preview_targets();
        }
        self.backend
            .request_material(&self.runtime, operation, canvas)
    }

    /// Submit a material graph with its assigned geometry sources. The engine compiler owns the
    /// source names and the unsupported-path diagnostic; this envelope only carries the selection.
    pub fn request_material_for_geometry(
        &mut self,
        operation: MaterialOperation,
        canvas: Vec<u8>,
        geometry: &[&str],
    ) -> Result<cy_editor_protocol::RequestId> {
        if geometry.is_empty() {
            return self.request_material(operation, canvas);
        }
        if !matches!(
            operation,
            MaterialOperation::Validate | MaterialOperation::Compile | MaterialOperation::Author
        ) {
            return Err(Problem::new(
                "submit a material geometry request",
                "geometry sources apply only to validation, compilation, and authoring",
            ));
        }
        self.request_material(operation, material_geometry_payload(canvas, geometry)?)
    }

    /// Distinct geometry sources assigned to a material in the active scene.
    pub fn assigned_material_geometry(&self, reference: Option<&str>) -> Vec<&'static str> {
        let Some(reference) = reference else {
            return Vec::new();
        };
        let Some(document) = self
            .workspace
            .active()
            .and_then(|id| self.documents.get(id))
        else {
            return Vec::new();
        };
        let mut sources = Vec::new();
        let mut static_mesh = false;
        let mut virtual_geometry = false;
        for node in document.content().nodes() {
            let Some(mesh) = mesh_of(document, node) else {
                continue;
            };
            if !material_slots_of(document, node)
                .iter()
                .any(|material| material == reference)
            {
                continue;
            }
            // CYVG is the Engine's cooked virtual-geometry format. An authored mesh reference to
            // that format must reach its named compiler path, even though this viewport currently
            // draws only static meshes.
            if std::path::Path::new(&mesh)
                .extension()
                .is_some_and(|extension| extension.eq_ignore_ascii_case("cyvg"))
            {
                virtual_geometry = true;
            } else {
                static_mesh = true;
            }
        }
        if static_mesh {
            sources.push("StaticMesh");
        }
        if virtual_geometry {
            sources.push("VirtualGeometry");
        }
        let schema = document.schema();
        if let (Some(terrain), Some(layer)) = (
            schema.type_named("TerrainAuthoring"),
            schema.type_named("TerrainMaterialLayer"),
        ) && let Some(material) = layer.field_named("material")
            && document.content().nodes().any(|node| {
                let Some(parent) = document.content().node(node).and_then(|item| item.parent)
                else {
                    return false;
                };
                document.content().has_component(parent, terrain.id)
                    && matches!(
                        document.content().field(node, layer.id, material.id),
                        Some(cy_editor_core::value::Value::Text(asset)) if asset == reference
                    )
            })
        {
            sources.push("Terrain");
        }
        sources
    }

    /// Ask the engine to compile an editable VFX document.
    pub fn request_vfx_compile(&mut self, source: String) -> Result<cy_editor_protocol::RequestId> {
        let bundled =
            crate::vfx_document::bundle_source(source, |path| self.project.read_source(path))?;
        self.backend.request_vfx_compile(&self.runtime, bundled)
    }

    /// Load an unsaved VFX document into the engine's isolated preview world.
    pub fn request_vfx_preview_load(
        &mut self,
        source: String,
    ) -> Result<cy_editor_protocol::RequestId> {
        let bundled =
            crate::vfx_document::bundle_source(source, |path| self.project.read_source(path))?;
        self.backend
            .request_vfx_preview_load(&self.runtime, bundled)
    }

    /// Control the engine VFX preview.
    pub fn request_vfx_preview_action(
        &mut self,
        action: crate::vfx_preview::VfxPreviewAction,
    ) -> Result<cy_editor_protocol::RequestId> {
        self.backend
            .request_vfx_preview_action(&self.runtime, action)
    }

    /// Advance the running VFX preview by one frame interval.
    pub fn request_vfx_preview_step(
        &mut self,
        seconds: f32,
    ) -> Result<cy_editor_protocol::RequestId> {
        self.backend
            .request_vfx_preview_step(&self.runtime, seconds)
    }

    /// Change one exposed parameter in the engine instance without compiling.
    pub fn request_vfx_preview_parameter(
        &mut self,
        name: &str,
        values: &[f32],
    ) -> Result<cy_editor_protocol::RequestId> {
        self.backend
            .request_vfx_preview_parameter(&self.runtime, name, values)
    }

    /// Apply an unsaved canvas to the hosted authored scene without writing the graph asset.
    pub fn preview_material_graph(
        &mut self,
        reference: &str,
        canvas: &str,
    ) -> Result<cy_editor_protocol::RequestId> {
        let geometry = self.assigned_material_geometry(Some(reference));
        let payload = material_preview_payload(reference, canvas, &geometry)?;
        self.request_material(MaterialOperation::Preview, payload)
    }

    /// Cooperatively cancel the currently pending material operation.
    pub fn cancel_material_request(&self) -> Result<()> {
        self.backend.cancel_material(&self.runtime)
    }

    fn finish_graph_save(&mut self) {
        let Some(pending) = self.pending_graph_save.as_ref() else {
            return;
        };
        let request = pending.request;
        let result = match self.backend.material_request_state() {
            MaterialRequestState::Authored {
                request: done,
                graph,
            } if done.as_u64() == request => Some(Ok(graph.clone())),
            MaterialRequestState::Failed {
                request: Some(done),
                diagnostics,
            } if done.as_u64() == request => Some(Err(format!(
                "{} diagnostic(s): {}",
                diagnostics.len(),
                diagnostics
                    .first()
                    .map_or("engine rejected graph", |item| item.message.as_str())
            ))),
            MaterialRequestState::Cancelled { request: done } if done.as_u64() == request => {
                Some(Err("request cancelled".into()))
            }
            _ => None,
        };
        let Some(result) = result else { return };
        let pending = self.pending_graph_save.take().expect("pending graph save");
        let reference = pending.reference;
        let source = pending.source;
        let prior = crate::material_graph::read(self.project.root(), &reference).ok();
        let prior_graph = crate::material_graph::paths(self.project.root(), &reference)
            .ok()
            .and_then(|(path, _)| std::fs::read_to_string(path).ok());
        let outcome = result.and_then(|graph| {
            if self.documents.get(pending.document).is_none() {
                return Err("the scene document closed before the graph save completed".to_owned());
            }
            crate::material_graph::save(self.project.root(), &reference, &source, &graph)
                .map_err(|problem| problem.because)?;
            let document = self
                .documents
                .get_mut(pending.document)
                .expect("checked above");
            document.begin(format!("Save material graph {reference}"), pending.actor);
            document
                .record(cy_editor_documents::operation::Operation::Domain {
                    node: None,
                    kind: format!(
                        "{}{}",
                        crate::material_graph::GRAPH_DOMAIN_PREFIX,
                        reference
                    ),
                    before: crate::material_graph::encode_pair(
                        prior_graph.as_deref(),
                        prior.as_deref(),
                    ),
                    after: crate::material_graph::encode_pair(Some(&graph), Some(&source)),
                })
                .map_err(|problem| problem.to_string())?;
            // Generated Inspector fields belong to the same save action. Their nested
            // transactions join this one, so one undo restores both files and scene fields.
            let property_sync =
                crate::material_parameters::sync(self, &reference, prior.as_deref());
            self.documents
                .get_mut(pending.document)
                .expect("checked above")
                .commit()
                .map_err(|problem| problem.to_string())?;
            Ok(property_sync)
        });
        self.graph_save_status = match outcome {
            Ok(Ok(_)) => {
                self.notifications
                    .post(Notification::info(format!("Saved {reference}")));
                format!("saved: {reference}")
            }
            Ok(Err(problem)) => {
                self.notifications.post(Notification::error(
                    "Material saved; scene properties could not be synced",
                    Problem::new("sync graph properties", problem.clone()),
                ));
                format!("saved: {reference}; property sync failed: {problem}")
            }
            Err(problem) => {
                self.notifications.post(Notification::error(
                    "Material save failed",
                    Problem::new("save material graph", problem.clone()),
                ));
                format!("failed: {problem}")
            }
        };
    }

    /// Open a document, offering recovery when its journal holds anything.
    ///
    /// Offering rather than performing: the notification says how many transactions are recoverable
    /// and the user decides, because an editor that recovered without asking would overwrite work
    /// somebody had decided to abandon.
    pub fn open_document(&mut self, primary_asset: &str) -> Result<DocumentId> {
        let (id, recovery, report) = self.documents.open_reporting(primary_asset)?;
        self.workspace.opened(id);
        if report.nodes > 0 {
            self.notifications.post(Notification::info(format!(
                "Opened {primary_asset}: {} node(s), {} component type(s)",
                report.nodes, report.types
            )));
        }
        if let Some(recovery) = recovery {
            // "Has", not "recovered": nothing has been replayed. `Document::recover` is what
            // replays, and it is called only when somebody accepts the offer.
            self.notifications.post(Notification::warning(format!(
                "{} has {} recoverable transaction(s) from a previous session{}",
                primary_asset,
                recovery.count(),
                if recovery.truncated {
                    ", and its journal ends mid-record"
                } else {
                    ""
                }
            )));
        }
        Ok(id)
    }

    /// Close a document and update the workspace only after the document service succeeds.
    ///
    /// This is the coordination point a tab, command, script, or agent uses. Keeping it here avoids
    /// the two half-closed states a caller could otherwise produce: a document with no tab, or a tab
    /// whose document was already removed after a failed save.
    pub fn close_document(
        &mut self,
        id: DocumentId,
        decision: Option<CloseDecision>,
    ) -> Result<CloseOutcome> {
        let was_active = self.workspace.active() == Some(id);
        let outcome = self.documents.request_close(id, decision)?;
        if outcome == CloseOutcome::Closed {
            self.workspace.closed(id);
            if was_active {
                self.selection.set(Selection::new());
            }
        }
        Ok(outcome)
    }

    /// Invoke a command by identifier, under a scope.
    ///
    /// The one entry point. A menu, a shortcut, the palette, a script, a test and an agent are all
    /// callers of this, which is what makes `editor-rust-application`'s "One action, six entry
    /// points" true rather than aspirational.
    pub fn invoke(
        &mut self,
        registry: &Registry,
        id: &str,
        scope: &Scope,
        arguments: &Arguments,
    ) -> Result<Outcome> {
        // The directory half of the scope travels with the invocation. The registry checks the
        // effect class and the documents; it cannot check a path, because it does not know which
        // argument is one — so the command does, and this is how it is told.
        let previous = std::mem::replace(
            &mut self.permitted,
            (scope.name.clone(), scope.directories.clone()),
        );
        let outcome = registry.invoke(id, scope, self, arguments);
        self.permitted = previous;
        outcome
    }

    /// Send a viewport pick to the runtime and retain the click intent until its answer arrives.
    pub fn request_pick(&mut self, pick: PickRequest, mode: SelectionMode) -> Result<RequestId> {
        let document = self.workspace.active().ok_or_else(|| {
            Problem::new("pick a scene actor", "no world is open")
                .with_remedy("open a world before selecting an actor")
        })?;
        let request = self.runtime.pick(pick.frame, pick.encode())?;
        let cycle = match pick.intent {
            PickIntent::Click { x, y } => self.viewports.focused_mut().click(x, y),
            _ => 0,
        };
        if self.pending_picks.len() >= 128 {
            self.pending_picks.pop_first();
        }
        self.pending_picks.insert(
            request.as_u64(),
            PendingPick {
                document,
                frame: pick.frame,
                intent: pick.intent,
                mode,
                cycle,
            },
        );
        Ok(request)
    }

    fn accept_pick_message(&mut self, message: &Message) {
        let Message::Picked {
            request,
            candidates,
        } = message
        else {
            return;
        };
        let Some(pending) = self.pending_picks.remove(&request.as_u64()) else {
            return;
        };
        if self.workspace.active() != Some(pending.document) {
            return;
        }
        let Ok(response) = PickResponse::decode(candidates) else {
            self.notifications.post(Notification::warning(
                "The runtime returned an unreadable pick answer",
            ));
            return;
        };
        if response.frame != pending.frame {
            return;
        }
        let Some(document) = self.documents.get(pending.document) else {
            return;
        };
        let identities = picking::identity_map(document);
        let filter = DocumentFilter::default();
        let resolution = PickResolution {
            identities: &identities,
            document,
            filter: &filter,
            granularity: Granularity::Instance,
        };
        let mut selection = self.selection.get().clone();
        match picking::resolve(
            candidates,
            &resolution,
            &pending.intent,
            pending.cycle,
            &mut selection,
            pending.mode,
        ) {
            Ok(_) => self.selection.set(selection),
            Err(problem) => self.notifications.post(Notification::error(
                "The runtime pick could not be applied",
                problem,
            )),
        }
    }

    /// The terrain being edited: the selected terrain root, else the active world's first.
    #[must_use]
    pub fn edited_terrain(&self) -> Option<NodeId> {
        let document = self.documents.get(self.workspace.active()?)?;
        let is_terrain =
            |node: NodeId| crate::terrain::TerrainStack::read(document, node).is_some();
        self.selection
            .get()
            .nodes()
            .find(|node| is_terrain(*node))
            .or_else(|| {
                document
                    .content()
                    .roots()
                    .iter()
                    .copied()
                    .find(|node| is_terrain(*node))
            })
    }

    /// The `terrain.evaluate` request for the edited terrain's current stack.
    #[must_use]
    pub fn edited_terrain_request(
        &self,
    ) -> Option<(NodeId, cy_editor_core::problem::Result<Vec<u8>>)> {
        let terrain = self.edited_terrain()?;
        let document = self.documents.get(self.workspace.active()?)?;
        crate::terrain_engine::evaluation_request(document, terrain)
            .map(|request| (terrain, request))
    }

    /// Save an audio asset as one undoable transaction on the active scene's history, and send a
    /// mixer to the engine when one is attached. Issue #29.
    /// Save a gameplay graph as one transaction in the open world's history. Issue #29.
    fn save_script_graph(&mut self, reference: &str, source: &str) -> Result<()> {
        crate::script_graph::validate_reference(reference)?;
        crate::script_graph::ScriptGraph::decode(source)?;
        if self.save_graph_source(
            reference,
            source,
            crate::script_graph::DOMAIN_PREFIX,
            "gameplay graph",
        )? {
            self.reload_running_graph(reference, Some(source));
        }
        Ok(())
    }

    /// Save an animation graph as one transaction in the open world's history. Issue #29. A graph
    /// the engine is previewing is previewed again from its new text.
    fn save_animation_graph(&mut self, reference: &str, source: &str) -> Result<()> {
        crate::animation_graph::validate_reference(reference)?;
        crate::script_graph::ScriptGraph::decode(source)?;
        if self.save_graph_source(
            reference,
            source,
            crate::animation_graph::DOMAIN_PREFIX,
            "animation graph",
        )? {
            self.preview_follows(reference, Some(source));
        }
        Ok(())
    }

    /// Write a graph's text and record its prior and new contents as one transaction in the open
    /// world's history, under `prefix` and the reference. False when the file already held `source`,
    /// which records nothing.
    fn save_graph_source(
        &mut self,
        reference: &str,
        source: &str,
        prefix: &str,
        what: &str,
    ) -> Result<bool> {
        let document_id = self.workspace.active().ok_or_else(|| {
            Problem::new(
                format!("save a {what}"),
                "no scene document is active for undo history",
            )
            .with_remedy(format!("open a world; {what} edits undo in its history"))
        })?;
        let prior = if self.project.source_exists(reference) {
            Some(self.project.read_source(reference)?)
        } else {
            None
        };
        if prior.as_deref() == Some(source) {
            return Ok(false);
        }
        self.project.put_source(reference, Some(source))?;
        let recorded = (|| {
            let document = self.documents.get_mut(document_id).ok_or_else(|| {
                Problem::new(format!("save a {what}"), "the active scene document closed")
            })?;
            document.begin(format!("Save {what} {reference}"), self.actor.clone());
            document.record(cy_editor_documents::operation::Operation::Domain {
                node: None,
                kind: format!("{prefix}{reference}"),
                before: crate::project::encode_source(prior.as_deref()),
                after: crate::project::encode_source(Some(source)),
            })?;
            document.commit()
        })();
        if let Err(problem) = recorded {
            let _ = self.project.put_source(reference, prior.as_deref());
            return Err(problem);
        }
        Ok(true)
    }

    /// THE PREVIEW FOLLOWS THE FILE: a graph the engine is previewing is sent again with its new
    /// text after an edit, an undo or a redo, so the character shows what the file now says. A
    /// graph undone out of existence stops the preview.
    fn preview_follows(&mut self, reference: &str, source: Option<&str>) {
        let Some(settings) = self.backend.animation.settings().cloned() else {
            return;
        };
        if settings.reference != reference || !self.runtime.is_connected() {
            return;
        }
        let _ = match source {
            Some(source) => self
                .backend
                .animation
                .preview(&self.runtime, settings, source),
            None => self.backend.animation.stop(&self.runtime),
        };
    }

    /// HOT RELOAD (#84): a graph saved while Play runs it is recompiled there and swapped in at
    /// the next tick. Nothing is sent outside Play or for a graph Play does not run, and a graph
    /// removed by an undo is left running.
    fn reload_running_graph(&mut self, reference: &str, source: Option<&str>) {
        let Some(source) = source else { return };
        let graph = crate::script_graph::graph_name(reference);
        let running = self.backend.script.state().is_some_and(|state| {
            state.playing
                && state
                    .instances
                    .iter()
                    .any(|instance| instance.graph == graph)
        });
        if running && self.runtime.is_connected() {
            let _ = self.backend.script.reload(&self.runtime, reference, source);
        }
    }

    fn save_audio_asset(&mut self, reference: &str, source: &str) -> Result<()> {
        let is_mixer = reference.ends_with(".cymixer");
        if is_mixer {
            crate::audio::validate_mixer_reference(reference)?;
            crate::audio::Mixer::decode(source)?;
        } else {
            crate::audio::validate_cue_reference(reference)?;
            crate::audio::Cue::decode(source)?;
        }
        let document_id = self.workspace.active().ok_or_else(|| {
            Problem::new(
                "save an audio asset",
                "no scene document is active for undo history",
            )
            .with_remedy("open a world; audio edits undo in its history")
        })?;
        let prior = if self.project.source_exists(reference) {
            Some(self.project.read_source(reference)?)
        } else {
            None
        };
        self.project.put_source(reference, Some(source))?;
        let recorded = (|| {
            let document = self.documents.get_mut(document_id).ok_or_else(|| {
                Problem::new("save an audio asset", "the active scene document closed")
            })?;
            document.begin(format!("Save audio {reference}"), self.actor.clone());
            document.record(cy_editor_documents::operation::Operation::Domain {
                node: None,
                kind: format!("{}{reference}", crate::audio::DOMAIN_PREFIX),
                before: crate::project::encode_source(prior.as_deref()),
                after: crate::project::encode_source(Some(source)),
            })?;
            document.commit()
        })();
        if let Err(problem) = recorded {
            let _ = self.project.put_source(reference, prior.as_deref());
            return Err(problem);
        }
        if is_mixer && self.runtime.is_connected() {
            self.backend.audio.request(
                &self.runtime,
                crate::audio::MIXER_APPLY,
                source.as_bytes().to_vec(),
            )?;
        }
        Ok(())
    }

    /// Send the project's mixer to an engine that has not had it since it connected.
    fn send_project_mixer(&mut self) {
        if !self.runtime.is_connected() || !self.backend.audio.needs_mixer() {
            return;
        }
        if !self.project.source_exists(crate::audio::DEFAULT_MIXER) {
            self.backend.audio.mixer_absent();
            return;
        }
        let sent = self
            .project
            .read_source(crate::audio::DEFAULT_MIXER)
            .and_then(|source| {
                self.backend.audio.request(
                    &self.runtime,
                    crate::audio::MIXER_APPLY,
                    source.into_bytes(),
                )
            });
        if let Err(problem) = sent {
            self.backend.audio.mixer_absent();
            self.notifications.post(Notification::error(
                "The project's mixer could not be sent to the engine",
                problem,
            ));
        }
    }

    /// One frame of the editor's own housekeeping.
    ///
    /// Everything here is bounded and non-blocking: drain what the runtime sent, forget settled
    /// operations. It is what an interface calls once a frame, and it is deliberately the only thing
    /// that has to be called at that rate — a view model rebuilds when a revision moved, not
    /// because a frame happened.
    pub fn pump(&mut self) {
        let messages = self.runtime.pump(&mut self.notifications);
        #[cfg(unix)]
        if self.runtime.reconnect_if_due() {
            self.mirror.runtime_restarted();
            self.notifications.post(Notification::info(
                "The hosted runtime restarted and the editor reconnected",
            ));
        }
        if !self.runtime.is_connected() {
            self.set_local_play_state(PlayState::Editing);
            self.pending_picks.clear();
        }
        // THE GIZMO ARRIVES HERE. `RuntimeMirror` takes a published layout, refuses one that
        // belongs to a frame the viewport is not showing, and hands what survives to the viewport
        // panel — which draws nothing and hit-tests everything, because the geometry is the
        // engine's (`editor-viewport-and-gizmos`). Every other message is drained rather than
        // queued, which is what keeps the channel bounded in a build with no viewport.
        for message in &messages {
            self.accept_pick_message(message);
            if let Message::Playing { state, detail, .. } = message {
                let confirmed = match state.as_str() {
                    "playing" => Some(PlayState::Playing),
                    "paused" => Some(PlayState::Paused),
                    "editing" => Some(PlayState::Editing),
                    _ => None,
                };
                if let Some(confirmed) = confirmed {
                    self.set_local_play_state(confirmed);
                }
                if detail.contains("unavailable") {
                    self.notifications
                        .post(Notification::warning(detail.clone()));
                }
            }
            if let Some(problem) = self.backend.accept(message) {
                self.notifications.post(Notification::error(
                    "The material backend request failed",
                    problem,
                ));
            }
            if let Some(problem) = self.terrain.accept(message) {
                self.notifications.post(Notification::error(
                    "The engine refused the terrain",
                    problem,
                ));
            }
            self.accept_reload_message(message);
            if let Some(problem) = self.navmesh.accept(message) {
                self.notifications.post(Notification::error(
                    "The navigation request failed",
                    problem,
                ));
            }
            self.mirror.accept(message, self.viewports.focused());
        }
        if let Some(problem) = self.backend.maintain(&self.runtime) {
            self.notifications.post(Notification::error(
                "The material backend is unavailable",
                problem,
            ));
        }
        let wanted = self.edited_terrain_request();
        if let Some(problem) = self.terrain.maintain(&self.runtime, wanted) {
            self.notifications.post(Notification::error(
                "The terrain could not be sent to the engine",
                problem,
            ));
        }
        self.send_project_mixer();
        self.finish_graph_save();
        self.finish_nav_bake();
        if !self.runtime.is_connected() && !self.pending_reloads.is_empty() {
            let pending = std::mem::take(&mut self.pending_reloads);
            if let Some((request, (module, generation))) = pending.into_iter().next_back() {
                self.reload_report = Some(ReloadReport {
                    request,
                    module,
                    generation,
                    state: "failed".into(),
                    preserved: Vec::new(),
                    dropped: Vec::new(),
                    diagnostic: Some("the runtime stopped before reporting reload state".into()),
                });
                self.bump_reload_revision();
            }
        }
        // WHERE THE CONTENT IS, once. A viewport opens at the origin looking down −Z, and the
        // runtime is the only side that knows where its world is; without this the first view is
        // inside whatever sits at the origin and every manipulation reports "0.000 m", because a
        // pivot at the camera's own position has no screen-space direction. See
        // `cy_editor_protocol::Message::ViewSuggested` for why the runtime may say this exactly
        // once.
        let _ = self.mirror.frame_the_world(self.viewports.focused_mut());

        // And the other direction: what the document committed or undid since the last frame, so
        // that the object the engine draws the gizmo on is where the editor believes it is. See
        // `crate::mirror::RuntimeMirror::sync`.
        let identities: Vec<u64> = self.selection.get().nodes().map(engine_identity).collect();
        let document = self
            .workspace
            .active()
            .and_then(|id| self.documents.get(id));
        self.mirror.sync(
            &self.runtime,
            document,
            self.viewports.focused(),
            identities,
        );
        self.operations.retain_running();
    }

    fn accept_reload_message(&mut self, message: &Message) {
        match message {
            Message::Reloaded {
                request,
                module,
                generation,
            } => {
                self.pending_reloads.remove(&request.as_u64());
                self.reload_report = Some(ReloadReport {
                    request: request.as_u64(),
                    module: module.clone(),
                    generation: *generation,
                    state: "succeeded".into(),
                    preserved: vec!["live world state".into()],
                    dropped: Vec::new(),
                    diagnostic: None,
                });
                self.notifications.post(Notification::info(format!(
                    "Reloaded {module} generation {generation}; preserved: live world state; dropped: none"
                )));
                self.bump_reload_revision();
            }
            Message::Rejected {
                request,
                reason,
                remedy,
            } if self.pending_reloads.remove(&request.as_u64()).is_some() => {
                let previous = self.reload_report.take();
                self.reload_report = Some(ReloadReport {
                    request: request.as_u64(),
                    module: previous
                        .as_ref()
                        .map_or_else(String::new, |report| report.module.clone()),
                    generation: previous.as_ref().map_or(0, |report| report.generation),
                    state: "failed".into(),
                    preserved: Vec::new(),
                    dropped: Vec::new(),
                    diagnostic: Some(format!("{reason}; {remedy}")),
                });
                self.bump_reload_revision();
            }
            _ => {}
        }
    }

    fn bump_reload_revision(&mut self) {
        self.reload_revision = Revision::from_u64(self.reload_revision.as_u64() + 1);
    }

    /// Observable revision of pending or completed module-reload state.
    #[must_use]
    pub const fn reload_revision(&self) -> Revision {
        self.reload_revision
    }

    /// A revision that moves when anything a status bar shows has moved.
    ///
    /// The sum of the parts, which is the honest thing for a summary view to watch: it rebuilds a
    /// little more often than strictly necessary and never misses a change, and the alternative —
    /// a status bar with six watches — is six chances to forget one.
    #[must_use]
    pub fn summary_revision(&self) -> Revision {
        Revision::from_u64(
            self.documents.revision().as_u64()
                + self.selection.revision().as_u64()
                + self.workspace.revision().as_u64()
                + self.notifications.revision().as_u64()
                + self.operations.revision().as_u64(),
        )
    }

    /// Prepare a semantic merge from provider revisions for the active authored document.
    pub fn start_semantic_merge(&mut self, base: &str, incoming: &str) -> Result<String> {
        let id = self.workspace.active().ok_or_else(|| {
            Problem::new("start a semantic merge", "no authored document is active")
                .with_remedy("open the document to merge")
        })?;
        let path = self
            .documents
            .get(id)
            .and_then(|document| document.assets().first())
            .cloned()
            .ok_or_else(|| Problem::not_found("the active document's backing asset"))?;
        let base_content = self
            .source_control
            .provider()
            .content_at(std::path::Path::new(&path), base)?;
        let incoming_content = self
            .source_control
            .provider()
            .content_at(std::path::Path::new(&path), incoming)?;
        let base_text = std::str::from_utf8(&base_content.content).map_err(|error| {
            Problem::new(
                format!("read {path} at {base}"),
                format!("the authored document is not UTF-8: {error}"),
            )
        })?;
        let incoming_text = std::str::from_utf8(&incoming_content.content).map_err(|error| {
            Problem::new(
                format!("read {path} at {incoming}"),
                format!("the authored document is not UTF-8: {error}"),
            )
        })?;
        let mut base_document = Document::new(&path);
        crate::worldfile::load(
            base_text,
            &mut base_document,
            Actor::system("semantic merge reader"),
        )?;
        let mut incoming_document = Document::new(&path);
        crate::worldfile::load(
            incoming_text,
            &mut incoming_document,
            Actor::system("semantic merge reader"),
        )?;
        let local = self
            .documents
            .get(id)
            .ok_or_else(|| Problem::not_found("the active document"))?;
        self.semantic_merge
            .begin(local, &base_document, &incoming_document, base, incoming)?;
        if self
            .semantic_merge
            .session()
            .is_some_and(crate::semantic_merge::MergeSession::ready)
        {
            self.commit_ready_merge()?;
            Ok(format!("Merged {incoming} into {path} without conflicts"))
        } else {
            let conflicts = self
                .semantic_merge
                .session()
                .map_or(0, |session| session.conflicts().len());
            Ok(format!(
                "Compared {path}: {conflicts} conflict{} require an explicit decision",
                if conflicts == 1 { "" } else { "s" }
            ))
        }
    }

    /// Record one decision and atomically commit the completed merge.
    pub fn resolve_semantic_merge(
        &mut self,
        index: usize,
        choice: &str,
        replacement: Option<cy_editor_core::value::Value>,
    ) -> Result<String> {
        self.semantic_merge.resolve(index, choice, replacement)?;
        if self
            .semantic_merge
            .session()
            .is_some_and(crate::semantic_merge::MergeSession::ready)
        {
            let count = self.commit_ready_merge()?;
            Ok(format!(
                "Committed semantic merge as one transaction ({count} operations)"
            ))
        } else {
            Ok(format!("Resolved merge conflict {index} as {choice}"))
        }
    }

    fn commit_ready_merge(&mut self) -> Result<usize> {
        let (document, operations) =
            self.semantic_merge.resolved_operations().ok_or_else(|| {
                Problem::new("commit a semantic merge", "conflicts remain unresolved")
            })?;
        let count = operations.len();
        let actor = self.actor.clone();
        self.documents
            .get_mut(document)
            .ok_or_else(|| Problem::not_found("the document receiving the merge"))?
            .with_transaction("Resolve semantic merge", actor, |document| {
                for operation in operations {
                    document.record(operation)?;
                }
                Ok(())
            })?;
        self.semantic_merge.finish();
        Ok(count)
    }
}

impl CommandContext for Editor {
    fn active_document(&self) -> Option<DocumentId> {
        self.workspace.active()
    }

    fn terrain_status(&self) -> Outcome {
        self.terrain.status()
    }

    fn document(&self, id: DocumentId) -> Option<&Document> {
        self.documents.get(id)
    }

    fn document_mut(&mut self, id: DocumentId) -> Option<&mut Document> {
        self.documents.get_mut(id)
    }

    fn actor(&self) -> Actor {
        self.actor.clone()
    }

    fn selection(&self) -> &Selection {
        self.selection.get()
    }

    fn set_selection(&mut self, selection: Selection) {
        self.selection.set(selection);
    }

    fn notify(&mut self, message: &str) {
        self.notifications.post(Notification::info(message));
    }

    fn viewport(&mut self) -> Option<&mut dyn cy_editor_commands::ViewportControls> {
        Some(&mut self.viewports)
    }

    fn project(&mut self) -> Option<&mut dyn cy_editor_commands::ProjectHost> {
        Some(self)
    }

    fn assets(&mut self) -> Option<&mut dyn cy_editor_commands::AssetHost> {
        Some(&mut self.imports)
    }

    fn start_external_asset_import(&mut self, source: &str, destination: &str) -> Result<u64> {
        self.import_external(std::path::PathBuf::from(source), destination.to_string())
    }

    fn start_lightmap_bake(&mut self, description: &str, output: &str) -> Result<u64> {
        self.bake_lightmaps(description, output)
    }

    fn cancel_lightmap_bake(&mut self, request: Option<u64>) -> Result<u64> {
        Editor::cancel_lightmap_bake(self, request)
    }

    fn write_lightmap_description(&mut self, path: &str, text: &str) -> Result<bool> {
        self.lightmaps.write_description(path, text)
    }

    fn settings(&mut self) -> Option<&mut dyn cy_editor_commands::SettingsHost> {
        Some(self)
    }

    fn source_control(&mut self) -> Option<&mut dyn cy_editor_commands::SourceControlHost> {
        Some(self)
    }

    fn start_semantic_merge(&mut self, base: &str, incoming: &str) -> Result<String> {
        Editor::start_semantic_merge(self, base, incoming)
    }

    fn resolve_semantic_merge(
        &mut self,
        index: usize,
        choice: &str,
        replacement: Option<cy_editor_core::value::Value>,
    ) -> Result<String> {
        Editor::resolve_semantic_merge(self, index, choice, replacement)
    }

    fn open_document(&mut self, asset: &str) -> Result<DocumentId> {
        Editor::open_document(self, asset)
    }

    fn play_state(&self) -> Option<String> {
        Some(cy_editor_commands::ProjectHost::play_state(self))
    }

    fn permitted_paths(&self) -> Option<(String, Vec<String>)> {
        Some(self.permitted.clone())
    }

    fn manipulate(&mut self, request: &cy_editor_commands::Manipulation) -> Result<String> {
        manipulate::apply(self, request)
    }

    fn navmesh(&mut self) -> Option<&mut dyn cy_editor_commands::NavmeshHost> {
        Some(self)
    }
}

impl cy_editor_commands::SettingsHost for Editor {
    fn set(
        &mut self,
        key: &str,
        platform: Option<&str>,
        user: bool,
        value: cy_editor_core::value::Value,
    ) -> Result<()> {
        let value = match value {
            cy_editor_core::value::Value::Bool(value) => crate::SettingValue::Flag(value),
            cy_editor_core::value::Value::Int(value) => crate::SettingValue::Whole(value),
            cy_editor_core::value::Value::Double(value) => crate::SettingValue::Real(value),
            cy_editor_core::value::Value::Text(value)
                if self
                    .settings
                    .catalogue()
                    .get(key)
                    .is_some_and(|declaration| {
                        matches!(declaration.default, crate::SettingValue::List(_))
                    }) =>
            {
                crate::SettingValue::List(
                    value
                        .split(',')
                        .map(str::trim)
                        .filter(|item| !item.is_empty())
                        .map(ToOwned::to_owned)
                        .collect(),
                )
            }
            cy_editor_core::value::Value::Text(value) => crate::SettingValue::Text(value),
            other => {
                return Err(Problem::new(
                    format!("set {key}"),
                    format!("{} is not a settings value", other.kind().name()),
                )
                .with_remedy("use a bool, integer, double, text, or comma-separated list"));
            }
        };
        match (user, platform) {
            (true, Some(_)) => Err(Problem::new(
                format!("set {key}"),
                "a user preference cannot also be a project platform override",
            )
            .with_remedy("choose user scope with no platform, or project scope with a platform")),
            (true, None) => self.settings.set_user(key, value),
            (false, Some(platform)) => self.settings.set_platform(platform, key, value),
            (false, None) => self.settings.set_project(key, value),
        }
    }

    fn reset(&mut self, key: &str, platform: Option<&str>) -> Result<()> {
        match platform {
            Some(platform) => self.settings.reset_platform(platform, key),
            None => self.settings.reset(key),
        }
    }
}

impl cy_editor_commands::SourceControlHost for Editor {
    fn refresh(&mut self) -> Result<String> {
        let paths = source_control_paths(self);
        self.source_control.request_refresh(paths);
        Ok(format!(
            "Refreshing status through {}",
            self.source_control.provider().name()
        ))
    }

    fn history(&mut self, path: &str) -> Result<String> {
        let revisions = self
            .source_control
            .fetch_history(std::path::Path::new(path))?;
        Ok(revisions
            .into_iter()
            .map(|revision| {
                format!(
                    "{} {} — {}",
                    revision.id, revision.author, revision.description
                )
            })
            .collect::<Vec<_>>()
            .join("\n"))
    }

    fn operate(&mut self, operation: &str, path: &str, description: &str) -> Result<String> {
        let paths = vec![std::path::PathBuf::from(path)];
        let provider = self.source_control.provider();
        let summary = match operation {
            "checkout" => {
                provider.check_out(&paths)?;
                format!("Checked out {path}")
            }
            "revert" => {
                provider.revert(&paths)?;
                format!("Reverted {path}")
            }
            "submit" => {
                let revision = provider.submit(&paths, description)?;
                format!("Submitted {path} as {revision}")
            }
            "lock" => {
                provider.lock(&paths)?;
                format!("Locked {path}")
            }
            "unlock" => {
                provider.unlock(&paths)?;
                format!("Unlocked {path}")
            }
            other => {
                return Err(Problem::not_found(format!(
                    "source-control operation {other}"
                )));
            }
        };
        self.source_control.request_refresh(paths);
        Ok(summary)
    }
}

fn source_control_paths(editor: &Editor) -> Vec<std::path::PathBuf> {
    editor
        .documents
        .ids()
        .filter_map(|id| editor.documents.get(id))
        .filter_map(|document| document.assets().first())
        .map(std::path::PathBuf::from)
        .collect()
}

/// What a human at the interface may touch: everything.
///
/// Not a claim that the human is trusted more than an agent — `cy_editor_commands::scope` makes that
/// argument in full — but the observation that an editor which refused its own menu items would be
/// refusing itself.
fn unrestricted() -> (String, Vec<String>) {
    ("interactive".to_string(), vec![String::new()])
}

/// The project's source tree, its build, and the runtime that runs it.
///
/// Implemented on [`Editor`] rather than on [`ProjectService`] because two of the three reach past
/// it: a reload goes to [`Editor::runtime`] and a play switch to [`Editor::viewports`]. They are one
/// trait because they are one loop — write, build, reload, play — and splitting them into three
/// would put three hooks on [`CommandContext`] for one capability.
impl cy_editor_commands::ProjectHost for Editor {
    fn project_root(&self) -> String {
        self.project.root().display().to_string()
    }

    fn source_paths(&self) -> Vec<String> {
        self.project.source_paths()
    }

    fn source_exists(&self, path: &str) -> bool {
        self.project.source_exists(path)
    }

    fn read_source(&self, path: &str) -> Result<String> {
        self.project.read_source(path)
    }

    fn source_fingerprint(&self, path: &str) -> Result<String> {
        self.sources
            .fingerprint(path)
            .map(|fingerprint| fingerprint.to_string())
    }

    fn source_is_restorable(&self, path: &str) -> bool {
        self.project.source_is_restorable(path)
    }

    fn put_source(&mut self, path: &str, contents: Option<&str>) -> Result<()> {
        self.project.put_source(path, contents)?;
        self.sources.refresh()?;
        Ok(())
    }

    fn put_source_if_unchanged(
        &mut self,
        path: &str,
        contents: &str,
        expected: &str,
    ) -> Result<cy_editor_commands::SourceWrite> {
        let expected = crate::SourceFingerprint::parse(expected)?;
        match self.sources.save_if_unchanged(path, contents, expected)? {
            crate::SourceSave::Written { fingerprint } => {
                Ok(cy_editor_commands::SourceWrite::Written {
                    fingerprint: fingerprint.to_string(),
                })
            }
            crate::SourceSave::Conflict { actual, disk } => {
                Ok(cy_editor_commands::SourceWrite::Conflict {
                    actual: actual.to_string(),
                    disk,
                })
            }
        }
    }

    fn asset_fingerprint(&self, path: &str) -> Result<String> {
        self.asset_catalogue.fingerprint(path)
    }

    fn move_asset_if_unchanged(
        &mut self,
        from: &str,
        to: &str,
        expected: &str,
    ) -> Result<cy_editor_commands::AssetMove> {
        match self.asset_catalogue.move_if_unchanged(from, to, expected)? {
            crate::AssetMove::Moved { fingerprint } => {
                Ok(cy_editor_commands::AssetMove::Moved { fingerprint })
            }
            crate::AssetMove::Conflict { actual } => {
                Ok(cy_editor_commands::AssetMove::Conflict { actual })
            }
        }
    }

    fn build(&mut self) -> Result<String> {
        let (label, work) = self.project.next_build()?;
        // Off the interface thread, where every other long operation goes. "The editor stays usable
        // while an agent works" is not satisfiable by a build that blocks the caller, and an agent
        // reads the operations resource for progress exactly as a person reads the same panel.
        let operation = self.operations.start(label.clone(), work);
        Ok(format!("{} — {}", label, operation.label()))
    }

    fn build_state(&self) -> String {
        self.project.describe_build()
    }

    fn reload(&mut self, module: &str) -> Result<String> {
        let (library, generation) = self.project.built()?;
        let module = if module.trim().is_empty() {
            self.project.module().to_string()
        } else {
            module.to_string()
        };
        let request = self
            .runtime
            .reload(&module, &library.display().to_string(), generation)?;
        self.pending_reloads
            .insert(request.as_u64(), (module.clone(), generation));
        self.reload_report = Some(ReloadReport {
            request: request.as_u64(),
            module: module.clone(),
            generation,
            state: "pending".into(),
            preserved: Vec::new(),
            dropped: Vec::new(),
            diagnostic: None,
        });
        self.bump_reload_revision();
        Ok(format!(
            "Reloading {module} generation {generation} (request {}); preservation report pending",
            request.as_u64()
        ))
    }

    fn set_play(&mut self, state: &str, mode: &str) -> Result<String> {
        // THE MODE IS RESOLVED BEFORE ANYTHING CHANGES, and a word this build does not know is
        // refused by name. M11.b task 3.1 and `specs/live-editing/`'s first scenario: *"the request
        // SHALL fail naming the mode and the reason, and no other mode SHALL start"*. Resolving it
        // first is what makes the second half true — a refusal after the badge was set would have
        // started something.
        let wanted_mode = PlayMode::from_name(mode).ok_or_else(|| {
            Problem::new(
                format!("play in {mode:?}"),
                "there is no such play mode in this build",
            )
            .with_remedy(
                "the modes are: in-editor, separate-process, remote-device. A mode that is not \
                 available is refused rather than replaced with another one.",
            )
        })?;
        let wanted = match state {
            "playing" => PlayState::Playing,
            "paused" => PlayState::Paused,
            "editing" => PlayState::Editing,
            other => {
                return Err(Problem::new(
                    format!("set the play state to {other:?}"),
                    "there is no such play state",
                )
                .with_remedy("the states are: editing, playing, paused"));
            }
        };
        // AND THE RUNTIME, WHICH IS WHAT M8.a ADDS. Until now this function set a badge and told
        // the engine nothing: pressing play changed a word in the corner of the viewport and
        // simulated nothing, which is design.md §4's "today it reports `hosting: NoRuntime`".
        //
        // The runtime request happens before the local badge changes. A disconnected editor remains
        // fully usable for authoring, but PLAYING and PAUSED describe a simulation and therefore
        // cannot be entered when no engine accepted the request.
        let hosted = match self.runtime.play(state, wanted_mode) {
            Ok(request) => {
                self.set_local_play_state(wanted);
                self.play_mode = wanted_mode;
                format!(" — asked the runtime (request {})", request.as_u64())
            }
            Err(problem) => {
                if wanted == PlayState::Editing {
                    self.set_local_play_state(PlayState::Editing);
                }
                self.notifications
                    .post(Notification::warning(format!("Play unchanged: {problem}.")));
                return Ok(format!(
                    "Play unchanged ({}) — no runtime is attached, so nothing is simulating",
                    cy_editor_commands::ProjectHost::play_state(self)
                ));
            }
        };

        Ok(format!(
            "{} ({}) — {}{hosted}",
            wanted.badge(),
            wanted_mode.badge(),
            wanted
                .persistence(cy_editor_viewport::play::Persistence::default())
                .statement()
        ))
    }

    fn play_state(&self) -> String {
        match self.viewports.focused().play {
            PlayState::Editing => "editing",
            PlayState::Playing => "playing",
            PlayState::Paused => "paused",
        }
        .to_string()
    }

    fn play_mode(&self) -> String {
        self.play_mode.name().to_string()
    }

    fn material_graph_read(&self, reference: &str) -> Result<String> {
        crate::material_graph::read(self.project.root(), reference)
    }

    fn material_graph_preview(&mut self, reference: &str, source: &str) -> Result<u64> {
        crate::material_graph::paths(self.project.root(), reference)?;
        if !source.starts_with("cymatcanvas 1\n") {
            return Err(Problem::new(
                "preview a material graph",
                "expected cymatcanvas 1 source",
            ));
        }
        self.preview_material_graph(reference, source)
            .map(cy_editor_protocol::RequestId::as_u64)
    }

    fn material_graph_save(&mut self, reference: &str, source: &str) -> Result<u64> {
        crate::material_graph::paths(self.project.root(), reference)?;
        if !source.starts_with("cymatcanvas 1\n") {
            return Err(Problem::new(
                "save a material graph",
                "expected cymatcanvas 1 source",
            ));
        }
        if self.pending_graph_save.is_some() {
            return Err(Problem::new(
                "save a material graph",
                "another graph save is pending",
            ));
        }
        let document = self.workspace.active().ok_or_else(|| {
            Problem::new(
                "save a material graph",
                "no scene document is active for undo history",
            )
        })?;
        let sources = self.assigned_material_geometry(Some(reference));
        let request = self.request_material_for_geometry(
            MaterialOperation::Author,
            source.as_bytes().to_vec(),
            &sources,
        )?;
        let id = request.as_u64();
        self.pending_graph_save = Some(PendingGraphSave {
            request: id,
            reference: reference.to_owned(),
            source: source.to_owned(),
            document,
            actor: self.actor.clone(),
        });
        self.graph_save_status = format!("pending: {id}");
        Ok(id)
    }

    fn material_graph_status(&self) -> String {
        let request = match self.backend.material_request_state() {
            MaterialRequestState::Idle => "idle".to_owned(),
            MaterialRequestState::Pending { request, operation } => {
                format!("pending {} {operation:?}", request.as_u64())
            }
            MaterialRequestState::Validated { request } => {
                format!("validated {}", request.as_u64())
            }
            MaterialRequestState::Authored { request, .. } => {
                format!("authored {}", request.as_u64())
            }
            MaterialRequestState::Previewed { request } => {
                format!("previewed {}", request.as_u64())
            }
            MaterialRequestState::Compiled { request, .. } => {
                format!("compiled {}", request.as_u64())
            }
            MaterialRequestState::Failed {
                request,
                diagnostics,
            } => format!(
                "failed {:?}: {} diagnostic(s)",
                request.map(cy_editor_protocol::RequestId::as_u64),
                diagnostics.len()
            ),
            MaterialRequestState::Cancelled { request } => {
                format!("cancelled {}", request.as_u64())
            }
        };
        format!(
            "save: {}; request: {}; preview: {:?}",
            self.graph_save_status,
            request,
            self.backend.material_preview_state()
        )
    }

    fn material_canvas_draft_save(&mut self, reference: &str, source: &str) -> Result<()> {
        let (_, canvas_path) = crate::material_graph::paths(self.project.root(), reference)?;
        if !source.starts_with("cymatcanvas 1\n") {
            return Err(Problem::new(
                "save a material canvas draft",
                "expected cymatcanvas 1 source",
            ));
        }
        let document_id = self.workspace.active().ok_or_else(|| {
            Problem::new(
                "save a material canvas draft",
                "no scene document is active for undo history",
            )
        })?;
        let canvas_reference = canvas_path
            .strip_prefix(self.project.root())
            .map_err(|error| Problem::new("save a material canvas draft", error.to_string()))?
            .to_string_lossy()
            .into_owned();
        let prior_source = self.project.read_source(&canvas_reference).ok();
        if prior_source.as_deref() == Some(source) {
            return Ok(());
        }
        let prior_graph = self.project.read_source(reference).ok();
        self.project.put_source(&canvas_reference, Some(source))?;
        let recorded = (|| -> Result<()> {
            let document = self.documents.get_mut(document_id).ok_or_else(|| {
                Problem::new(
                    "save a material canvas draft",
                    "the active scene document closed",
                )
            })?;
            document.with_transaction(
                format!("Edit material canvas {reference}"),
                self.actor.clone(),
                |document| {
                    document.record(cy_editor_documents::operation::Operation::Domain {
                        node: None,
                        kind: format!("{}{reference}", crate::material_graph::GRAPH_DOMAIN_PREFIX),
                        before: crate::material_graph::encode_pair(
                            prior_graph.as_deref(),
                            prior_source.as_deref(),
                        ),
                        after: crate::material_graph::encode_pair(
                            prior_graph.as_deref(),
                            Some(source),
                        ),
                    })
                },
            )
        })();
        if let Err(problem) = recorded {
            self.project
                .put_source(&canvas_reference, prior_source.as_deref())?;
            return Err(problem);
        }
        Ok(())
    }

    fn material_graph_authored(&self, reference: &str) -> bool {
        self.project.source_exists(reference)
    }

    fn vfx_document_read(&self, reference: &str) -> Result<String> {
        crate::vfx_document::validate_reference(reference)?;
        self.project.read_source(reference)
    }

    fn vfx_document_save(&mut self, reference: &str, source: &str) -> Result<()> {
        crate::vfx_document::validate_reference(reference)?;
        crate::vfx_document::validate_source(source)?;
        let document_id = self.workspace.active().ok_or_else(|| {
            Problem::new(
                "save a VFX document",
                "no scene document is active for undo history",
            )
        })?;
        let prior = if self.project.source_exists(reference) {
            Some(self.project.read_source(reference)?)
        } else {
            None
        };
        self.project.put_source(reference, Some(source))?;
        let document = self.documents.get_mut(document_id).ok_or_else(|| {
            Problem::new("save a VFX document", "the active scene document closed")
        })?;
        document.begin(format!("Save VFX document {reference}"), self.actor.clone());
        document.record(cy_editor_documents::operation::Operation::Domain {
            node: None,
            kind: format!("{}{}", crate::vfx_document::DOMAIN_PREFIX, reference),
            before: crate::project::encode_source(prior.as_deref()),
            after: crate::project::encode_source(Some(source)),
        })?;
        document.commit()?;
        Ok(())
    }

    fn vfx_document_exists(&self, reference: &str) -> bool {
        self.project.source_exists(reference)
    }

    fn vfx_catalogue(&self) -> Option<Vec<u8>> {
        self.backend.vfx_catalogue().map(<[u8]>::to_vec)
    }

    fn material_catalogue(&self) -> Option<Vec<u8>> {
        self.backend.material_catalogue().map(<[u8]>::to_vec)
    }

    fn vfx_module_read(&self, reference: &str) -> Result<String> {
        crate::vfx_module::validate_reference(reference)?;
        self.project.read_source(reference)
    }

    fn vfx_module_exists(&self, reference: &str) -> bool {
        self.project.source_exists(reference)
    }

    fn vfx_module_save(&mut self, reference: &str, source: &str) -> Result<()> {
        crate::vfx_module::validate_reference(reference)?;
        crate::vfx_module::validate_source(source)?;
        let document_id = self.workspace.active().ok_or_else(|| {
            Problem::new(
                "save a VFX module",
                "no scene document is active for undo history",
            )
        })?;
        let prior = if self.project.source_exists(reference) {
            Some(self.project.read_source(reference)?)
        } else {
            None
        };
        self.project.put_source(reference, Some(source))?;
        let document = self
            .documents
            .get_mut(document_id)
            .ok_or_else(|| Problem::new("save a VFX module", "the active scene document closed"))?;
        document.begin(format!("Save VFX module {reference}"), self.actor.clone());
        document.record(cy_editor_documents::operation::Operation::Domain {
            node: None,
            kind: format!("{}{}", crate::vfx_module::DOMAIN_PREFIX, reference),
            before: crate::project::encode_source(prior.as_deref()),
            after: crate::project::encode_source(Some(source)),
        })?;
        document.commit()?;
        Ok(())
    }

    fn audio_asset_save(&mut self, reference: &str, source: &str) -> Result<()> {
        self.save_audio_asset(reference, source)
    }

    fn script_graph_save(&mut self, reference: &str, source: &str) -> Result<()> {
        self.save_script_graph(reference, source)
    }

    fn script_catalogue(&mut self) -> Option<Vec<u8>> {
        self.backend.script.want();
        self.backend.script.catalogue().map(<[u8]>::to_vec)
    }

    fn script_compile(&mut self, reference: &str, source: &str) -> Result<u64> {
        let sent = self
            .backend
            .script
            .compile(&self.runtime, reference, source)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn script_raise(&mut self, payload: Vec<u8>) -> Result<u64> {
        let sent = self.backend.script.raise(&self.runtime, payload)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn script_refresh(&mut self) -> Result<u64> {
        let sent = self.backend.script.refresh(&self.runtime)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn script_status(&self, reference: &str) -> cy_editor_commands::Outcome {
        let current = (!reference.is_empty() && self.project.source_exists(reference))
            .then(|| self.project.read_source(reference).ok())
            .flatten();
        crate::script_commands::status_outcome(&self.backend.script, reference, current.as_deref())
    }

    fn script_breakpoint(
        &mut self,
        graph: &str,
        node: u64,
        entity: u64,
        enabled: bool,
    ) -> Result<u64> {
        let breakpoint = crate::script_debug::Breakpoint {
            graph: graph.to_owned(),
            node,
            entity,
        };
        let sent = self
            .backend
            .script
            .set_breakpoint(&self.runtime, breakpoint, enabled)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn script_debug_control(&mut self, action: &str) -> Result<u64> {
        let action = match action {
            "pause" => crate::script_debug::DebugAction::Pause,
            "continue" => crate::script_debug::DebugAction::Continue,
            mode => crate::script_debug::DebugAction::step(mode)?,
        };
        let sent = self.backend.script.control(&self.runtime, action)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn script_watch(
        &mut self,
        graph: &str,
        entity: Option<u64>,
        node: u64,
        pin: &str,
        enabled: bool,
    ) -> Result<()> {
        let mut watch = self.backend.script.watch().clone();
        if watch.graph != graph {
            // Pins are a graph's: watching another graph starts its own list.
            watch.pins.clear();
            graph.clone_into(&mut watch.graph);
        }
        if let Some(entity) = entity {
            watch.entity = entity;
        }
        let pair = (node, pin.to_owned());
        watch.pins.retain(|watched| watched != &pair);
        if enabled {
            watch.pins.push(pair);
        }
        self.backend.script.set_watch(watch);
        Ok(())
    }

    fn script_inspect(&mut self, graph: &str, entity: u64) -> Result<()> {
        let mut watch = self.backend.script.watch().clone();
        if watch.graph != graph {
            watch.pins.clear();
            graph.clone_into(&mut watch.graph);
        }
        watch.entity = entity;
        self.backend.script.set_watch(watch);
        Ok(())
    }

    fn script_debug_refresh(&mut self) -> Result<u64> {
        let sent = self.backend.script.refresh_debug(&self.runtime)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn script_reload(&mut self, reference: &str, source: &str) -> Result<u64> {
        let sent = self
            .backend
            .script
            .reload(&self.runtime, reference, source)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn script_graph_changed(&mut self, reference: &str, source: Option<&str>) {
        self.reload_running_graph(reference, source);
    }

    fn script_debug_status(&self, reference: &str) -> cy_editor_commands::Outcome {
        crate::script_debug_commands::debug_outcome(&self.backend.script, reference)
    }

    fn animation_graph_save(&mut self, reference: &str, source: &str) -> Result<()> {
        self.save_animation_graph(reference, source)
    }

    fn animation_catalogue(&mut self) -> Option<Vec<u8>> {
        self.backend.animation.want();
        self.backend.animation.catalogue().map(<[u8]>::to_vec)
    }

    fn animation_compile(&mut self, reference: &str, source: &str) -> Result<u64> {
        let sent = self
            .backend
            .animation
            .compile(&self.runtime, reference, source)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn animation_preview(
        &mut self,
        change: &cy_editor_commands::AnimationPreviewChange,
    ) -> Result<u64> {
        let mut settings = self
            .backend
            .animation
            .settings()
            .cloned()
            .unwrap_or_default();
        if change.time.is_none() && change.playing == Some(false) {
            // A pause holds the pose the engine reached, not the one the editor last asked for.
            if let Some(state) = self.backend.animation.preview_state().filter(|s| s.active) {
                settings.time = state.time;
            }
        }
        if let Some(reference) = &change.reference {
            reference.clone_into(&mut settings.reference);
        }
        if settings.reference.is_empty() {
            return Err(
                Problem::new("change the animation preview", "nothing is previewed yet")
                    .with_remedy("scrub or play a graph first: animation.preview.scrub"),
            );
        }
        if let Some(focus) = change.focus {
            settings.focus = focus;
        }
        if let Some(time) = change.time {
            settings.time = time;
        }
        if let Some(playing) = change.playing {
            settings.playing = playing;
        }
        if let Some((name, value)) = &change.parameter {
            settings.parameters.insert(name.clone(), *value);
        }
        let source = self.project.read_source(&settings.reference)?;
        let sent = self
            .backend
            .animation
            .preview(&self.runtime, settings, &source)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn animation_preview_stop(&mut self) -> Result<u64> {
        let sent = self.backend.animation.stop(&self.runtime)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn animation_status(&self, reference: &str) -> cy_editor_commands::Outcome {
        let current = (!reference.is_empty() && self.project.source_exists(reference))
            .then(|| self.project.read_source(reference).ok())
            .flatten();
        crate::animation_commands::status_outcome(
            &self.backend.animation,
            reference,
            current.as_deref(),
        )
    }

    fn animation_graph_changed(&mut self, reference: &str, source: Option<&str>) {
        self.preview_follows(reference, source);
    }

    fn audio_request(&mut self, operation: &str, payload: Vec<u8>) -> Result<u64> {
        let sent = self
            .backend
            .audio
            .request(&self.runtime, operation, payload)?;
        Ok(sent.map_or(0, RequestId::as_u64))
    }

    fn audio_status(&self) -> cy_editor_commands::Outcome {
        crate::audio_status::outcome(&self.backend.audio)
    }

    fn audio_listener(&self) -> ([f32; 3], [f32; 3]) {
        let camera = self.viewports.focused().state.camera;
        let forward = camera.forward();
        (
            [camera.position.x, camera.position.y, camera.position.z],
            [forward.x, forward.y, forward.z],
        )
    }

    fn vfx_preview_load(&mut self, source: &str) -> Result<u64> {
        crate::vfx_document::validate_source(source)?;
        self.request_vfx_preview_load(source.to_owned())
            .map(RequestId::as_u64)
    }

    fn vfx_preview_control(&mut self, action: &str, value: f32) -> Result<u64> {
        let action = match action {
            "play" => crate::vfx_preview::VfxPreviewAction::Play,
            "pause" => crate::vfx_preview::VfxPreviewAction::Pause,
            "restart" => crate::vfx_preview::VfxPreviewAction::Restart,
            "scrub" => crate::vfx_preview::VfxPreviewAction::Scrub(value),
            "time-scale" => crate::vfx_preview::VfxPreviewAction::TimeScale(value),
            _ => {
                return Err(Problem::new(
                    "control VFX preview",
                    "action must be play, pause, restart, scrub, or time-scale",
                ));
            }
        };
        self.request_vfx_preview_action(action)
            .map(RequestId::as_u64)
    }

    fn vfx_preview_step(&mut self, seconds: f32) -> Result<u64> {
        self.request_vfx_preview_step(seconds)
            .map(RequestId::as_u64)
    }

    fn vfx_preview_parameter(&mut self, name: &str, values: &[f32]) -> Result<u64> {
        self.request_vfx_preview_parameter(name, values)
            .map(RequestId::as_u64)
    }

    fn vfx_preview_status(&self) -> Outcome {
        use cy_editor_core::value::Value;

        let mut result = Outcome::new("Engine VFX preview state")
            .with("pending", Value::Bool(self.backend.vfx_preview_pending()));
        if let Some(problem) = self.backend.vfx_preview_problem() {
            result = result.with("problem", Value::Text(problem.to_owned()));
        }
        if let Some(state) = self.backend.vfx_preview_snapshot() {
            result = result
                .with("cook_key", Value::Text(state.cook_key.to_string()))
                .with("playing", Value::Bool(state.playing))
                .with("time_seconds", Value::Float(state.time_seconds))
                .with("time_scale", Value::Float(state.time_scale))
                .with(
                    "live_particles",
                    Value::Int(i64::from(state.live_particles)),
                )
                .with("spawned", Value::Int(i64::from(state.spawned)))
                .with("killed", Value::Int(i64::from(state.killed)))
                .with("cpu_fallbacks", Value::Int(i64::from(state.cpu_fallbacks)))
                .with(
                    "pool_used_bytes",
                    Value::Text(state.pool_used_bytes.to_string()),
                )
                .with(
                    "pool_total_bytes",
                    Value::Text(state.pool_total_bytes.to_string()),
                )
                .with(
                    "pool_shortfall_particles",
                    Value::Int(i64::from(state.pool_shortfall_particles)),
                )
                .with(
                    "pool_reduced_requests",
                    Value::Int(i64::from(state.pool_reduced_requests)),
                )
                .with("events_raised", Value::Int(i64::from(state.events_raised)))
                .with(
                    "events_delivered",
                    Value::Int(i64::from(state.events_delivered)),
                )
                .with(
                    "events_dropped",
                    Value::Int(i64::from(state.events_dropped)),
                )
                .with(
                    "events_truncated",
                    Value::Int(i64::from(state.events_truncated)),
                )
                .with(
                    "readback_deferred",
                    Value::Int(i64::from(state.readback_deferred)),
                )
                .with(
                    "emitter_count",
                    Value::Int(i64::try_from(state.emitters.len()).unwrap_or(i64::MAX)),
                );
            for (index, emitter) in state.emitters.iter().enumerate() {
                result = result
                    .with(
                        format!("emitter_{index}_name"),
                        Value::Text(emitter.name.clone()),
                    )
                    .with(
                        format!("emitter_{index}_live"),
                        Value::Int(i64::from(emitter.live)),
                    );
            }
            if let Some(sample) = &state.sample {
                result = result
                    .with("sample_emitter", Value::Int(i64::from(sample.emitter)))
                    .with("sample_slot", Value::Int(i64::from(sample.slot)))
                    .with(
                        "sample_attribute_count",
                        Value::Int(i64::try_from(sample.attributes.len()).unwrap_or(i64::MAX)),
                    );
                for (index, attribute) in sample.attributes.iter().enumerate() {
                    let mut lanes = [0.0; 4];
                    lanes[..attribute.values.len()].copy_from_slice(&attribute.values);
                    result = result
                        .with(
                            format!("sample_attribute_{index}_name"),
                            Value::Text(attribute.name.clone()),
                        )
                        .with(
                            format!("sample_attribute_{index}_lanes"),
                            Value::Int(i64::try_from(attribute.values.len()).unwrap_or(i64::MAX)),
                        )
                        .with(
                            format!("sample_attribute_{index}_values"),
                            Value::Vec4(lanes),
                        );
                }
            }
        }
        result
    }
}

impl Editor {
    /// Latest material graph save result, used to settle the desktop canvas after asynchronous
    /// engine authoring completes.
    pub fn material_graph_save_status(&self) -> &str {
        &self.graph_save_status
    }

    fn set_local_play_state(&mut self, state: PlayState) {
        // Every viewport, because play is a property of the runtime rather than of a panel: two
        // viewports showing different play states would describe two runtimes.
        for viewport in self.viewports.all_mut().iter_mut() {
            viewport.play = state;
            match state {
                PlayState::Playing => viewport.attach_to_game_camera(0),
                PlayState::Editing => viewport.detach_camera(),
                PlayState::Paused => {}
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::primitives::{MaterialBinding, create_mesh_instance};
    use cy_editor_core::value::{Value, ValueKind};
    use cy_editor_protocol::FrameId;
    use cy_editor_viewport::gizmo::Transform3;
    use cy_editor_viewport::picking::PickCandidate;

    #[test]
    fn material_geometry_request_preserves_the_canvas_and_rejects_bad_names() {
        let canvas = b"cymatcanvas 1\nmaterial sway\n".to_vec();
        let payload =
            material_geometry_payload(canvas.clone(), &["StaticMesh", "VirtualGeometry"]).unwrap();
        assert_eq!(
            payload,
            [
                b"cymatrequest 1\ngeometry StaticMesh,VirtualGeometry\n".as_slice(),
                canvas.as_slice()
            ]
            .concat()
        );
        assert_eq!(
            material_geometry_payload(canvas.clone(), &[]).unwrap(),
            canvas
        );
        assert!(
            material_geometry_payload(canvas.clone(), &["StaticMesh\nmaterial forged"]).is_err()
        );
        assert!(material_geometry_payload(canvas, &[""]).is_err());
    }

    #[test]
    fn material_preview_carries_assigned_geometry_to_the_engine() {
        let canvas = "cymatcanvas 1\nmaterial sway\n";
        let payload = material_preview_payload(
            "materials/sway.cygraph",
            canvas,
            &["StaticMesh", "VirtualGeometry"],
        )
        .unwrap();
        let mut reader = cy_editor_core::codec::Reader::new(&payload);
        assert_eq!(reader.text().unwrap(), "materials/sway.cygraph");
        assert_eq!(
            reader.text().unwrap(),
            format!("cymatrequest 1\ngeometry StaticMesh,VirtualGeometry\n{canvas}")
        );
    }

    #[test]
    fn assigned_material_geometry_includes_meshes_and_terrain_layers() {
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/materials.cyworld").unwrap();
        editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction("Assign material", Actor::human("designer"), |document| {
                let mesh = create_mesh_instance(
                    document,
                    None,
                    "meshes/box.cyprim",
                    Transform3::default(),
                )?;
                let binding = MaterialBinding::of_schema(document.schema()).unwrap();
                document.record(
                    cy_editor_documents::operation::Operation::SetAssetReference {
                        node: mesh,
                        component: binding.component,
                        field: binding.material,
                        before: String::new(),
                        after: "materials/shared.cygraph".into(),
                    },
                )?;

                let terrain = document
                    .schema_mut()
                    .declare_type("TerrainAuthoring", false);
                let layer = document
                    .schema_mut()
                    .declare_type("TerrainMaterialLayer", true);
                let material = document.schema_mut().declare_field(
                    layer,
                    "material",
                    ValueKind::Text,
                    "Terrain layer material",
                )?;
                let root = document.create_node(None)?;
                document.add_component(root, terrain, vec![])?;
                let child = document.create_node(Some(root))?;
                document.add_component(
                    child,
                    layer,
                    vec![(material, Value::Text("materials/shared.cygraph".into()))],
                )?;
                let terrain_only = document.create_node(Some(root))?;
                document.add_component(
                    terrain_only,
                    layer,
                    vec![(material, Value::Text("materials/terrain.cygraph".into()))],
                )?;
                let orphan = document.create_node(None)?;
                document.add_component(
                    orphan,
                    layer,
                    vec![(material, Value::Text("materials/orphan.cygraph".into()))],
                )?;
                let clustered = create_mesh_instance(
                    document,
                    None,
                    "meshes/stone.cyvg",
                    Transform3::default(),
                )?;
                let binding = MaterialBinding::of_schema(document.schema()).unwrap();
                document.record(
                    cy_editor_documents::operation::Operation::SetAssetReference {
                        node: clustered,
                        component: binding.component,
                        field: binding.material,
                        before: String::new(),
                        after: "materials/shared.cygraph".into(),
                    },
                )?;
                Ok(())
            })
            .unwrap();
        let geometry = editor.assigned_material_geometry(Some("materials/shared.cygraph"));
        assert_eq!(geometry, ["StaticMesh", "VirtualGeometry", "Terrain"]);
        let payload = material_geometry_payload(b"cymatcanvas 1\n".to_vec(), &geometry).unwrap();
        assert!(
            payload.starts_with(b"cymatrequest 1\ngeometry StaticMesh,VirtualGeometry,Terrain\n")
        );
        let preview =
            material_preview_payload("materials/shared.cygraph", "cymatcanvas 1\n", &geometry)
                .unwrap();
        let mut reader = cy_editor_core::codec::Reader::new(&preview);
        assert_eq!(reader.text().unwrap(), "materials/shared.cygraph");
        assert!(
            reader
                .text()
                .unwrap()
                .starts_with("cymatrequest 1\ngeometry StaticMesh,VirtualGeometry,Terrain\n")
        );
        assert_eq!(
            editor.assigned_material_geometry(Some("materials/terrain.cygraph")),
            ["Terrain"]
        );
        assert!(
            editor
                .assigned_material_geometry(Some("materials/orphan.cygraph"))
                .is_empty()
        );
    }

    #[test]
    fn an_editor_with_no_runtime_still_opens_and_edits_documents() {
        // The argument for `NoRuntime` being a mode: everything below happens with no engine at all.
        let mut editor = Editor::default();
        assert_eq!(editor.hosting_mode(), HostingMode::NoRuntime);

        let id = editor.open_document("worlds/city.cyworld").unwrap();
        assert_eq!(editor.workspace.active(), Some(id));

        editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();
        assert!(editor.documents.any_dirty());
    }

    #[test]
    fn acting_as_an_agent_changes_who_history_names() {
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        editor.acting_as(Actor::agent("claude", "s-3", "add a lamp at the corner"));

        let actor = editor.actor();
        editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction("Create lamp", actor, |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();

        let entry = &editor.documents.get(id).unwrap().history().entries()[0];
        assert!(entry.actor.is_agent());
        assert_eq!(entry.actor.intent(), Some("add a lamp at the corner"));
    }

    #[test]
    fn pumping_an_editor_with_no_runtime_does_nothing_and_says_nothing() {
        let mut editor = Editor::default();
        let revision = editor.summary_revision();
        for _ in 0..100 {
            editor.pump();
        }
        assert_eq!(
            editor.summary_revision(),
            revision,
            "an idle editor costs nothing"
        );
    }

    #[test]
    fn play_selects_the_game_camera_and_stop_restores_the_editor_view() {
        let mut editor = Editor::default();
        assert_eq!(editor.viewports.focused().attachment.game_camera(), None);

        editor.set_local_play_state(PlayState::Playing);
        assert_eq!(editor.viewports.focused().attachment.game_camera(), Some(0));
        assert_eq!(editor.viewports.focused().play, PlayState::Playing);

        editor.set_local_play_state(PlayState::Paused);
        assert_eq!(editor.viewports.focused().attachment.game_camera(), Some(0));

        editor.set_local_play_state(PlayState::Editing);
        assert_eq!(editor.viewports.focused().attachment.game_camera(), None);
        assert_eq!(editor.viewports.focused().play, PlayState::Editing);
    }

    #[test]
    fn a_runtime_pick_selects_the_camera_from_the_document_identity() {
        let mut editor = Editor::default();
        let document_id = editor.open_document("worlds/pick.cyworld").unwrap();
        let node = editor
            .documents
            .get_mut(document_id)
            .unwrap()
            .with_transaction("Create Camera", Actor::human("designer"), |document| {
                document.create_node(None)
            })
            .unwrap();
        let frame = FrameId::from_raw(42);
        editor.pending_picks.insert(
            7,
            PendingPick {
                document: document_id,
                frame,
                intent: PickIntent::Click { x: 50.0, y: 30.0 },
                mode: SelectionMode::Replace,
                cycle: 0,
            },
        );
        editor.accept_pick_message(&Message::Picked {
            request: RequestId::from_raw(7),
            candidates: PickResponse {
                frame,
                candidates: vec![PickCandidate {
                    identity: engine_identity(node),
                    distance: 1.0,
                    transparent: false,
                }],
            }
            .encode(),
        });
        assert_eq!(
            editor.selection.get().nodes().collect::<Vec<_>>(),
            vec![node]
        );
        assert!(editor.pending_picks.is_empty());
    }

    #[test]
    fn closing_documents_keeps_the_service_and_workspace_in_step() {
        let mut editor = Editor::default();
        let city = editor.open_document("worlds/city.cyworld").unwrap();
        let forest = editor.open_document("worlds/forest.cyworld").unwrap();
        assert_eq!(editor.workspace.active(), Some(forest));

        assert_eq!(
            editor.close_document(city, None).unwrap(),
            CloseOutcome::Closed
        );
        assert!(editor.documents.get(city).is_none());
        assert_eq!(editor.workspace.open_documents(), &[forest]);
        assert_eq!(editor.workspace.active(), Some(forest));

        assert_eq!(
            editor.close_document(forest, None).unwrap(),
            CloseOutcome::Closed
        );
        assert!(editor.documents.is_empty());
        assert!(editor.workspace.open_documents().is_empty());
        assert_eq!(editor.workspace.active(), None);
    }

    #[test]
    fn a_cancelled_or_failed_close_changes_neither_half() {
        let mut editor = Editor::default();
        let id = editor.open_document("worlds/city.cyworld").unwrap();
        editor
            .documents
            .get_mut(id)
            .unwrap()
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None).map(|_| ())
            })
            .unwrap();

        assert_eq!(
            editor
                .close_document(id, Some(CloseDecision::Cancel))
                .unwrap(),
            CloseOutcome::Cancelled
        );
        assert!(editor.documents.get(id).is_some());
        assert_eq!(editor.workspace.open_documents(), &[id]);
        assert_eq!(editor.workspace.active(), Some(id));

        let missing = DocumentId::of_asset("worlds/missing.cyworld");
        assert!(editor.close_document(missing, None).is_err());
        assert!(editor.documents.get(id).is_some());
        assert_eq!(editor.workspace.open_documents(), &[id]);
    }
}
