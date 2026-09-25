// SPDX-License-Identifier: MIT
//! State for engine-owned editor backend services.
//!
//! The wire stays generic, while this service records the material catalogue the editor currently
//! knows.  Keeping the bytes here (below presentation) lets a runtime disappear without taking an
//! authored graph or the last compatible catalogue snapshot with it.

use cy_editor_core::codec::Reader;
use cy_editor_core::observe::{Revision, Versioned};
use cy_editor_core::problem::Problem;
use cy_editor_protocol::{Message, RequestId, ServiceEventKind};

use crate::runtime::RuntimeSession;
use crate::vfx_capabilities::VfxAuthoringCapabilities;

const MATERIAL_CATALOGUE_OPERATION: &str = "material.catalogue.get";
const VFX_CATALOGUE_OPERATION: &str = "vfx.catalogue.get";
const VFX_CAPABILITIES_OPERATION: &str = "vfx.authoring-capabilities.get";
const MATERIAL_VALIDATE_OPERATION: &str = "material.validate";
const MATERIAL_COMPILE_OPERATION: &str = "material.compile";
const MATERIAL_AUTHOR_OPERATION: &str = "material.author";
const MATERIAL_PREVIEW_OPERATION: &str = "material.preview.set";
const PREVIEW_CREATE_OPERATION: &str = "preview.create";
const PREVIEW_RELOAD_OPERATION: &str = "preview.reload";
const PREVIEW_DESTROY_OPERATION: &str = "preview.destroy";
const PREVIEW_PARAMETER_OPERATION: &str = "preview.parameter.update";
const SERVICE_SCHEMA_VERSION: u32 = 1;

/// Where the material catalogue request currently is.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum MaterialCatalogueState {
    /// No runtime has supplied a catalogue in this editor session.
    #[default]
    Unavailable,
    /// A request is in flight.
    Loading,
    /// A compatible catalogue snapshot is available.
    Ready,
    /// The last request failed. A previous snapshot, if any, is retained.
    Failed,
}

/// Operation represented by the material request state.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MaterialOperation {
    /// Validate without producing GPU programs.
    Validate,
    /// Compile and return a stable artefact identity.
    Compile,
    /// Validate and obtain engine-canonical graph text for saving.
    Author,
    /// Apply an unsaved graph to the authored scene.
    Preview,
}

/// One renderer material slot which shall receive a compiled artefact.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct MaterialPreviewTarget {
    /// Stable scene entity identity.
    pub entity: u128,
    /// Mesh material-slot index.
    pub slot: u32,
}

/// A typed live material parameter value.
#[derive(Clone, PartialEq, Debug)]
pub enum MaterialParameterValue {
    /// Boolean scalar.
    Bool(bool),
    /// Signed integer scalar.
    Int(i64),
    /// Floating-point scalar.
    Float(f64),
    /// Four floating-point lanes.
    Vec4([f32; 4]),
    /// Stable texture asset identity.
    Texture(String),
}

/// Stable severity carried by backend diagnostics.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MaterialDiagnosticSeverity {
    /// Informational context.
    Info,
    /// The graph remains usable but deserves attention.
    Warning,
    /// The requested operation cannot complete.
    Error,
}

/// Navigable location in a submitted material graph.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct MaterialDiagnosticLocation {
    /// Graph/document name supplied with the request.
    pub document: String,
    /// Stable node instance key, or zero for a document-level diagnostic.
    pub node: u64,
    /// Manifest-assigned node type identity, or zero when unavailable.
    pub node_type: u32,
    /// Manifest-assigned pin identity, or zero for a node/document location.
    pub pin: u32,
    /// Human-readable pin metadata; identity remains authoritative.
    pub pin_name: String,
}

/// One ordered engine diagnostic.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct MaterialDiagnostic {
    /// Stable severity.
    pub severity: MaterialDiagnosticSeverity,
    /// Stable machine-readable code.
    pub code: String,
    /// Human-readable explanation.
    pub message: String,
    /// Optional type, feature, or plugin metadata.
    pub detail: String,
    /// Primary location selected by the editor.
    pub primary: MaterialDiagnosticLocation,
    /// Other graph locations involved in the failure.
    pub related: Vec<MaterialDiagnosticLocation>,
    /// Optional corrective action.
    pub remedy: String,
}

/// Request-correlated state shown by the material authoring surface.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub enum MaterialRequestState {
    /// No validation or compilation has been requested.
    #[default]
    Idle,
    /// The backend owns this request.
    Pending {
        /// Stable live-protocol request identity.
        request: RequestId,
        /// Work requested by the author.
        operation: MaterialOperation,
    },
    /// The submitted graph passed backend validation.
    Validated {
        /// Request which produced the result.
        request: RequestId,
    },
    /// Engine-canonical graph bytes, ready for an atomic project write.
    Authored {
        /// Request which produced the graph.
        request: RequestId,
        /// Canonical `.cygraph` text.
        graph: String,
    },
    /// The authored scene accepted the unsaved graph.
    Previewed {
        /// Request which produced the result.
        request: RequestId,
    },
    /// The compiler produced a stable artefact.
    Compiled {
        /// Request which produced the result.
        request: RequestId,
        /// Compiler-owned content/derivation identity.
        artefact: u64,
        /// Semantic source-graph identity.
        graph: u64,
        /// Number of compiled programs in the artefact.
        programs: u32,
        /// Stable asset identities read by the compiled material.
        dependencies: Vec<String>,
    },
    /// Terminal backend refusal or service loss.
    Failed {
        /// Matching request, when submission had succeeded.
        request: Option<RequestId>,
        /// Ordered structured diagnostics returned by the backend.
        diagnostics: Vec<MaterialDiagnostic>,
    },
    /// Cooperative cancellation was acknowledged.
    Cancelled {
        /// Matching request identity.
        request: RequestId,
    },
}

/// Runtime-visible state of the material preview lease.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub enum MaterialPreviewState {
    /// No preview has been requested in this session.
    #[default]
    Idle,
    /// Preview creation or artefact reload is in flight.
    Pending {
        /// Correlated service request.
        request: RequestId,
        /// Human-readable operation name.
        operation: String,
    },
    /// Runtime acknowledged the exact artefact currently presented.
    Applied {
        /// Generational preview-world handle.
        preview: u64,
        /// Compiler artefact acknowledged by the runtime.
        artefact: u64,
    },
    /// Preview failed while the previous applied artefact, if any, remains valid.
    Failed {
        /// Structured failure.
        problem: Problem,
    },
}

#[derive(Clone, Copy)]
enum PreviewOperation {
    Create,
    Reload { artefact: u64 },
    Destroy,
}

/// Backend-service state owned by the editor rather than by a panel.
pub struct BackendServices {
    material_catalogue: Versioned<Option<Vec<u8>>>,
    catalogue_state: MaterialCatalogueState,
    catalogue_request: Option<RequestId>,
    vfx_catalogue: Versioned<Option<Vec<u8>>>,
    vfx_catalogue_state: MaterialCatalogueState,
    vfx_catalogue_request: Option<RequestId>,
    vfx_catalogue_requested: bool,
    vfx_capabilities: Versioned<Option<VfxAuthoringCapabilities>>,
    vfx_capabilities_state: MaterialCatalogueState,
    vfx_capabilities_request: Option<RequestId>,
    vfx_capabilities_requested: bool,
    material_request: Option<(RequestId, MaterialOperation)>,
    material_state: MaterialRequestState,
    preview_handle: Option<u64>,
    preview_request: Option<(RequestId, PreviewOperation)>,
    preview_pending_artefact: Option<u64>,
    preview_applied_artefact: Option<u64>,
    preview_targets: Vec<MaterialPreviewTarget>,
    preview_state: MaterialPreviewState,
    connected: bool,
}

impl Default for BackendServices {
    fn default() -> Self {
        Self {
            material_catalogue: Versioned::new(None),
            catalogue_state: MaterialCatalogueState::Unavailable,
            catalogue_request: None,
            vfx_catalogue: Versioned::new(None),
            vfx_catalogue_state: MaterialCatalogueState::Unavailable,
            vfx_catalogue_request: None,
            vfx_catalogue_requested: false,
            vfx_capabilities: Versioned::new(None),
            vfx_capabilities_state: MaterialCatalogueState::Unavailable,
            vfx_capabilities_request: None,
            vfx_capabilities_requested: false,
            material_request: None,
            material_state: MaterialRequestState::Idle,
            preview_handle: None,
            preview_request: None,
            preview_pending_artefact: None,
            preview_applied_artefact: None,
            preview_targets: vec![MaterialPreviewTarget { entity: 0, slot: 0 }],
            preview_state: MaterialPreviewState::Idle,
            connected: false,
        }
    }
}

impl BackendServices {
    /// Create an empty service view. A catalogue is requested when a runtime becomes reachable.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Submit discovery work once per runtime connection.
    pub fn maintain(&mut self, runtime: &RuntimeSession) -> Option<Problem> {
        if !runtime.is_connected() {
            self.connected = false;
            self.catalogue_request = None;
            self.vfx_catalogue_request = None;
            self.vfx_catalogue_requested = false;
            self.vfx_capabilities_request = None;
            self.vfx_capabilities_requested = false;
            if self.catalogue_state == MaterialCatalogueState::Loading {
                self.catalogue_state = MaterialCatalogueState::Failed;
            }
            if self.vfx_catalogue_state == MaterialCatalogueState::Loading {
                self.vfx_catalogue_state = MaterialCatalogueState::Failed;
            }
            if self.vfx_capabilities_state == MaterialCatalogueState::Loading {
                self.vfx_capabilities_state = MaterialCatalogueState::Failed;
            }
            if let Some((request, _)) = self.material_request.take() {
                self.material_state = MaterialRequestState::Failed {
                    request: Some(request),
                    diagnostics: vec![local_diagnostic(
                        "service-disconnected",
                        "the runtime disconnected before publishing a terminal event",
                    )],
                };
            }
            self.preview_handle = None;
            self.preview_request = None;
            self.preview_pending_artefact = self.preview_applied_artefact;
            return None;
        }
        if self.connected {
            if self.catalogue_request.is_none() && !self.vfx_catalogue_requested {
                self.vfx_catalogue_requested = true;
                match runtime.service_request(
                    SERVICE_SCHEMA_VERSION,
                    VFX_CATALOGUE_OPERATION,
                    Vec::new(),
                ) {
                    Ok(request) => {
                        self.vfx_catalogue_request = Some(request);
                        self.vfx_catalogue_state = MaterialCatalogueState::Loading;
                    }
                    Err(problem) => {
                        self.vfx_catalogue_state = MaterialCatalogueState::Failed;
                        return Some(problem);
                    }
                }
            } else if self.vfx_catalogue_requested
                && self.vfx_catalogue_request.is_none()
                && !self.vfx_capabilities_requested
            {
                self.vfx_capabilities_requested = true;
                match runtime.service_request(
                    SERVICE_SCHEMA_VERSION,
                    VFX_CAPABILITIES_OPERATION,
                    Vec::new(),
                ) {
                    Ok(request) => {
                        self.vfx_capabilities_request = Some(request);
                        self.vfx_capabilities_state = MaterialCatalogueState::Loading;
                    }
                    Err(problem) => {
                        self.vfx_capabilities_state = MaterialCatalogueState::Failed;
                        return Some(problem);
                    }
                }
            }
            return self.advance_preview(runtime);
        }

        self.connected = true;
        match runtime.service_request(
            SERVICE_SCHEMA_VERSION,
            MATERIAL_CATALOGUE_OPERATION,
            Vec::new(),
        ) {
            Ok(request) => {
                self.catalogue_request = Some(request);
                self.catalogue_state = MaterialCatalogueState::Loading;
                self.advance_preview(runtime)
            }
            Err(problem) => {
                self.connected = false;
                self.catalogue_state = MaterialCatalogueState::Failed;
                Some(problem)
            }
        }
    }

    /// Reconcile service events drained by the editor's ordinary frame pump.
    pub fn accept(&mut self, message: &Message) -> Option<Problem> {
        let Message::ServiceEvent {
            request,
            kind,
            schema_version,
            payload,
        } = message
        else {
            return None;
        };
        if Some(*request) == self.catalogue_request {
            return self.accept_catalogue(*kind, *schema_version, payload);
        }
        if Some(*request) == self.vfx_catalogue_request {
            return self.accept_vfx_catalogue(*kind, *schema_version, payload);
        }
        if Some(*request) == self.vfx_capabilities_request {
            return self.accept_vfx_capabilities(*kind, *schema_version, payload);
        }
        if self.material_request.map(|pending| pending.0) == Some(*request) {
            return self.accept_material(*request, *kind, *schema_version, payload);
        }
        if self.preview_request.map(|pending| pending.0) == Some(*request) {
            return self.accept_preview(*request, *kind, *schema_version, payload);
        }
        None
    }

    fn accept_catalogue(
        &mut self,
        kind: ServiceEventKind,
        schema_version: u32,
        payload: &[u8],
    ) -> Option<Problem> {
        match kind {
            ServiceEventKind::Accepted | ServiceEventKind::Progress => None,
            ServiceEventKind::Completed => {
                self.catalogue_request = None;
                if schema_version != SERVICE_SCHEMA_VERSION {
                    self.catalogue_state = MaterialCatalogueState::Failed;
                    return Some(Problem::new(
                        "load the material node catalogue",
                        format!(
                            "the runtime returned schema {schema_version}; this editor supports schema 1"
                        ),
                    )
                    .with_remedy("use an editor and runtime with compatible service schemas"));
                }
                self.material_catalogue.set(Some(payload.to_vec()));
                self.catalogue_state = MaterialCatalogueState::Ready;
                None
            }
            ServiceEventKind::Failed => {
                self.catalogue_request = None;
                self.catalogue_state = MaterialCatalogueState::Failed;
                Some(decode_failure(payload))
            }
            ServiceEventKind::Cancelled => {
                self.catalogue_request = None;
                self.catalogue_state = MaterialCatalogueState::Failed;
                Some(
                    Problem::new(
                        "load the material node catalogue",
                        "the backend cancelled the catalogue request",
                    )
                    .with_remedy("retry after the runtime is ready"),
                )
            }
        }
    }

    fn accept_vfx_catalogue(
        &mut self,
        kind: ServiceEventKind,
        schema_version: u32,
        payload: &[u8],
    ) -> Option<Problem> {
        match kind {
            ServiceEventKind::Accepted | ServiceEventKind::Progress => None,
            ServiceEventKind::Completed => {
                self.vfx_catalogue_request = None;
                if schema_version != SERVICE_SCHEMA_VERSION {
                    self.vfx_catalogue_state = MaterialCatalogueState::Failed;
                    return Some(Problem::new(
                        "load the VFX node catalogue",
                        format!("the runtime returned unsupported schema {schema_version}"),
                    ));
                }
                self.vfx_catalogue.set(Some(payload.to_vec()));
                self.vfx_catalogue_state = MaterialCatalogueState::Ready;
                None
            }
            ServiceEventKind::Failed | ServiceEventKind::Cancelled => {
                self.vfx_catalogue_request = None;
                self.vfx_catalogue_state = MaterialCatalogueState::Failed;
                Some(Problem::new(
                    "load the VFX node catalogue",
                    "the runtime did not provide a compatible VFX catalogue",
                ))
            }
        }
    }

    fn accept_vfx_capabilities(
        &mut self,
        kind: ServiceEventKind,
        schema_version: u32,
        payload: &[u8],
    ) -> Option<Problem> {
        match kind {
            ServiceEventKind::Accepted | ServiceEventKind::Progress => None,
            ServiceEventKind::Completed => {
                self.vfx_capabilities_request = None;
                if schema_version != SERVICE_SCHEMA_VERSION {
                    self.vfx_capabilities_state = MaterialCatalogueState::Failed;
                    return Some(Problem::new(
                        "load VFX authoring capabilities",
                        format!("the runtime returned unsupported schema {schema_version}"),
                    ));
                }
                match VfxAuthoringCapabilities::decode(payload) {
                    Ok(capabilities) => {
                        self.vfx_capabilities.set(Some(capabilities));
                        self.vfx_capabilities_state = MaterialCatalogueState::Ready;
                        None
                    }
                    Err(problem) => {
                        self.vfx_capabilities_state = MaterialCatalogueState::Failed;
                        Some(problem)
                    }
                }
            }
            ServiceEventKind::Failed | ServiceEventKind::Cancelled => {
                self.vfx_capabilities_request = None;
                self.vfx_capabilities_state = MaterialCatalogueState::Failed;
                Some(Problem::new(
                    "load VFX authoring capabilities",
                    "the runtime did not provide renderer and target options",
                ))
            }
        }
    }

    /// Submit validation or compilation for the current transient material canvas.
    pub fn request_material(
        &mut self,
        runtime: &RuntimeSession,
        operation: MaterialOperation,
        payload: Vec<u8>,
    ) -> cy_editor_core::problem::Result<RequestId> {
        if self.material_request.is_some() {
            return Err(Problem::new(
                "submit a material backend request",
                "validation or compilation is already pending",
            )
            .with_remedy("cancel it or wait for its terminal event"));
        }
        if self.preview_request.is_some() {
            return Err(Problem::new(
                "submit a material backend request",
                "the preview is still applying the previous result",
            )
            .with_remedy("wait for the reload acknowledgement"));
        }
        let operation_name = match operation {
            MaterialOperation::Validate => MATERIAL_VALIDATE_OPERATION,
            MaterialOperation::Compile => MATERIAL_COMPILE_OPERATION,
            MaterialOperation::Author => MATERIAL_AUTHOR_OPERATION,
            MaterialOperation::Preview => MATERIAL_PREVIEW_OPERATION,
        };
        let request = runtime.service_request(SERVICE_SCHEMA_VERSION, operation_name, payload)?;
        self.material_request = Some((request, operation));
        self.material_state = MaterialRequestState::Pending { request, operation };
        Ok(request)
    }

    /// Ask the backend to cancel the pending material operation.
    pub fn cancel_material(&self, runtime: &RuntimeSession) -> cy_editor_core::problem::Result<()> {
        let request = self
            .material_request
            .map(|pending| pending.0)
            .ok_or_else(|| {
                Problem::new(
                    "cancel a material backend request",
                    "no material request is pending",
                )
            })?;
        runtime.cancel_service(request)
    }

    /// Latest request-correlated material result. Terminal state survives disconnects.
    #[must_use]
    pub const fn material_request_state(&self) -> &MaterialRequestState {
        &self.material_state
    }

    fn accept_material(
        &mut self,
        request: RequestId,
        kind: ServiceEventKind,
        schema_version: u32,
        payload: &[u8],
    ) -> Option<Problem> {
        if matches!(
            kind,
            ServiceEventKind::Accepted | ServiceEventKind::Progress
        ) {
            return None;
        }
        let operation = self
            .material_request
            .take()
            .expect("request identity matched")
            .1;
        match kind {
            ServiceEventKind::Completed => {
                match decode_material_result(operation, payload, schema_version) {
                    Ok(result) => {
                        self.material_state = result.with_request(request);
                        if let MaterialRequestState::Compiled { artefact, .. } =
                            &self.material_state
                            && !self.preview_targets.is_empty()
                        {
                            self.preview_pending_artefact = Some(*artefact);
                        }
                    }
                    Err(problem) => {
                        self.material_state = MaterialRequestState::Failed {
                            request: Some(request),
                            diagnostics: vec![local_diagnostic(
                                "result-unreadable",
                                &problem.because,
                            )],
                        };
                        return Some(problem);
                    }
                }
            }
            ServiceEventKind::Failed => {
                let diagnostics = decode_diagnostics(payload).unwrap_or_else(|problem| {
                    vec![local_diagnostic("diagnostic-unreadable", &problem.because)]
                });
                let summary = diagnostics.first().map_or_else(
                    || "the backend returned no diagnostic".into(),
                    |diagnostic| format!("{}: {}", diagnostic.code, diagnostic.message),
                );
                self.material_state = MaterialRequestState::Failed {
                    request: Some(request),
                    diagnostics,
                };
                return Some(Problem::new("process a material graph", summary));
            }
            ServiceEventKind::Cancelled => {
                self.material_state = MaterialRequestState::Cancelled { request };
            }
            ServiceEventKind::Accepted | ServiceEventKind::Progress => unreachable!(),
        }
        None
    }

    /// Latest compatible snapshot. It remains available across runtime loss.
    #[must_use]
    pub fn material_catalogue(&self) -> Option<&[u8]> {
        self.material_catalogue.get().as_deref()
    }

    /// Latest engine VFX catalogue snapshot, retained across disconnects.
    #[must_use]
    pub fn vfx_catalogue(&self) -> Option<&[u8]> {
        self.vfx_catalogue.get().as_deref()
    }

    /// Revision for installing a new VFX palette in the shared canvas.
    #[must_use]
    pub const fn vfx_catalogue_revision(&self) -> Revision {
        self.vfx_catalogue.revision()
    }

    /// Current request state for the engine VFX definitions.
    #[must_use]
    pub const fn vfx_catalogue_state(&self) -> MaterialCatalogueState {
        self.vfx_catalogue_state
    }

    /// Last validated engine renderer and target options, retained across disconnects.
    #[must_use]
    pub fn vfx_authoring_capabilities(&self) -> Option<&VfxAuthoringCapabilities> {
        self.vfx_capabilities.get().as_ref()
    }

    /// State of the renderer and target query.
    #[must_use]
    pub const fn vfx_authoring_capabilities_state(&self) -> MaterialCatalogueState {
        self.vfx_capabilities_state
    }

    /// Revision used by presentation to install a snapshot only once.
    #[must_use]
    pub const fn material_catalogue_revision(&self) -> Revision {
        self.material_catalogue.revision()
    }

    /// Current request state for status surfaces.
    #[must_use]
    pub const fn material_catalogue_state(&self) -> MaterialCatalogueState {
        self.catalogue_state
    }

    /// Runtime acknowledgement state for the latest compiled material.
    #[must_use]
    pub const fn material_preview_state(&self) -> &MaterialPreviewState {
        &self.preview_state
    }

    /// Choose the selected viewport mesh slots which the next compile shall update.
    pub fn set_material_preview_targets(
        &mut self,
        targets: Vec<MaterialPreviewTarget>,
    ) -> cy_editor_core::problem::Result<()> {
        if targets.is_empty() || targets.iter().any(|target| target.slot >= 16) {
            return Err(Problem::new(
                "select material preview targets",
                "at least one renderer slot from zero through fifteen is required",
            ));
        }
        self.preview_targets = targets;
        Ok(())
    }

    /// Keep compilation independent of the first-light preview for authored scenes.
    pub fn clear_material_preview_targets(&mut self) {
        self.preview_targets.clear();
        self.preview_pending_artefact = None;
        self.preview_state = MaterialPreviewState::Idle;
    }

    /// Apply a typed value to the currently acknowledged artefact generation.
    pub fn update_material_parameter(
        &self,
        runtime: &RuntimeSession,
        parameter: u32,
        value: MaterialParameterValue,
    ) -> cy_editor_core::problem::Result<RequestId> {
        let preview = self.preview_handle.ok_or_else(|| {
            Problem::new("update a material parameter", "no preview world is alive")
        })?;
        let artefact = self.preview_applied_artefact.ok_or_else(|| {
            Problem::new(
                "update a material parameter",
                "no compiled artefact is applied",
            )
        })?;
        if parameter == 0 {
            return Err(Problem::new(
                "update a material parameter",
                "parameter identity zero is reserved",
            ));
        }
        let mut payload = Vec::new();
        payload.extend_from_slice(&preview.to_le_bytes());
        payload.extend_from_slice(&artefact.to_le_bytes());
        payload.extend_from_slice(&parameter.to_le_bytes());
        match value {
            MaterialParameterValue::Bool(value) => {
                payload.push(1);
                payload.push(u8::from(value));
            }
            MaterialParameterValue::Int(value) => {
                payload.push(2);
                payload.extend_from_slice(&value.to_le_bytes());
            }
            MaterialParameterValue::Float(value) => {
                payload.push(3);
                payload.extend_from_slice(&value.to_le_bytes());
            }
            MaterialParameterValue::Vec4(value) => {
                payload.push(4);
                for lane in value {
                    payload.extend_from_slice(&lane.to_le_bytes());
                }
            }
            MaterialParameterValue::Texture(value) => {
                payload.push(5);
                let length = u32::try_from(value.len()).map_err(|_| {
                    Problem::new(
                        "update a material parameter",
                        "the texture identity is too long",
                    )
                })?;
                payload.extend_from_slice(&length.to_le_bytes());
                payload.extend_from_slice(value.as_bytes());
            }
        }
        runtime.service_request(SERVICE_SCHEMA_VERSION, PREVIEW_PARAMETER_OPERATION, payload)
    }

    /// Destroy the session preview. Repeated calls after acknowledgement are harmless locally.
    pub fn destroy_material_preview(
        &mut self,
        runtime: &RuntimeSession,
    ) -> cy_editor_core::problem::Result<Option<RequestId>> {
        let Some(preview) = self.preview_handle else {
            return Ok(None);
        };
        if self.preview_request.is_some() {
            return Err(Problem::new(
                "destroy a material preview",
                "another preview operation is pending",
            ));
        }
        let request = runtime.service_request(
            SERVICE_SCHEMA_VERSION,
            PREVIEW_DESTROY_OPERATION,
            preview.to_le_bytes().to_vec(),
        )?;
        self.preview_request = Some((request, PreviewOperation::Destroy));
        self.preview_state = MaterialPreviewState::Pending {
            request,
            operation: PREVIEW_DESTROY_OPERATION.to_string(),
        };
        Ok(Some(request))
    }

    fn advance_preview(&mut self, runtime: &RuntimeSession) -> Option<Problem> {
        if self.preview_request.is_some() || self.preview_pending_artefact.is_none() {
            return None;
        }
        let artefact = self.preview_pending_artefact.expect("checked above");
        let (name, payload, operation) = if let Some(preview) = self.preview_handle {
            let mut payload = Vec::with_capacity(20 + self.preview_targets.len() * 20);
            payload.extend_from_slice(&preview.to_le_bytes());
            payload.extend_from_slice(&artefact.to_le_bytes());
            payload.extend_from_slice(
                &u32::try_from(self.preview_targets.len())
                    .unwrap_or(u32::MAX)
                    .to_le_bytes(),
            );
            for target in &self.preview_targets {
                payload.extend_from_slice(&target.entity.to_le_bytes());
                payload.extend_from_slice(&target.slot.to_le_bytes());
            }
            (
                PREVIEW_RELOAD_OPERATION,
                payload,
                PreviewOperation::Reload { artefact },
            )
        } else {
            (
                PREVIEW_CREATE_OPERATION,
                Vec::new(),
                PreviewOperation::Create,
            )
        };
        match runtime.service_request(SERVICE_SCHEMA_VERSION, name, payload) {
            Ok(request) => {
                self.preview_request = Some((request, operation));
                self.preview_state = MaterialPreviewState::Pending {
                    request,
                    operation: name.to_string(),
                };
                None
            }
            Err(problem) => {
                self.preview_state = MaterialPreviewState::Failed {
                    problem: problem.clone(),
                };
                Some(problem)
            }
        }
    }

    fn accept_preview(
        &mut self,
        request: RequestId,
        kind: ServiceEventKind,
        schema_version: u32,
        payload: &[u8],
    ) -> Option<Problem> {
        if matches!(
            kind,
            ServiceEventKind::Accepted | ServiceEventKind::Progress
        ) {
            return None;
        }
        let (_, operation) = self
            .preview_request
            .take()
            .expect("request identity matched");
        if schema_version != SERVICE_SCHEMA_VERSION {
            return Some(self.fail_preview(Problem::new(
                "apply a material preview",
                "the runtime returned an unsupported preview schema",
            )));
        }
        match kind {
            ServiceEventKind::Completed => match operation {
                PreviewOperation::Create => {
                    let preview = read_u64_payload(payload, 0).map_err(|problem| {
                        Problem::new("create a material preview", problem.because)
                    });
                    match preview {
                        Ok(preview) => {
                            self.preview_handle = Some(preview);
                            self.preview_state = MaterialPreviewState::Pending {
                                request,
                                operation: PREVIEW_RELOAD_OPERATION.to_string(),
                            };
                            None
                        }
                        Err(problem) => Some(self.fail_preview(problem)),
                    }
                }
                PreviewOperation::Reload { artefact } => {
                    let requested = read_u64_payload(payload, 0);
                    let applied = read_u64_payload(payload, 8);
                    let acknowledged_targets = payload
                        .get(16..20)
                        .and_then(|bytes| <[u8; 4]>::try_from(bytes).ok())
                        .map(u32::from_le_bytes)
                        .and_then(|count| usize::try_from(count).ok());
                    let bindings_match = acknowledged_targets
                        .is_some_and(|count| count == self.preview_targets.len())
                        && payload.get(20..).is_some_and(|bindings| {
                            let mut expected = Vec::with_capacity(self.preview_targets.len() * 20);
                            for target in &self.preview_targets {
                                expected.extend_from_slice(&target.entity.to_le_bytes());
                                expected.extend_from_slice(&target.slot.to_le_bytes());
                            }
                            bindings == expected
                        });
                    match (requested, applied, bindings_match) {
                        (Ok(requested), Ok(applied), true)
                            if requested == artefact && applied == artefact =>
                        {
                            self.preview_pending_artefact = None;
                            self.preview_applied_artefact = Some(applied);
                            self.preview_state = MaterialPreviewState::Applied {
                                preview: self.preview_handle.unwrap_or_default(),
                                artefact: applied,
                            };
                            None
                        }
                        _ => Some(self.fail_preview(Problem::new(
                            "reload a material preview",
                            "the acknowledgement did not name the requested artefact",
                        ))),
                    }
                }
                PreviewOperation::Destroy => {
                    if payload.is_empty() {
                        self.preview_handle = None;
                        self.preview_pending_artefact = None;
                        self.preview_applied_artefact = None;
                        self.preview_state = MaterialPreviewState::Idle;
                        None
                    } else {
                        Some(self.fail_preview(Problem::new(
                            "destroy a material preview",
                            "the destruction acknowledgement carried unexpected bytes",
                        )))
                    }
                }
            },
            ServiceEventKind::Failed => Some(self.fail_preview(decode_failure(payload))),
            ServiceEventKind::Cancelled => Some(self.fail_preview(Problem::new(
                "apply a material preview",
                "the preview request was cancelled",
            ))),
            ServiceEventKind::Accepted | ServiceEventKind::Progress => unreachable!(),
        }
    }

    fn fail_preview(&mut self, problem: Problem) -> Problem {
        self.preview_pending_artefact = None;
        self.preview_state = MaterialPreviewState::Failed {
            problem: problem.clone(),
        };
        problem
    }
}

fn read_u64_payload(payload: &[u8], offset: usize) -> cy_editor_core::problem::Result<u64> {
    let bytes: [u8; 8] = payload
        .get(offset..offset.saturating_add(8))
        .and_then(|bytes| bytes.try_into().ok())
        .ok_or_else(|| Problem::new("read a preview result", "the payload is truncated"))?;
    Ok(u64::from_le_bytes(bytes))
}

enum DecodedMaterialResult {
    Validated,
    Authored(String),
    Previewed,
    Compiled {
        artefact: u64,
        graph: u64,
        programs: u32,
        dependencies: Vec<String>,
    },
}

impl DecodedMaterialResult {
    fn with_request(self, request: RequestId) -> MaterialRequestState {
        match self {
            Self::Validated => MaterialRequestState::Validated { request },
            Self::Authored(graph) => MaterialRequestState::Authored { request, graph },
            Self::Previewed => MaterialRequestState::Previewed { request },
            Self::Compiled {
                artefact,
                graph,
                programs,
                dependencies,
            } => MaterialRequestState::Compiled {
                request,
                artefact,
                graph,
                programs,
                dependencies,
            },
        }
    }
}

fn decode_material_result(
    operation: MaterialOperation,
    payload: &[u8],
    schema_version: u32,
) -> cy_editor_core::problem::Result<DecodedMaterialResult> {
    if schema_version != SERVICE_SCHEMA_VERSION {
        return Err(Problem::new(
            "decode a material result",
            "unsupported result schema",
        ));
    }
    let mut reader = Reader::new(payload);
    let payload_schema = reader.u32()?;
    if !matches!(payload_schema, 1 | 2) || reader.u8()? != 1 {
        return Err(Problem::new(
            "decode a material result",
            "the result did not report success",
        ));
    }
    let result = match operation {
        MaterialOperation::Validate => DecodedMaterialResult::Validated,
        MaterialOperation::Author => DecodedMaterialResult::Authored(reader.text()?),
        MaterialOperation::Preview => DecodedMaterialResult::Previewed,
        MaterialOperation::Compile => {
            let artefact = reader.u64()?;
            let graph = reader.u64()?;
            let programs = reader.u32()?;
            let dependencies = if payload_schema >= 2 {
                let count = reader.u32()?;
                (0..count)
                    .map(|_| reader.text())
                    .collect::<cy_editor_core::problem::Result<Vec<_>>>()?
            } else {
                Vec::new()
            };
            DecodedMaterialResult::Compiled {
                artefact,
                graph,
                programs,
                dependencies,
            }
        }
    };
    if !reader.is_empty() {
        return Err(Problem::new(
            "decode a material result",
            "bytes remain after the result",
        ));
    }
    Ok(result)
}

fn decode_diagnostics(payload: &[u8]) -> cy_editor_core::problem::Result<Vec<MaterialDiagnostic>> {
    let mut reader = Reader::new(payload);
    let schema = reader.u32()?;
    let diagnostics = match schema {
        1 => vec![local_diagnostic(&reader.text()?, &reader.text()?)],
        2 => {
            let count = reader.u32()?;
            let mut diagnostics = Vec::with_capacity(count as usize);
            for _ in 0..count {
                let severity = match reader.u8()? {
                    0 => MaterialDiagnosticSeverity::Info,
                    1 => MaterialDiagnosticSeverity::Warning,
                    2 => MaterialDiagnosticSeverity::Error,
                    value => {
                        return Err(Problem::new(
                            "decode a backend diagnostic",
                            format!("severity {value} is not supported"),
                        ));
                    }
                };
                let code = reader.text()?;
                let message = reader.text()?;
                let detail = reader.text()?;
                let primary = decode_location(&mut reader)?;
                let related_count = reader.u32()?;
                let mut related = Vec::with_capacity(related_count as usize);
                for _ in 0..related_count {
                    related.push(decode_location(&mut reader)?);
                }
                diagnostics.push(MaterialDiagnostic {
                    severity,
                    code,
                    message,
                    detail,
                    primary,
                    related,
                    remedy: reader.text()?,
                });
            }
            diagnostics
        }
        _ => {
            return Err(Problem::new(
                "decode a backend diagnostic",
                format!("diagnostic schema {schema} is not supported"),
            ));
        }
    };
    if !reader.is_empty() {
        return Err(Problem::new(
            "decode a backend diagnostic",
            "bytes remain after the diagnostic list",
        ));
    }
    Ok(diagnostics)
}

fn decode_location(
    reader: &mut Reader<'_>,
) -> cy_editor_core::problem::Result<MaterialDiagnosticLocation> {
    Ok(MaterialDiagnosticLocation {
        document: reader.text()?,
        node: reader.u64()?,
        node_type: reader.u32()?,
        pin: reader.u32()?,
        pin_name: reader.text()?,
    })
}

fn local_diagnostic(code: &str, message: &str) -> MaterialDiagnostic {
    MaterialDiagnostic {
        severity: MaterialDiagnosticSeverity::Error,
        code: code.into(),
        message: message.into(),
        detail: String::new(),
        primary: MaterialDiagnosticLocation::default(),
        related: Vec::new(),
        remedy: String::new(),
    }
}

fn decode_failure(payload: &[u8]) -> Problem {
    match decode_diagnostics(payload).and_then(|diagnostics| {
        diagnostics.into_iter().next().ok_or_else(|| {
            Problem::new(
                "decode a backend diagnostic",
                "the diagnostic list is empty",
            )
        })
    }) {
        Ok(diagnostic) => Problem::new(
            "load the material node catalogue",
            format!("{}: {}", diagnostic.code, diagnostic.message),
        )
        .with_remedy("inspect runtime capabilities or use a compatible engine build"),
        Err(_) => Problem::new(
            "load the material node catalogue",
            "the runtime returned an unreadable failure diagnostic",
        )
        .with_remedy("rebuild the editor and runtime from compatible revisions"),
    }
}

#[cfg(test)]
mod tests {
    use std::time::{Duration, Instant};

    use cy_editor_core::codec::Writer;
    use cy_editor_protocol::{Message, ServiceEventKind, Session, read_frame, write_frame};

    use super::*;
    use crate::notifications::NotificationService;

    #[test]
    fn authored_material_result_contains_only_canonical_text() {
        let mut payload = Writer::new();
        payload.u32(2);
        payload.u8(1);
        payload.text("cygraph 1\ngraph \"paint\" version 1\n");
        let result = decode_material_result(MaterialOperation::Author, &payload.finish(), 1)
            .expect("valid author result");
        assert!(
            matches!(result, DecodedMaterialResult::Authored(text) if text.starts_with("cygraph 1"))
        );
    }

    #[test]
    fn a_runtime_catalogue_is_requested_asynchronously_and_survives_disconnect() {
        let (editor_reader, mut runtime_writer) = std::io::pipe().unwrap();
        let (mut runtime_reader, editor_writer) = std::io::pipe().unwrap();
        let mut runtime = RuntimeSession::over(Session::over(editor_reader, editor_writer));
        let mut backend = BackendServices::new();

        assert!(backend.maintain(&runtime).is_none());
        assert_eq!(
            backend.material_catalogue_state(),
            MaterialCatalogueState::Loading
        );
        let request = Message::decode(&read_frame(&mut runtime_reader).unwrap().unwrap()).unwrap();
        let Message::ServiceRequest {
            request,
            schema_version,
            operation,
            payload,
        } = request
        else {
            panic!("the first backend message was not a service request")
        };
        assert_ne!(request.as_u64(), 0);
        assert_eq!(schema_version, 1);
        assert_eq!(operation, MATERIAL_CATALOGUE_OPERATION);
        assert!(payload.is_empty());

        let mut catalogue = Writer::new();
        catalogue.u32(1);
        catalogue.u32(7);
        catalogue.u32(0);
        let catalogue = catalogue.finish();
        write_frame(
            &mut runtime_writer,
            &Message::ServiceEvent {
                request,
                kind: ServiceEventKind::Completed,
                schema_version: 1,
                payload: catalogue.clone(),
            }
            .encode(),
        )
        .unwrap();

        let mut notifications = NotificationService::new();
        let deadline = Instant::now() + Duration::from_secs(5);
        while backend.material_catalogue().is_none() && Instant::now() < deadline {
            for message in runtime.pump(&mut notifications) {
                assert!(backend.accept(&message).is_none());
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(backend.material_catalogue(), Some(catalogue.as_slice()));
        assert_eq!(
            backend.material_catalogue_state(),
            MaterialCatalogueState::Ready
        );

        assert!(backend.maintain(&runtime).is_none());
        let request = Message::decode(&read_frame(&mut runtime_reader).unwrap().unwrap()).unwrap();
        let Message::ServiceRequest {
            request,
            schema_version,
            operation,
            payload,
        } = request
        else {
            panic!("the second backend message was not a service request")
        };
        assert_eq!(schema_version, 1);
        assert_eq!(operation, VFX_CATALOGUE_OPERATION);
        assert!(payload.is_empty());
        let mut vfx_catalogue = Writer::new();
        vfx_catalogue.u32(1);
        vfx_catalogue.u32(1);
        vfx_catalogue.u32(0);
        let vfx_catalogue = vfx_catalogue.finish();
        write_frame(
            &mut runtime_writer,
            &Message::ServiceEvent {
                request,
                kind: ServiceEventKind::Completed,
                schema_version: 1,
                payload: vfx_catalogue.clone(),
            }
            .encode(),
        )
        .unwrap();
        let deadline = Instant::now() + Duration::from_secs(5);
        while backend.vfx_catalogue().is_none() && Instant::now() < deadline {
            for message in runtime.pump(&mut notifications) {
                assert!(backend.accept(&message).is_none());
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(backend.vfx_catalogue(), Some(vfx_catalogue.as_slice()));

        assert!(backend.maintain(&runtime).is_none());
        let request = Message::decode(&read_frame(&mut runtime_reader).unwrap().unwrap()).unwrap();
        let Message::ServiceRequest {
            request,
            operation,
            payload,
            ..
        } = request
        else {
            panic!("the third backend message was not a service request")
        };
        assert_eq!(operation, VFX_CAPABILITIES_OPERATION);
        assert!(payload.is_empty());
        let mut capabilities = Writer::new();
        capabilities.u32(1);
        capabilities.u32(1);
        capabilities.u8(5);
        capabilities.text("Decal");
        capabilities.u8(0);
        capabilities.text("No projection pass");
        capabilities.u32(2);
        for (path, name, ready) in [(0, "GpuPreferred", false), (1, "CpuRequired", true)] {
            capabilities.u8(path);
            capabilities.text(name);
            capabilities.u8(1);
            capabilities.u8(u8::from(ready));
            capabilities.text(if ready {
                "EffectRequiresCpu"
            } else {
                "NoDeviceInThisWorld"
            });
            capabilities.text("reason");
        }
        write_frame(
            &mut runtime_writer,
            &Message::ServiceEvent {
                request,
                kind: ServiceEventKind::Completed,
                schema_version: 1,
                payload: capabilities.finish(),
            }
            .encode(),
        )
        .unwrap();
        let deadline = Instant::now() + Duration::from_secs(5);
        while backend.vfx_authoring_capabilities().is_none() && Instant::now() < deadline {
            for message in runtime.pump(&mut notifications) {
                assert!(backend.accept(&message).is_none());
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(
            backend.vfx_authoring_capabilities().unwrap().renderers[0].reason,
            "No projection pass"
        );

        drop(runtime_writer);
        let deadline = Instant::now() + Duration::from_secs(5);
        while runtime.is_connected() && Instant::now() < deadline {
            runtime.pump(&mut notifications);
            std::thread::sleep(Duration::from_millis(1));
        }
        backend.maintain(&runtime);
        assert_eq!(
            backend.material_catalogue(),
            Some(catalogue.as_slice()),
            "runtime loss must not discard the catalogue a live authored graph uses"
        );
        assert_eq!(backend.vfx_catalogue(), Some(vfx_catalogue.as_slice()));
        assert!(backend.vfx_authoring_capabilities().is_some());
    }

    #[test]
    fn a_structured_backend_failure_is_retained_as_a_problem() {
        let mut payload = Writer::new();
        payload.u32(1);
        payload.text("catalogue-unavailable");
        payload.text("material node registration failed");
        let problem = decode_failure(&payload.finish());
        assert!(
            problem.because.contains("catalogue-unavailable"),
            "{problem}"
        );
        assert!(problem.because.contains("node registration"), "{problem}");
        assert!(problem.remedy.is_some());
    }

    #[test]
    fn a_versioned_material_diagnostic_retains_stable_and_related_locations() {
        let mut payload = Writer::new();
        payload.u32(2);
        payload.u32(1);
        payload.u8(2);
        payload.text("graph.link.type-mismatch");
        payload.text("this pin does not accept the connected value");
        payload.text("closure");
        payload.text("editor_preview");
        payload.u64(2);
        payload.u32(5);
        payload.u32(1);
        payload.text("uv");
        payload.u32(1);
        payload.text("editor_preview");
        payload.u64(1);
        payload.u32(16);
        payload.u32(3);
        payload.text("out");
        payload.text("connect a value output instead");

        let diagnostics = decode_diagnostics(&payload.finish()).expect("schema 2 decodes");
        assert_eq!(diagnostics.len(), 1);
        assert_eq!(diagnostics[0].code, "graph.link.type-mismatch");
        assert_eq!(diagnostics[0].primary.node, 2);
        assert_eq!(diagnostics[0].primary.node_type, 5);
        assert_eq!(diagnostics[0].primary.pin, 1);
        assert_eq!(diagnostics[0].related[0].node, 1);
        assert_eq!(diagnostics[0].related[0].node_type, 16);
        assert_eq!(diagnostics[0].related[0].pin, 3);
    }

    #[test]
    fn a_compile_result_is_applied_only_to_its_request_identity() {
        let request = cy_editor_protocol::RequestId::from_raw(77);
        let mut backend = BackendServices::new();
        backend.material_request = Some((request, MaterialOperation::Compile));
        backend.material_state = MaterialRequestState::Pending {
            request,
            operation: MaterialOperation::Compile,
        };

        let mut payload = Writer::new();
        payload.u32(2);
        payload.u8(1);
        payload.u64(0xCAFE);
        payload.u64(0xBEEF);
        payload.u32(3);
        payload.u32(1);
        payload.text("0123456789abcdef0123456789abcdef");
        let payload = payload.finish();
        assert!(
            backend
                .accept(&Message::ServiceEvent {
                    request: cy_editor_protocol::RequestId::from_raw(76),
                    kind: ServiceEventKind::Completed,
                    schema_version: 1,
                    payload: payload.clone(),
                })
                .is_none()
        );
        assert!(matches!(
            backend.material_request_state(),
            MaterialRequestState::Pending { request: pending, .. } if *pending == request
        ));

        assert!(
            backend
                .accept(&Message::ServiceEvent {
                    request,
                    kind: ServiceEventKind::Completed,
                    schema_version: 1,
                    payload,
                })
                .is_none()
        );
        assert_eq!(
            backend.material_request_state(),
            &MaterialRequestState::Compiled {
                request,
                artefact: 0xCAFE,
                graph: 0xBEEF,
                programs: 3,
                dependencies: vec!["0123456789abcdef0123456789abcdef".into()],
            }
        );
    }

    #[test]
    fn a_pending_material_request_becomes_a_visible_failure_on_disconnect() {
        let request = cy_editor_protocol::RequestId::from_raw(9);
        let mut backend = BackendServices::new();
        backend.material_request = Some((request, MaterialOperation::Validate));
        backend.material_state = MaterialRequestState::Pending {
            request,
            operation: MaterialOperation::Validate,
        };
        let runtime = RuntimeSession::none();

        assert!(backend.maintain(&runtime).is_none());
        assert!(matches!(
            backend.material_request_state(),
            MaterialRequestState::Failed {
                request: Some(failed),
                diagnostics,
            } if *failed == request && diagnostics[0].code == "service-disconnected"
        ));
    }

    #[test]
    fn preview_is_current_only_after_the_exact_reload_acknowledgement() {
        let create = RequestId::from_raw(10);
        let reload = RequestId::from_raw(11);
        let preview = 0x1000_0002_u64;
        let artefact = 0xCAFE_BABE_u64;
        let mut backend = BackendServices::new();
        backend.preview_pending_artefact = Some(artefact);
        backend.preview_request = Some((create, PreviewOperation::Create));

        assert!(
            backend
                .accept(&Message::ServiceEvent {
                    request: create,
                    kind: ServiceEventKind::Completed,
                    schema_version: 1,
                    payload: preview.to_le_bytes().to_vec(),
                })
                .is_none()
        );
        assert_eq!(backend.preview_handle, Some(preview));
        assert_ne!(
            backend.material_preview_state(),
            &MaterialPreviewState::Applied { preview, artefact }
        );

        backend.preview_request = Some((reload, PreviewOperation::Reload { artefact }));
        let mut acknowledgement = Vec::new();
        acknowledgement.extend_from_slice(&artefact.to_le_bytes());
        acknowledgement.extend_from_slice(&artefact.to_le_bytes());
        acknowledgement.extend_from_slice(&1_u32.to_le_bytes());
        acknowledgement.extend_from_slice(&0_u128.to_le_bytes());
        acknowledgement.extend_from_slice(&0_u32.to_le_bytes());
        assert!(
            backend
                .accept(&Message::ServiceEvent {
                    request: reload,
                    kind: ServiceEventKind::Completed,
                    schema_version: 1,
                    payload: acknowledgement,
                })
                .is_none()
        );
        assert_eq!(
            backend.material_preview_state(),
            &MaterialPreviewState::Applied { preview, artefact }
        );
    }

    #[test]
    fn runtime_parameter_updates_carry_type_and_applied_generation() {
        let (editor_reader, runtime_writer) = std::io::pipe().unwrap();
        let (mut runtime_reader, editor_writer) = std::io::pipe().unwrap();
        let runtime = RuntimeSession::over(Session::over(editor_reader, editor_writer));
        let mut backend = BackendServices::new();
        backend.preview_handle = Some(0x1_0000_0001);
        backend.preview_applied_artefact = Some(0xCAFE);

        let request = backend
            .update_material_parameter(
                &runtime,
                7,
                MaterialParameterValue::Vec4([1.0, 2.0, 3.0, 4.0]),
            )
            .expect("submit a typed update");
        let frame = read_frame(&mut runtime_reader).unwrap().unwrap();
        let Message::ServiceRequest {
            request: encoded_request,
            operation,
            payload,
            ..
        } = Message::decode(&frame).unwrap()
        else {
            panic!("the update was not a service request");
        };
        assert_eq!(encoded_request, request);
        assert_eq!(operation, PREVIEW_PARAMETER_OPERATION);
        assert_eq!(&payload[0..8], &0x1_0000_0001_u64.to_le_bytes());
        assert_eq!(&payload[8..16], &0xCAFE_u64.to_le_bytes());
        assert_eq!(&payload[16..20], &7_u32.to_le_bytes());
        assert_eq!(payload[20], 4, "vec4 has a stable wire type");
        assert_eq!(payload.len(), 37);
        drop(runtime_writer);
    }

    #[test]
    fn reconnect_recreates_and_rebinds_the_last_acknowledged_artefact() {
        let mut backend = BackendServices::new();
        backend.connected = true;
        backend.preview_handle = Some(0x1_0000_0001);
        backend.preview_applied_artefact = Some(0xCAFE);
        backend.maintain(&RuntimeSession::none());
        assert_eq!(backend.preview_handle, None);
        assert_eq!(backend.preview_pending_artefact, Some(0xCAFE));
    }
}
